#include "main.h"

struct AddrButton { 
    HWND hwnd;
    struct FileNode* node;
    int width;
    bool isArrow;
    bool isHidden;
};

static const int buttonSize = 30;

// 地址栏最多显示的路径段数。每段在完整路径里至少占 2 个字符（"x\\"），
// 故 MAX_PATH 深度下不会超过这个值；用固定上限替代变长数组（S10）。
#define MAX_ADDR_BUTTONS (MAX_PATH / 2 + 2)

static WNDPROC OrigWndProc;
static WNDPROC AddrEditOrigWndProc;
static WNDPROC SearchEditOrigWndProc;
static WNDPROC SearchEditWrapperOrigWndProc;

static HWND hwndAddrEditWrapper;
static HWND hwndAddrEdit;
static HWND hwndSearchEditWrapper;
static HWND hwndSearchEdit;
static HWND hwndGoButton;
static HWND hwndRefreshButton;
static HWND hwndSearchButton;
static HANDLE hMorePopupMenu;

static bool isEditMode;
static wchar_t keyword[64] = {0};
static bool searchEditEmpty = true;
static struct AddrButton* addrButtons = NULL;
static int numAddrButtons = 0;

extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;
extern HWND hwndContentView;

HWND hwndNavbar = NULL;

// 地址栏箭头按钮的下拉菜单：列出该路径段下的子目录。
//
// 旧实现直接调用 buildChildNodes(parent)，而它的第一步就是 freeChildNodes ——
// 会释放 UI 线程正在使用的节点（items[].node、TreeView 的 lParam 全部悬空 →
// 使用后释放），而且每次点击都新建 HMENU 却从不销毁（内存泄漏）。
// 这里改成「只读快照」：把目录名与完整路径拷进局部数组，菜单项只引用这些字符串，
// 菜单用完立即销毁。
#define MORE_MENU_MAX 256
struct MoreMenuItem {
    wchar_t* text;
    wchar_t* path;
};

static void addMoreMenuItem(struct MoreMenuItem* entries, int* count, const wchar_t* name,
                            const wchar_t* parentDir, bool nameIsFullPath) {
    if (*count >= MORE_MENU_MAX) return;

    wchar_t fullPath[MAX_PATH] = {0};
    if (nameIsFullPath) wcsncpy_s(fullPath, MAX_PATH, name, _TRUNCATE);
    else joinPaths((wchar_t*)parentDir, (wchar_t*)name, fullPath, MAX_PATH);

    wchar_t* text = wcsdup(name);
    wchar_t* path = wcsdup(fullPath);
    if (!text || !path) {
        free(text);
        free(path);
        return;
    }
    entries[*count].text = text;
    entries[*count].path = path;
    (*count)++;
}

static void createMorePopupMenu(struct FileNode* node) {
    if (!node) return;

    struct MoreMenuItem entries[MORE_MENU_MAX];
    int count = 0;

    if (node->type == TYPE_COMPUTER) {
        // "此电脑" 没有文件系统路径，子项是驱动器
        wchar_t drives[MAX_PATH] = {0};
        if (GetLogicalDriveStrings(MAX_PATH, drives)) {
            int i = 0;
            while (drives[i] != L'\0' && count < MORE_MENU_MAX) {
                wchar_t* drive = &drives[i];
                i += (int)wcslen(drive) + 1;
                addMoreMenuItem(entries, &count, drive, NULL, true);
            }
        }
    }
    else {
        wchar_t dirPath[MAX_PATH] = {0};
        getFileNodePath(node, dirPath);
        if (dirPath[0] != L'\0') {
            wchar_t pattern[MAX_PATH] = {0};
            swprintfTrunc(pattern, MAX_PATH, L"%ls%ls*", dirPath,
                          dirPath[wcslen(dirPath) - 1] == L'\\' ? L"" : L"\\");

            WIN32_FIND_DATA wfd = {0};
            HANDLE handle = FindFirstFile(pattern, &wfd);
            if (handle != INVALID_HANDLE_VALUE) {
                do {
                    if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;
                    if (!g_showHiddenFiles && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
                    if (count >= MORE_MENU_MAX) break;
                    addMoreMenuItem(entries, &count, wfd.cFileName, dirPath, false);
                }
                while (FindNextFile(handle, &wfd));
                FindClose(handle);
            }
        }
    }

    if (count == 0) return;   // 没有子目录就不弹空菜单

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        for (int i = 0; i < count; i++) { free(entries[i].text); free(entries[i].path); }
        return;
    }
    hMorePopupMenu = menu;

    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_DATA | MIIM_ID;
    item.fType = MFT_STRING;

    for (int i = 0; i < count; i++) {
        item.dwTypeData = entries[i].text;
        item.cch = (UINT)wcslen(entries[i].text);
        item.wID = (UINT)(i + 1);
        item.dwItemData = (ULONG_PTR)entries[i].path;
        InsertMenuItem(menu, -1, TRUE, &item);
    }

    POINT cursor;
    GetCursorPos(&cursor);

    // 关键：必须用 TPM_RETURNCMD 让 TrackPopupMenu 同步返回选中项 ID。
    // 否则 Wine 会用 NtUserPostMessage 投递 WM_COMMAND（win32u/menu.c:3510），
    // 该消息在 TrackPopupMenu 返回之后才被处理 —— 那时菜单已销毁、entries 已释放，
    // 处理器再去 GetMenuItemInfo / 读 dwItemData 就是野指针。
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, cursor.x, cursor.y, 0, hwndNavbar, NULL);
    if (cmd > 0 && cmd <= count) {
        navigateToPath(entries[cmd - 1].path);
        SetFocus(hwndContentView);
    }

    hMorePopupMenu = NULL;
    DestroyMenu(menu);
    for (int i = 0; i < count; i++) {
        free(entries[i].text);
        free(entries[i].path);
    }
}

static void resizeAddrButtons() {
    RECT addrEditRect;
    GetWindowRectInParent(hwndAddrEditWrapper, &addrEditRect);
    int maxWidth = (addrEditRect.right - addrEditRect.left) - 50;
    int currX = addrEditRect.left + 1;
    int height = (addrEditRect.bottom - addrEditRect.top) - 2;  

    int currWidth = 0;
    for (int i = 0; i < numAddrButtons; i++) {
        struct AddrButton* button = &addrButtons[i];
        currWidth += button->width;
        button->isHidden = currWidth > maxWidth;
        
        if (button->isHidden) {
            ShowWindow(button->hwnd, SW_HIDE);
        }
        else {
            SetWindowPos(button->hwnd, NULL, currX, addrEditRect.top + 1, button->width, height, SWP_SHOWWINDOW);
            currX += button->width;
        }       
    }
}

static void setEditMode(bool value) {
    isEditMode = value;
    for (int i = 0; i < numAddrButtons; i++) {
        struct AddrButton* button = &addrButtons[i];
        if (!button->isHidden) ShowWindow(button->hwnd, isEditMode ? SW_HIDE : SW_SHOW);
    }

    SendMessage(hwndAddrEdit, EM_SETREADONLY, isEditMode ? FALSE : TRUE, 0);

    if (isEditMode) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(currPathFileNode, path);
        SendMessage(hwndAddrEdit, WM_SETTEXT, 0, (LPARAM)path);
        PostMessage(hwndAddrEdit, EM_SETSEL, 0, -1);
    }
    else SendMessage(hwndAddrEdit, WM_SETTEXT, 0, (LPARAM)L"");

    if (!isEditMode) resizeAddrButtons();
    InvalidateRect(hwndAddrEditWrapper, NULL, TRUE);
}

static void updateSearchEdit() {
    SendMessage(hwndSearchEdit, WM_GETTEXT, 64, (LPARAM)keyword);
    searchEditEmpty = wcslen(keyword) == 0;
    if (searchEditEmpty) {
        swprintf_s(keyword, 64, L"%ls %ls", lc_str.search, currPathFileNode->name);
        SendMessage(hwndSearchEdit, WM_SETTEXT, 0, (LPARAM)keyword);
    }
}

LRESULT CALLBACK NavbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) { 
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HWND hwndControl = (HWND)lParam;
            if (hwndControl == hwndAddrEditWrapper) {
                return (LRESULT)(isEditMode ? GetSysColorBrush(COLOR_WINDOW) : GetSysColorBrush(COLOR_BTNFACE));
            }
            else if (hwndControl == hwndSearchEditWrapper) {
                return (LRESULT)GetStockObject(WHITE_BRUSH);
            }
            break;
        }
        case WM_SIZE: {
            RECT rect;
            GetClientRect(hwnd, &rect);

            const int margin = 4;
            const int searchEditWidth = 160;
            const int editWrapperHeight = buttonSize - margin;

            HDC hdc = GetDC(hwnd);
            HGDIOBJ prevFont = SelectObject(hdc, hGuiFont);
            SIZE textSize;
            GetTextExtentPoint32W(hdc, L"Ay", 2, &textSize);
            int addrEditHeight = textSize.cy + 4;
            SelectObject(hdc, prevFont);
            ReleaseDC(hwnd, hdc);

            const int addrEditY = (editWrapperHeight - addrEditHeight) / 2;
            
            int offsetX = rect.right - (margin + buttonSize);
            
            SetWindowPos(hwndSearchButton, NULL, offsetX, 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            offsetX -= searchEditWidth + margin;
            
            SetWindowPos(hwndSearchEditWrapper, NULL, offsetX, margin, searchEditWidth, editWrapperHeight, SWP_NOZORDER);
            SetWindowPos(hwndSearchEdit, NULL, margin, addrEditY, searchEditWidth, addrEditHeight, SWP_NOZORDER);
            offsetX -= buttonSize + margin;
            
            SetWindowPos(hwndRefreshButton, NULL, offsetX, 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            offsetX -= buttonSize + margin;
            
            SetWindowPos(hwndGoButton, NULL, offsetX, 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            
            int addrEditWidth = offsetX - margin - margin;
            SetWindowPos(hwndAddrEditWrapper, NULL, margin, margin, addrEditWidth, editWrapperHeight, SWP_NOZORDER);
            SetWindowPos(hwndAddrEdit, NULL, margin, addrEditY, addrEditWidth, addrEditHeight, SWP_NOZORDER);
            break;
        }
        case WM_COMMAND: {
            HWND hwndControl = (HWND)lParam;
            for (int i = 0; i < numAddrButtons; i++) {
                struct AddrButton* button = &addrButtons[i];
                if (hwndControl == button->hwnd) {
                    if (button->isArrow) {
                        createMorePopupMenu(button->node);
                    }
                    else {
                        navigateToFileNode(button->node);
                        SetFocus(hwndContentView);
                    }
                    return 0;
                }
            }

            if (hwndControl == hwndGoButton) {
                // 固定缓冲 + 由控件裁剪长度：原来按 GetWindowTextLength 开 VLA，
                // 极端长文本会在栈上开大数组（S10）
                wchar_t path[MAX_PATH] = {0};
                SendMessage(hwndAddrEdit, WM_GETTEXT, MAX_PATH, (LPARAM)path);
                setEditMode(false);
                navigateToPath(path);
                SetFocus(hwndContentView);
            }
            else if (hwndControl == hwndRefreshButton) {
                navigateRefresh();
                SetFocus(hwndContentView);
            }
            else if (hwndControl == hwndSearchButton) {
                if (!searchEditEmpty) {
                    SendMessage(hwndSearchEdit, WM_GETTEXT, 64, (LPARAM)keyword);
                    searchFor(keyword);
                }
                else refreshContentView();
            }
            else if (hwndControl == 0) {
                // 下拉菜单现已改用 TPM_RETURNCMD 同步处理（见 createMorePopupMenu），
                // 不会再投递 WM_COMMAND。此处保留为防御性分支：初始化结构体并检查返回值，
                // 避免读栈垃圾当指针（S5）。
                MENUITEMINFO item = {0};
                item.cbSize = sizeof(MENUITEMINFO);
                item.fMask = MIIM_DATA;
                if (!hMorePopupMenu) return 0;
                if (!GetMenuItemInfo(hMorePopupMenu, LOWORD(wParam), FALSE, &item)) return 0;

                wchar_t* path = (wchar_t*)item.dwItemData;
                if (!path) return 0;

                navigateToPath(path);
                SetFocus(hwndContentView);
            }
            return 0;
        }
    }

    return OrigWndProc(hwnd, msg, wParam, lParam);  
}

LRESULT CALLBACK SearchEditWrapperWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLOREDIT) {
        HWND hwndControl = (HWND)lParam;
        if (hwndControl == hwndSearchEdit) {
            SetTextColor((HDC)wParam, searchEditEmpty ? RGB(128, 128, 128) : RGB(0, 0, 0));
            return (LRESULT)((HBRUSH)GetStockObject(WHITE_BRUSH));
        }
    }
    return SearchEditWrapperOrigWndProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK AddrEditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KILLFOCUS: {
            if ((HWND)wParam != hwndGoButton) setEditMode(false);
            break;
        }
        case WM_LBUTTONDOWN: {
            if (!isEditMode) setEditMode(true);
            break;
        }
        case WM_SIZE: {
            resizeAddrButtons();
            break;
        }
    }

    return AddrEditOrigWndProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SearchEditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KILLFOCUS: {
            updateSearchEdit();
            break;
        }
        case WM_SETFOCUS:
        case WM_LBUTTONDOWN: {
            searchEditEmpty = false;
            SendMessage(hwndSearchEdit, WM_SETTEXT, 0, (LPARAM)L"");
            break;
        }
    }

    return SearchEditOrigWndProc(hwnd, msg, wParam, lParam);
}

int getNavbarHeight() {
    RECT rect;
    GetWindowRectInParent(hwndRefreshButton, &rect);
    return rect.bottom - rect.top + 6;  
}

static struct AddrButton* addAddrButton() {
    int index = numAddrButtons++;
    struct AddrButton* tmp = realloc(addrButtons, numAddrButtons * sizeof(struct AddrButton));
    if (!tmp) {                    // realloc 失败时保留旧指针，回滚计数，避免泄漏与 NULL 解引用
        numAddrButtons--;
        return NULL;
    }
    addrButtons = tmp;
    struct AddrButton* button = &addrButtons[index];
    button->hwnd = CreateWindowEx(0, WC_BUTTON, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwndNavbar, (HMENU)NULL, globalHInstance, NULL);
    button->isArrow = false;
    SendMessage(button->hwnd, WM_SETFONT, (WPARAM)hGuiFont, 0);
    return button;
}

static void addArrowAddrButton(struct FileNode* node) {
    struct AddrButton* button = addAddrButton();
    if (!button) return;

    button->isArrow = true;
    button->node = node;

    SetWindowLongPtr(button->hwnd, GWL_STYLE, GetWindowLongPtr(button->hwnd, GWL_STYLE) | BS_ICON);

    SendMessageW(button->hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM)uiIcons[ICON_NAV_ARROW]);
    button->width = 16;
}

static void calcAddrButtonWidth(struct AddrButton* button) {
    HDC hdc = GetDC(button->hwnd);
    HGDIOBJ prevFont = SelectObject(hdc, (HFONT)SendMessage(button->hwnd, WM_GETFONT, 0, 0));
    SIZE size;
    int textLen = wcslen(button->node->name);
    GetTextExtentPoint32(hdc, button->node->name, textLen, &size);
    button->width = size.cx + 12;
    SelectObject(hdc, prevFont);
    ReleaseDC(button->hwnd, hdc);
}

void clearAddrButtons() {
    for (int i = 0; i < numAddrButtons; i++) DestroyWindow(addrButtons[i].hwnd);
    
    if (addrButtons) {
        free(addrButtons);
        addrButtons = NULL;
    }
    
    numAddrButtons = 0; 
}

void updateAddrButtons() {
    clearAddrButtons();
    
    // 路径分段数受 MAX_PATH 约束（每段至少 2 字符 "x\\"），用固定上限替代 VLA
    int count = 0;
    struct FileNode* node = currPathFileNode;
    while (node && count < MAX_ADDR_BUTTONS) {
        count++;
        node = node->parent;
    }
    
    struct FileNode* nodes[MAX_ADDR_BUTTONS];
    node = currPathFileNode;
    int i = 0;
    while (node && i < MAX_ADDR_BUTTONS) {
        nodes[i++] = node;
        node = node->parent;
    }   

    for (i = count-1; i >= 0; i--) {
        struct AddrButton* button = addAddrButton();
        if (!button) continue;

        button->node = nodes[i];
        SendMessage(button->hwnd, WM_SETTEXT, 0, (LPARAM)nodes[i]->name);
        calcAddrButtonWidth(button);
        addArrowAddrButton(nodes[i]);
    }

    resizeAddrButtons();
    
    SendMessage(hwndSearchEdit, WM_SETTEXT, 0, (LPARAM)L"");
    updateSearchEdit();
}

static void createNavButtons() {
    hwndGoButton = CreateWindowEx(0, WC_BUTTON, NULL, WS_VISIBLE | WS_CHILD | BS_ICON,
                                  0, 0, buttonSize, buttonSize, hwndNavbar, NULL, globalHInstance, NULL);
    SendMessage(hwndGoButton, BM_SETIMAGE, IMAGE_ICON, (LPARAM)uiIcons[ICON_GO]);
    SetWindowPos(hwndGoButton, NULL, 0, 0, buttonSize, buttonSize, SWP_NOZORDER | SWP_NOMOVE);

    hwndRefreshButton = CreateWindowEx(0, WC_BUTTON, NULL, WS_VISIBLE | WS_CHILD | BS_ICON,
                                       0, 0, buttonSize, buttonSize, hwndNavbar, NULL, globalHInstance, NULL);
    SendMessage(hwndRefreshButton, BM_SETIMAGE, IMAGE_ICON, (LPARAM)uiIcons[ICON_REFRESH]);
    SetWindowPos(hwndRefreshButton, NULL, 0, 0, buttonSize, buttonSize, SWP_NOZORDER | SWP_NOMOVE);

    hwndSearchButton = CreateWindowEx(0, WC_BUTTON, NULL, WS_VISIBLE | WS_CHILD | BS_ICON,
                                      0, 0, buttonSize, buttonSize, hwndNavbar, NULL, globalHInstance, NULL);
    SendMessage(hwndSearchButton, BM_SETIMAGE, IMAGE_ICON, (LPARAM)uiIcons[ICON_SEARCH]);
    SetWindowPos(hwndSearchButton, NULL, 0, 0, buttonSize, buttonSize, SWP_NOZORDER | SWP_NOMOVE);
}

void createNavbar() {
    hwndNavbar = CreateWindowEx(0, WC_STATIC, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER, 
                                0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
                                
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndNavbar, GWLP_WNDPROC, (LONG_PTR)NavbarWndProc);

    hwndAddrEditWrapper = CreateWindowEx(0, WC_STATIC, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER, 
                                         0, 0, 0, 0, hwndNavbar, (HMENU)NULL, globalHInstance, NULL);   
                                
    hwndAddrEdit = CreateWindowEx(0, WC_EDIT, NULL, WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_LEFT,
                                  0, 0, 0, 0, hwndAddrEditWrapper, (HMENU)NULL, globalHInstance, NULL);
    SendMessage(hwndAddrEdit, WM_SETFONT, (WPARAM)hGuiFont, 0);
    AddrEditOrigWndProc = (WNDPROC)SetWindowLongPtr(hwndAddrEdit, GWLP_WNDPROC, (LONG_PTR)AddrEditWndProc);     
    
    hwndSearchEditWrapper = CreateWindowEx(0, WC_STATIC, NULL, WS_VISIBLE | WS_CHILD | WS_BORDER, 
                                           0, 0, 0, 0, hwndNavbar, (HMENU)NULL, globalHInstance, NULL);
    SearchEditWrapperOrigWndProc = (WNDPROC)SetWindowLongPtr(hwndSearchEditWrapper, GWLP_WNDPROC, (LONG_PTR)SearchEditWrapperWndProc);
                                           
    hwndSearchEdit = CreateWindowEx(0, WC_EDIT, NULL, WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 
                                    0, 0, 0, 0, hwndSearchEditWrapper, (HMENU)NULL, globalHInstance, NULL);
    SendMessage(hwndSearchEdit, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SearchEditOrigWndProc = (WNDPROC)SetWindowLongPtr(hwndSearchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditWndProc);           

    setEditMode(false);
    createNavButtons(); 
    UpdateWindow(hwndNavbar);                               
}