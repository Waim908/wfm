#include "main.h"
#include "bookmarks.h"

struct Bookmark g_bookmarks[MAX_BOOKMARKS];
int g_bookmarkCount = 0;
int g_autoOpenBookmarkIndex = -1;  // -1 means no auto-open bookmark

extern HWND hwndTreeview;
extern HINSTANCE globalHInstance;

//=============================================================================
// 图标处理
//
// 【问题】原代码在 buildBookmarkTree 中调用 Shell_GetImageLists 获取系统共享
// 图像列表后，用 ImageList_AddIcon 将 bookmark 图标追加到系统列表中。这永久
// 修改了 Shell 的全局共享资源，导致其他组件（ListView、SHGetFileInfo）返回的
// 图标索引可能错乱。
//
// 【修复】不再修改系统图像列表。Bookmark 根节点改用 Windows 收藏夹(Favorites)
// 的星形图标——它已经是系统图像列表的一部分，不需要额外添加任何内容。
// 这样既保留了 bookmark 的视觉标识，又不污染系统列表。
//=============================================================================

// 获取收藏夹(Favorites)文件夹的系统图标索引，用于 bookmark 根节点
static int getBookmarkRootIconIndex() {
    static int s_cachedFavIcon = -1;
    if (s_cachedFavIcon >= 0) return s_cachedFavIcon;
    
    ITEMIDLIST* pidl = NULL;
    if (SHGetSpecialFolderLocation(NULL, CSIDL_FAVORITES, &pidl) == S_OK && pidl) {
        SHFILEINFO sfi = {0};
        SHGetFileInfo((LPCWSTR)pidl, 0, &sfi, sizeof(SHFILEINFO),
                      SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_PIDL);
        s_cachedFavIcon = sfi.iIcon;
        CoTaskMemFree(pidl);
    }
    
    // 如果获取失败，回退到通用文件夹图标
    if (s_cachedFavIcon < 0) {
        SHFILEINFO sfi = {0};
        SHGetFileInfo(L"", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO),
                      SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        s_cachedFavIcon = sfi.iIcon;
    }
    
    return s_cachedFavIcon;
}

//=============================================================================
// 图标缓存持久化（注册表）
//
// 扩展名 → 系统图标索引 的映射被保存到注册表中，以便跨会话复用。
// 缓存位置：HKEY_CURRENT_USER\SOFTWARE\Winlator\WFM\IconCache
//
// 这样用户可以通过 regedit 查看和管理图标缓存。
//=============================================================================

void saveExtIconCacheToRegistry(const wchar_t* ext, int iconIndex) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, ICONCACHE_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueEx(hkey, ext, 0, REG_DWORD, (const BYTE*)&iconIndex, sizeof(iconIndex));
    RegCloseKey(hkey);
}

int loadExtIconCacheFromRegistry(const wchar_t* ext, int* outIconIndex) {
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, ICONCACHE_REGISTRY_PATH, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD type = 0;
    DWORD dataSize = sizeof(int);
    LONG result = RegQueryValueEx(hkey, ext, NULL, &type, (LPBYTE)outIconIndex, &dataSize);
    RegCloseKey(hkey);
    return (result == ERROR_SUCCESS && type == REG_DWORD) ? 1 : 0;
}

void loadBookmarks() {
    g_bookmarkCount = 0;
    
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return;
    }
    
    DWORD index = 0;
    while (g_bookmarkCount < MAX_BOOKMARKS) {
        wchar_t valueName[MAX_PATH] = {0};
        // S12：RegEnumValue 的 lpcbValueName 单位是「字节」，必须乘以 sizeof(wchar_t)，
        // 否则较长名称会被静默截断/返回失败。（valueDataLen 本来就已是字节，正确）
        DWORD valueNameLen = sizeof(valueName);
        wchar_t valueData[MAX_PATH] = {0};
        DWORD valueDataLen = sizeof(valueData);
        DWORD type = 0;
        
        LONG result = RegEnumValue(hkey, index, valueName, &valueNameLen, NULL, &type, (LPBYTE)valueData, &valueDataLen);
        if (result != ERROR_SUCCESS) break;
        
        // 空串是历史遗留的无效收藏（虚拟节点曾被允许收藏），加载时一并丢弃
        if (type == REG_SZ && valueDataLen > 0 && valueData[0] != L'\0') {
            wcsncpy_s(g_bookmarks[g_bookmarkCount].path, MAX_PATH, valueData, MAX_PATH - 1);
            wcsncpy_s(g_bookmarks[g_bookmarkCount].name, MAX_PATH, valueData, MAX_PATH - 1);
            g_bookmarkCount++;
        }
        index++;
    }
    
    RegCloseKey(hkey);
}

void saveBookmarks() {
    HKEY hkey;
    RegDeleteTree(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH);
    
    if (RegCreateKeyEx(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    
    for (int i = 0; i < g_bookmarkCount; i++) {
        wchar_t valueName[32];
        swprintf_s(valueName, 32, L"Bookmark%d", i);
        RegSetValueEx(hkey, valueName, 0, REG_SZ, (const BYTE*)g_bookmarks[i].path,
                      (wcslen(g_bookmarks[i].path) + 1) * sizeof(wchar_t));
    }
    RegCloseKey(hkey);
}

void loadAutoOpenBookmark() {
    g_autoOpenBookmarkIndex = -1;
    
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return;
    }
    
    wchar_t valueData[MAX_PATH] = {0};
    DWORD valueDataLen = MAX_PATH * sizeof(wchar_t);
    DWORD type = 0;
    
    LONG result = RegQueryValueEx(hkey, L"Path", NULL, &type, (LPBYTE)valueData, &valueDataLen);
    if (result == ERROR_SUCCESS && type == REG_SZ && valueDataLen > 0) {
        g_autoOpenBookmarkIndex = findBookmark(valueData);
    }
    RegCloseKey(hkey);
}

void saveAutoOpenBookmark() {
    HKEY hkey;
    RegDeleteTree(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH);
    
    if (g_autoOpenBookmarkIndex < 0 || g_autoOpenBookmarkIndex >= g_bookmarkCount) {
        return;
    }
    
    if (RegCreateKeyEx(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    
    RegSetValueEx(hkey, L"Path", 0, REG_SZ, (const BYTE*)g_bookmarks[g_autoOpenBookmarkIndex].path,
                  (wcslen(g_bookmarks[g_autoOpenBookmarkIndex].path) + 1) * sizeof(wchar_t));
    RegCloseKey(hkey);
}

void setAutoOpenBookmark(int index) {
    if (index < 0 || index >= g_bookmarkCount) return;
    g_autoOpenBookmarkIndex = (g_autoOpenBookmarkIndex == index) ? -1 : index;
    saveAutoOpenBookmark();
    buildBookmarkTree();
}

void openAutoOpenBookmark() {
    extern HWND hwndMain;
    
    if (g_autoOpenBookmarkIndex < 0 || g_autoOpenBookmarkIndex >= g_bookmarkCount) {
        return;
    }
    
    if (isPathExists(g_bookmarks[g_autoOpenBookmarkIndex].path)) {
        navigateToPath(g_bookmarks[g_autoOpenBookmarkIndex].path);
    } else {
        wchar_t msg[MAX_PATH + 128];
        swprintf_s(msg, MAX_PATH + 128, lc_str.auto_open_path_not_found, g_bookmarks[g_autoOpenBookmarkIndex].path);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
        g_autoOpenBookmarkIndex = -1;
        RegDeleteTree(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH);
    }
}

void addBookmark(const wchar_t* path) {
    // 「此电脑」/盘符列表等虚拟节点没有文件系统路径（getFileNodePath
    // 返回空串），收藏它只会产生一条打不开的空记录
    if (!path || path[0] == L'\0') return;
    if (g_bookmarkCount >= MAX_BOOKMARKS) return;
    if (findBookmark(path) >= 0) return;
    
    wcsncpy_s(g_bookmarks[g_bookmarkCount].path, MAX_PATH, path, MAX_PATH - 1);
    wcsncpy_s(g_bookmarks[g_bookmarkCount].name, MAX_PATH, path, MAX_PATH - 1);
    g_bookmarkCount++;
    saveBookmarks();
}

void removeBookmark(int index) {
    if (index < 0 || index >= g_bookmarkCount) return;
    for (int i = index; i < g_bookmarkCount - 1; i++) {
        g_bookmarks[i] = g_bookmarks[i + 1];
    }
    g_bookmarkCount--;
    saveBookmarks();
}

int findBookmark(const wchar_t* path) {
    for (int i = 0; i < g_bookmarkCount; i++) {
        if (wcsicmp(g_bookmarks[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static HTREEITEM hBookmarksRoot = NULL;

void buildBookmarkTree() {
    // Clear existing items first
    if (hBookmarksRoot) {
        HTREEITEM child = TreeView_GetChild(hwndTreeview, hBookmarksRoot);
        while (child != NULL) {
            HTREEITEM itemToDelete = child;
            child = TreeView_GetNextSibling(hwndTreeview, child);
            TreeView_DeleteItem(hwndTreeview, itemToDelete);
        }
    }
    
    // 设置树形视图使用系统图像列表（但不修改它）
    // 不使用 ImageList_AddIcon 追加图标，避免污染系统列表
    HIMAGELIST himlBig, himlSmall;
    Shell_GetImageLists(&himlBig, &himlSmall);
    TreeView_SetImageList(hwndTreeview, himlSmall, TVSIL_NORMAL);
    
    // 获取 bookmark 根节点图标：使用 Windows 收藏夹(Favorites)星形图标
    // 该图标已存在于系统图像列表中，无需额外添加
    int bookmarkRootIcon = getBookmarkRootIconIndex();
    
    if (!hBookmarksRoot) {
        TVINSERTSTRUCT tvis = {0};
        tvis.hParent = NULL;
        tvis.hInsertAfter = TVI_FIRST;
        tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT | TVIF_STATE;
        tvis.itemex.pszText = lc_str.bookmarks;
        tvis.itemex.cchTextMax = wcslen(lc_str.bookmarks);
        tvis.itemex.lParam = (LPARAM)TYPE_BOOKMARK_ROOT;
        tvis.itemex.cChildren = (g_bookmarkCount > 0) ? 1 : 0;
        tvis.itemex.state = (g_bookmarkCount > 0) ? TVIS_EXPANDED : 0;
        tvis.itemex.stateMask = TVIS_EXPANDED;
        
        // 使用收藏夹图标，而非向系统列表追加自定义图标
        tvis.itemex.iImage = bookmarkRootIcon;
        tvis.itemex.iSelectedImage = bookmarkRootIcon;
        
        hBookmarksRoot = TreeView_InsertItem(hwndTreeview, &tvis);
    } else {
        TVITEM tvi = {0};
        tvi.hItem = hBookmarksRoot;
        tvi.mask = TVIF_CHILDREN | TVIF_STATE;
        tvi.cChildren = (g_bookmarkCount > 0) ? 1 : 0;
        tvi.state = (g_bookmarkCount > 0) ? TVIS_EXPANDED : 0;
        tvi.stateMask = TVIS_EXPANDED | TVIS_EXPANDEDONCE;
        TreeView_SetItem(hwndTreeview, &tvi);
    }
    
    // Add bookmark items
    TVINSERTSTRUCT tvis = {0};
    tvis.hParent = hBookmarksRoot;
    tvis.hInsertAfter = TVI_LAST;
    tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT;
    tvis.itemex.cChildren = 0;
    
    SHFILEINFO sfi = {0};
    
    for (int i = 0; i < g_bookmarkCount; i++) {
        DWORD dwAttrib = GetFileAttributes(g_bookmarks[i].path);
        BOOL pathExists = (dwAttrib != INVALID_FILE_ATTRIBUTES);
        
        // 用截断版格式化，避免超长收藏路径触发 swprintf_s 的无效参数处理器
        wchar_t displayText[MAX_PATH + 32] = {0};
        if (i == g_autoOpenBookmarkIndex) {
            swprintfTrunc(displayText, MAX_PATH + 32, L"[启动] %ls", g_bookmarks[i].path);
        } else {
            wcsncpy_s(displayText, MAX_PATH + 32, g_bookmarks[i].path, _TRUNCATE);
        }
        
        tvis.itemex.pszText = displayText;
        tvis.itemex.cchTextMax = (int)wcslen(displayText) + 1;
        tvis.itemex.lParam = (LPARAM)(TYPE_BOOKMARK_ITEM | (i << 16));
        
        if (pathExists) {
            SHGetFileInfo(g_bookmarks[i].path, 0, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            tvis.itemex.iImage = sfi.iIcon;
            tvis.itemex.iSelectedImage = sfi.iIcon;
        } else {
            SHGetFileInfo(L"", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO),
                          SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            tvis.itemex.iImage = sfi.iIcon;
            tvis.itemex.iSelectedImage = sfi.iIcon;
        }
        
        TreeView_InsertItem(hwndTreeview, &tvis);
    }
}

void addCurrentPathToBookmark() {
    extern struct FileNode* currPathFileNode;
    extern HWND hwndMain;
    
    if (!currPathFileNode) return;
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    
    if (findBookmark(path) >= 0) {
        MessageBox(hwndMain, lc_str.bookmark_exists, lc_str.alert, MB_OK | MB_ICONWARNING);
    } else {
        addBookmark(path);
        buildBookmarkTree();
    }
}

//=============================================================================
// 语言持久化
//=============================================================================

void saveLanguageToRegistry(const wchar_t* lang) {
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LANGUAGE_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(hkey, L"lang", 0, REG_SZ, (const BYTE*)lang, (DWORD)((wcslen(lang) + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
}

int loadLanguageFromRegistry(wchar_t* lang, DWORD langSize) {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LANGUAGE_REGISTRY_PATH, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD type = 0;
    DWORD dataSize = langSize;
    LONG result = RegQueryValueExW(hkey, L"lang", NULL, &type, (LPBYTE)lang, &dataSize);
    RegCloseKey(hkey);
    return (result == ERROR_SUCCESS && type == REG_SZ) ? 1 : 0;
}
