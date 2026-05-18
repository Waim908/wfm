#include "main.h"
#include "bookmarks.h"

extern struct FileNode* treeFileNode;
extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndTreeview = NULL;

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

        HIMAGELIST himlBig, himlSmall;
        Shell_GetImageLists(&himlBig, &himlSmall);
        TreeView_SetImageList(hwndTreeview, himlSmall, TVSIL_NORMAL);

        struct FileNode* node = parentNode->children;
        do {
            swprintf_s(path, MAX_PATH, L"%ls%ls", parentPath, node->name);

            int iconIndex = getTreeIcon(path, node->type);

            bool isExpandable = node->hasChildDirs;
            // 用户和文档节点不显示展开/折叠按钮，和桌面一样
            if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL) {
                isExpandable = false;
            }

            tvis.itemex.cChildren = isExpandable ? 1 : 0;
            tvis.itemex.state = node->children ? TVIS_EXPANDED : 0;
            tvis.itemex.stateMask = TVIS_EXPANDED;
            tvis.itemex.pszText = node->name;
            tvis.itemex.cchTextMax = wcslen(node->name);
            tvis.itemex.iImage = iconIndex;
            tvis.itemex.iSelectedImage = iconIndex;
            tvis.itemex.lParam = (LPARAM)node;

            TreeView_InsertItem(hwndTreeview, &tvis);
        }
        while ((node = node->sibling) != NULL);
    }
}

static void updateTreeItems() {
    TreeView_DeleteAllItems(hwndTreeview);

    TVINSERTSTRUCT tvis;
    tvis.hParent = NULL;
    tvis.hInsertAfter = TVI_ROOT;
    tvis.itemex.mask = TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_TEXT | TVIF_STATE;

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
            default:
                break;
        }

        SHFILEINFO sfi = {0};
        HIMAGELIST himl = (HIMAGELIST)SHGetFileInfo((LPCWSTR)pidl, 0, &sfi, sizeof(SHFILEINFO), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_PIDL);
        CoTaskMemFree(pidl);
        TreeView_SetImageList(hwndTreeview, himl, TVSIL_NORMAL);

        bool isExpandable = node->hasChildDirs;
        // 用户和文档节点不显示展开/折叠按钮，和桌面一样
        if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL) {
            isExpandable = false;
        }

        tvis.itemex.cChildren = isExpandable ? 1 : 0;
        tvis.itemex.state = node->children ? TVIS_EXPANDED : 0;
        tvis.itemex.stateMask = TVIS_EXPANDED;
        tvis.itemex.pszText = node->name;
        tvis.itemex.cchTextMax = wcslen(node->name);
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
    
    // Add bookmarks section
    buildBookmarkTree();
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
            // 用户和文档节点不允许展开/折叠，和桌面一样
            if (node->type == TYPE_USERPROFILE || node->type == TYPE_PERSONAL) {
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
    hwndTreeview = CreateWindowEx(0, WC_TREEVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, 0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);

    // 初始化私有图像列表（替代共享系统列表，避免图标污染问题）

    updateTreeItems();
    UpdateWindow(hwndTreeview);
}
