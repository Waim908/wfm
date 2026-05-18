#include "main.h"
#include "bookmarks.h"

struct Bookmark g_bookmarks[MAX_BOOKMARKS];
int g_bookmarkCount = 0;
int g_autoOpenBookmarkIndex = -1;  // -1 means no auto-open bookmark

extern HWND hwndTreeview;
extern HINSTANCE globalHInstance;

void loadBookmarks() {
    g_bookmarkCount = 0;
    
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return;
    }
    
    DWORD index = 0;
    while (g_bookmarkCount < MAX_BOOKMARKS) {
        wchar_t valueName[MAX_PATH] = {0};
        DWORD valueNameLen = MAX_PATH;
        wchar_t valueData[MAX_PATH] = {0};
        DWORD valueDataLen = MAX_PATH * sizeof(wchar_t);
        DWORD type = 0;
        
        LONG result = RegEnumValue(hkey, index, valueName, &valueNameLen, NULL, &type, (LPBYTE)valueData, &valueDataLen);
        if (result != ERROR_SUCCESS) break;
        
        if (type == REG_SZ && valueDataLen > 0) {
            wcsncpy_s(g_bookmarks[g_bookmarkCount].path, MAX_PATH, valueData, MAX_PATH - 1);
            
            // Use full path as display name to differentiate same-name folders
            wcsncpy_s(g_bookmarks[g_bookmarkCount].name, MAX_PATH, valueData, MAX_PATH - 1);
            
            g_bookmarkCount++;
        }
        index++;
    }
    
    RegCloseKey(hkey);
}

void saveBookmarks() {
    HKEY hkey;
    
    // Delete existing bookmarks key
    RegDeleteTree(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH);
    
    // Create new key
    if (RegCreateKeyEx(HKEY_CURRENT_USER, BOOKMARK_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    
    for (int i = 0; i < g_bookmarkCount; i++) {
        wchar_t valueName[32];
        swprintf_s(valueName, 32, L"Bookmark%d", i);
        RegSetValueEx(hkey, valueName, 0, REG_SZ, (const BYTE*)g_bookmarks[i].path, (wcslen(g_bookmarks[i].path) + 1) * sizeof(wchar_t));
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
        // Find matching bookmark index
        g_autoOpenBookmarkIndex = findBookmark(valueData);
    }
    
    RegCloseKey(hkey);
}

void saveAutoOpenBookmark() {
    HKEY hkey;
    
    // Delete existing key
    RegDeleteTree(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH);
    
    if (g_autoOpenBookmarkIndex < 0 || g_autoOpenBookmarkIndex >= g_bookmarkCount) {
        return;
    }
    
    // Create new key and save the path
    if (RegCreateKeyEx(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) != ERROR_SUCCESS) {
        return;
    }
    
    RegSetValueEx(hkey, L"Path", 0, REG_SZ, (const BYTE*)g_bookmarks[g_autoOpenBookmarkIndex].path, (wcslen(g_bookmarks[g_autoOpenBookmarkIndex].path) + 1) * sizeof(wchar_t));
    
    RegCloseKey(hkey);
}

void setAutoOpenBookmark(int index) {
    if (index < 0 || index >= g_bookmarkCount) return;
    
    // Toggle: if already set, clear it; otherwise set it
    if (g_autoOpenBookmarkIndex == index) {
        g_autoOpenBookmarkIndex = -1;
    } else {
        g_autoOpenBookmarkIndex = index;
    }
    
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
        // Show error for non-existent path
        wchar_t msg[MAX_PATH + 128];
        swprintf_s(msg, MAX_PATH + 128, lc_str.auto_open_path_not_found, g_bookmarks[g_autoOpenBookmarkIndex].path);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
        // Clear the invalid auto-open setting
        g_autoOpenBookmarkIndex = -1;
        RegDeleteTree(HKEY_CURRENT_USER, AUTOOPEN_REGISTRY_PATH);
    }
}

void addBookmark(const wchar_t* path) {
    if (g_bookmarkCount >= MAX_BOOKMARKS) return;
    if (findBookmark(path) >= 0) return;
    
    wcsncpy_s(g_bookmarks[g_bookmarkCount].path, MAX_PATH, path, MAX_PATH - 1);
    
    // Use full path as display name
    wcsncpy_s(g_bookmarks[g_bookmarkCount].name, MAX_PATH, path, MAX_PATH - 1);
    
    g_bookmarkCount++;
    saveBookmarks();
}

void removeBookmark(int index) {
    if (index < 0 || index >= g_bookmarkCount) return;
    
    // Shift remaining bookmarks
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
    
    if (!hBookmarksRoot) {
        TVINSERTSTRUCT tvis = {0};
        tvis.hParent = NULL;
        tvis.hInsertAfter = TVI_FIRST;
        tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT | TVIF_STATE;
        tvis.itemex.pszText = lc_str.bookmarks;
        tvis.itemex.cchTextMax = wcslen(lc_str.bookmarks);
        tvis.itemex.lParam = (LPARAM)TYPE_BOOKMARK_ROOT;
        
        // Only show expand button if there are bookmarks
        tvis.itemex.cChildren = (g_bookmarkCount > 0) ? 1 : 0;
        
        // State: if no bookmarks, don't show expanded state
        if (g_bookmarkCount > 0) {
            tvis.itemex.state = TVIS_EXPANDED;
        } else {
            tvis.itemex.state = 0;
        }
        tvis.itemex.stateMask = TVIS_EXPANDED;
        
        // Get folder icon
        SHFILEINFO sfi = {0};
        HIMAGELIST himlBig, himlSmall;
        Shell_GetImageLists(&himlBig, &himlSmall);
        TreeView_SetImageList(hwndTreeview, himlSmall, TVSIL_NORMAL);
        SHGetFileInfo(L"", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        tvis.itemex.iImage = sfi.iIcon;
        tvis.itemex.iSelectedImage = sfi.iIcon;
        
        hBookmarksRoot = TreeView_InsertItem(hwndTreeview, &tvis);
    } else {
        // Update the root item's children count based on bookmark count
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
        // Check if path exists
        DWORD dwAttrib = GetFileAttributes(g_bookmarks[i].path);
        BOOL pathExists = (dwAttrib != INVALID_FILE_ATTRIBUTES);
        
        // Build display text: [启动] path for auto-open bookmark
        wchar_t displayText[MAX_PATH + 32];
        if (i == g_autoOpenBookmarkIndex) {
            swprintf_s(displayText, MAX_PATH + 32, L"[启动] %ls", g_bookmarks[i].path);
        } else {
            wcsncpy_s(displayText, MAX_PATH + 32, g_bookmarks[i].path, MAX_PATH + 31);
        }
        
        tvis.itemex.pszText = displayText;
        tvis.itemex.cchTextMax = wcslen(displayText);
        tvis.itemex.lParam = (LPARAM)(TYPE_BOOKMARK_ITEM | (i << 16));
        
        if (pathExists) {
            SHGetFileInfo(g_bookmarks[i].path, 0, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            tvis.itemex.iImage = sfi.iIcon;
            tvis.itemex.iSelectedImage = sfi.iIcon;
        } else {
            // Use error/warning icon for non-existent paths
            SHGetFileInfo(L"", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            tvis.itemex.iImage = sfi.iIcon;
            tvis.itemex.iSelectedImage = sfi.iIcon;
        }
        
        TreeView_InsertItem(hwndTreeview, &tvis);
    }
}

void addCurrentPathToBookmark() {
    extern struct FileNode* currPathFileNode;
    extern HWND hwndMain;
    
    if (!currPathFileNode) {
        return;
    }
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    
    if (findBookmark(path) >= 0) {
        MessageBox(hwndMain, lc_str.bookmark_exists, lc_str.alert, MB_OK | MB_ICONWARNING);
    } else {
        addBookmark(path);
        buildBookmarkTree();
    }
}
