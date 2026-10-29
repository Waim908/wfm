#include "main.h"
#include "bookmarks.h"

extern struct FileNode* treeFileNode;
extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndTreeview = NULL;

// 本次 updateTreeItems 插入了多少个子节点。只服务于启动打点的 n= 计数：
// 节点数直接决定树视图那一段要发起多少次 shell 调用（每个节点一次 SHGetFileInfo）。
static int treeInsertCount = 0;

static void updateTreeItemsDeep(HTREEITEM parentItem, struct FileNode* parentNode) {
    HTREEITEM child = TreeView_GetChild(hwndTreeview, parentItem);

    while (child != NULL) {
        HTREEITEM itemToDelete = child;
        child = TreeView_GetNextSibling(hwndTreeview, child);
        TreeView_DeleteItem(hwndTreeview, itemToDelete);
    }

    if (parentNode->children) {
        TVINSERTSTRUCT tvis;
        tvis.hParent = parentItem;
        tvis.hInsertAfter = TVI_LAST;
        tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT | TVIF_STATE;

        wchar_t parentPath[MAX_PATH] = {0};
        if (getFileNodePath(parentNode, parentPath)) wcscat_s(parentPath, MAX_PATH, L"\\");
        wchar_t path[MAX_PATH] = {0};

        // 这里**不再**重挂图像列表：updateTreeItems 在建树前已经用 Shell_GetImageLists
        // 拿同一份 shell 小图标列表挂上了（同一节里那段长注释解释过为什么用
        // Shell_GetImageLists 而不是绕 PIDL），展开节点走的也是这条路径。
        // TreeView_SetImageList 会让控件按新列表把整棵树重排重画一遍，白做。

        // 目录图标只取一次：树里只放目录（buildChildNodes(node, true)），而 TYPE_DIR 那条
        // 分支用的是 SHGFI_USEFILEATTRIBUTES + FILE_ATTRIBUTE_DIRECTORY，图标与路径无关，
        // 每个目录拿到的 iIcon 都一样。旧实现每插一个子项就 SHGetFileInfo 一次，展开一个
        // 几百个子目录的节点就是几百次 shell 调用去换同一个数字。
        // 其它类型（驱动器）的图标与路径有关，仍然逐项取。
        int dirIcon = -1;

        struct FileNode* node = parentNode->children;
        do {
            swprintf_s(path, MAX_PATH, L"%ls%ls", parentPath, node->name);

            int iconIndex;
            if (node->type == TYPE_DIR) {
                if (dirIcon < 0) dirIcon = getTreeIcon(path, TYPE_DIR);
                iconIndex = dirIcon;
            }
            else {
                iconIndex = getTreeIcon(path, node->type);
            }

            bool isExpandable = node->hasChildDirs;
            // 用户、文档、临时目录、开始菜单都是「点一下就导航」的叶子项，不给展开/折叠按钮
            if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL ||
                node->type == TYPE_TEMP || node->type == TYPE_STARTMENU) {
                isExpandable = false;
            }

            tvis.itemex.cChildren = isExpandable ? 1 : 0;
            tvis.itemex.state = node->children ? TVIS_EXPANDED : 0;
            tvis.itemex.stateMask = TVIS_EXPANDED;
            tvis.itemex.pszText = node->name;
            tvis.itemex.cchTextMax = (int)wcslen(node->name) + 1;
            tvis.itemex.iImage = iconIndex;
            tvis.itemex.iSelectedImage = iconIndex;
            tvis.itemex.lParam = (LPARAM)node;

            TreeView_InsertItem(hwndTreeview, &tvis);
            treeInsertCount++;
        }
        while ((node = node->sibling) != NULL);
    }
}

static void updateTreeItems() {
    TreeView_DeleteAllItems(hwndTreeview);
    treeInsertCount = 0;

    TVINSERTSTRUCT tvis;
    tvis.hParent = NULL;
    tvis.hInsertAfter = TVI_ROOT;
    tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT | TVIF_STATE;

    // 把系统小图标列表挂到树上：下面每个节点给的 iImage 都是 shell 图标索引，
    // 只有配上这份列表才画得出来。
    // 原先这里是「先取 CSIDL_DRIVES 的 PIDL，再拿它查一次 SHGetFileInfo
    // (SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)」，绕一大圈只是为了换到
    // 这个句柄 —— 而 SHGetFileInfo 那条分支内部走的就是 SHGetImageList(SHIL_SMALL)
    // （wine/dlls/shell32/shell32_main.c:392），返回的正是 shell_imagelists[SHIL_SMALL]。
    // 同一个列表用 Shell_GetImageLists 直接拿（iconcache.c:620），省掉一次 shell
    // 命名空间绑定 —— 这一笔实测占了 createTreeview 里最大的一段。
    HIMAGELIST himlSmallTree = NULL;
    if (Shell_GetImageLists(NULL, &himlSmallTree) && himlSmallTree)
        TreeView_SetImageList(hwndTreeview, himlSmallTree, TVSIL_NORMAL);
    startupMark("    tv: shell imagelist");

    struct FileNode* node = treeFileNode;
    do {
        ITEMIDLIST* pidl = NULL;
        switch (node->type) {
            case TYPE_DESKTOP:
                SHGetSpecialFolderLocation(NULL, CSIDL_DESKTOP, &pidl);
                break;
            case TYPE_PERSONAL:
                SHGetSpecialFolderLocation(NULL, CSIDL_PERSONAL, &pidl);
                break;
            case TYPE_COMPUTER:
                SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pidl);
                break;
            case TYPE_USERPROFILE: {
                wchar_t userProfilePath[MAX_PATH];
                if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userProfilePath))) {
                    pidl = ILCreateFromPathW(userProfilePath);
                }
                break;
            }
            case TYPE_TEMP: {
                // %TEMP% 没有对应的 CSIDL，只能按真实路径建 PIDL —— 这样拿到的才是
                // shell 认的那个文件夹图标，而不是退到通用文件图标
                wchar_t tempDir[MAX_PATH] = {0};
                if (getFileNodePath(node, tempDir) && tempDir[0])
                    pidl = ILCreateFromPathW(tempDir);
                break;
            }
            case TYPE_STARTMENU:
                SHGetSpecialFolderLocation(NULL, CSIDL_PROGRAMS, &pidl);
                break;
            default:
                break;
        }

        SHFILEINFO sfi = {0};
        SHGetFileInfo((LPCWSTR)pidl, 0, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_PIDL);
        CoTaskMemFree(pidl);

        bool isExpandable = node->hasChildDirs;
        // 用户、文档、临时目录、开始菜单都是「点一下就导航」的叶子项，不给展开/折叠按钮
        if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL ||
            node->type == TYPE_TEMP || node->type == TYPE_STARTMENU) {
            isExpandable = false;
        }

        tvis.itemex.cChildren = isExpandable ? 1 : 0;
        tvis.itemex.state = node->children ? TVIS_EXPANDED : 0;
        tvis.itemex.stateMask = TVIS_EXPANDED;
        tvis.itemex.pszText = node->name;
        tvis.itemex.cchTextMax = (int)wcslen(node->name) + 1;
        tvis.itemex.iImage = sfi.iIcon;
        tvis.itemex.iSelectedImage = sfi.iIcon;
        tvis.itemex.lParam = (LPARAM)node;

        HTREEITEM handle = TreeView_InsertItem(hwndTreeview, &tvis);

        // 对于已有子节点的项，直接在UI上创建子项
        if (node->children) {
            updateTreeItemsDeep(handle, node);
        }
    }
    while ((node = node->sibling) != NULL);
    
    startupMarkN("    tv: insert nodes", treeInsertCount);

    // Add bookmarks section
    buildBookmarkTree();
    startupMark("    tv: bookmark tree");
}

static void treeItemExpand(HTREEITEM treeItem, struct FileNode* node) {
    buildChildNodes(node, true);
    updateTreeItemsDeep(treeItem, node);
}

static void treeItemCollapse(HTREEITEM treeItem, struct FileNode* node) {
    UNREFERENCED_PARAMETER(treeItem);
    freeChildNodes(node);
}

LRESULT treeviewNotify(NMHDR* nmhdr) {
    switch (nmhdr->code) {
        case TVN_ITEMEXPANDING: {
            NMTREEVIEW* nmtv = (NMTREEVIEW*)nmhdr;
            LPARAM lParam = nmtv->itemNew.lParam;
            
            // Handle bookmark root expanding
            if (lParam == TYPE_BOOKMARK_ROOT) {
                // If no bookmarks, prevent expand
                if (g_bookmarkCount == 0) {
                    return TRUE;
                }
                return 0;
            }
            
            struct FileNode* node = (struct FileNode*)lParam;
            // 用户、文档、临时目录、开始菜单都是叶子项，不允许展开/折叠
            if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL ||
                node->type == TYPE_TEMP || node->type == TYPE_STARTMENU) {
                return TRUE; // 阻止展开
            }
            if (nmtv->action == TVE_EXPAND) treeItemExpand(nmtv->itemNew.hItem, node);
            break;
        }
        case TVN_ITEMEXPANDED: {
            NMTREEVIEW* nmtv = (NMTREEVIEW*)nmhdr;
            LPARAM lParam = nmtv->itemNew.lParam;
            
            // Don't process bookmark items as FileNode
            if (lParam == TYPE_BOOKMARK_ROOT || (lParam & 0xFFFF) == TYPE_BOOKMARK_ITEM) {
                break;
            }
            
            struct FileNode* node = (struct FileNode*)lParam;
            if (nmtv->action == TVE_COLLAPSE) {
                treeItemCollapse(nmtv->itemNew.hItem, node);
            }
            break;
        }
        case NM_CLICK: {
            TVHITTESTINFO tvhti;
            GetCursorPos(&tvhti.pt);
            ScreenToClient(hwndTreeview, &tvhti.pt);
            TreeView_HitTest(hwndTreeview, &tvhti);

            if (tvhti.hItem != NULL && (tvhti.flags & TVHT_ONITEM)) {
                TVITEM item;
                item.hItem = tvhti.hItem;
                item.mask = TVIF_PARAM;
                TreeView_GetItem(hwndTreeview, &item);
                
                LPARAM lParam = item.lParam;
                
                // Handle bookmark item click
                if (lParam == TYPE_BOOKMARK_ROOT) {
                    return 0;
                }
                
                if ((lParam & 0xFFFF) == TYPE_BOOKMARK_ITEM) {
                    int bookmarkIndex = (int)(lParam >> 16);
                    if (bookmarkIndex >= 0 && bookmarkIndex < g_bookmarkCount) {
                        // Check if path exists
                        if (!isPathExists(g_bookmarks[bookmarkIndex].path)) {
                            wchar_t msg[MAX_PATH + 128];
                            swprintf_s(msg, MAX_PATH + 128, lc_str.bookmark_path_not_found, g_bookmarks[bookmarkIndex].path);
                            MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
                            return 0;
                        }
                        navigateToPath(g_bookmarks[bookmarkIndex].path);
                    }
                    return 0;
                }
                
                struct FileNode* node = (struct FileNode*)lParam;
                navigateToFileNode(node);
            }
            break;
        }
        case NM_RCLICK: {
            TVHITTESTINFO tvhti;
            POINT pt;
            GetCursorPos(&pt);
            
            // Get the item at cursor
            tvhti.pt = pt;
            ScreenToClient(hwndTreeview, &tvhti.pt);
            TreeView_HitTest(hwndTreeview, &tvhti);

            if (tvhti.hItem != NULL && (tvhti.flags & TVHT_ONITEM)) {
                TVITEM item;
                item.hItem = tvhti.hItem;
                item.mask = TVIF_PARAM;
                TreeView_GetItem(hwndTreeview, &item);
                
                LPARAM lParam = item.lParam;
                
                // Select the item
                TreeView_SelectItem(hwndTreeview, tvhti.hItem);
                
                // Show context menu at cursor position (screen coordinates)
                HMENU hMenu = CreatePopupMenu();
                
                if (lParam == TYPE_BOOKMARK_ROOT) {
                    // For bookmark root, allow adding current path
                    AppendMenuW(hMenu, MF_STRING, 1, lc_str.add_bookmark);
                } else if ((lParam & 0xFFFF) == TYPE_BOOKMARK_ITEM) {
                    int bookmarkIndex = (int)(lParam >> 16);
                    if (bookmarkIndex >= 0 && bookmarkIndex < g_bookmarkCount) {
                        // Show toggle text based on current state
                        if (g_autoOpenBookmarkIndex == bookmarkIndex) {
                            AppendMenuW(hMenu, MF_STRING, 3, lc_str.cancel_auto_open);
                        } else {
                            AppendMenuW(hMenu, MF_STRING, 3, lc_str.auto_open_on_start);
                        }
                        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                        AppendMenuW(hMenu, MF_STRING, 2, lc_str.remove_bookmark);
                    }
                } else {
                    // For regular file nodes, allow adding to bookmarks
                    struct FileNode* node = (struct FileNode*)lParam;
                    if (node && node->type == TYPE_DIR) {
                        wchar_t path[MAX_PATH] = {0};
                        getFileNodePath(node, path);
                        
                        // Check if already bookmarked
                        int bookmarkIdx = findBookmark(path);
                        if (bookmarkIdx >= 0) {
                            // Show auto-open toggle based on current state
                            if (g_autoOpenBookmarkIndex == bookmarkIdx) {
                                AppendMenuW(hMenu, MF_STRING, 3, lc_str.cancel_auto_open);
                            } else {
                                AppendMenuW(hMenu, MF_STRING, 3, lc_str.auto_open_on_start);
                            }
                            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuW(hMenu, MF_STRING, 4, lc_str.remove_bookmark);
                        } else {
                            AppendMenuW(hMenu, MF_STRING, 1, lc_str.add_bookmark);
                        }
                    }
                }
                
                // Check if menu has any items
                int menuItemCount = GetMenuItemCount(hMenu);
                if (menuItemCount > 0) {
                    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwndMain, NULL);
                    
                    if (cmd == 1) {
                        // Add bookmark - use current navigation path
                        if (currPathFileNode) {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(currPathFileNode, path);
                            
                            if (findBookmark(path) >= 0) {
                                MessageBox(hwndMain, lc_str.bookmark_exists, lc_str.alert, MB_OK | MB_ICONWARNING);
                            } else {
                                addBookmark(path);
                                buildBookmarkTree();
                            }
                        }
                    } else if (cmd == 2) {
                        // Remove bookmark
                        int bookmarkIndex = (int)(lParam >> 16);
                        // If removing auto-open bookmark, clear it first
                        if (g_autoOpenBookmarkIndex == bookmarkIndex) {
                            g_autoOpenBookmarkIndex = -1;
                            saveAutoOpenBookmark();
                        }
                        removeBookmark(bookmarkIndex);
                        buildBookmarkTree();
                    } else if (cmd == 3) {
                        // Toggle auto-open for bookmark
                        int bookmarkIndex = (int)(lParam >> 16);
                        if (bookmarkIndex >= 0 && bookmarkIndex < g_bookmarkCount) {
                            setAutoOpenBookmark(bookmarkIndex);
                        } else {
                            // Could be from directory context menu
                            if (currPathFileNode) {
                                wchar_t path[MAX_PATH] = {0};
                                getFileNodePath(currPathFileNode, path);
                                int idx = findBookmark(path);
                                if (idx >= 0) {
                                    setAutoOpenBookmark(idx);
                                }
                            }
                        }
                    } else if (cmd == 4) {
                        // Remove bookmark for current path (from directory context menu)
                        if (currPathFileNode) {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(currPathFileNode, path);
                            int idx = findBookmark(path);
                            if (idx >= 0) {
                                // If removing auto-open bookmark, clear it first
                                if (g_autoOpenBookmarkIndex == idx) {
                                    g_autoOpenBookmarkIndex = -1;
                                    saveAutoOpenBookmark();
                                }
                                removeBookmark(idx);
                                buildBookmarkTree();
                            }
                        }
                    }
                }
                DestroyMenu(hMenu);
            }
            break;
        }
    }

    return 0;
}

void createTreeview() {
    startupMark("  tv: entry");
    hwndTreeview = CreateWindowEx(0, WC_TREEVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, 0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
    startupMark("  tv: CreateWindowEx");

    // 初始化私有图像列表（替代共享系统列表，避免图标污染问题）

    updateTreeItems();
    startupMark("  tv: updateTreeItems");
    UpdateWindow(hwndTreeview);
    startupMark("  tv: UpdateWindow paint");
}
