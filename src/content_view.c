#include "main.h"

#define COLUMN_NAME_IDX 0
#define COLUMN_TYPE_IDX 1
#define COLUMN_SIZE_IDX 2
#define COLUMN_DATE_IDX 3
#define COLUMN_PATH_IDX 4


// 目录图标缓存
// 缓存键必须带上「大图标/小图标」维度：getFileInfo 返回的是对应图像列表内的
// 索引，大小图标图像列表的索引空间不同，不区分维度必然串图标。
static int folderIconCachedForStyle = -1;   // -1 表示未缓存
static int folderIconIndex = 0;

// exe/lnk 文件图标缓存（按路径 + 图标尺寸缓存图标索引）
#define EXE_ICON_CACHE_SIZE 64
static struct {
    wchar_t path[MAX_PATH];
    int iconIndex;
    bool large;
} exeIconCache[EXE_ICON_CACHE_SIZE];
static int exeIconCacheCount = 0;
static HIMAGELIST currentImageList = NULL;

// 状态栏节流：搜索期间每批（100 项）都刷新一次状态栏没有意义，限制到约 5 次/秒。
// 最终值由 MSG_SEARCH_DONE 里的 updateStatusbar() 保证正确。
static DWORD lastStatusbarTick = 0;

// 扩展名 → 图标索引 内存缓存（仅限当次会话，不持久化）
#define EXT_CACHE_SIZE 64
struct ExtIconCacheEntry {
    wchar_t ext[16];
    int icon;
    wchar_t typeName[64];
    bool large;
};
static struct ExtIconCacheEntry extIconCache[EXT_CACHE_SIZE];
static int extCacheCount = 0;

// 一次查找同时拿到图标索引与类型名（旧实现分两次线性扫描）
static struct ExtIconCacheEntry* findExtCache(const wchar_t* ext, bool large) {
    if (!ext) return NULL;
    for (int i = 0; i < extCacheCount; i++) {
        if (extIconCache[i].large == large && wcsicmp(extIconCache[i].ext, ext) == 0)
            return &extIconCache[i];
    }
    return NULL;
}

static void addExtIconCache(const wchar_t* ext, int icon, const wchar_t* typeName, bool large) {
    if (!ext || extCacheCount >= EXT_CACHE_SIZE) return;
    wcsncpy_s(extIconCache[extCacheCount].ext, 16, ext, 15);
    extIconCache[extCacheCount].icon = icon;
    extIconCache[extCacheCount].large = large;
    if (typeName) wcsncpy_s(extIconCache[extCacheCount].typeName, 64, typeName, 63);
    else extIconCache[extCacheCount].typeName[0] = L'\0';
    extCacheCount++;
}

static int findExeIconCache(wchar_t* path, bool large) {
    if (!path || !currentImageList) return -1;
    for (int i = 0; i < exeIconCacheCount; i++) {
        if (exeIconCache[i].large == large && wcsicmp(exeIconCache[i].path, path) == 0) {
            return exeIconCache[i].iconIndex;
        }
    }
    return -1;
}

static int addExeIconCache(wchar_t* path, int iconIndex, bool large) {
    if (!path || exeIconCacheCount >= EXE_ICON_CACHE_SIZE) return iconIndex;
    wcsncpy_s(exeIconCache[exeIconCacheCount].path, MAX_PATH, path, MAX_PATH - 1);
    exeIconCache[exeIconCacheCount].iconIndex = iconIndex;
    exeIconCache[exeIconCacheCount].large = large;
    return exeIconCacheCount++, iconIndex;
}



// 添加扩展名图标缓存


enum Msg {
    MSG_ADD_ITEMS_BATCH = WM_APP,
    MSG_SEARCH_DONE,
    MSG_NAVIGATE_TO_PATH
};

enum ContextMenuType {
    MENU_SINGLE,
    MENU_MULTIPLE,
    MENU_EMPTY
};

struct ListItem {
    int icon;
    struct FileNode* node;
    wchar_t type[64];
    wchar_t formattedSize[64];
    wchar_t formattedDate[32];
    bool loaded;
    bool isHidden;
    uint64_t size;
    wchar_t* path;
    FILETIME modifiedTime;
    uint64_t driveTotalBytes;
    uint64_t driveFreeBytes;
};

// 搜索线程持有的只读节点池。
// 搜索线程绝不调用 buildChildNodes（那会 free 掉 UI 线程正在使用的节点），
// 而是用 FindFirstFile 在本地建立一棵只属于自己的树，退出时统一释放。
struct SearchNodePool {
    struct FileNode** nodes;
    int count;
    int capacity;
};

struct SearchData {
    wchar_t keyword[64];  // 拥有自己的副本，避免悬空指针
    wchar_t rootPath[MAX_PATH];  // 搜索起点路径，创建线程前由 UI 线程抓取
    volatile bool active;
    volatile bool canceled;
    HANDLE threadHandle;  // 线程句柄，用于等待线程退出
    struct SearchNodePool* pool;      // 线程建立的只读节点池
    struct FileNode** results;        // 结果数组（MSG_SEARCH_DONE 时移交 UI 线程）
    int resultCount;
};

struct SearchCache {
    wchar_t path[MAX_PATH];
    wchar_t keyword[64];
    struct FileNode** results;
    int count;
    struct SearchNodePool* pool;   // results 指向的节点由该池拥有
    time_t timestamp;
};

static struct SearchCache searchCache = {0};

// ===== 搜索节点池 =====
// 池中记录搜索线程分配过的每一个节点，释放时不依赖链表结构，
// 避免某条链忘记登记造成泄漏。
static void freeSearchPool(struct SearchNodePool* pool) {
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) {
        struct FileNode* node = pool->nodes[i];
        if (!node) continue;
        free(node->name);
        free(node);
    }
    free(pool->nodes);
    free(pool);
}

static struct FileNode* allocSearchNode(struct SearchNodePool* pool, const wchar_t* name, enum FileType type) {
    if (!pool) return NULL;
    struct FileNode* node = calloc(1, sizeof(struct FileNode));
    if (!node) return NULL;
    node->name = wcsdup(name);
    if (!node->name) { free(node); return NULL; }
    node->type = type;

    if (pool->count >= pool->capacity) {
        int newCap = pool->capacity ? pool->capacity * 2 : 64;
        struct FileNode** tmp = realloc(pool->nodes, newCap * sizeof(struct FileNode*));
        if (!tmp) { free(node->name); free(node); return NULL; }
        pool->nodes = tmp;
        pool->capacity = newCap;
    }
    pool->nodes[pool->count++] = node;
    return node;
}

struct BatchItems {
    struct FileNode** nodes;
    int count;
    int capacity;
};

struct ContextMenuItem {
    wchar_t* text;
    void(*proc)();
    wchar_t* cmdData;
};

#ifdef USE_LIBCDIO
static void onMenuItemLoadISOImageClick();
void onMenuItemUnloadISOImageClick();
#endif
static void onMenuItemShowIconClick();
static void onMenuItemOpenFileLocationClick();
static void onMenuItemOpenWithClick();
static bool isInSearchMode();

static struct ContextMenuItem cmiOpen = {NULL, &onMenuItemOpenClick, NULL};
static struct ContextMenuItem cmiEdit = {NULL, &onMenuItemEditClick, NULL};
static struct ContextMenuItem cmiCut = {NULL, &onMenuItemCutClick, NULL};
static struct ContextMenuItem cmiCopy = {NULL, &onMenuItemCopyClick, NULL};
static struct ContextMenuItem cmiCreateShortcut = {NULL, &onMenuItemCreateShortcutClick, NULL};
static struct ContextMenuItem cmiDelete = {NULL, &onMenuItemDeleteClick, NULL};
static struct ContextMenuItem cmiRename = {NULL, &onMenuItemRenameClick, NULL};
static struct ContextMenuItem cmiPaste = {NULL, &onMenuItemPasteClick, NULL};
static struct ContextMenuItem cmiPasteShortcut = {NULL, &onMenuItemPasteShortcutClick, NULL};
static struct ContextMenuItem cmiNewFolder = {NULL, &onMenuItemNewFolderClick, NULL};
static struct ContextMenuItem cmiNewFile = {NULL, &onMenuItemNewFileClick, NULL};
#ifdef USE_LIBCDIO
static struct ContextMenuItem cmiLoadISOImage = {NULL, &onMenuItemLoadISOImageClick, NULL};
static struct ContextMenuItem cmiUnloadISOImage = {NULL, &onMenuItemUnloadISOImageClick, NULL};
#endif
static struct ContextMenuItem cmiShowIcon = {NULL, &onMenuItemShowIconClick, NULL};
static struct ContextMenuItem cmiOpenWith = {NULL, &onMenuItemOpenWithClick, NULL};
static struct ContextMenuItem cmiOpenFileLocation = {NULL, &onMenuItemOpenFileLocationClick, NULL};
static void onMenuItemImportRegClick();
static struct ContextMenuItem cmiImportReg = {NULL, &onMenuItemImportRegClick, NULL};

static WNDPROC OrigWndProc;
static struct ListItem* items = NULL;
static int numItems = 0;
static int itemsCapacity = 0;
static enum ViewStyle viewStyle = STYLE_DETAILS;
static HMENU hContextMenu;
static char sortColumnIdx = COLUMN_NAME_IDX;
static bool sortAscending = true;

static struct FileNode** selectedItems = NULL;
static int numSelectedItems = 0;

static struct ContextMenuItem* menuItems = NULL;
static int numMenuItems = 0;

static struct SearchData* searchData;

// 延迟导航缓冲区（避免菜单回调深调用链导致栈溢出）
static wchar_t pendingNavigatePath[MAX_PATH] = {0};
static wchar_t pendingSelectName[MAX_PATH] = {0};

// Icon viewer dialog globals
#define MAX_ICON_GROUPS 16
#define MAX_ICONS_PER_GROUP 16
static HWND hwndIconViewer = NULL;
static int iconGroupCount = 0;
static int currentGroupIndex = 0;
static int currentIconIndex = 0;
static wchar_t iconViewerTitle[MAX_PATH] = {0};
static wchar_t iconViewerFileName[MAX_PATH] = {0};

struct IconGroup {
    HICON icons[MAX_ICONS_PER_GROUP];
    int sizes[MAX_ICONS_PER_GROUP];
    int iconCount;
};
static struct IconGroup iconGroups[MAX_ICON_GROUPS];
static void cleanupIconGroups(void);

extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;
extern HMENU hMenuView;

HWND hwndContentView = NULL;

static void fillFileInfo(struct FileNode* node, struct ListItem* item) {
    // 直接使用已保存的文件属性，无需再次调用 API
    item->size = node->size;
    item->isHidden = node->isHidden;
    memcpy(&item->modifiedTime, &node->modifiedTime, sizeof(FILETIME));
}

static void updateStatusbar() {
    wchar_t statusText[32] = {0};
    swprintf_s(statusText, 32, L"%d %ls", numItems, lc_str.items);
    setStatusbarText(statusText);   
}

static void freeMenuItems() {
    if (menuItems) {
        for (int i = 0; i < numMenuItems; i++) {
            if (menuItems[i].text) {
                free(menuItems[i].text);
                menuItems[i].text = NULL;
            }
            if (menuItems[i].cmdData) {
                free(menuItems[i].cmdData);
                menuItems[i].cmdData = NULL;
            }
        }
        free(menuItems);
        menuItems = NULL;
    }
    numMenuItems = 0;
}

void clearContentView() {    
    ListView_SetItemCountEx(hwndContentView, 0, 0);
    // 删除所有现有列（在非REPORT视图下清除列，避免残留）
    HWND hHeader = ListView_GetHeader(hwndContentView);
    if (hHeader) {
        int numCols = Header_GetItemCount(hHeader);
        for (int i = numCols - 1; i >= 0; i--) {
            ListView_DeleteColumn(hwndContentView, i);
        }
    } else {
        // 无法获取表头时，尝试删除搜索模式添加的第4列
        ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
    }

    if (items) {
        for (int i = 0; i < numItems; i++) {
            if (items[i].path) {
                free(items[i].path);
                items[i].path = NULL;
            }
        }
        free(items);
        items = NULL;
    }
    numItems = 0;
    itemsCapacity = 0;

    // 清除搜索缓存，防止文件树重建后指针悬空
    free(searchCache.results);
    searchCache.results = NULL;
    searchCache.count = 0;
    // 搜索结果所引用的节点由搜索节点池拥有，与结果同生命周期
    freeSearchPool(searchCache.pool);
    searchCache.pool = NULL;

    // 注意：图标缓存不再在此清空，以保持跨导航的加速效果
    // 只有在视图样式切换时才需要重建图像列表
    freeMenuItems();

    // Cleanup icon viewer resources
    cleanupIconGroups();
}

// 后台等待线程的参数：command 的所有权在线程结束前归它所有。
struct CommandWaitData {
    wchar_t* command;
};

// 在后台线程里执行并等待 cmd.exe 退出。
// 旧实现在 UI 线程上 WaitForSingleObject(INFINITE)：只要自定义命令启动的程序
// 不退出，整个 WFM 就完全无响应（连窗口都不能拖动）。
static DWORD WINAPI commandWaitTask(void* param) {
    struct CommandWaitData* data = (struct CommandWaitData*)param;

    SHELLEXECUTEINFO shExecInfo = {0};
    shExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExecInfo.hwnd = hwndMain;
    shExecInfo.lpVerb = NULL;
    shExecInfo.lpFile = L"C:\\windows\\system32\\cmd.exe";
    shExecInfo.lpParameters = data->command;
    shExecInfo.lpDirectory = NULL;
    shExecInfo.nShow = SW_SHOW;
    shExecInfo.hInstApp = NULL;

    // ShellExecuteEx 失败时 hProcess 为 NULL，直接传给 WaitForSingleObject 无意义
    if (ShellExecuteEx(&shExecInfo) && shExecInfo.hProcess) {
        WaitForSingleObject(shExecInfo.hProcess, INFINITE);
        CloseHandle(shExecInfo.hProcess);
    }

    free(data->command);
    free(data);
    return 0;
}

// 接管 command 的所有权（成功创建线程时由线程释放）
static void execCommandLine(wchar_t* command) {
    struct CommandWaitData* data = calloc(1, sizeof(struct CommandWaitData));
    if (!data) {
        free(command);
        return;
    }
    data->command = command;

    HANDLE thread = CreateThread(NULL, 0, commandWaitTask, data, 0, NULL);
    if (!thread) {
        free(command);
        free(data);
        return;
    }
    CloseHandle(thread);
}

LRESULT CALLBACK ContentViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            if ((HWND)lParam == 0) {
                // dwItemData 只在 GetMenuItemInfo 成功后才有效，否则是栈垃圾
                MENUITEMINFO item = {0};
                item.cbSize = sizeof(MENUITEMINFO);
                item.fMask = MIIM_DATA;
                if (!GetMenuItemInfo(hContextMenu, LOWORD(wParam), FALSE, &item)) break;
                struct ContextMenuItem* cmItem = (struct ContextMenuItem*)item.dwItemData;
                if (!cmItem) break;

                if (cmItem->cmdData) {
                    // 缓冲区按命令实际长度分配：固定 MAX_PATH 会静默丢弃较长的自定义命令
                    size_t cmdLen = wcslen(cmItem->cmdData) + 4;
                    wchar_t* command = malloc(cmdLen * sizeof(wchar_t));
                    if (command) {
                        wcscpy_s(command, cmdLen, L"/C ");
                        wcscat_s(command, cmdLen, cmItem->cmdData);
                        execCommandLine(command);   // 所有权移交（内部线程负责释放）
                    }
                    navigateRefresh();
                }
                else if (cmItem->proc) cmItem->proc();
            }           
            break;
        }
        case MSG_ADD_ITEMS_BATCH: {
            if (searchData != NULL && searchData->active) {
                struct BatchItems* batch = (struct BatchItems*)lParam;
                int newCount = numItems + batch->count;

                if (newCount > itemsCapacity) {
                    int newCapacity = itemsCapacity == 0 ? 1000 : itemsCapacity * 2;
                    if (newCapacity < newCount) newCapacity = newCount;
                    struct ListItem* tmp = realloc(items, newCapacity * sizeof(struct ListItem));
                    if (!tmp) break;
                    items = tmp;
                    itemsCapacity = newCapacity;
                }
                
                for (int i = 0; i < batch->count; i++) {
                    struct ListItem* item = &items[numItems + i];
                    item->node = batch->nodes[i];
                    item->path = NULL;
                    item->loaded = false;
                    fillFileInfo(batch->nodes[i], item);
                }
                
                numItems = newCount;
                ListView_SetItemCountEx(hwndContentView, numItems, LVSICF_NOINVALIDATEALL);

                DWORD now = GetTickCount();
                if (now - lastStatusbarTick >= 200) {
                    lastStatusbarTick = now;
                    updateStatusbar();
                }
            }
            break;
        }
        case MSG_SEARCH_DONE: {
            if (!searchData) break;   // 防御：正常情况下该消息只在 searchData 非空时投递
            bool canceled = searchData->canceled;
            searchData->active = false;
            if (searchData->threadHandle) {
                CloseHandle(searchData->threadHandle);
                searchData->threadHandle = NULL;
            }

            if (!canceled && searchData->results) {
                // 在 UI 线程接管搜索结果的数组与节点池的所有权
                free(searchCache.results);
                freeSearchPool(searchCache.pool);
                searchCache.results = searchData->results;
                searchCache.pool = searchData->pool;
                searchCache.count = searchData->resultCount;
                searchCache.timestamp = time(NULL);
                wcsncpy_s(searchCache.path, MAX_PATH, searchData->rootPath, _TRUNCATE);
                wcscpy_s(searchCache.keyword, 64, searchData->keyword);
                searchData->results = NULL;
                searchData->pool = NULL;
            }
            else {
                free(searchData->results);
                freeSearchPool(searchData->pool);
                searchData->results = NULL;
                searchData->pool = NULL;
            }

            free(searchData);
            searchData = NULL;
            if (canceled) {
                refreshContentView();
            }
            else updateStatusbar();
            break;
        }
        case MSG_NAVIGATE_TO_PATH: {
            if (pendingNavigatePath[0]) {
                // 搜索线程可能还没退出，等待MSG_SEARCH_DONE处理完再导航
                if (searchData != NULL) {
                    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
                    break;
                }
                navigateToPath(pendingNavigatePath);
                // 在新目录中查找并选中目标文件
                if (pendingSelectName[0]) {
                    for (int i = 0; i < numItems; i++) {
                        if (items[i].node && items[i].node->name &&
                            wcscmp(items[i].node->name, pendingSelectName) == 0) {
                            ListView_SetItemState(hwndContentView, -1, 0, LVIS_SELECTED);
                            ListView_SetItemState(hwndContentView, i, LVIS_SELECTED, LVIS_SELECTED);
                            ListView_EnsureVisible(hwndContentView, i, FALSE);
                            SetFocus(hwndContentView);
                            break;
                        }
                    }
                    pendingSelectName[0] = L'\0';
                }
                pendingNavigatePath[0] = L'\0';
            }
            break;
        }
        case WM_NOTIFY: {
            // Force full redraw when SIZE column is resized
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr->code == HDN_ITEMCHANGEDW || hdr->code == HDN_ITEMCHANGEDA) {
                NMHEADERW* nmh = (NMHEADERW*)hdr;
                if (nmh->iItem == COLUMN_SIZE_IDX) {
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            break;
        }
    }
    return OrigWndProc(hwnd, msg, wParam, lParam);  
}

void updateSelectedItems() {
    MEMFREE(selectedItems);
    numSelectedItems = 0;
    if (!items || numItems <= 0) return;

    int total = ListView_GetSelectedCount(hwndContentView);
    if (total <= 0) return;

    // 一次分配：旧实现逐项 realloc，全选 N 项是 O(N^2)。
    struct FileNode** tmp = calloc((size_t)total, sizeof(struct FileNode*));
    if (!tmp) return;
    selectedItems = tmp;

    for (int i = 0; i < numItems && numSelectedItems < total; i++) {
        if (ListView_GetItemState(hwndContentView, i, LVIS_SELECTED) & LVIS_SELECTED) {
            selectedItems[numSelectedItems++] = items[i].node;
        }
    }

    // 防御：如果控件上报的选中数大于实际能取到的条目（或个别条目的 node 为空），
    // 在这里把空槽压缩掉，保证下游所有 selectedItems[i]->... 的解引用都安全。
    int write = 0;
    for (int i = 0; i < numSelectedItems; i++) {
        if (selectedItems[i]) selectedItems[write++] = selectedItems[i];
    }
    numSelectedItems = write;
}

static void addContextMenuItem(HMENU hMenu, int id, struct ContextMenuItem* cmItem, bool separate) {
    if (!cmItem->text) return;
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_DATA | MIIM_ID;
    item.fType = MFT_STRING;
    item.dwTypeData = cmItem->text;
    item.cch = wcslen(cmItem->text);
    item.wID = id;
    item.dwItemData = (ULONG_PTR)cmItem;

    InsertMenuItem(hMenu, -1, TRUE, &item);

    if (separate) {
        item.fMask = MIIM_TYPE;
        item.fType = MFT_SEPARATOR;
        InsertMenuItem(hMenu, -1, TRUE, &item);
    }
}

// ===== 自定义菜单命令的安全替换 =====
// 文件名可能包含 " & ^ | % 等字符（Linux/Winlator 文件名允许，Wine 会原样映射），
// 直接拼接就等于命令注入。这里按「模板是否已经给占位符加引号」分别处理：
//   - 模板已加引号（如官方模板 ... x "%FILE%" ...）：引号内无法转义 " 和 %，
//     遇到这类字符就放弃整个菜单项，其余原样替换（保持既有模板完全兼容）；
//   - 模板未加引号：值里含空白或 cmd 元字符时补一层引号，避免参数被拆分或注入。
static bool placeholderIsQuoted(const wchar_t* cmd, const wchar_t* placeholder) {
    const wchar_t* p = wcsstr(cmd, placeholder);
    return p && p != cmd && p[-1] == L'"';
}

static wchar_t* buildCmdArg(const wchar_t* cmd, const wchar_t* placeholder, const wchar_t* value) {
    if (!value) return NULL;
    if (wcspbrk(value, L"\"%")) return NULL;

    bool quoted = placeholderIsQuoted(cmd, placeholder);
    if (quoted || !wcspbrk(value, L" \t&|<>^()")) return wcsdup(value);

    size_t len = wcslen(value);
    wchar_t* out = malloc((len + 3) * sizeof(wchar_t));
    if (!out) return NULL;
    out[0] = L'"';
    memcpy(out + 1, value, len * sizeof(wchar_t));
    out[len + 1] = L'"';
    out[len + 2] = L'\0';
    return out;
}

// 把注册表里的命令模板替换成最终命令行；任一处无法安全替换就返回 NULL
static wchar_t* buildContextMenuCommand(const wchar_t* tmpl, const wchar_t* filePath,
                                        const wchar_t* basename, const wchar_t* dirPath) {
    if (!tmpl) return NULL;
    wchar_t* cmdData = wcsdup(tmpl);
    if (!cmdData) return NULL;

    const wchar_t* specs[3][2] = {
        { L"%FILE%", filePath },
        { L"%BASENAME%", basename },
        { L"%DIR%", dirPath }
    };

    for (int s = 0; s < 3; s++) {
        if (!wcsstr(cmdData, specs[s][0])) continue;
        wchar_t* arg = buildCmdArg(cmdData, specs[s][0], specs[s][1]);
        if (!arg) { free(cmdData); return NULL; }
        wchar_t* replaced = strReplace(cmdData, specs[s][0], arg, true);
        free(arg);
        // strReplace 在分配失败时返回原串（而不是 NULL）。这种情况下占位符没被替换，
        // 留在命令行里的 %FILE% 会被 cmd.exe 当环境变量展开 —— 必须整条放弃。
        if (replaced == cmdData) { free(cmdData); return NULL; }
        cmdData = replaced;
    }
    return cmdData;
}

static void createContextMenuFromRegistry(int* id) {
    freeMenuItems();
    HKEY hkeyContextMenu, hkeyItem;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\ContextMenu", &hkeyContextMenu) != ERROR_SUCCESS) return;
    if (!selectedItems || numSelectedItems < 1 || !selectedItems[0]) {
        RegCloseKey(hkeyContextMenu);
        return;
    }

    WCHAR itemName[64] = {0};
    WCHAR subitemName[100] = {0};
    WCHAR itemValue[MAX_PATH] = {0};
    DWORD i, j, itemNameLen, itemValueLen;

    i = 0;
    while (i < 10) {
        // RegEnumKey 的 lpcchName 单位是「字符」，不是字节
        itemNameLen = (DWORD)(sizeof(itemName) / sizeof(itemName[0]));
        if (RegEnumKey(hkeyContextMenu, i++, itemName, itemNameLen) != ERROR_SUCCESS) break;
        if (RegOpenKey(hkeyContextMenu, itemName, &hkeyItem) == ERROR_SUCCESS) {
            MENUITEMINFO item = {0};
            item.cbSize = sizeof(MENUITEMINFO);
            item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
            item.fType = MFT_STRING;
            item.dwTypeData = itemName;
            item.cch = itemNameLen;
            item.wID = ++(*id);
            
            HMENU hSubmenu = CreatePopupMenu();
            
            // 结果条目文本与当前路径只取一次，循环内复用
            wchar_t filePath[MAX_PATH] = {0};
            wchar_t dirPath[MAX_PATH] = {0};
            wchar_t basename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0], filePath);
            getBasenameFromPath(filePath, basename, MAX_PATH, true);
            getFileNodePath(selectedItems[0]->parent, dirPath);

            j = 0;
            while (j < 10) {
                // name 长度单位是字符，data 长度单位是字节（Wine: RegEnumValueW 内部按
                // info->NameLength/sizeof(WCHAR) 比较 val_count，按字节比较 *count）
                itemNameLen = (DWORD)(sizeof(subitemName) / sizeof(subitemName[0]));
                itemValueLen = sizeof(itemValue);
                if (RegEnumValue(hkeyItem, j++, subitemName, &itemNameLen, NULL, NULL, (LPBYTE)itemValue, &itemValueLen) != ERROR_SUCCESS) break;

                // 无法安全替换（值里含 " 或 %）时放弃该菜单项：
                // 宁可少一项，也不执行被文件名篡改过的命令
                wchar_t* cmdData = buildContextMenuCommand(itemValue, filePath, basename, dirPath);
                if (!cmdData) break;
                if (numMenuItems >= 100) { free(cmdData); break; }

                struct ContextMenuItem* tmp = realloc(menuItems, (numMenuItems + 1) * sizeof(struct ContextMenuItem));
                if (!tmp) { free(cmdData); break; }
                menuItems = tmp;

                wchar_t* text = wcsdup(subitemName);
                if (!text) { free(cmdData); break; }

                struct ContextMenuItem* cmItem = &menuItems[numMenuItems];
                cmItem->text = text;
                cmItem->proc = NULL;
                cmItem->cmdData = cmdData;
                numMenuItems++;

                addContextMenuItem(hSubmenu, (*id)++, cmItem, false);
            }
            
            item.hSubMenu = hSubmenu;
            
            InsertMenuItem(hContextMenu, -1, TRUE, &item);
            
            item.fMask = MIIM_TYPE;
            item.fType = MFT_SEPARATOR;
            InsertMenuItem(hContextMenu, -1, TRUE, &item);
            
            RegCloseKey(hkeyItem);
        }
    }
    
    RegCloseKey(hkeyContextMenu);
}

#ifdef USE_LIBCDIO
static void createCDDriveContextMenu(int* id) {
    HMENU hSubmenu = CreatePopupMenu();
    
    wchar_t currentISOPath[MAX_PATH] = {0};
    LONG currentISOPathLen = sizeof(currentISOPath);   // RegQueryValue 的长度单位是字节
    HKEY hkey;
    bool hasISO = false;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        LONG ret = RegQueryValue(hkey, NULL, currentISOPath, &currentISOPathLen);
        // Wine 在值不存在时返回 ERROR_SUCCESS 且给出空串，必须同时判断内容
        hasISO = (ret == ERROR_SUCCESS && currentISOPath[0] != L'\0');
        RegCloseKey(hkey);
    }
    
    // 缓冲区要同时容纳前缀、完整 ISO 路径（最长 MAX_PATH）和尖括号。
    // 旧实现是 wchar_t itemText[64] 却告诉 CRT 有 MAX_PATH，属必然触发的栈溢出。
    wchar_t itemText[MAX_PATH + 64] = {0};
    swprintfTrunc(itemText, MAX_PATH + 64, L"%ls <%ls>",
                  lc_str.load_iso_image, hasISO ? currentISOPath : lc_str.no_media);

    cmiLoadISOImage.text = itemText;
    addContextMenuItem(hSubmenu, (*id)++, &cmiLoadISOImage, false);
    
    cmiLoadISOImage.text = NULL;
    addContextMenuItem(hSubmenu, (*id)++, &cmiUnloadISOImage, false);
    
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    swprintfTrunc(itemText, MAX_PATH + 64, L"%ls [X:]", lc_str.cd_drive);
    item.dwTypeData = itemText;
    item.cch = wcslen(itemText);
    item.wID = ++(*id);    
    
    item.hSubMenu = hSubmenu;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
    
    item.fMask = MIIM_TYPE;
    item.fType = MFT_SEPARATOR;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
}
#endif /* USE_LIBCDIO */


static void createContextMenu(enum ContextMenuType type) {
    HMENU hMenu = CreatePopupMenu();
    hContextMenu = hMenu;

    int id = 0;

    if (type == MENU_SINGLE || type == MENU_MULTIPLE) {
        if (type == MENU_SINGLE) {
            if (selectedItems[0]->type == TYPE_FILE) {
                addContextMenuItem(hMenu, id++, &cmiOpen, false);
                addContextMenuItem(hMenu, id++, &cmiEdit, true);
                addContextMenuItem(hMenu, id++, &cmiOpenWith, true);
                addContextMenuItem(hMenu, id++, &cmiShowIcon, true);
                #ifdef USE_LIBCDIO
                createCDDriveContextMenu(&id);
                #endif
                createContextMenuFromRegistry(&id);
            }
            else addContextMenuItem(hMenu, id++, &cmiOpen, true);
        }
        addContextMenuItem(hMenu, id++, &cmiCut, false);
        addContextMenuItem(hMenu, id++, &cmiCopy, true);
        addContextMenuItem(hMenu, id++, &cmiCreateShortcut, false);
        addContextMenuItem(hMenu, id++, &cmiDelete, false);
        
        if (type == MENU_SINGLE) {
            bool inSearch = isInSearchMode();
            addContextMenuItem(hMenu, id++, &cmiRename, inSearch);
            // 搜索模式下显示"定位到文件所在路径"
            if (inSearch) {
                addContextMenuItem(hMenu, id++, &cmiOpenFileLocation, false);
            }
            // .reg 文件显示导入到注册表菜单项
            if (selectedItems[0]->type == TYPE_FILE) {
                wchar_t filePath[MAX_PATH] = {0};
                getFileNodePath(selectedItems[0], filePath);
                if (hasFileExtension(filePath, L"reg")) {
                    addContextMenuItem(hMenu, id++, &cmiImportReg, false);
                }
            }
        }
    }
    else {
        addContextMenuItem(hMenu, id++, &cmiPaste, false);
        addContextMenuItem(hMenu, id++, &cmiPasteShortcut, true);
        #ifdef USE_LIBCDIO
        createCDDriveContextMenu(&id);
        #endif
        addContextMenuItem(hMenu, id++, &cmiNewFolder, false);
        addContextMenuItem(hMenu, id++, &cmiNewFile, false);
    }

    POINT cursor;
    GetCursorPos(&cursor);
    TrackPopupMenu(hMenu, 0, cursor.x, cursor.y, 0, hwndContentView, NULL);

    // 菜单在 TrackPopupMenu 的模态循环里被使用，返回后才能安全销毁。
    // 旧实现从不销毁，每次右键泄漏一个 HMENU 及其全部菜单项。
    hContextMenu = NULL;
    DestroyMenu(hMenu);
}

// 绘制时反复 CreateSolidBrush/DeleteObject 会带来可观的 GDI 开销，这里按颜色复用
static HBRUSH brushDriveFree = NULL;
static HBRUSH brushDriveLow = NULL;
static HBRUSH brushDriveMid = NULL;
static HBRUSH brushDriveHigh = NULL;

static HBRUSH getUiBrush(HBRUSH* slot, COLORREF color) {
    if (!*slot) *slot = CreateSolidBrush(color);
    return *slot;
}

// 按需加载单个条目的图标 / 类型名 / 大小 / 日期。
// 只在 LVN_GETDISPINFO（即可见行）里调用 —— 排序阶段绝不能调用它，
// 否则虚拟列表会被物化，排序时就把整个目录的图标都解析一遍。
static void loadItemData(struct ListItem* item) {
    if (!item || !item->node || item->loaded) return;

    const bool large = (viewStyle == STYLE_LARGE_ICON);

    if (item->node->type == TYPE_DIR) {
        // 目录图标缓存：记录缓存时使用的视图样式（大小图标的索引空间不同）
        if (folderIconCachedForStyle != (int)viewStyle) {
            wchar_t path[MAX_PATH] = {0};
            getFileNodePath(item->node, path);
            struct FileInfo fi = {0};
            getFileInfo(path, TYPE_DIR, large, &fi);
            folderIconIndex = fi.icon;
            folderIconCachedForStyle = (int)viewStyle;
        }
        item->icon = folderIconIndex;
        wcsncpy_s(item->type, 64, lc_str.folder, _TRUNCATE);
    }
    else if (item->node->type == TYPE_FILE) {
        // 获取扩展名
        wchar_t* ext = wcsrchr(item->node->name, L'.');

        // exe 和 lnk 文件不使用扩展名缓存，每个文件可能有不同图标
        bool isExeOrLnk = ext && (wcsicmp(ext, L".exe") == 0 || wcsicmp(ext, L".lnk") == 0);

        if (isExeOrLnk) {
            // exe/lnk 使用路径缓存
            wchar_t path[MAX_PATH] = {0};
            getFileNodePath(item->node, path);
            int cachedIcon = findExeIconCache(path, large);
            const wchar_t* typeName = wcsicmp(ext + 1, L"exe") == 0 ? lc_str.application : lc_str.shortcut;
            if (cachedIcon >= 0) {
                item->icon = cachedIcon;
            } else {
                struct FileInfo fi = {0};
                getFileInfo(path, TYPE_FILE, large, &fi);
                item->icon = fi.icon;
                addExeIconCache(path, fi.icon, large);
            }
            wcsncpy_s(item->type, 64, typeName, _TRUNCATE);
        } else {
            // 非 exe/lnk：先查内存缓存（一次查到图标索引 + 类型名）
            struct ExtIconCacheEntry* ce = findExtCache(ext, large);
            if (ce) {
                item->icon = ce->icon;
                if (ce->typeName[0]) wcsncpy_s(item->type, 64, ce->typeName, _TRUNCATE);
                else {
                    wchar_t upper[30] = {0};
                    if (ext && ext[1]) strToUpper(ext + 1, upper, 30);
                    swprintfTrunc(item->type, 64, lc_str.fmt_file, upper);
                }
            } else {
                wchar_t path[MAX_PATH] = {0};
                getFileNodePath(item->node, path);
                struct FileInfo fi = {0};
                getFileInfo(path, TYPE_FILE, large, &fi);
                item->icon = fi.icon;
                wcsncpy_s(item->type, 64, fi.typeName, _TRUNCATE);
                addExtIconCache(ext, fi.icon, fi.typeName, large);
            }
        }

        // 格式化文件大小和日期
        formatFileSize(item->size, item->formattedSize);
        SYSTEMTIME systemTime = {0};
        FILETIME localFiletime;
        if (FileTimeToLocalFileTime(&item->modifiedTime, &localFiletime) && FileTimeToSystemTime(&localFiletime, &systemTime)) {
            formatModifiedDate(systemTime.wMonth, systemTime.wDay, systemTime.wYear, systemTime.wHour, systemTime.wMinute, item->formattedDate, 32);
        }
    }
    else {
        // 其他类型（驱动器等）
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(item->node, path);
        struct FileInfo fi = {0};
        getFileInfo(path, item->node->type, large, &fi);
        item->icon = fi.icon;
        wcsncpy_s(item->type, 64, fi.typeName, _TRUNCATE);
        if (item->node->type == TYPE_DRIVE) {
            wchar_t rootPath[4] = {0};
            swprintfTrunc(rootPath, 4, L"%lc:\\", path[0]);
            ULARGE_INTEGER freeBytesAvail, totalBytes, freeBytesTotal;
            if (GetDiskFreeSpaceExW(rootPath, &freeBytesAvail, &totalBytes, &freeBytesTotal)) {
                item->driveTotalBytes = totalBytes.QuadPart;
                item->driveFreeBytes = freeBytesAvail.QuadPart;
                formatDriveSpace(totalBytes.QuadPart, freeBytesAvail.QuadPart, item->formattedSize, 64);
            }
        }
    }

    item->loaded = true;
}

LRESULT contentViewNotify(NMHDR* nmhdr) {
    switch (nmhdr->code) {
        case NM_CUSTOMDRAW: {
            NMLVCUSTOMDRAW* lvcd = (NMLVCUSTOMDRAW*)nmhdr;
            switch (lvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    int idx = (int)lvcd->nmcd.dwItemSpec;
                    if (idx >= 0 && idx < numItems && items[idx].node->type == TYPE_DRIVE
                        && items[idx].driveTotalBytes > 0) {
                        return CDRF_NOTIFYSUBITEMDRAW;
                    }
                    // 隐藏文件用灰色文字
                    if (idx >= 0 && idx < numItems && items[idx].isHidden) {
                        lvcd->clrText = RGB(160, 160, 160);
                    }
                    return CDRF_DODEFAULT;
                }
                case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                    int idx = (int)lvcd->nmcd.dwItemSpec;
                    if (idx >= 0 && idx < numItems && items[idx].node->type == TYPE_DRIVE
                        && items[idx].driveTotalBytes > 0 && lvcd->iSubItem == COLUMN_SIZE_IDX) {
                        
                        struct ListItem* item = &items[idx];
                        HDC hdc = lvcd->nmcd.hdc;
                        RECT rc = lvcd->nmcd.rc;
                        
                        // Margin inside the cell
                        InflateRect(&rc, -2, -1);
                        
                        // Background bar: light gray for free space
                        FillRect(hdc, &rc, getUiBrush(&brushDriveFree, RGB(230, 235, 240)));
                        
                        // Used space bar
                        double usedPct = (double)(item->driveTotalBytes - item->driveFreeBytes)
                                       / (double)item->driveTotalBytes;
                        if (usedPct < 0.0) usedPct = 0.0;
                        if (usedPct > 1.0) usedPct = 1.0;
                        int usedWidth = (int)((rc.right - rc.left) * usedPct);
                        
                        if (usedWidth > 0) {
                            RECT usedRc = rc;
                            usedRc.right = rc.left + usedWidth;
                            
                            // Color: blue (<80%), yellow (80-90%), red (>90%)
                            COLORREF barColor;
                            if (usedPct < 0.8) barColor = RGB(100, 181, 246);
                            else if (usedPct < 0.9) barColor = RGB(255, 213, 79);
                            else barColor = RGB(239, 154, 154);
                            
                            HBRUSH* slot = (barColor == RGB(100, 181, 246)) ? &brushDriveLow :
                                           (barColor == RGB(255, 213, 79))  ? &brushDriveMid : &brushDriveHigh;
                            FillRect(hdc, &usedRc, getUiBrush(slot, barColor));
                        }
                        
                        // Text overlay
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, RGB(50, 50, 50));
                        HFONT hOldFont = SelectObject(hdc, hGuiFont);
                        DrawTextW(hdc, item->formattedSize, -1, &rc,
                                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
                        SelectObject(hdc, hOldFont);
                        
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            return CDRF_DODEFAULT;
        }
        case LVN_GETDISPINFO: {
            NMLVDISPINFO* nmlvdi = (NMLVDISPINFO*)nmhdr;
            UINT mask = nmlvdi->item.mask;
            if (!items || nmlvdi->item.iItem < 0 || nmlvdi->item.iItem >= numItems) break;
            struct ListItem* item = &items[nmlvdi->item.iItem];
            
            if (!item->loaded) loadItemData(item);
            if (!item->node) break;

            // 不回应 LVIF_STATE：owner-data 模式下选中/焦点状态由控件自行维护。
            // 旧实现无条件把 state 清 0（且从不设置 stateMask），语义是错的 ——
            // 这里保持不修改，由控件决定。

            if (mask & LVIF_IMAGE) {
                nmlvdi->item.iImage = item->icon;
            }           
            
            if (mask & LVIF_TEXT) {
                switch (nmlvdi->item.iSubItem) {
                    case COLUMN_NAME_IDX:
                        nmlvdi->item.pszText = item->node->name;
                        break;
                    case COLUMN_TYPE_IDX:
                        nmlvdi->item.pszText = item->type;
                        break;
                    case COLUMN_SIZE_IDX:
                        nmlvdi->item.pszText = (item->node->type == TYPE_FILE || item->node->type == TYPE_DRIVE) ? item->formattedSize : L"";
                        break;
                    case COLUMN_DATE_IDX:
                        nmlvdi->item.pszText = item->node->type == TYPE_FILE ? item->formattedDate : L"";
                        break;
                    case COLUMN_PATH_IDX: {
                        if (!item->path) {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(item->node, path);
                            item->path = wcsdup(path);
                        }
                        nmlvdi->item.pszText = item->path;
                        break;
                    }                       
                }
            }           
            break;
        }
        case NM_RCLICK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;

            // 不限制 iSubItem：启用整行选中后，右键落在哪一列都应当弹出菜单
            if (nmia->iItem != -1) {
                updateSelectedItems();
                
                bool show = true;
                for (int i = 0; i < numSelectedItems; i++) {
                    if (!(selectedItems[i]->type == TYPE_FILE || selectedItems[i]->type == TYPE_DIR)) {
                        show = false;
                        break;
                    }
                }
                if (show) createContextMenu(numSelectedItems == 1 ? MENU_SINGLE : MENU_MULTIPLE);
            }
            else createContextMenu(MENU_EMPTY);         
            break;
        }
        case NM_DBLCLK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;
            if (nmia->iItem == -1) break;
            
            if (!items || nmia->iItem >= numItems) break;
            struct ListItem* item = &items[nmia->iItem];            
            openFileNode(item->node);
            break;
        }
        case LVN_COLUMNCLICK: {
            LPNMLISTVIEW plvInfo = (LPNMLISTVIEW)nmhdr;

            if (plvInfo->iSubItem == sortColumnIdx) {
                sortAscending = !sortAscending;
            }
            else {
                sortColumnIdx = plvInfo->iSubItem;
                sortAscending = true;
            }

            refreshContentView();
            break;
        }       
    }

    return 0;   
}

// 按路径字符串建立一条属于搜索线程的节点链，返回最深一级（搜索起点）。
// 拆分方式与 setCurrPathFromString 保持一致：第一段是盘符。
static struct FileNode* createSearchRootNode(struct SearchNodePool* pool, const wchar_t* path) {
    if (!pool || !path || path[0] == L'\0') return NULL;

    wchar_t tmp[MAX_PATH] = {0};
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);

    struct FileNode* leaf = NULL;
    wchar_t* saveptr = NULL;
    wchar_t* token = wcstok(tmp, L"\\", &saveptr);
    int i = 0;
    while (token) {
        enum FileType type = (i++ == 0) ? TYPE_DRIVE : TYPE_DIR;
        struct FileNode* node = allocSearchNode(pool, token, type);
        if (!node) return NULL;
        node->parent = leaf;
        leaf = node;
        token = wcstok(NULL, L"\\", &saveptr);
    }
    return leaf;
}

// 只读枚举目录项并在搜索节点池里建链 —— 绝不修改（更不能释放）UI 线程的文件树。
static struct FileNode* enumChildrenForSearch(struct SearchNodePool* pool, struct FileNode* parent) {
    if (!pool || !parent) return NULL;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(parent, path);
    if (path[0] == L'\0') return NULL;
    if (wcslen(path) + 2 >= MAX_PATH) return NULL;
    wcscat_s(path, MAX_PATH, path[wcslen(path) - 1] == L'\\' ? L"*" : L"\\*");

    WIN32_FIND_DATA wfd = {0};
    HANDLE handle = FindFirstFile(path, &wfd);
    if (handle == INVALID_HANDLE_VALUE) return NULL;

    struct FileNode* first = NULL;
    struct FileNode* last = NULL;
    do {
        if (wfd.cFileName[0] == L'.' && (wfd.cFileName[1] == L'\0' ||
            (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))) continue;
        if (!g_showHiddenFiles && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;

        bool isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        struct FileNode* child = allocSearchNode(pool, wfd.cFileName, isDir ? TYPE_DIR : TYPE_FILE);
        if (!child) continue;
        child->parent = parent;
        child->isHidden = (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        if (isDir) {
            child->hasChildDirs = true;
        }
        else {
            LARGE_INTEGER filesize;
            filesize.LowPart = wfd.nFileSizeLow;
            filesize.HighPart = wfd.nFileSizeHigh;
            child->size = filesize.QuadPart;
            memcpy(&child->modifiedTime, &wfd.ftLastWriteTime, sizeof(FILETIME));
        }

        if (!first) first = child;
        if (last) last->sibling = child;
        last = child;
    }
    while (FindNextFile(handle, &wfd));
    FindClose(handle);
    return first;
}

static DWORD WINAPI searchTask(void* param) {
    struct SearchData* searchData = (struct SearchData*)param;
    const int maxStackSize = 50;
    struct FileNode* stack[maxStackSize];
    int stackSize = 0;

    struct SearchNodePool* pool = calloc(1, sizeof(struct SearchNodePool));
    wchar_t keyword[64] = {0};
    strToLower(searchData->keyword, keyword, 64);

    const int BATCH_SIZE = 100;
    struct BatchItems batch;
    batch.capacity = BATCH_SIZE;
    batch.nodes = malloc(batch.capacity * sizeof(struct FileNode*));
    batch.count = 0;

    int cacheCapacity = 1000;
    struct FileNode** cacheResults = malloc(cacheCapacity * sizeof(struct FileNode*));
    int cacheCount = 0;
    int addedCount = 0;   // 本线程自己的计数，不再读 UI 线程的 numItems

    if (!pool || !batch.nodes || !cacheResults) {
        free(batch.nodes);
        free(cacheResults);
        searchData->pool = pool;
        SendMessage(hwndContentView, MSG_SEARCH_DONE, 0, 0);
        return 0;
    }

    // 搜索起点：用线程自己建立的节点链，完全不依赖 UI 线程正在使用的文件树
    struct FileNode* root = createSearchRootNode(pool, searchData->rootPath);
    if (root) {
        struct FileNode* rootChildren = enumChildrenForSearch(pool, root);
        if (rootChildren) stack[stackSize++] = rootChildren;
    }

    while (stackSize > 0 && addedCount < 10000 && searchData->active) {
        struct FileNode* node = stack[--stackSize];
        while (node && searchData->active) {
            if (wcsstrIgnoreCase(node->name, keyword)) {
                batch.nodes[batch.count++] = node;

                if (cacheCount >= cacheCapacity) {
                    int newCap = cacheCapacity * 2;
                    struct FileNode** tmp = realloc(cacheResults, newCap * sizeof(struct FileNode*));
                    if (!tmp) break;
                    cacheResults = tmp;
                    cacheCapacity = newCap;
                }
                cacheResults[cacheCount++] = node;
                
                if (batch.count >= BATCH_SIZE) {
                    SendMessage(hwndContentView, MSG_ADD_ITEMS_BATCH, 0, (LPARAM)&batch);
                    addedCount += batch.count;
                    batch.count = 0;
                }
            }
            
            if (addedCount >= 10000) break;
            
            if (node->type == TYPE_DIR && stackSize < maxStackSize) {
                struct FileNode* children = enumChildrenForSearch(pool, node);
                if (children) stack[stackSize++] = children;
            }
            node = node->sibling;       
        }
    }
    
    if (batch.count > 0 && searchData->active) {
        SendMessage(hwndContentView, MSG_ADD_ITEMS_BATCH, 0, (LPARAM)&batch);
        addedCount += batch.count;
    }
    
    free(batch.nodes);

    // 结果与节点池一起交给 UI 线程处理（MSG_SEARCH_DONE 里决定并入缓存还是释放）
    if (searchData->active) {
        searchData->results = cacheResults;
        searchData->resultCount = cacheCount;
    }
    else {
        free(cacheResults);
        searchData->results = NULL;
        searchData->resultCount = 0;
    }
    searchData->pool = pool;

    SendMessage(hwndContentView, MSG_SEARCH_DONE, 0, 0);
    return 0;
}

static bool isSearchCacheValid(const wchar_t* path, const wchar_t* keyword) {
    if (searchCache.count == 0) return false;
    if (wcscmp(searchCache.path, path) != 0) return false;
    if (wcscmp(searchCache.keyword, keyword) != 0) return false;
    time_t now = time(NULL);
    if (now - searchCache.timestamp > 30) return false;
    return true;
}

void createLVColumns();

// 取消当前搜索（不等待线程退出，避免死锁）
static void cancelSearch() {
    if (searchData == NULL) return;
    if (searchData->active) {
        searchData->active = false;
        searchData->canceled = true;
    }
}

void searchFor(wchar_t* keyword) {
    if (wcslen(keyword) == 0) return;
    // 如果有搜索正在进行或正在清理中，取消并等待 MSG_SEARCH_DONE 自然清理
    if (searchData != NULL) {
        if (searchData->active) {
            searchData->active = false;
            searchData->canceled = true;
        }
        return;
    }
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    
    if (isSearchCacheValid(path, keyword)) {
        // 先保存缓存数据到局部变量，因为clearContentView会清空缓存
        int cachedCount = searchCache.count;
        struct FileNode** cachedResults = searchCache.results;
        // 结果节点由节点池拥有，必须连同池一起移出，否则会被 clearContentView 释放
        struct SearchNodePool* cachedPool = searchCache.pool;
        searchCache.results = NULL;
        searchCache.count = 0;
        searchCache.pool = NULL;

        clearContentView();
        createLVColumns();

        LVCOLUMN column = {0};
        column.mask = LVCF_WIDTH | LVCF_TEXT;
        column.cx = 250;
        column.pszText = lc_str.path;
        ListView_InsertColumn(hwndContentView, COLUMN_PATH_IDX, &column);

        items = malloc(cachedCount * sizeof(struct ListItem));
        if (items) {
            itemsCapacity = cachedCount;
            numItems = cachedCount;

            for (int i = 0; i < cachedCount; i++) {
                struct ListItem* item = &items[i];
                memset(item, 0, sizeof(struct ListItem));
                item->node = cachedResults[i];
                item->path = NULL;
                item->loaded = false;
                fillFileInfo(cachedResults[i], item);
            }
        }
        else {
            itemsCapacity = 0;
            numItems = 0;
        }

        // 重新缓存（指针仍然有效，因为currPathFileNode未变）
        searchCache.results = cachedResults;
        searchCache.count = cachedCount;
        searchCache.pool = cachedPool;
        searchCache.timestamp = time(NULL);

        ListView_SetItemCountEx(hwndContentView, numItems, 0);
        updateStatusbar();
        return;
    }

    clearContentView();
    createLVColumns();

    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 250;
    column.pszText = lc_str.path;
    ListView_InsertColumn(hwndContentView, COLUMN_PATH_IDX, &column);
    UpdateWindow(hwndContentView);
    
    searchData = calloc(1, sizeof(struct SearchData));
    if (!searchData) return;
    wcscpy_s(searchData->keyword, 64, keyword);
    // 搜索起点路径在此处（UI 线程）抓取，搜索线程不再读取 UI 线程的文件树
    wcsncpy_s(searchData->rootPath, MAX_PATH, path, _TRUNCATE);
    searchData->active = true;
    searchData->canceled = false;
    searchData->results = NULL;
    searchData->resultCount = 0;
    searchData->pool = NULL;
    searchData->threadHandle = CreateThread(NULL, 0, searchTask, searchData, 0, NULL);
    if (!searchData->threadHandle) {
        // 线程创建失败：自己清理，否则 refreshContentView 会一直被 searchData 挡住
        free(searchData);
        searchData = NULL;
    }
}

static void saveViewStyle(void);

void setViewStyle(enum ViewStyle newViewStyle) {
    LONG_PTR wndstyle = GetWindowLongPtr(hwndContentView, GWL_STYLE);
    wndstyle &= ~LVS_TYPEMASK;

    switch (newViewStyle) {
        case STYLE_LARGE_ICON:
            wndstyle |= LVS_ICON;
            break;
        case STYLE_SMALL_ICON:
            wndstyle |= LVS_SMALLICON;
            break;
        case STYLE_LIST:
            wndstyle |= LVS_LIST;
            break;
        case STYLE_DETAILS:
            wndstyle |= LVS_REPORT;
            break;
    }

    SetWindowLongPtr(hwndContentView, GWL_STYLE, wndstyle);
    // 强制 ListView 识别样式变更并重新布局
    SetWindowPos(hwndContentView, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 必须先把图标缓存清空再刷新：大/小图标在系统图像列表里的索引不同，
    // 旧实现先 refresh 再清缓存，refresh 里命中的是上一个尺寸的索引 —— 这正是
    // README 里「图标混淆」的根因。
    clearIconCaches();
    viewStyle = newViewStyle;
    refreshContentView();

    // 图标视图需要重新排列：样式切换时 LISTVIEW_StyleChanged 会按旧的条目数排布
    if (viewStyle == STYLE_LARGE_ICON || viewStyle == STYLE_SMALL_ICON) {
        ListView_Arrange(hwndContentView, LVA_DEFAULT);
        InvalidateRect(hwndContentView, NULL, FALSE);
    }

    // 持久化视图样式到注册表
    saveViewStyle();
    // 更新菜单栏选中标记
    updateViewMenuCheckmarks();
}

static void saveViewStyle(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)viewStyle;
        RegSetValueEx(hkey, L"ViewStyle", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

enum ViewStyle loadViewStyle(void) {
    HKEY hkey;
    DWORD val = (DWORD)STYLE_DETAILS;
    DWORD size = sizeof(val);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegQueryValueEx(hkey, L"ViewStyle", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hkey);
    }
    if (val > STYLE_DETAILS) val = (DWORD)STYLE_DETAILS;
    return (enum ViewStyle)val;
}

void updateViewMenuCheckmarks(void) {
    if (!hMenuView) return;
    UINT first = ID_VIEW_LARGEICONS;
    UINT last  = ID_VIEW_DETAILS;
    UINT check;
    switch (viewStyle) {
        case STYLE_LARGE_ICON:  check = ID_VIEW_LARGEICONS; break;
        case STYLE_SMALL_ICON:  check = ID_VIEW_SMALLICONS; break;
        case STYLE_LIST:        check = ID_VIEW_LIST;       break;
        case STYLE_DETAILS:
        default:                check = ID_VIEW_DETAILS;    break;
    }
    CheckMenuRadioItem(hMenuView, first, last, check, MF_BYCOMMAND);
}

void createLVColumns() {
    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;

    column.cx = 220;
    column.pszText = lc_str.name;
    ListView_InsertColumn(hwndContentView, COLUMN_NAME_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.type;
    ListView_InsertColumn(hwndContentView, COLUMN_TYPE_IDX, &column);

    column.cx = 170;
    column.pszText = lc_str.size;
    ListView_InsertColumn(hwndContentView, COLUMN_SIZE_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.date;
    ListView_InsertColumn(hwndContentView, COLUMN_DATE_IDX, &column);
}

void createContentView() {
    hwndContentView = CreateWindowEx(0, WC_LISTVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS,
                                     0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);

    // LVS_EX_DOUBLEBUFFER：Wine 的 comctl32 完整实现（拦截 WM_ERASEBKGND + 内存 DC 绘制），
    // 同时改善闪烁与重绘开销；LVS_EX_FULLROWSELECT：整行可选中
    ListView_SetExtendedListViewStyle(hwndContentView, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);

    cmiOpen.text = lc_str.open;
    cmiEdit.text = lc_str.edit;
    cmiCut.text = lc_str.cut;
    cmiCopy.text = lc_str.copy;
    cmiCreateShortcut.text = lc_str.create_shortcut;
    cmiDelete.text = lc_str.delete;
    cmiRename.text = lc_str.rename;
    cmiPaste.text = lc_str.paste;
    cmiPasteShortcut.text = lc_str.paste_shortcut;
    cmiNewFolder.text = lc_str.new_folder;
    cmiNewFile.text = lc_str.new_file;
#ifdef USE_LIBCDIO
    cmiLoadISOImage.text = NULL;
    cmiUnloadISOImage.text = lc_str.unload_iso_image;
#endif
    cmiShowIcon.text = lc_str.show_icon;
    cmiOpenWith.text = lc_str.open_with_menu;
    cmiOpenFileLocation.text = lc_str.open_file_location;
    cmiImportReg.text = lc_str.import_reg;
    
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndContentView, GWLP_WNDPROC, (LONG_PTR)ContentViewWndProc);
    createLVColumns();
    UpdateWindow(hwndContentView);
}

void onMenuItemUpClick() {
    navigateUp();
}

void onMenuItemOpenClick() {
    if (numSelectedItems == 1) openFileNode(selectedItems[0]);
}

static void onMenuItemOpenWithClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        wchar_t parentPath[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        getFileNodePath(selectedItems[0]->parent, parentPath);
        
        wchar_t* ext = wcsrchr(selectedItems[0]->name, L'.');
        bool removeAssoc = false;
        wchar_t* chosen = showOpenWithDialog(path, ext, &removeAssoc);
        
        if (removeAssoc && ext) {
            removeFileAssociation(ext);
        } else if (chosen) {
            ShellExecute(hwndMain, L"open", chosen, path, parentPath, SW_SHOW);
            free(chosen);
        }
    }
}

void onMenuItemEditClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        static const wchar_t editorPath[] = L"C:\\windows\\notepad.exe";
        
        wchar_t path[MAX_PATH] = {0};
        wchar_t parameters[MAX_PATH + 8] = {0};
        getFileNodePath(selectedItems[0], path);
        if (wcschr(path, L'"')) return;   // 含引号的路径无法安全拼进命令行
        swprintfTrunc(parameters, MAX_PATH + 8, L"\"%ls\"", path);
        getFileNodePath(selectedItems[0]->parent, path);
        ShellExecute(hwndMain, L"open", editorPath, parameters, path, SW_SHOW);     
    }   
}

void onMenuItemCutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) cutFiles(selectedItems, numSelectedItems);    
}

void onMenuItemCopyClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) copyFiles(selectedItems, numSelectedItems);
}

void onMenuItemCreateShortcutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) createDesktopShortcuts(selectedItems, numSelectedItems);  
}

void onMenuItemDeleteClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) deleteFiles(selectedItems, numSelectedItems); 
}

void onMenuItemRenameClick() {
    if (numSelectedItems == 1) {
        wchar_t* result = InputDialog(lc_str.rename, lc_str.enter_new_name, selectedItems[0]->name, true);
        if (result) {
            wchar_t newFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0]->parent, newFilename);
            wcscat_s(newFilename, MAX_PATH, L"\\");
            wcscat_s(newFilename, MAX_PATH, result);
            free(result);
            
            wchar_t oldFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0], oldFilename);
            MoveFileW(oldFilename, newFilename);
            navigateRefresh();
        }
    }
}

void onMenuItemPasteClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteFiles(path);
}

void onMenuItemPasteShortcutClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteShortcuts(path);   
}

void onMenuItemNewFolderClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    
    wchar_t* result = InputDialog(lc_str.new_folder, lc_str.enter_folder_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);
        
        if (!isPathExists(path)) {
            CreateDirectory(path, NULL);
            navigateRefresh();          
        }
    }   
}

void onMenuItemNewFileClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    
    wchar_t* result = InputDialog(lc_str.new_file, lc_str.enter_file_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);
        
        if (!isPathExists(path)) {
            HANDLE handle = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            navigateRefresh();  
        }
    }       
}

void onMenuItemSelectAllClick() {
    ListView_SetItemState(hwndContentView, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(hwndContentView, -1, LVIS_SELECTED, LVIS_SELECTED);
    SetFocus(hwndContentView);
}

static bool isInSearchMode() {
    HWND hHeader = ListView_GetHeader(hwndContentView);
    if (!hHeader) return false;
    return Header_GetItemCount(hHeader) > COLUMN_PATH_IDX;
}

static void onMenuItemOpenFileLocationClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;

    struct FileNode* node = selectedItems[0];
    if (!node || !node->parent) return;

    // 仅支持文件和文件夹
    if (node->type != TYPE_FILE && node->type != TYPE_DIR) return;

    // 先保存路径字符串，因为导航后旧文件树会被释放，node指针失效
    wchar_t parentPath[MAX_PATH] = {0};
    getFileNodePath(node->parent, parentPath);
    wcscpy_s(pendingSelectName, MAX_PATH, node->name);

    // 路径为空或不存在则提示
    if (parentPath[0] == L'\0' || !isPathExists(parentPath)) {
        wchar_t msg[MAX_PATH + 64] = {0};
        swprintf_s(msg, MAX_PATH + 64, lc_str.bookmark_path_not_found,
                   parentPath[0] ? parentPath : node->name);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK);
        return;
    }

    // 如果搜索还在运行，先取消，否则refreshContentView会提前返回导致悬空指针
    cancelSearch();

    // 延迟导航：菜单回调深调用链会导致栈溢出，用PostMessage在回调返回后执行
    wcscpy_s(pendingNavigatePath, MAX_PATH, parentPath);
    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
}

void onBookmarkButtonClick() {
    addCurrentPathToBookmark();
}

static void onMenuItemImportRegClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;
    if (selectedItems[0]->type != TYPE_FILE) return;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(selectedItems[0], path);
    if (path[0] == L'\0') return;

    // 使用 regedit.exe /s 静默导入 .reg 文件
    // 先提示用户确认
    wchar_t msg[MAX_PATH + 64] = {0};
    swprintf_s(msg, MAX_PATH + 64, L"%ls\n\n%ls", selectedItems[0]->name, lc_str.import_reg);
    int result = MessageBox(hwndMain, msg, lc_str.import_reg, MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES) {
        if (!wcschr(path, L'"')) {   // 含引号的路径会逃逸引号，直接拒绝
            wchar_t params[MAX_PATH + 8] = {0};
            swprintfTrunc(params, MAX_PATH + 8, L"/s \"%ls\"", path);
            ShellExecute(hwndMain, L"open", L"regedit.exe", params, NULL, SW_SHOW);
        }
    }
}

#ifdef USE_LIBCDIO
static void onMenuItemLoadISOImageClick() {
    if (numSelectedItems != 1) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    wchar_t currentISOPath[MAX_PATH] = {0};
    HKEY hkey;
    getFileNodePath(selectedItems[0], currentISOPath);

    if (!isPathExists(currentISOPath) || !(hasFileExtension(currentISOPath, L"iso") ||
                                           hasFileExtension(currentISOPath, L"bin") ||
                                           hasFileExtension(currentISOPath, L"cue"))) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    if (GetDriveTypeW(L"X:\\") == DRIVE_NO_ROOT_DIR) {
        MessageBox(hwndMain, lc_str.msg_x_drive_not_found, lc_str.alert, MB_OK | MB_ICONWARNING);
        return;
    }

    if (RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        RegSetValue(hkey, NULL, REG_SZ, currentISOPath, (wcslen(currentISOPath) + 1) * sizeof(wchar_t));
        RegCloseKey(hkey);
    }

    clearDirectory(L"X:");
    extractFilesFromISOImage(currentISOPath, L"X:\\");
}

void onMenuItemUnloadISOImageClick() {
    // S9：clearDirectory(L"X:") 是递归永久删除，而 README 指导用户把 X: 软链到真实目录。
    // 一次误点就会清空真实目录，因此这里必须先二次确认。
    if (MessageBox(hwndMain, lc_str.msg_confirm_unmount_iso, lc_str.unmount_iso,
                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    clearDirectory(L"X:");
    RegDeleteKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath");
    navigateRefresh();
}

void onMenuItemLocateISOImageClick() {
    wchar_t currentISOPath[MAX_PATH] = {0};
    if (!getCurrentISOPath(currentISOPath)) {
        MessageBox(NULL, lc_str.msg_no_mounted_image, lc_str.alert, MB_OK);
        return;
    }
    wchar_t parentDir[MAX_PATH] = {0};
    getParentDirFromPath(currentISOPath, parentDir);
    if (!isPathExists(parentDir)) {
        MessageBox(NULL, lc_str.msg_image_dir_not_found, lc_str.alert, MB_OK);
        return;
    }
    // 提取ISO文件名，导航后选中该文件
    wchar_t isoFileName[MAX_PATH] = {0};
    getBasenameFromPath(currentISOPath, isoFileName, MAX_PATH, false);
    wcscpy_s(pendingSelectName, MAX_PATH, isoFileName);
    
    // 如果搜索还在运行，先取消
    cancelSearch();
    
    // 延迟导航：使用消息机制确保导航后能选中文件
    wcscpy_s(pendingNavigatePath, MAX_PATH, parentDir);
    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
}
#endif /* USE_LIBCDIO */

// ========== GDI+ PNG decoder (bypasses Wine's buggy PNG icon loading) ==========
#include <shlwapi.h>

/* GDI+ flat API - manually declared for C99 compatibility */
typedef int GpStatus;
typedef void GpBitmap;
typedef void GpImage;
typedef struct {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;
typedef struct { int dummy; } GdiplusStartupOutput;
#define WINGDIPAPI __stdcall
#define GDIPCONST const

/* GpStatus values */
enum { Ok = 0 };

/* Forward declarations for GDI+ flat API (gdiplus.dll) */
GpStatus WINGDIPAPI GdiplusStartup(ULONG_PTR*, GDIPCONST GdiplusStartupInput*, GdiplusStartupOutput*);
VOID     WINGDIPAPI GdiplusShutdown(ULONG_PTR);
GpStatus WINGDIPAPI GdipCreateBitmapFromStream(IStream*, GpBitmap**);
GpStatus WINGDIPAPI GdipCreateHICONFromBitmap(GpBitmap*, HICON*);
GpStatus WINGDIPAPI GdipDisposeImage(GpImage*);

static ULONG_PTR g_gdiplusToken = 0;

static void initGdiplus(void) {
    if (g_gdiplusToken) return;
    GdiplusStartupInput input;
    memset(&input, 0, sizeof(input));
    input.GdiplusVersion = 1;
    GdiplusStartup(&g_gdiplusToken, &input, NULL);
}

/* Decode PNG data directly using GDI+, bypassing Wine's load_png which has
   a bug where png_set_bgr() is missing for 24-bit RGB PNGs (R/B swapped).
   Returns an HICON or NULL on failure. Caller must DestroyIcon(). */
static HICON createIconFromPngData(const BYTE* data, DWORD dataSize) {
    if (!data || dataSize < 8) return NULL;
    // 上限校验：这是不受信任的 PNG 数据交给 GDI+ 解析的攻击面，先限制体积
    if (dataSize > 64u * 1024u * 1024u) return NULL;

    /* Verify PNG signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A */
    static const BYTE pngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(data, pngSig, 8) != 0) return NULL;

    initGdiplus();

    /* Create IStream from memory */
    IStream* pStream = SHCreateMemStream(data, dataSize);
    if (!pStream) return NULL;

    /* Decode PNG to GDI+ Bitmap */
    GpBitmap* pBitmap = NULL;
    GpStatus status = GdipCreateBitmapFromStream(pStream, &pBitmap);
    IStream_Release(pStream);

    if (status != Ok || !pBitmap) return NULL;

    /* Convert Bitmap to HICON */
    HICON hIcon = NULL;
    status = GdipCreateHICONFromBitmap(pBitmap, &hIcon);
    GdipDisposeImage((GpImage*)pBitmap);

    return (status == Ok) ? hIcon : NULL;
}

// ========== PE icon extraction (bypasses Wine's buggy icon APIs) ==========

#pragma pack(push, 2)
typedef struct {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    WORD nID;
} GRPICONDIRENTRY;

typedef struct {
    WORD idReserved;
    WORD idType;
    WORD idCount;
    GRPICONDIRENTRY idEntries[1];
} GRPICONDIR;
#pragma pack(pop)

// Create HICON from raw ICO image data (BITMAPINFOHEADER + XOR data + AND mask).
// This parses the icon data directly, bypassing Wine's icon compositing which
// can produce color-inverted results for multi-size icon groups.
static HICON createIconFromRawData(const BYTE* data, DWORD dataSize) {
    if (!data || dataSize < sizeof(BITMAPINFOHEADER)) return NULL;

    const BITMAPINFOHEADER* bih = (const BITMAPINFOHEADER*)data;
    int width = bih->biWidth;
    int height = bih->biHeight ? bih->biHeight / 2 : 0;
    int bpp = bih->biBitCount;

    // 尺寸上限：宽高直接来自被解析文件，必须限制，否则下面的乘法会溢出
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;

    const BYTE* xorData = data + sizeof(BITMAPINFOHEADER);
    size_t xorRowSize;

    if (bpp == 32) {
        xorRowSize = (size_t)width * 4;
    } else if (bpp == 24) {
        xorRowSize = ((size_t)width * 3 + 3) / 4 * 4;
    } else if (bpp == 8) {
        xorRowSize = ((size_t)width + 3) / 4 * 4;
    } else if (bpp == 4) {
        xorRowSize = (((size_t)width + 1) / 2 + 3) / 4 * 4;
    } else {
        return NULL;
    }

    // For palettized formats, skip the palette
    size_t paletteEntries = 0;
    if (bpp == 8) paletteEntries = 256;
    else if (bpp == 4) paletteEntries = 16;
    size_t paletteSize = paletteEntries * 4;
    size_t xorTotalSize = xorRowSize * (size_t)height;
    size_t andRowSize = ((size_t)width + 31) / 32 * 4;

    // 边界检查全部用 size_t 计算：旧实现用 int，宽高过大时 xorTotalSize 溢出为小值，
    // 于是这里检查通过，随后按巨大尺寸越界读取像素
    size_t requiredSize = sizeof(BITMAPINFOHEADER) + paletteSize + xorTotalSize + andRowSize * (size_t)height;
    if (requiredSize > dataSize) return NULL;

    const BYTE* xorPixels = xorData + paletteSize;
    const BYTE* andData = xorPixels + xorTotalSize;

    // Create 32bpp ARGB DIB section
    HDC hdc = GetDC(NULL);
    if (!hdc) return NULL;

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* bits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    ReleaseDC(NULL, hdc);

    if (!hBmp || !bits) return NULL;

    // Convert XOR data to 32bpp ARGB (source is bottom-up in ICO format)
    for (int y = 0; y < height; y++) {
        const BYTE* srcRow = xorPixels + (size_t)(height - 1 - y) * xorRowSize;
        BYTE* dstRow = bits + (size_t)y * width * 4;
        if (bpp == 32) {
            memcpy(dstRow, srcRow, width * 4);
        } else if (bpp == 24) {
            for (int x = 0; x < width; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // B
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // R
                dstRow[x * 4 + 3] = 255;
            }
        } else if (bpp == 8) {
            const BYTE* palette = xorData;
            for (int x = 0; x < width; x++) {
                BYTE idx = srcRow[x];
                dstRow[x * 4 + 0] = palette[idx * 4 + 0]; // B
                dstRow[x * 4 + 1] = palette[idx * 4 + 1]; // G
                dstRow[x * 4 + 2] = palette[idx * 4 + 2]; // R
                dstRow[x * 4 + 3] = 255;
            }
        } else if (bpp == 4) {
            const BYTE* palette = xorData;
            for (int x = 0; x < width; x++) {
                BYTE val = srcRow[x / 2];
                BYTE idx = (x & 1) ? (val & 0x0F) : (val >> 4);
                dstRow[x * 4 + 0] = palette[idx * 4 + 0];
                dstRow[x * 4 + 1] = palette[idx * 4 + 1];
                dstRow[x * 4 + 2] = palette[idx * 4 + 2];
                dstRow[x * 4 + 3] = 255;
            }
        }
    }

    // Check if alpha channel is present and valid
    BOOL hasAlpha = FALSE;
    if (bpp == 32) {
        for (int i = 0; i < width * height && !hasAlpha; i++) {
            if (bits[i * 4 + 3] != 0) hasAlpha = TRUE;
        }
    }

    // If no alpha (common Wine bug for multi-icon EXEs), use AND mask
    if (!hasAlpha) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int andByteIdx = y * andRowSize + (x / 8);
                int andBitIdx = 7 - (x % 8);
                BOOL transparent = (andData[andByteIdx] >> andBitIdx) & 1;

                int px = (y * width + x) * 4;
                if (transparent) {
                    bits[px + 0] = 0;
                    bits[px + 1] = 0;
                    bits[px + 2] = 0;
                    bits[px + 3] = 0;
                } else {
                    bits[px + 3] = 255;
                }
            }
        }
    }

    // Create icon from fixed bitmap
    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    ICONINFO iconInfo = {0};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = hBmp;
    iconInfo.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&iconInfo);

    DeleteObject(hBmp);
    DeleteObject(hMask);
    return hIcon;
}

struct GroupIconEnumData {
    HRSRC hRes[MAX_ICON_GROUPS];
    int count;
};

static BOOL CALLBACK enumGroupIconProc(HMODULE hModule, LPCWSTR lpType, LPWSTR lpName, LONG_PTR lParam) {
    (void)lpType;   // 枚举时只关心 RT_GROUP_ICON，类型参数不使用
    struct GroupIconEnumData* data = (struct GroupIconEnumData*)lParam;
    if (data->count < MAX_ICON_GROUPS) {
        HRSRC hRes = FindResourceW(hModule, lpName, RT_GROUP_ICON);
        if (hRes) {
            data->hRes[data->count++] = hRes;
        }
    }
    return TRUE; // Continue enumerating
}

// Extract ALL icon groups from a PE file, each with all its icon sizes.
// Returns number of icon groups extracted. Populates iconGroups[].
static int extractAllIconGroupsFromPE(const wchar_t* filePath) {
    for (int i = 0; i < MAX_ICON_GROUPS; i++) {
        iconGroups[i].iconCount = 0;
        for (int j = 0; j < MAX_ICONS_PER_GROUP; j++) {
            iconGroups[i].icons[j] = NULL;
        }
    }
    if (!filePath) return 0;

    HMODULE hModule = LoadLibraryExW(filePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hModule) return 0;

    struct GroupIconEnumData enumData = {0};
    EnumResourceNamesW(hModule, RT_GROUP_ICON, enumGroupIconProc, (LONG_PTR)&enumData);

    int totalGroups = 0;
    for (int g = 0; g < enumData.count && totalGroups < MAX_ICON_GROUPS; g++) {
        HGLOBAL hGlob = LoadResource(hModule, enumData.hRes[g]);
        if (!hGlob) continue;

        const GRPICONDIR* grpDir = (const GRPICONDIR*)LockResource(hGlob);
        if (!grpDir) continue;

        int count = 0;
        WORD nIcons = grpDir->idCount;
        if (nIcons > MAX_ICONS_PER_GROUP) nIcons = MAX_ICONS_PER_GROUP;

        for (WORD i = 0; i < nIcons; i++) {
            const GRPICONDIRENTRY* entry = &grpDir->idEntries[i];

            HRSRC hIconRes = FindResourceW(hModule, MAKEINTRESOURCE(entry->nID), RT_ICON);
            if (!hIconRes) continue;

            DWORD iconResSize = SizeofResource(hModule, hIconRes);
            HGLOBAL hIconGlob = LoadResource(hModule, hIconRes);
            if (!hIconGlob) continue;

            const BYTE* iconData = (const BYTE*)LockResource(hIconGlob);
            if (!iconData || iconResSize == 0) continue;

            HICON hIcon = createIconFromRawData(iconData, iconResSize);
            if (!hIcon) {
                /* Try GDI+ PNG decoder first (bypasses Wine's load_png R/B swap bug) */
                hIcon = createIconFromPngData(iconData, iconResSize);
            }
            if (!hIcon) {
                /* Last resort: use Wine's API (may have color issues for some formats) */
                hIcon = CreateIconFromResourceEx((PBYTE)iconData, iconResSize,
                    TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
            }
            if (hIcon) {
                int size = entry->bWidth;
                if (size == 0) size = 256; // 0 means 256 in ICO format
                iconGroups[totalGroups].icons[count] = hIcon;
                iconGroups[totalGroups].sizes[count] = size;
                count++;
            }
        }

        if (count > 0) {
            iconGroups[totalGroups].iconCount = count;
            totalGroups++;
        }
    }

    FreeLibrary(hModule);
    return totalGroups;
}

// ========== Icon viewer window ==========

static void cleanupIconGroups(void) {
    for (int g = 0; g < MAX_ICON_GROUPS; g++) {
        for (int i = 0; i < MAX_ICONS_PER_GROUP; i++) {
            if (iconGroups[g].icons[i]) {
                DestroyIcon(iconGroups[g].icons[i]);
                iconGroups[g].icons[i] = NULL;
            }
        }
        iconGroups[g].iconCount = 0;
    }
    iconGroupCount = 0;
}

static BOOL saveIconToFile(HICON hIcon, const wchar_t* filePath) {
    if (!hIcon || !filePath) return FALSE;

    ICONINFO ii = {0};
    if (!GetIconInfo(hIcon, &ii)) return FALSE;

    int iconWidth = 0, iconHeight = 0;

    if (ii.hbmColor) {
        BITMAP bm = {0};
        GetObjectW(ii.hbmColor, sizeof(BITMAP), &bm);
        iconWidth = bm.bmWidth;
        iconHeight = bm.bmHeight;
    }

    if (ii.hbmMask && iconWidth <= 0) {
        BITMAP bmMask = {0};
        GetObjectW(ii.hbmMask, sizeof(BITMAP), &bmMask);
        iconWidth = bmMask.bmWidth;
        iconHeight = bmMask.bmHeight;
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (iconWidth <= 0 || iconHeight <= 0) return FALSE;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = iconWidth;
    bmi.bmiHeader.biHeight = -iconHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* dibBits = NULL;
    HBITMAP hbmDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&dibBits, NULL, 0);
    if (!hbmDib || !dibBits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmDib);

    RECT rc = {0, 0, iconWidth, iconHeight};
    HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, hBr);
    DeleteObject(hBr);

    DrawIconEx(hdcMem, 0, 0, hIcon, iconWidth, iconHeight, 0, NULL, DI_NORMAL);
    GdiFlush();

    size_t colorSize = (size_t)iconWidth * (size_t)iconHeight * 4;
    BYTE* colorBits = (BYTE*)malloc(colorSize);
    if (colorBits) {
        memcpy(colorBits, dibBits, colorSize);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmDib);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (!colorBits) return FALSE;

    size_t maskSize = ((size_t)(iconWidth + 31) / 32) * 4 * (size_t)iconHeight;
    BYTE* maskBits = (BYTE*)calloc(1, maskSize);
    if (!maskBits) {
        free(colorBits);
        return FALSE;
    }

    // 行缓冲在打开文件之前分配：分配失败就整体失败，
    // 而不是退化写入导致生成的 .ico 上下颠倒却毫无提示
    size_t rowSize = (size_t)iconWidth * 4;
    BYTE* rowBuf = (BYTE*)malloc(rowSize);
    if (!rowBuf) {
        free(colorBits);
        free(maskBits);
        return FALSE;
    }

    BOOL result = FALSE;
    HANDLE hFile = CreateFileW(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        DWORD dibSize = sizeof(BITMAPINFOHEADER);
        DWORD totalImageSize = dibSize + (DWORD)colorSize + (DWORD)maskSize;
        DWORD dataOffset = 6 + 16;

        WORD reserved = 0, type = 1, count = 1;
        WriteFile(hFile, &reserved, 2, &written, NULL);
        WriteFile(hFile, &type, 2, &written, NULL);
        WriteFile(hFile, &count, 2, &written, NULL);

        BYTE w = (iconWidth >= 256) ? 0 : (BYTE)iconWidth;
        BYTE h = (iconHeight >= 256) ? 0 : (BYTE)iconHeight;
        BYTE colors = 0, res = 0;
        WORD planes = 1, bpp = 32;
        WriteFile(hFile, &w, 1, &written, NULL);
        WriteFile(hFile, &h, 1, &written, NULL);
        WriteFile(hFile, &colors, 1, &written, NULL);
        WriteFile(hFile, &res, 1, &written, NULL);
        WriteFile(hFile, &planes, 2, &written, NULL);
        WriteFile(hFile, &bpp, 2, &written, NULL);
        WriteFile(hFile, &totalImageSize, 4, &written, NULL);
        WriteFile(hFile, &dataOffset, 4, &written, NULL);

        BITMAPINFOHEADER bih = {0};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = iconWidth;
        bih.biHeight = iconHeight * 2;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        WriteFile(hFile, &bih, sizeof(bih), &written, NULL);

        for (int row = 0; row < iconHeight; row++) {
            memcpy(rowBuf, colorBits + (size_t)(iconHeight - 1 - row) * rowSize, rowSize);
            WriteFile(hFile, rowBuf, (DWORD)rowSize, &written, NULL);
        }

        WriteFile(hFile, maskBits, (DWORD)maskSize, &written, NULL);

        CloseHandle(hFile);
        result = TRUE;
    }

    free(rowBuf);
    free(colorBits);
    free(maskBits);

    return result;
}

static HICON getCurrentIcon(void) {
    if (currentGroupIndex < 0 || currentGroupIndex >= iconGroupCount) return NULL;
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    if (currentIconIndex < 0 || currentIconIndex >= grp->iconCount) return NULL;
    return grp->icons[currentIconIndex];
}

static void updateViewerTitle(void) {
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    int size = grp->sizes[currentIconIndex];
    swprintf_s(iconViewerTitle, MAX_PATH, L"%ls - [%d/%d] %dx%d (%d/%d) - %ls",
        iconViewerFileName,
        currentGroupIndex + 1, iconGroupCount,
        size, size,
        currentIconIndex + 1, grp->iconCount,
        lc_str.show_icon);
    SetWindowTextW(hwndIconViewer, iconViewerTitle);
}

static void destroySizeButtons(HWND hwnd) {
    for (int i = 0; i < MAX_ICONS_PER_GROUP; i++) {
        HWND hBtn = GetDlgItem(hwnd, IDC_SIZE_BASE + i);
        if (hBtn) DestroyWindow(hBtn);
    }
}

static void createSizeButtons(HWND hwnd) {
    destroySizeButtons(hwnd);

    if (currentGroupIndex < 0 || currentGroupIndex >= iconGroupCount) return;
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    int n = grp->iconCount;

    int btnW = 70, btnH = 28, gap = 6;
    int btnsPerRow = 4;
    int rowW = btnsPerRow * btnW + (btnsPerRow - 1) * gap;
    int startX = (380 - rowW) / 2;
    int btnY = 285;  // Adjusted for larger icon area

    // Track unique sizes to avoid duplicate buttons
    int uniqueSizes[MAX_ICONS_PER_GROUP];
    int uniqueCount = 0;
    
    for (int i = 0; i < n; i++) {
        int s = grp->sizes[i];
        BOOL isDuplicate = FALSE;
        
        // Check if we already have this size
        for (int j = 0; j < uniqueCount; j++) {
            if (uniqueSizes[j] == s) {
                isDuplicate = TRUE;
                break;
            }
        }
        
        if (!isDuplicate) {
            uniqueSizes[uniqueCount] = s;
            uniqueCount++;
            
            int row = (uniqueCount - 1) / btnsPerRow;
            int col = (uniqueCount - 1) % btnsPerRow;
            wchar_t label[16];
            swprintf_s(label, 16, L"%dx%d", s, s);

            HWND hBtn = CreateWindowW(L"BUTTON", label,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                startX + col * (btnW + gap), btnY + row * (btnH + gap), btnW, btnH,
                hwnd, (HMENU)(INT_PTR)(IDC_SIZE_BASE + i), globalHInstance, NULL);
            if (hBtn && hGuiFont) {
                SendMessageW(hBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
            }
        }
    }

    // Reposition the save button below the last row of size buttons
    int nRows = (uniqueCount + btnsPerRow - 1) / btnsPerRow;
    int saveBtnY = btnY + nRows * (btnH + gap) + 5;
    HWND hSaveBtn = GetDlgItem(hwnd, IDC_SAVE_ICON);
    if (hSaveBtn) {
        SetWindowPos(hSaveBtn, NULL, 90, saveBtnY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

// Static variable to store filename clickable area
static RECT g_filenameRect = {0};

// Window procedure for filename popup window
static LRESULT CALLBACK FilenamePopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK IconViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Group navigation arrows
            int navY = 285;
            int arrowW = 28, arrowH = 28;

            HWND hPrev = CreateWindowW(L"BUTTON", L"\u25C0",
                WS_CHILD | WS_VISIBLE | (iconGroupCount > 1 ? 0 : WS_DISABLED) | BS_PUSHBUTTON,
                10, navY, arrowW, arrowH,
                hwnd, (HMENU)IDC_PREV_GROUP, globalHInstance, NULL);
            if (hPrev && hGuiFont) SendMessageW(hPrev, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            HWND hNext = CreateWindowW(L"BUTTON", L"\u25B6",
                WS_CHILD | WS_VISIBLE | (iconGroupCount > 1 ? 0 : WS_DISABLED) | BS_PUSHBUTTON,
                380 - 10 - arrowW, navY, arrowW, arrowH,
                hwnd, (HMENU)IDC_NEXT_GROUP, globalHInstance, NULL);
            if (hNext && hGuiFont) SendMessageW(hNext, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            // Save button - create first so createSizeButtons can reposition it
            HWND hSaveBtn = CreateWindowW(L"BUTTON", lc_str.save_icon,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                90, 400, 200, 28,
                hwnd, (HMENU)IDC_SAVE_ICON, globalHInstance, NULL);
            if (hSaveBtn && hGuiFont) SendMessageW(hSaveBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            // Dynamic size buttons + reposition save button
            createSizeButtons(hwnd);

            updateViewerTitle();
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HICON hCurrent = getCurrentIcon();
            if (hCurrent) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int cx = rc.right - rc.left;
                int iconAreaCy = 280;

                int curSize = iconGroups[currentGroupIndex].sizes[currentIconIndex];
                int iconCx = min(cx - 20, curSize);
                int iconCy = min(iconAreaCy - 10, curSize);
                int drawSize = min(iconCx, iconCy);
                int x = (cx - drawSize) / 2;
                int y = (iconAreaCy - drawSize) / 2;
                if (y < 5) y = 5;

                DrawIconEx(hdc, x, y, hCurrent, drawSize, drawSize, 0, NULL, DI_NORMAL);
            }

            // Draw separator line below Save Icon button
            RECT clientRc;
            GetClientRect(hwnd, &clientRc);
            int separatorY = clientRc.bottom - 35;  // 35 pixels from bottom for filename
            
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
            HPEN hOldPen = SelectObject(hdc, hPen);
            MoveToEx(hdc, 10, separatorY, NULL);
            LineTo(hdc, clientRc.right - 10, separatorY);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            // Draw filename below separator (clickable)
            if (iconViewerFileName[0] != L'\0') {
                RECT textRc = {10, separatorY + 5, clientRc.right - 10, clientRc.bottom - 5};
                g_filenameRect = textRc;  // Store for click detection
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(0, 102, 204));  // Blue color for clickable text
                
                // Create underlined font for clickable effect
                HFONT hUnderlineFont = CreateFontW(
                    -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72),  // Height
                    0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,  // Underline=TRUE
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI");
                
                HFONT hOldFont = SelectObject(hdc, hUnderlineFont);
                DrawTextW(hdc, iconViewerFileName, -1, &textRc, 
                    DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(hdc, hOldFont);
                DeleteObject(hUnderlineFont);
            }

            EndPaint(hwnd, &ps);
            break;
        }
        case WM_COMMAND: {
            int cmdId = LOWORD(wParam);
            if (HIWORD(wParam) == BN_CLICKED) {
                if (cmdId == IDC_PREV_GROUP) {
                    if (iconGroupCount > 1) {
                        currentGroupIndex = (currentGroupIndex - 1 + iconGroupCount) % iconGroupCount;
                        currentIconIndex = 0;
                        createSizeButtons(hwnd);
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId == IDC_NEXT_GROUP) {
                    if (iconGroupCount > 1) {
                        currentGroupIndex = (currentGroupIndex + 1) % iconGroupCount;
                        currentIconIndex = 0;
                        createSizeButtons(hwnd);
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId >= IDC_SIZE_BASE && cmdId < IDC_SIZE_BASE + MAX_ICONS_PER_GROUP) {
                    int idx = cmdId - IDC_SIZE_BASE;
                    if (idx != currentIconIndex && idx < iconGroups[currentGroupIndex].iconCount) {
                        currentIconIndex = idx;
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId == IDC_SAVE_ICON) {
                    HICON hCurrent = getCurrentIcon();
                    if (!hCurrent) break;

                    wchar_t filePath[MAX_PATH + 8] = {0};
                    swprintfTrunc(filePath, MAX_PATH + 8, L"%ls.ico", iconViewerFileName);
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH + 8;
                    ofn.lpstrFilter = L"Icon Files (*.ico)\0*.ico\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrDefExt = L"ico";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                    if (GetSaveFileNameW(&ofn)) {
                        if (!saveIconToFile(hCurrent, filePath)) {
                            MessageBoxW(hwnd, L"保存图标失败", lc_str.alert, MB_OK | MB_ICONERROR);
                        }
                    }
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int xPos = LOWORD(lParam);
            int yPos = HIWORD(lParam);
            POINT pt = {xPos, yPos};
            
            // Check if click is within filename area
            if (PtInRect(&g_filenameRect, pt) && iconViewerFileName[0] != L'\0') {
                // Register popup window class if not already registered
                WNDCLASSEX wcPopup = {0};
                wcPopup.cbSize = sizeof(WNDCLASSEX);
                wcPopup.lpfnWndProc = FilenamePopupWndProc;
                wcPopup.hInstance = globalHInstance;
                wcPopup.hCursor = LoadCursor(NULL, IDC_ARROW);
                wcPopup.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
                wcPopup.lpszClassName = L"FilenamePopupClass";
                RegisterClassEx(&wcPopup);
                
                // Create popup window to show full filename
                HWND hPopup = CreateWindowEx(
                    WS_EX_TOPMOST,
                    L"FilenamePopupClass",
                    L"File Name",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 420, 180,
                    hwnd, NULL, globalHInstance, NULL);
                
                if (hPopup) {
                    // Calculate position to center popup on screen
                    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                    int popupW = 420, popupH = 180;
                    int popupX = (screenWidth - popupW) / 2;
                    int popupY = (screenHeight - popupH) / 2;
                    
                    SetWindowPos(hPopup, NULL, popupX, popupY, popupW, popupH, SWP_SHOWWINDOW);
                    
                    // Create edit control to display full filename with scrolling
                    HWND hEdit = CreateWindowEx(
                        WS_EX_CLIENTEDGE, L"EDIT", iconViewerFileName,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | 
                        ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        10, 10, popupW - 36, popupH - 95,
                        hPopup, NULL, globalHInstance, NULL);
                    
                    if (hEdit && hGuiFont) {
                        SendMessageW(hEdit, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
                    }
                    
                    // Create close button
                    HWND hCloseBtn = CreateWindowW(L"BUTTON", L"Close",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        (popupW - 100) / 2, popupH - 55, 100, 28,
                        hPopup, (HMENU)IDCANCEL, globalHInstance, NULL);
                    
                    if (hCloseBtn && hGuiFont) {
                        SendMessageW(hCloseBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
                    }
                    
                    // Set focus to edit control
                    SetFocus(hEdit);
                    
                    // Show the window
                    ShowWindow(hPopup, SW_SHOW);
                    UpdateWindow(hPopup);
                }
            }
            break;
        }
        case WM_SETCURSOR: {
            // Change cursor to hand when hovering over filename
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g_filenameRect, pt)) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
            FillRect(hdc, &rc, hBrush);
            return 1;
        }
        case WM_CLOSE: {
            cleanupIconGroups();
            hwndIconViewer = NULL;
            DestroyWindow(hwnd);
            break;
        }
        case WM_DESTROY: {
            break;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void registerIconViewerClass() {
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = IconViewerWndProc;
    wc.hInstance = globalHInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"IconViewerClass";
    RegisterClassEx(&wc);
}

static void showIconInNewWindow(wchar_t* filePath, wchar_t* fileName) {
    cleanupIconGroups();

    iconGroupCount = extractAllIconGroupsFromPE(filePath);
    currentGroupIndex = 0;
    currentIconIndex = 0;

    if (iconGroupCount == 0) {
        MessageBox(hwndMain, L"无法提取文件图标", lc_str.alert, MB_OK);
        return;
    }

    // Pick the best (largest) icon in the first group as default
    struct IconGroup* grp = &iconGroups[0];
    int bestIdx = 0;
    for (int i = 1; i < grp->iconCount; i++) {
        if (grp->sizes[i] > grp->sizes[bestIdx]) bestIdx = i;
    }
    currentIconIndex = bestIdx;

    wcscpy_s(iconViewerFileName, MAX_PATH, fileName);

    WNDCLASSEX wcCheck = {0};
    if (!GetClassInfoEx(globalHInstance, L"IconViewerClass", &wcCheck)) {
        registerIconViewerClass();
    }

    // Compute client area height based on the group with the most unique icon sizes
    int maxUniqueSizes = 0;
    for (int g = 0; g < iconGroupCount; g++) {
        int uniqueSizes[MAX_ICONS_PER_GROUP];
        int uniqueCount = 0;
        
        for (int i = 0; i < iconGroups[g].iconCount; i++) {
            int s = iconGroups[g].sizes[i];
            BOOL isDuplicate = FALSE;
            
            // Check if we already have this size
            for (int j = 0; j < uniqueCount; j++) {
                if (uniqueSizes[j] == s) {
                    isDuplicate = TRUE;
                    break;
                }
            }
            
            if (!isDuplicate) {
                uniqueSizes[uniqueCount] = s;
                uniqueCount++;
            }
        }
        
        if (uniqueCount > maxUniqueSizes)
            maxUniqueSizes = uniqueCount;
    }
    
    int btnsPerRow = 4;
    int maxRows = (maxUniqueSizes + btnsPerRow - 1) / btnsPerRow;
    // Client layout: icon(280) + navRow(35) + sizeBtnRows(34 each) + saveBtn(28) + separator(10) + filename(20) + padding(15)
    int clientH = 280 + 35 + maxRows * 34 + 28 + 10 + 20 + 15;
    if (clientH < 420) clientH = 420;
    int clientW = 380;

    // Convert client size to window size (includes title bar, borders)
    DWORD dwStyle = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
    RECT rc = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - winWidth) / 2;
    int y = (screenHeight - winHeight) / 2;

    hwndIconViewer = CreateWindowEx(
        0,
        L"IconViewerClass",
        L"",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        x, y, winWidth, winHeight,
        hwndMain,
        NULL,
        globalHInstance,
        NULL
    );

    if (!hwndIconViewer) {
        cleanupIconGroups();
        MessageBox(hwndMain, L"无法创建图标查看窗口", lc_str.alert, MB_OK);
        return;
    }

    ShowWindow(hwndIconViewer, SW_SHOW);
    UpdateWindow(hwndIconViewer);
}

static void onMenuItemShowIconClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;
    
    struct FileNode* node = selectedItems[0];
    if (!node || !node->name) return;
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(node, path);
    
    if (!isPathExists(path)) {
        MessageBox(hwndMain, L"文件不存在", lc_str.alert, MB_OK);
        return;
    }
    
    showIconInNewWindow(path, node->name);
}


// ===== 排序键 =====
// 排序键严格无副作用：不查图标缓存、不访问文件系统。
// 旧实现的比较器会调用 ensureItemTypeLoaded（内部走 SHGetFileInfo / 路径拼接），
// 而 qsort 必然触达每个元素 —— 连「按名称排序」都会把整个目录的图标物化一遍，
// LVS_OWNERDATA 的懒加载设计被完全抵消（含 exe 的目录会白屏数百毫秒）。
static void getSortTypeName(const struct ListItem* item, wchar_t* buf, int bufSize) {
    buf[0] = L'\0';
    if (!item || !item->node) return;
    const struct FileNode* node = item->node;

    switch (node->type) {
        case TYPE_DIR:
        case TYPE_PERSONAL:
        case TYPE_USERPROFILE:
            wcsncpy_s(buf, (size_t)bufSize, lc_str.folder, _TRUNCATE);
            return;
        case TYPE_DRIVE:
            wcsncpy_s(buf, (size_t)bufSize,
                      isCDDrivePath(node->name) ? lc_str.cd_drive : lc_str.local_drive, _TRUNCATE);
            return;
        case TYPE_DESKTOP:
            wcsncpy_s(buf, (size_t)bufSize, lc_str.desktop, _TRUNCATE);
            return;
        case TYPE_COMPUTER:
            wcsncpy_s(buf, (size_t)bufSize, lc_str.computer, _TRUNCATE);
            return;
        default:
            break;
    }

    wchar_t* ext = wcsrchr(node->name, L'.');
    if (!ext || ext == node->name) {
        wcsncpy_s(buf, (size_t)bufSize, lc_str.file, _TRUNCATE);
        return;
    }
    if (wcsicmp(ext, L".exe") == 0) {
        wcsncpy_s(buf, (size_t)bufSize, lc_str.application, _TRUNCATE);
        return;
    }
    if (wcsicmp(ext, L".lnk") == 0) {
        wcsncpy_s(buf, (size_t)bufSize, lc_str.shortcut, _TRUNCATE);
        return;
    }
    wchar_t upper[30] = {0};
    strToUpper(ext + 1, upper, 30);
    swprintfTrunc(buf, (size_t)bufSize, lc_str.fmt_file, upper);
}

static int compareTypeName(const struct ListItem* ia, const struct ListItem* ib) {
    wchar_t typeA[64] = {0};
    wchar_t typeB[64] = {0};
    getSortTypeName(ia, typeA, 64);
    getSortTypeName(ib, typeB, 64);
    return wcscmp(typeA, typeB);
}

static int compareNameOnly(const struct ListItem* ia, const struct ListItem* ib) {
    const wchar_t* na = (ia->node && ia->node->name) ? ia->node->name : L"";
    const wchar_t* nb = (ib->node && ib->node->name) ? ib->node->name : L"";
    return wcscmp(na, nb);
}

static int finalizeCompare(int res) {
    return sortAscending ? res : -res;
}

static int compareName(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareNameOnly(ia, ib);
    if (res == 0) res = compareTypeName(ia, ib);
    return finalizeCompare(res);
}

static int compareType(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareTypeName(ia, ib);
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

static int compareSize(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    // 主键就是大小。旧实现先比类型/名称，而且用 uint64 相减截断成 int，
    // 两个 >2GB 的文件差值超过 INT_MAX 时排序结果随机错乱。
    int res = 0;
    if (ia->size < ib->size) res = -1;
    else if (ia->size > ib->size) res = 1;
    if (res == 0) res = compareTypeName(ia, ib);
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

static int compareDate(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = CompareFileTime(&ia->modifiedTime, &ib->modifiedTime);
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

void clearIconCaches() {
    extCacheCount = 0;
    exeIconCacheCount = 0;
    folderIconCachedForStyle = -1;
}

void sortItems() {
    if (!items || numItems <= 0) return;
    switch (sortColumnIdx) {
        case COLUMN_NAME_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareName);
            break;
        case COLUMN_TYPE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareType);
            break;
        case COLUMN_SIZE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareSize);
            break;
        case COLUMN_DATE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareDate);
            break;
    }
}

void refreshContentView() {
    if (searchData != NULL) {
        if (searchData->active) {
            searchData->active = false;
            searchData->canceled = true;
        }
        return;
    }
    
    // 清空项目数据（保留列，避免在详细信息视图中删除/重建列导致的闪烁）
    ListView_SetItemCountEx(hwndContentView, 0, 0);
    
    if (items) {
        for (int i = 0; i < numItems; i++) {
            if (items[i].path) {
                free(items[i].path);
                items[i].path = NULL;
            }
        }
        free(items);
        items = NULL;
    }
    numItems = 0;
    freeMenuItems();

    // 清理图标查看器资源
    cleanupIconGroups();
    
    // 仅在非详细信息视图中删除列（避免列闪烁）
    if (viewStyle != STYLE_DETAILS) {
        HWND hHeader = ListView_GetHeader(hwndContentView);
        if (hHeader) {
            int numCols = Header_GetItemCount(hHeader);
            for (int i = numCols - 1; i >= 0; i--) {
                ListView_DeleteColumn(hwndContentView, i);
            }
        } else {
            ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
        }
    }
    
    // 详细信息视图：保留列（避免删除/重建导致的闪烁），仅在首次进入时创建
    if (viewStyle == STYLE_DETAILS) {
        HWND hHeader = ListView_GetHeader(hwndContentView);
        if (!hHeader || Header_GetItemCount(hHeader) == 0) {
            createLVColumns();
        } else {
            // 搜索模式遗留的PATH列需要移除（非搜索模式下不需要）
            int numCols = Header_GetItemCount(hHeader);
            if (numCols > COLUMN_PATH_IDX) {
                ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
            }
        }
    }

    struct FileNode* child = currPathFileNode->children;

    // 单次遍历：计数并填充
    int capacity = 64;
    numItems = 0;
    items = malloc(capacity * sizeof(struct ListItem));
    if (!items) { itemsCapacity = 0; return; }

    while (child) {
        if (numItems >= capacity) {
            capacity *= 2;
            struct ListItem* tmp = realloc(items, capacity * sizeof(struct ListItem));
            if (!tmp) break;
            items = tmp;
        }
        struct ListItem* item = &items[numItems++];
        memset(item, 0, sizeof(struct ListItem));
        item->node = child;
        item->loaded = false;

        fillFileInfo(child, item);
        
        child = child->sibling;
    }

    // 图标缓存保留（不清空），以加速相邻导航
    // 视图切换时更新图像列表
    HIMAGELIST himlBig, himlSmall;
    Shell_GetImageLists(&himlBig, &himlSmall);
    
    if (viewStyle == STYLE_LARGE_ICON) {
        currentImageList = himlBig;
        ListView_SetImageList(hwndContentView, himlBig, LVSIL_NORMAL);
    }
    else {
        currentImageList = himlSmall;
        ListView_SetImageList(hwndContentView, himlSmall, LVSIL_SMALL);
    }

    if (sortColumnIdx != -1) sortItems();

    // 合并重绘：一次设置条目数 + 一次滚动更新即完成刷新。
    // 旧实现每次导航触发 4 次整表失效（SetItemCountEx(0) → SetItemCountEx(n) →
    // icon 视图的 Arrange + 再次 SetItemCountEx → InvalidateRect），每次失效都会
    // 为所有可见行重新派发 LVN_GETDISPINFO。
    // 注意：Wine 在图标视图下会强制丢弃 LVSICF_NOINVALIDATEALL（见
    // dlls/comctl32/listview.c LISTVIEW_SetItemCount），此时退化为整表失效，仍然正确。
    ListView_SetItemCountEx(hwndContentView, numItems, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    updateStatusbar();
}