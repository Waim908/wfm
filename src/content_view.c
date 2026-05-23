#include "main.h"

#define COLUMN_NAME_IDX 0
#define COLUMN_TYPE_IDX 1
#define COLUMN_SIZE_IDX 2
#define COLUMN_DATE_IDX 3
#define COLUMN_PATH_IDX 4


// 目录图标缓存
static int folderIconCached = 0;
static int folderIconIndex = 0;

// exe/lnk 文件图标缓存（按路径缓存图标索引）
#define EXE_ICON_CACHE_SIZE 64
static struct {
    wchar_t path[MAX_PATH];
    int iconIndex;
} exeIconCache[EXE_ICON_CACHE_SIZE];
static int exeIconCacheCount = 0;
static HIMAGELIST currentImageList = NULL;

// 扩展名 → 图标索引 内存缓存（仅限当次会话，不持久化）
#define EXT_CACHE_SIZE 64
static struct {
    wchar_t ext[16];
    int icon;
    wchar_t typeName[64];
} extIconCache[EXT_CACHE_SIZE];
static int extCacheCount = 0;

// 快速查找
static int findExtIconCache(const wchar_t* ext) {
    if (!ext) return -1;
    for (int i = 0; i < extCacheCount; i++) {
        if (wcsicmp(extIconCache[i].ext, ext) == 0)
            return extIconCache[i].icon;
    }
    return -1;
}

static const wchar_t* findExtTypeNameCache(const wchar_t* ext) {
    if (!ext) return NULL;
    for (int i = 0; i < extCacheCount; i++) {
        if (wcsicmp(extIconCache[i].ext, ext) == 0)
            return extIconCache[i].typeName;
    }
    return NULL;
}

static void addExtIconCache(const wchar_t* ext, int icon, const wchar_t* typeName) {
    if (!ext || extCacheCount >= EXT_CACHE_SIZE) return;
    wcsncpy_s(extIconCache[extCacheCount].ext, 16, ext, 15);
    extIconCache[extCacheCount].icon = icon;
    if (typeName) wcsncpy_s(extIconCache[extCacheCount].typeName, 64, typeName, 63);
    else extIconCache[extCacheCount].typeName[0] = L'\0';
    extCacheCount++;
}

static int findExeIconCache(wchar_t* path) {
    if (!path || !currentImageList) return -1;
    for (int i = 0; i < exeIconCacheCount; i++) {
        if (wcsicmp(exeIconCache[i].path, path) == 0) {
            return exeIconCache[i].iconIndex;
        }
    }
    return -1;
}

static int addExeIconCache(wchar_t* path, int iconIndex) {
    if (!path || exeIconCacheCount >= EXE_ICON_CACHE_SIZE) return iconIndex;
    wcsncpy_s(exeIconCache[exeIconCacheCount].path, MAX_PATH, path, MAX_PATH - 1);
    exeIconCache[exeIconCacheCount].iconIndex = iconIndex;
    return exeIconCacheCount++, iconIndex;
}



// 添加扩展名图标缓存


enum Msg {
    MSG_ADD_ITEM = WM_APP,
    MSG_SEARCH_DONE
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
    wchar_t formattedSize[32];
    wchar_t formattedDate[32];
    bool loaded;
    uint64_t size;
    wchar_t* path;
    FILETIME modifiedTime;
};

struct SearchData {
    wchar_t* keyword;
    bool active;
    bool canceled;
};

struct ContextMenuItem {
    wchar_t* text;
    void(*proc)();
    wchar_t* cmdData;
};

static void onMenuItemLoadISOImageClick();
void onMenuItemUnloadISOImageClick();
static void onMenuItemShowIconClick();

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
static struct ContextMenuItem cmiLoadISOImage = {NULL, &onMenuItemLoadISOImageClick, NULL};
static struct ContextMenuItem cmiUnloadISOImage = {NULL, &onMenuItemUnloadISOImageClick, NULL};
static struct ContextMenuItem cmiShowIcon = {NULL, &onMenuItemShowIconClick, NULL};

static WNDPROC OrigWndProc;
static struct ListItem* items = NULL;
static int numItems = 0;
static enum ViewStyle viewStyle = STYLE_DETAILS;
static HMENU hContextMenu;
static char sortColumnIdx = COLUMN_NAME_IDX;
static bool sortAscending = true;

static struct FileNode** selectedItems = NULL;
static int numSelectedItems = 0;

static struct ContextMenuItem* menuItems = NULL;
static int numMenuItems = 0;

static struct SearchData* searchData;

// Icon viewer dialog globals
static HWND hwndIconViewer = NULL;
static HICON hIconLarge = NULL;
static HICON hIconSmall = NULL;
static wchar_t iconViewerTitle[MAX_PATH] = {0};
static wchar_t iconViewerFileName[MAX_PATH] = {0};

extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndContentView = NULL;

static void fillFileInfo(struct FileNode* node, struct ListItem* item) {
    // 直接使用已保存的文件属性，无需再次调用 API
    item->size = node->size;
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
    
    // 注意：图标缓存不再在此清空，以保持跨导航的加速效果
    // 只有在视图样式切换时才需要重建图像列表
    freeMenuItems();
    
    // Cleanup icon viewer resources
    if (hIconLarge) {
        DestroyIcon(hIconLarge);
        hIconLarge = NULL;
    }
    if (hIconSmall) {
        DestroyIcon(hIconSmall);
        hIconSmall = NULL;
    }
}

static void execCommandLine(wchar_t *command) {
    SHELLEXECUTEINFO shExecInfo = {0};
    shExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExecInfo.hwnd = hwndMain;
    shExecInfo.lpVerb = NULL;
    shExecInfo.lpFile = L"C:\\windows\\system32\\cmd.exe";        
    shExecInfo.lpParameters = command;   
    shExecInfo.lpDirectory = NULL;
    shExecInfo.nShow = SW_SHOW;
    shExecInfo.hInstApp = NULL; 
    ShellExecuteEx(&shExecInfo);
    WaitForSingleObject(shExecInfo.hProcess, INFINITE);
    CloseHandle(shExecInfo.hProcess);    
}

LRESULT CALLBACK ContentViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            if ((HWND)lParam == 0) {
                MENUITEMINFO item;
                item.cbSize = sizeof(MENUITEMINFO);
                item.fMask = MIIM_DATA;
                GetMenuItemInfo(hContextMenu, LOWORD(wParam), FALSE, &item);
                struct ContextMenuItem* cmItem = (struct ContextMenuItem*)item.dwItemData;
                
                if (cmItem->cmdData) {
                    wchar_t command[MAX_PATH];
                    wcscpy_s(command, MAX_PATH, L"/C ");
                    wcscat_s(command, MAX_PATH, cmItem->cmdData);
                    execCommandLine(command);
                    navigateRefresh();
                }
                else cmItem->proc();
            }           
            break;
        }
        case MSG_ADD_ITEM: {
            if (searchData != NULL && searchData->active) {
                struct FileNode* node = (struct FileNode*)lParam;
                int index = numItems++;
                items = realloc(items, numItems * sizeof(struct ListItem));     
                struct ListItem* item = &items[index];
                item->node = node;
                item->path = NULL;
                item->loaded = false;
                
                fillFileInfo(node, item);
                
                ListView_SetItemCountEx(hwndContentView, numItems, LVSICF_NOINVALIDATEALL);
                updateStatusbar();
            }
            break;
        }
        case MSG_SEARCH_DONE: {
            searchData->active = false;
            bool canceled = searchData->canceled;
            free(searchData);
            searchData = NULL;
            if (canceled) {
                refreshContentView();
            }
            else updateStatusbar();
            break;
        }
    }
    return OrigWndProc(hwnd, msg, wParam, lParam);  
}

void updateSelectedItems() {
    MEMFREE(selectedItems);
    numSelectedItems = 0;

    int i = ListView_GetNextItem(hwndContentView, -1, LVNI_SELECTED);
    while (i != -1 && items && i < numItems) {       
        int index = numSelectedItems++;
        selectedItems = realloc(selectedItems, numSelectedItems * sizeof(struct FileNode*));
        if (!selectedItems) break;
        selectedItems[index] = items[i].node;
        i = ListView_GetNextItem(hwndContentView, i, LVNI_SELECTED);
    }
}

static void addContextMenuItem(HMENU hMenu, int id, struct ContextMenuItem* cmItem, bool separate) {
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

static void createContextMenuFromRegistry(int* id) {
    freeMenuItems();
    HKEY hkeyContextMenu, hkeyItem;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\ContextMenu", &hkeyContextMenu) != ERROR_SUCCESS) return;
    
    WCHAR itemName[30] = {0};
    WCHAR subitemName[100] = {0};
    WCHAR itemValue[MAX_PATH];
    DWORD i, j, itemNameLen, itemValueLen;
    
    i = 0;
    while (i < 10) {
        itemNameLen = 30;    
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
            
            j = 0;
            while (j < 10) {
                itemNameLen = 100;
                itemValueLen = MAX_PATH;
                if (RegEnumValue(hkeyItem, j++, subitemName, &itemNameLen, NULL, NULL, (LPBYTE)itemValue, &itemValueLen) != ERROR_SUCCESS) break;
                
                int index = numMenuItems++;
                menuItems = realloc(menuItems, numMenuItems * sizeof(struct ContextMenuItem));     
                
                struct ContextMenuItem* cmItem = &menuItems[index];
                cmItem->text = subitemName;
                cmItem->proc = NULL;
                
                wchar_t *cmdData = malloc(1024);
                wcscpy_s(cmdData, MAX_PATH, itemValue);
                
                wchar_t path[MAX_PATH] = {0};
                getFileNodePath(selectedItems[0], path);
                cmdData = strReplace(cmdData, L"%FILE%", path, true);
                
                wchar_t basename[80] = {0};
                getBasenameFromPath(path, basename, true);
                cmdData = strReplace(cmdData, L"%BASENAME%", basename, true);                
                
                getFileNodePath(selectedItems[0]->parent, path);
                cmdData = strReplace(cmdData, L"%DIR%", path, true);
                
                cmItem->cmdData = cmdData;
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

static void createCDDriveContextMenu(int* id) {
    HMENU hSubmenu = CreatePopupMenu();
    
    wchar_t currentISOPath[MAX_PATH] = {0};
    int currentISOPathLen = MAX_PATH;
    HKEY hkey;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        RegQueryValue(hkey, NULL, currentISOPath, (PLONG)&currentISOPathLen);
        RegCloseKey(hkey);
    }
    
    wchar_t itemText[64] = {0};
    swprintf_s(itemText, MAX_PATH, L"%ls <%ls>", lc_str.load_iso_image, currentISOPathLen != MAX_PATH ? currentISOPath : lc_str.no_media);
    cmiLoadISOImage.text = itemText;
    addContextMenuItem(hSubmenu, (*id)++, &cmiLoadISOImage, false);
    cmiLoadISOImage.text = NULL;
    
    addContextMenuItem(hSubmenu, (*id)++, &cmiUnloadISOImage, false);
    
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    swprintf_s(itemText, 64, L"%ls [X:]", lc_str.cd_drive);
    item.dwTypeData = itemText;
    item.cch = wcslen(itemText);
    item.wID = ++(*id);    
    
    item.hSubMenu = hSubmenu;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
    
    item.fMask = MIIM_TYPE;
    item.fType = MFT_SEPARATOR;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
}

static void createContextMenu(enum ContextMenuType type) {
    HMENU hMenu = CreatePopupMenu();
    hContextMenu = hMenu;

    int id = 0;

    if (type == MENU_SINGLE || type == MENU_MULTIPLE) {
        if (type == MENU_SINGLE) {
            if (selectedItems[0]->type == TYPE_FILE) {
                addContextMenuItem(hMenu, id++, &cmiOpen, false);
                addContextMenuItem(hMenu, id++, &cmiEdit, true);
                addContextMenuItem(hMenu, id++, &cmiShowIcon, true);
                createCDDriveContextMenu(&id);
                createContextMenuFromRegistry(&id);
            }
            else addContextMenuItem(hMenu, id++, &cmiOpen, true);
        }
        addContextMenuItem(hMenu, id++, &cmiCut, false);
        addContextMenuItem(hMenu, id++, &cmiCopy, true);
        addContextMenuItem(hMenu, id++, &cmiCreateShortcut, false);
        addContextMenuItem(hMenu, id++, &cmiDelete, false);
        
        if (type == MENU_SINGLE) addContextMenuItem(hMenu, id++, &cmiRename, false);
    }
    else {
        addContextMenuItem(hMenu, id++, &cmiPaste, false);
        addContextMenuItem(hMenu, id++, &cmiPasteShortcut, true);
        createCDDriveContextMenu(&id);
        addContextMenuItem(hMenu, id++, &cmiNewFolder, false);
        addContextMenuItem(hMenu, id++, &cmiNewFile, false);
    }

    POINT cursor;
    GetCursorPos(&cursor);
    TrackPopupMenu(hMenu, 0, cursor.x, cursor.y, 0, hwndContentView, NULL);
}

LRESULT contentViewNotify(NMHDR* nmhdr) {
    switch (nmhdr->code) {
        case LVN_GETDISPINFO: {
            NMLVDISPINFO* nmlvdi = (NMLVDISPINFO*)nmhdr;
            UINT mask = nmlvdi->item.mask;
            if (!items || nmlvdi->item.iItem < 0 || nmlvdi->item.iItem >= numItems) break;
            struct ListItem* item = &items[nmlvdi->item.iItem];
            
            if (!item->loaded) {
                // 使用图标缓存优化
                if (item->node->type == TYPE_DIR) {
                    // 目录图标缓存
                    if (!folderIconCached) {
                        wchar_t path[MAX_PATH] = {0};
                        getFileNodePath(item->node, path);
                        struct FileInfo fi = {0};
                        getFileInfo(path, TYPE_DIR, viewStyle == STYLE_LARGE_ICON, &fi);
                        folderIconIndex = fi.icon;
                        folderIconCached = 1;
                        wcscpy_s(item->type, 80, lc_str.folder);
                    }
                    item->icon = folderIconIndex;
                    wcscpy_s(item->type, 80, lc_str.folder);
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
                        int cachedIcon = findExeIconCache(path);
                        if (cachedIcon >= 0) {
                            item->icon = cachedIcon;
                            wcscpy_s(item->type, 80, ext && wcsicmp(ext + 1, L"exe") == 0 ? lc_str.application : lc_str.shortcut);
                        } else {
                            struct FileInfo fi = {0};
                            getFileInfo(path, TYPE_FILE, viewStyle == STYLE_LARGE_ICON, &fi);
                            item->icon = fi.icon;
                            addExeIconCache(path, fi.icon);
                            wcscpy_s(item->type, 80, ext && wcsicmp(ext + 1, L"exe") == 0 ? lc_str.application : lc_str.shortcut);
                        }
                    } else {
                        // 非 exe/lnk：先查内存缓存（扩展名 → 图标索引 + 类型名）
                        int ci = findExtIconCache(ext);
                        if (ci >= 0) {
                            item->icon = ci;
                            const wchar_t* ct = findExtTypeNameCache(ext);
                            if (ct && ct[0]) wcscpy_s(item->type, 80, ct);
                            else {
                                wchar_t upper[30] = {0};
                                strToUpper(ext + 1, upper);
                                swprintf_s(item->type, 80, lc_str.fmt_file, upper);
                            }
                        } else {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(item->node, path);
                            struct FileInfo fi = {0};
                            getFileInfo(path, TYPE_FILE, viewStyle == STYLE_LARGE_ICON, &fi);
                            item->icon = fi.icon;
                            wcscpy_s(item->type, 80, fi.typeName);
                            addExtIconCache(ext, fi.icon, fi.typeName);
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
                    getFileInfo(path, item->node->type, viewStyle == STYLE_LARGE_ICON, &fi);
                    item->icon = fi.icon;
                    wcscpy_s(item->type, 80, fi.typeName);
                }
                
                item->loaded = true;                
            }
            
            if (mask & LVIF_STATE) {
                nmlvdi->item.state = 0;
            }

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
                        nmlvdi->item.pszText = item->node->type == TYPE_FILE ? item->formattedSize : L"";
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

            if (nmia->iItem != -1 && nmia->iSubItem == 0) {
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
            if (nmia->iItem == -1 || nmia->iSubItem != 0) break;
            
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

static DWORD WINAPI searchTask(void* param) {
    struct SearchData* searchData = (struct SearchData*)param;
    
    const int maxStackSize = 50;
    struct FileNode* stack[maxStackSize];
    int stackSize = 0;
    stack[stackSize++] = currPathFileNode->children;
    
    wchar_t keyword[64] = {0};
    wchar_t name[64] = {0};
    
    strToLower(searchData->keyword, keyword);
    
    while (stackSize > 0 && numItems < 10000 && searchData->active) {
        struct FileNode* node = stack[--stackSize];
        while (node && searchData->active) {
            strToLower(node->name, name);
            if (wcsstr(name, keyword)) {
                SendMessage(hwndContentView, MSG_ADD_ITEM, 0, (LPARAM)node);
            }
            
            if (numItems >= 10000) break;
            
            if (node->type == TYPE_DIR && stackSize < maxStackSize) {
                buildChildNodes(node, false);
                if (node->children) stack[stackSize++] = node->children;
            }
            node = node->sibling;       
        }
    }
    
    SendMessage(hwndContentView, MSG_SEARCH_DONE, 0, 0);
    return 0;
}

void searchFor(wchar_t* keyword) {
    if (wcslen(keyword) == 0) return;
    if (searchData != NULL && searchData->active) {
        searchData->active = false;
        return;
    }
    
    clearContentView();
    
    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 250;
    column.pszText = lc_str.path;
    ListView_InsertColumn(hwndContentView, COLUMN_PATH_IDX, &column);
    UpdateWindow(hwndContentView);
    
    searchData = malloc(sizeof(struct SearchData));
    searchData->keyword = keyword;
    searchData->active = true;
    searchData->canceled = false;

    CreateThread(NULL, 0, searchTask, searchData, 0, NULL);
}

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

    viewStyle = newViewStyle;
    refreshContentView();
    // 视图样式切换时（大/小图标），图标索引在系统图像列表中不同，需要清空缓存重新获取
    folderIconCached = 0;
    exeIconCacheCount = 0;
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

    column.cx = 60;
    column.pszText = lc_str.size;
    ListView_InsertColumn(hwndContentView, COLUMN_SIZE_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.date;
    ListView_InsertColumn(hwndContentView, COLUMN_DATE_IDX, &column);
}

void createContentView() {
    hwndContentView = CreateWindowEx(0, WC_LISTVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS,
                                     0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);

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
    cmiLoadISOImage.text = NULL;
    cmiUnloadISOImage.text = lc_str.unload_iso_image;
    cmiShowIcon.text = lc_str.show_icon;
    
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

void onMenuItemEditClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        static const wchar_t editorPath[] = L"C:\\windows\\notepad.exe";
        
        wchar_t path[MAX_PATH] = {0};
        wchar_t parameters[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        swprintf_s(parameters, MAX_PATH, L"\"%ls\"", path);
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

void onBookmarkButtonClick() {
    addCurrentPathToBookmark();
}

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
    navigateToPath(parentDir);
}

static BOOL saveIconToFile(HICON hIcon, const wchar_t* filePath) {
    if (!hIcon || !filePath) return FALSE;

    // Get icon dimensions via GetIconInfo
    ICONINFO ii = {0};
    if (!GetIconInfo(hIcon, &ii)) return FALSE;

    int iconWidth = 0, iconHeight = 0;

    if (ii.hbmColor) {
        BITMAP bm = {0};
        GetObjectW(ii.hbmColor, sizeof(BITMAP), &bm);
        iconWidth = bm.bmWidth;
        iconHeight = bm.bmHeight;
    }

    // Fallback: use mask bitmap for dimensions
    if (ii.hbmMask && iconWidth <= 0) {
        BITMAP bmMask = {0};
        GetObjectW(ii.hbmMask, sizeof(BITMAP), &bmMask);
        iconWidth = bmMask.bmWidth;
        iconHeight = bmMask.bmHeight;
    }

    // Clean up GetIconInfo handles early — we render from the HICON
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (iconWidth <= 0 || iconHeight <= 0) return FALSE;

    // Render icon to a 32bpp top-down DIBSection
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = iconWidth;
    bmi.bmiHeader.biHeight = -iconHeight;  // top-down for natural pixel order
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

    // Fill with transparent white
    RECT rc = {0, 0, iconWidth, iconHeight};
    HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, hBr);
    DeleteObject(hBr);

    // Render icon at full resolution
    DrawIconEx(hdcMem, 0, 0, hIcon, iconWidth, iconHeight, 0, NULL, DI_NORMAL);
    GdiFlush();

    // Copy pixel data out of the DIBSection before destroying it
    int colorSize = iconWidth * iconHeight * 4;
    BYTE* colorBits = (BYTE*)malloc(colorSize);
    if (colorBits) {
        memcpy(colorBits, dibBits, colorSize);
    }

    // Destroy the DIBSection and DC
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmDib);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (!colorBits) return FALSE;

    // Generate zero AND mask (all pixels visible; alpha is in the color data)
    int maskSize = ((iconWidth + 31) / 32) * 4 * iconHeight;
    BYTE* maskBits = (BYTE*)calloc(1, maskSize);
    if (!maskBits) {
        free(colorBits);
        return FALSE;
    }

    // Write ICO file
    BOOL result = FALSE;
    HANDLE hFile = CreateFileW(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        DWORD dibSize = sizeof(BITMAPINFOHEADER);
        DWORD totalImageSize = dibSize + colorSize + maskSize;
        DWORD dataOffset = 6 + 16;

        // ICO header (6 bytes)
        WORD reserved = 0, type = 1, count = 1;
        WriteFile(hFile, &reserved, 2, &written, NULL);
        WriteFile(hFile, &type, 2, &written, NULL);
        WriteFile(hFile, &count, 2, &written, NULL);

        // Directory entry (16 bytes)
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

        // BITMAPINFOHEADER: biHeight = iconHeight * 2 (XOR + AND combined)
        BITMAPINFOHEADER bih = {0};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = iconWidth;
        bih.biHeight = iconHeight * 2;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        WriteFile(hFile, &bih, sizeof(bih), &written, NULL);

        // XOR bitmap (reverse rows: DIBSection is top-down, ICO XOR expects bottom-up)
        int rowSize = iconWidth * 4;
        BYTE* rowBuf = (BYTE*)malloc(rowSize);
        if (rowBuf) {
            for (int row = 0; row < iconHeight; row++) {
                memcpy(rowBuf, colorBits + (iconHeight - 1 - row) * rowSize, rowSize);
                WriteFile(hFile, rowBuf, rowSize, &written, NULL);
            }
            free(rowBuf);
        } else {
            WriteFile(hFile, colorBits, colorSize, &written, NULL);
        }

        // AND mask
        WriteFile(hFile, maskBits, maskSize, &written, NULL);

        CloseHandle(hFile);
        result = TRUE;
    }

    free(colorBits);
    free(maskBits);

    return result;
}

static LRESULT CALLBACK IconViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HWND hBtn = CreateWindowW(L"BUTTON", lc_str.save_icon,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                70, 269, 160, 28,
                hwnd, (HMENU)IDC_SAVE_ICON, globalHInstance, NULL);
            if (hBtn && hGuiFont) {
                SendMessageW(hBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            if (hIconLarge) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int cx = rc.right - rc.left;
                int cy = rc.bottom - rc.top;
                int iconAreaCy = cy - 50;

                int iconCx = min(cx - 20, 256);
                int iconCy = min(iconAreaCy - 10, 256);
                int size = min(iconCx, iconCy);
                int x = (cx - size) / 2;
                int y = (iconAreaCy - size) / 2;
                if (y < 5) y = 5;

                DrawIconEx(hdc, x, y, hIconLarge, size, size, 0, NULL, DI_NORMAL);
            }
            
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_SAVE_ICON && HIWORD(wParam) == BN_CLICKED) {
                wchar_t filePath[MAX_PATH] = {0};
                swprintf_s(filePath, MAX_PATH, L"%ls.ico", iconViewerFileName);
                OPENFILENAMEW ofn = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = filePath;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"Icon Files (*.ico)\0*.ico\0All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.lpstrDefExt = L"ico";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    if (!saveIconToFile(hIconLarge, filePath)) {
                        MessageBoxW(hwnd, L"保存图标失败", lc_str.alert, MB_OK | MB_ICONERROR);
                    }
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
            // Destroy icon resource
            if (hIconLarge) {
                DestroyIcon(hIconLarge);
                hIconLarge = NULL;
            }
            hwndIconViewer = NULL;
            DestroyWindow(hwnd);
            break;
        }
        case WM_DESTROY: {
            // DO NOT call PostQuitMessage here - that would kill the main app
            // Just clean up, the window is already being destroyed by WM_CLOSE
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
    // Try to extract 256x256 icon using PrivateExtractIconsW
    UINT iconCount = PrivateExtractIconsW(filePath, 0, 256, 256, &hIconLarge, NULL, 1, 0);
    
    // Fallback to SHGetFileInfo if PrivateExtractIcons failed
    if (iconCount == 0 || !hIconLarge) {
        SHFILEINFO sfi = {0};
        DWORD flags = SHGFI_ICON | SHGFI_LARGEICON;
        
        if (!SHGetFileInfo(filePath, 0, &sfi, sizeof(SHFILEINFO), flags) || !sfi.hIcon) {
            MessageBox(hwndMain, L"无法提取文件图标", lc_str.alert, MB_OK);
            return;
        }
        hIconLarge = sfi.hIcon;
    }
    
    // Build window title
    wmemset(iconViewerTitle, 0, MAX_PATH);
    swprintf_s(iconViewerTitle, MAX_PATH, L"%ls - %ls", fileName, lc_str.show_icon);
    wcscpy_s(iconViewerFileName, MAX_PATH, fileName);
    
    // Register window class if not already registered
    WNDCLASSEX wcCheck = {0};
    if (!GetClassInfoEx(globalHInstance, L"IconViewerClass", &wcCheck)) {
        registerIconViewerClass();
    }
    
    // Window size: compact layout
    int winWidth = 300;
    int winHeight = 350;
    
    // Center window on screen
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - winWidth) / 2;
    int y = (screenHeight - winHeight) / 2;
    
    // Create window
    hwndIconViewer = CreateWindowEx(
        0,
        L"IconViewerClass",
        iconViewerTitle,
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        x, y, winWidth, winHeight,
        hwndMain,
        NULL,
        globalHInstance,
        NULL
    );
    
    if (!hwndIconViewer) {
        DestroyIcon(hIconLarge);
        hIconLarge = NULL;
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

static int compareType(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    return sortAscending ? ia->node->type - ib->node->type : ib->node->type - ia->node->type;
}

static int compareName(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = sortAscending ? wcscoll(ia->node->name, ib->node->name) : wcscoll(ib->node->name, ia->node->name);
    return res;
}

static int compareSize(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = sortAscending ? ia->size - ib->size : ib->size - ia->size;
    return res;
}

static int compareDate(const void* a, const void* b) {
    struct ListItem* ia = (struct ListItem*)a;
    struct ListItem* ib = (struct ListItem*)b;
    int res = compareType(a, b);
    if (res == 0) res = sortAscending ? CompareFileTime(&ia->modifiedTime, &ib->modifiedTime) : CompareFileTime(&ib->modifiedTime, &ia->modifiedTime);
    return res;
}

void clearIconCaches() {
    extCacheCount = 0;
    folderIconCached = 0;
    exeIconCacheCount = 0;
}
void sortItems() {
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
    if (searchData != NULL && searchData->active) {
        searchData->active = false;     
        searchData->canceled = true;
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
    if (hIconLarge) {
        DestroyIcon(hIconLarge);
        hIconLarge = NULL;
    }
    if (hIconSmall) {
        DestroyIcon(hIconSmall);
        hIconSmall = NULL;
    }
    
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
        }
    }

    struct FileNode* child = currPathFileNode->children;
    
    // 单次遍历：计数并填充
    int capacity = 64;
    numItems = 0;
    items = malloc(capacity * sizeof(struct ListItem));
    
    while (child) {
        if (numItems >= capacity) {
            capacity *= 2;
            items = realloc(items, capacity * sizeof(struct ListItem));
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
    ListView_SetItemCountEx(hwndContentView, numItems, 0);
    
    // 大图标/小图标视图：强制重排所有项目，覆盖 SetWindowLongPtr 切换样式时
    // LISTVIEW_StyleChanged → Arrange 在旧 ItemCount 下写入的错误位置。
    // 同时重设 ItemCount 触发 LISTVIEW_UpdateScroll，修复滚动范围。
    if (viewStyle == STYLE_LARGE_ICON || viewStyle == STYLE_SMALL_ICON) {
        ListView_Arrange(hwndContentView, LVA_DEFAULT);
        ListView_SetItemCountEx(hwndContentView, numItems, 0);
    }
    
    InvalidateRect(hwndContentView, NULL, TRUE);
    updateStatusbar();  
}