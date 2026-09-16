#include "main.h"
#ifdef USE_LIBCDIO
#include "libcdio_loader.h"
#endif

static const wchar_t mainWndClass[] = L"WFM-MainWnd";

extern struct FileNode* currPathFileNode;
extern HWND hwndContentView;
extern HWND hwndNavbar;
extern HWND hwndSizebar;
extern HWND hwndStatusbar;
extern HWND hwndToolbar;
extern HWND hwndTreeview;

HMENU hMenuView = NULL;
HMENU hMenuLang = NULL;
// 「查看 → 文件夹位置」子菜单。由 content_view.c 负责打单选勾，
// 这里只创建并暴露句柄（与 hMenuView 同一种做法）。
HMENU hMenuFolderSort = NULL;
// 「查看 → 大图标视图」子菜单，内含「图标大小」「文件名行数」两个子菜单。
// 这两项只对大图标视图生效，所以统一挂在明确点明范围的父项下；非大图标视图下
// 整个父项置灰（content_view.c 的 updateIconViewMenuCheckmarks 负责）。
HMENU hMenuIconView = NULL;
HMENU hMenuIconSize = NULL;
HMENU hMenuLines = NULL;
HMENU hMenuDriveBar = NULL;
// 「查看 → 详细信息视图」子菜单。磁盘占用显示只作用于「大小」列，而这一列只有
// 详细信息视图才有 —— 所以归到这个明确点明范围的父项下（与「大图标视图」同一做法），
// 非详细信息视图下整个父项置灰（content_view.c 的 updateDriveBarMenuCheckmarks 负责）。
HMENU hMenuDetailsView = NULL;
static wchar_t currentLocale[16] = {0};

bool g_showHiddenFiles = false;

HINSTANCE globalHInstance = NULL;
HWND hwndMain = NULL;
HFONT hGuiFont = NULL;
struct LC_STR lc_str = {0};

void updateGuiFont() {
    if (hGuiFont) DeleteObject(hGuiFont);

    NONCLIENTMETRICS ncm = {0};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    hGuiFont = CreateFontIndirect(&ncm.lfMessageFont);

    // Update font on all controls
    SendMessage(hwndToolbar, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndTreeview, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndContentView, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndStatusbar, WM_SETFONT, (WPARAM)hGuiFont, 0);

    // Trigger navbar to recalculate layout with new font
    if (hwndNavbar) {
        InvalidateRect(hwndNavbar, NULL, TRUE);
        SendMessage(hwndNavbar, WM_SIZE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 启动计时打点（诊断用，默认关闭）
//
// 「启动变慢了」这句话在代码里对应的是一串同步调用：建窗口 → 枚举目录 → 渲染首屏
// 图标。光看总时长分不清是哪一段，这里把它切成逐段的增量。
//
// 只有 WFM_STARTUP_TRACE 环境变量非空时才写 stderr；未设时每次调用只剩一次
// bool 判断，正常启动一行都不输出、不多花一次 GetTickCount。
//
// 用法：WFM_STARTUP_TRACE=1 wine wfm 2>&1 | grep WFM-STARTUP
//       +N ms  = 距上一个打点的增量（就是这一段自己花的时间）
//       t=N ms = 距 WinMain 开头的累计值
// ---------------------------------------------------------------------------
static DWORD startupTraceT0 = 0;
static DWORD startupTracePrev = 0;
static bool  startupTraceOn = false;

void startupMark(const char* label) {
    startupMarkN(label, -1);
}

void startupMarkN(const char* label, long value) {
    if (!startupTraceOn || !label) return;

    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr == NULL || hErr == INVALID_HANDLE_VALUE) return;

    DWORD now = GetTickCount();
    char buf[192];
    // label 全是本文件/同项目里的短字面量，192 字节足够，不需要截断逻辑。
    // value < 0 表示不打计数值（多数打点不需要）。
    if (value < 0) {
        sprintf(buf, "[WFM-STARTUP] %-26s +%5lu ms   (t=%lu ms)\n",
                label,
                (unsigned long)(now - startupTracePrev),
                (unsigned long)(now - startupTraceT0));
    }
    else {
        sprintf(buf, "[WFM-STARTUP] %-26s +%5lu ms   (t=%lu ms)  n=%ld\n",
                label,
                (unsigned long)(now - startupTracePrev),
                (unsigned long)(now - startupTraceT0),
                value);
    }
    startupTracePrev = now;

    DWORD written = 0;
    WriteFile(hErr, buf, (DWORD)strlen(buf), &written, NULL);
}

HICON uiIcons[NUM_UI_ICONS] = {0};

struct IconMapping {
    int iconId;
    int resourceId;
};

static const struct IconMapping iconMap[] = {
    {ICON_UP, IDI_UP},
    {ICON_COPY, IDI_COPY},
    {ICON_CUT, IDI_CUT},
    {ICON_PASTE, IDI_PASTE},
    {ICON_DELETE, IDI_DELETE},
    {ICON_NEW_FOLDER, IDI_NEW_FOLDER},
    {ICON_NEW_FILE, IDI_NEW_FILE},
    {ICON_GO, IDI_GO},
    {ICON_REFRESH, IDI_REFRESH},
    {ICON_SEARCH, IDI_SEARCH},
    {ICON_NAV_ARROW, IDI_NAV_ARROW},
    {ICON_BOOKMARK, IDI_BOOKMARK},
    // ICON_CMD / ICON_EXPLORER 不内嵌 ico，见 loadIconFromSystemExe()
};

// CMD/Explorer 工具栏按钮的图标直接取系统 exe 的真实图标：
// cmd.exe 在 %SystemRoot%\System32，explorer.exe 在 %SystemRoot%。
// 依次尝试自有 PE 解析器 → shell 关联图标 → 共享应用图标，
// 保证返回的句柄恒非 NULL，杜绝空 HICON 进 ImageList。
static HICON loadIconFromSystemExe(const wchar_t* exeName, BOOL useWindowsDir) {
    wchar_t dir[MAX_PATH];
    UINT len = useWindowsDir ? GetWindowsDirectoryW(dir, MAX_PATH)
                             : GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return LoadIconW(NULL, IDI_APPLICATION);

    // 去掉可能的尾反斜杠，再用 strsafe 拼接，整体超界就走末级兜底
    size_t dirLen = wcslen(dir);
    if (dirLen > 0 && dir[dirLen - 1] == L'\\') dir[dirLen - 1] = L'\0';

    wchar_t path[MAX_PATH];
    if (SUCCEEDED(StringCchCopyW(path, MAX_PATH, dir)) &&
        SUCCEEDED(StringCchCatW(path, MAX_PATH, L"\\")) &&
        SUCCEEDED(StringCchCatW(path, MAX_PATH, exeName))) {
        HICON hIcon = extractIconFromExe(path, 16, 16);
        if (hIcon) return hIcon;

        // 次选：shell 关联图标（项目已有 SHGetFileInfo 使用先例，Wine 可用）。
        // 返回的 HICON 归调用方，与 uiIcons 的释放约定一致。
        SHFILEINFOW sfi = {0};
        if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
            return sfi.hIcon;
        }
    }

    // 末选：共享应用图标（DestroyIcon 对共享图标无害失败）
    return LoadIconW(NULL, IDI_APPLICATION);
}

void preloadIcons() {
    int mapCount = (int)(sizeof(iconMap) / sizeof(iconMap[0]));
    for (int i = 0; i < mapCount; i++) {
        uiIcons[iconMap[i].iconId] = (HICON)LoadImage(
            globalHInstance, MAKEINTRESOURCE(iconMap[i].resourceId),
            IMAGE_ICON, 16, 16, 0);
    }
    // 系统图标只在启动时各提取一次，之后运行期直接用缓存
    uiIcons[ICON_CMD] = loadIconFromSystemExe(L"cmd.exe", FALSE);
    uiIcons[ICON_EXPLORER] = loadIconFromSystemExe(L"explorer.exe", TRUE);
    startupMark("  icons: LoadImage x12");
}

void freeUIcons() {
    for (int i = 0; i < NUM_UI_ICONS; i++) {
        if (uiIcons[i]) DestroyIcon(uiIcons[i]);
    }
}

void GetWindowRectInParent(HWND hwnd, RECT* rect) {
    GetWindowRect(hwnd, rect);
    MapWindowPoints(HWND_DESKTOP, GetParent(hwnd), (LPPOINT)rect, 2);
}

INT_PTR CALLBACK AboutDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {      
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK:
                case IDCANCEL: {
                  EndDialog(hwndDlg, (INT_PTR) LOWORD(wParam));
                  return (INT_PTR) TRUE;
                }
                case IDC_APP_URL: {
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ShellExecuteW(NULL, L"open", L"https://github.com/Waim908/wfm", NULL, NULL, SW_SHOW);
                        return (INT_PTR) TRUE;
                    }
                    break;
                }
            }
            break;
        }
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(GetParent(hwndDlg), &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL, (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2, (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            
            SetWindowText(hwndDlg, lc_str.about);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_NAME), lc_str.app_name);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_VERSION), lc_str.app_version);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_DEV_NAME), lc_str.app_dev_name);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_MOD_NAME), lc_str.app_mod_name);
            SetWindowText(GetDlgItem(hwndDlg, IDC_APP_URL), lc_str.app_url);
            return (INT_PTR)TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hwndCtrl = (HWND)lParam;
            if (GetDlgCtrlID(hwndCtrl) == IDC_APP_URL) {
                SetTextColor(hdc, RGB(0, 0, 255));
                SetBkMode(hdc, TRANSPARENT);
                return (INT_PTR)GetStockObject(HOLLOW_BRUSH);
            }
            break;
        }
        case WM_SETCURSOR: {
            if ((HWND)wParam == GetDlgItem(hwndDlg, IDC_APP_URL)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
            break;
        }
    }

    return (INT_PTR)FALSE;
}

static void createMainMenu(void);
static void switchLanguage(const wchar_t* lang);
static void updateLangMenuCheckmarks(void);
static void toggleShowHidden(void);

static HMENU hMenuEdit = NULL;

// 剪贴板为空时把「编辑」菜单里的粘贴项与「清空剪贴板」置灰，
// 与右键菜单/工具栏联动。三者可用性完全同源：都以 CF_HDROP 是否存在为准。
void updatePasteMenuState() {
    if (!hMenuEdit) return;
    UINT flag = clipboardHasItems() ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
    EnableMenuItem(hMenuEdit, ID_EDIT_PASTE, MF_BYCOMMAND | flag);
    EnableMenuItem(hMenuEdit, ID_EDIT_PASTE_SHORTCUT, MF_BYCOMMAND | flag);
    EnableMenuItem(hMenuEdit, ID_EDIT_CLEAR_CLIPBOARD, MF_BYCOMMAND | flag);
}

// 按 CommandLineToArgvW 的规则把参数写成带引号形式（追加到 buf，pos 随之推进）。
// 结尾的连续反斜杠必须翻倍：2n 个反斜杠 + 引号会被解析成 n 个反斜杠 + 定界引号，
// 否则盘根 "C:\" 会与闭合引号组成 \"、被当成字面引号，整个参数字符串错位。
// 规则见 wine/dlls/shcore/main.c 的 CommandLineToArgvW 注释。
static bool appendQuotedArg(wchar_t* buf, size_t bufCch, size_t* pos, const wchar_t* arg) {
    if (!arg || !arg[0]) return false;

    size_t len = wcslen(arg);
    size_t trailing = 0;
    while (trailing < len && arg[len - 1 - trailing] == L'\\') trailing++;
    // 开引号 + 正文 + 翻倍的反斜杠 + 闭引号 + 结束符
    if (*pos + len + trailing + 3 > bufCch) return false;

    buf[(*pos)++] = L'"';
    memcpy(&buf[*pos], arg, len * sizeof(wchar_t));
    *pos += len;
    for (size_t i = 0; i < trailing; i++) buf[(*pos)++] = L'\\';
    buf[(*pos)++] = L'"';
    buf[*pos] = L'\0';
    return true;
}

// 打开新窗口：另起一个自身进程，并把当前目录作为启动路径交给它（WinMain 把第一
// 个参数当导航路径）。wfm 没有单实例互斥，第二个进程会正常建自己的窗口。
// exe 路径单独走 lpApplicationName（不靠命令行解析），命令行首段只是子进程的
// argv[0]，所以路径含空格也不会被拆错。
static void openNewWindow() {
    wchar_t exePath[MAX_PATH] = {0};
    wchar_t currPath[MAX_PATH] = {0};
    wchar_t cmdLine[MAX_PATH * 2 + 8] = {0};
    const size_t cmdCch = sizeof(cmdLine) / sizeof(cmdLine[0]);
    size_t pos = 0;

    // 返回长度等于缓冲区大小说明已被截断，截断的路径不能拿去当 exe 路径
    DWORD exeLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    bool ok = (exeLen > 0 && exeLen < MAX_PATH);
    if (ok) {
        // 「此电脑」这类虚拟节点没有文件系统路径，得到空串 → 子进程走默认位置
        if (currPathFileNode) getFileNodePath(currPathFileNode, currPath);
        ok = appendQuotedArg(cmdLine, cmdCch, &pos, exePath) &&
             (!currPath[0] || appendQuotedArg(cmdLine, cmdCch, &pos, currPath));
    }

    if (ok) {
        STARTUPINFOW si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        ok = CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) != 0;
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    if (!ok) {
        wchar_t msg[512] = {0};
        swprintfTrunc(msg, sizeof(msg) / sizeof(msg[0]), lc_str.msg_cannot_open_new_window, exePath);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
    }
}

// 【绝不要在菜单还开着的时候建模态对话框】—— 这就是「关窗后的确认框按钮点不动、回车却能
// 确认退出」那个远古 bug 的根因。改这里之前请把下面几条读完（都核对过 Wine 源码）。
//
// 机制：
//  · Wine 的菜单跟踪是**嵌套在 NtUserTrackPopupMenuEx / 菜单栏跟踪里的一个循环**。它一开头就
//    `set_capture_window(capture_win, GUI_INMENUMODE, NULL)`（win32u/menu.c:4134-4135：菜单栏
//    用属主窗口、弹出菜单用菜单自己的窗口），**把整个线程的捕获钉在那个窗口上**；一直钉到循环
//    结束之后的 menu.c:4343 才解开。
//  · 这个循环还**替应用派发消息**：menu.c:4148 用 `NtUserPeekMessage(&msg, 0, 0, 0, ...)` 取
//    「任何窗口的任何消息」，凡是不属于菜单的就在 menu.c:4330-4331 `NtUserDispatchMessage()`
//    直接派发出去。所以「点关闭框」触发的 WM_CLOSE 是在**菜单循环内部**进到 MainWndProc 的
//    （menu.c:4345 的注释写的就是这个场景：dropdown 还在、关闭框被点）。
//  · 于是 WM_CLOSE 里建对话框 = 在菜单循环内部开一个模态循环，而此刻 win32u/message.c 的
//    process_mouse_message 开头依然是
//        if (info.hwndCapture) { hittest = HTCLIENT; msg->hwnd = info.hwndCapture; }
//    ——捕获钉在菜单/主窗口上，**整个线程的鼠标消息都被改送到那里**，对话框的按钮永远收不到；
//    键盘路径（process_keyboard_message）不查捕获，所以回车 / Esc 一切正常。
//  · 应用侧**没法**自救式地解开捕获：server/queue.c:3878-3883 明确拒绝
//        /* if in menu mode, reject all requests to change focus, except if the menu bit is set */
//        if (input_shm->menu_owner && !(req->flags & CAPTURE_MENU)) { set_error(STATUS_ACCESS_DENIED); return; }
//    ReleaseCapture() / SetCapture() 发的都是 flags = 0 → 一律被拒（Windows 也是这个语义：菜单
//    期间只有菜单能改捕获）。
//
// 所以唯一的解法是：**先请菜单循环出去，再隔一次消息派发建对话框**。
//  · EndMenu()（user32.spec:464 → NtUserEndMenu）在 menu.c:4682-4696 里做的是
//    `exit_menu = TRUE` + `NtUserPostMessage(top_popup, WM_CANCELMODE, 0, 0)`；而菜单循环在
//    menu.c:4167 专门盯着 WM_CANCELMODE 收摊 —— 这是官方取消路径，Wine 自己的测试
//    （dlls/user32/tests/msg.c:20401/20422）就是在窗口过程里这么调的。
//  · EndMenu() 只是**投递**一条消息，循环必须等我们**从窗口过程返回**之后才能看到它 —— 所以
//    绝不能在这条路径上同步建对话框。顺序是：先 EndMenu()、再 PostMessage 一条自己的消息、
//    然后立刻返回。等菜单循环收到 WM_CANCELMODE → 解开捕获（4343）→ 销毁菜单窗口（4359）→
//    退出，我们那条消息才会被自己的消息循环取到，那时捕获已经是干净的。消息队列是 FIFO，
//    WM_CANCELMODE 必定排在我们那条消息前面。
//  · 两个反面教训，别再走一遍：
//    ① 「只把对话框推迟一次派发」**不**够用 —— 那个循环会把我们推迟的消息也捞出来派发，
//       所以 EndMenu() 才是关键，推迟只是为了不与当前这次派发抢跑。
//    ② 菜单项命令（WM_COMMAND）**不**需要这层处理：exec_focused_item 是 PostMessage
//       （menu.c:3499/3507），且它会让 exit_menu 置真、循环先退出、捕获先解开，然后才轮到
//       我们的 WM_COMMAND —— 时序本来就是安全的。
#define MSG_DEFERRED_EXIT_CONFIRM (WM_APP + 0x100)
static bool exitConfirmPending = false;

void mainMenuCommand(WPARAM wParam) {
    switch (LOWORD(wParam)) {
        case ID_EDIT_CUT:
            onMenuItemCutClick();
            break;
        case ID_EDIT_COPY:
            onMenuItemCopyClick();
            break;
        case ID_EDIT_PASTE:
            onMenuItemPasteClick();
            break;
        case ID_EDIT_PASTE_SHORTCUT:
            onMenuItemPasteShortcutClick();
            break;
        case ID_EDIT_CLEAR_CLIPBOARD:
            // 只丢掉待粘贴的内容（清空 CF_HDROP），文件本身不动。
            // clearClipboard() 内部会调 onClipboardChanged() 同步状态栏、
            // 工具栏粘贴按钮与本次这一项的置灰状态。
            clearClipboard();
            break;
        case ID_EDIT_SELECT_ALL:
            onMenuItemSelectAllClick();
            break;                  
        case ID_HELP_ABOUT:
            DialogBox(globalHInstance, MAKEINTRESOURCE(IDD_ABOUT), hwndMain, &AboutDialogProc);
            break;
        case ID_VIEW_LARGEICONS:
            setViewStyle(STYLE_LARGE_ICON);
            break;
        case ID_FILE_EXIT:
            DestroyWindow(hwndMain);
            break;
        case ID_FILE_NEW_WINDOW:
            openNewWindow();
            break;
        case ID_VIEW_SMALLICONS:
            setViewStyle(STYLE_SMALL_ICON);
            break;
        case ID_VIEW_LIST:
            setViewStyle(STYLE_LIST);
            break;
        case ID_VIEW_DETAILS:
            setViewStyle(STYLE_DETAILS);
            break;                      
        case ID_VIEW_CLEAR_ICON_CACHE:
            clearIconCaches();
            navigateRefresh();
            break;
        case ID_VIEW_SHOW_HIDDEN:
            toggleShowHidden();
            break;
        case ID_VIEW_DRIVE_BAR:
            setDriveBarMode(DRIVE_BAR_GRAPH);
            break;
        case ID_VIEW_DRIVE_BAR_TOTAL:
            setDriveBarMode(DRIVE_BAR_TOTAL);
            break;
        case ID_VIEW_DRIVE_BAR_NONE:
            setDriveBarMode(DRIVE_BAR_NONE);
            break;
        case ID_VIEW_FOLDER_CLASSIC:
            setFolderSortMode(FOLDER_SORT_CLASSIC);
            break;
        case ID_VIEW_FOLDER_TOP:
            setFolderSortMode(FOLDER_SORT_TOP);
            break;
        case ID_VIEW_FOLDER_BOTTOM:
            setFolderSortMode(FOLDER_SORT_BOTTOM);
            break;
        case ID_VIEW_FOLDER_PLAIN:
            setFolderSortMode(FOLDER_SORT_PLAIN);
            break;
        case ID_VIEW_ICONSIZE_32:  setIconViewIconSize(32);  break;
        case ID_VIEW_ICONSIZE_48:  setIconViewIconSize(48);  break;
        case ID_VIEW_ICONSIZE_64:  setIconViewIconSize(64);  break;
        case ID_VIEW_ICONSIZE_96:  setIconViewIconSize(96);  break;
        case ID_VIEW_ICONSIZE_128: setIconViewIconSize(128); break;
        case ID_VIEW_LINES_AUTO: setIconViewLabelLines(0); break;
        case ID_VIEW_LINES_1:    setIconViewLabelLines(1); break;
        case ID_VIEW_LINES_2:    setIconViewLabelLines(2); break;
        case ID_VIEW_LINES_3:    setIconViewLabelLines(3); break;
        case ID_VIEW_LINES_4:    setIconViewLabelLines(4); break;
        case ID_VIEW_LINES_5:    setIconViewLabelLines(5); break;
        case ID_MOUNT_LOCATE_ISO:
#ifdef USE_LIBCDIO
            onMenuItemLocateISOImageClick();
#else
            MessageBox(hwndMain, lc_str.msg_no_libcdio, lc_str.alert, MB_OK);
#endif
            break;
        case ID_MOUNT_UNMOUNT_ISO:
#ifdef USE_LIBCDIO
            onMenuItemUnloadISOImageClick();
#else
            MessageBox(hwndMain, lc_str.msg_no_libcdio, lc_str.alert, MB_OK);
#endif
            break;
        case ID_LANG_EN: switchLanguage(L"en"); break;
        case ID_LANG_ZH: switchLanguage(L"zh"); break;
        case ID_LANG_PT: switchLanguage(L"pt"); break;
        case ID_LANG_RU: switchLanguage(L"ru"); break;
    }
}

void resizeControls() {
    RECT rect;
    GetClientRect(hwndMain, &rect);

    RECT toolbarRect;
    RECT buttonRect;
    SendMessage(hwndToolbar, TB_GETITEMRECT, 0, (LPARAM)&buttonRect);
    SetWindowPos(hwndToolbar, NULL, 0, 0, rect.right, buttonRect.bottom + 3, SWP_NOZORDER);
    GetWindowRectInParent(hwndToolbar, &toolbarRect);

    RECT statusbarRect;
    SendMessage(hwndStatusbar, WM_SIZE, 0, 0);
    GetWindowRectInParent(hwndStatusbar, &statusbarRect);

    RECT navbarRect;
    int navbarHeight = getNavbarHeight();
    SetWindowPos(hwndNavbar, NULL, 0, toolbarRect.bottom, rect.right, navbarHeight, SWP_NOZORDER);
    GetWindowRectInParent(hwndNavbar, &navbarRect);
    
    RECT treeviewRect;
    GetWindowRectInParent(hwndTreeview, &treeviewRect);
    int treeviewHeight = statusbarRect.top - navbarRect.bottom;
    SetWindowPos(hwndTreeview, NULL, 0, navbarRect.bottom, treeviewRect.right, treeviewHeight, SWP_NOZORDER);
    
    const int sizebarWidth = 5;
    SetWindowPos(hwndSizebar, NULL, treeviewRect.right, navbarRect.bottom, sizebarWidth, treeviewHeight, SWP_NOZORDER);
    
    int contentViewX = treeviewRect.right + sizebarWidth;
    SetWindowPos(hwndContentView, NULL, contentViewX, navbarRect.bottom, rect.right - contentViewX, treeviewHeight, SWP_NOZORDER);  
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) { 
        case WM_SIZE: {
            resizeControls();
            break;
        }   
        case WM_COMMAND: {
            if (lParam == 0 && HIWORD(wParam) == 0) {
                mainMenuCommand(wParam);
                return 0;
            }
            else if ((HWND)lParam == hwndToolbar) {
                toolbarCommand(LOWORD(wParam));         
            }
            break;
        }
        case WM_ACTIVATE: {
            // 剪贴板是全局资源，别的程序（Wine 下还包括宿主系统本身）随时可以改，
            // 而 WFM 只在「自己」做复制/剪切/清空时同步一次粘贴组与「清空剪贴板」
            // 的可用性 —— 别处改了剪贴板，菜单就会停在旧状态上（例如启动那一刻
            // 读到的剪贴板状态与菜单实际绘制时的状态不一致）。
            // 重新获得焦点时再对齐一次，保证菜单/工具栏/状态栏显示的永远是实时状态。
            if (LOWORD(wParam) != WA_INACTIVE && hwndToolbar && hwndStatusbar) {
                onClipboardChanged();
            }
            break;
        }
        case WM_SYSCOMMAND: {
            switch (LOWORD(wParam)) {
                case ID_HELP_ABOUT: {
                    DialogBox(globalHInstance, MAKEINTRESOURCE(IDD_ABOUT), hwnd, &AboutDialogProc);
                    return 0;
                }
            }
            break;
        }
        case MSG_DEFERRED_EXIT_CONFIRM: {
            // 推迟过一次的退出确认：此刻菜单循环已经收摊、线程捕获也解开了（见本文件上方长注释）。
            // owner 必须给主窗口、不能用 NULL：Wine 的 DIALOG_CreateIndirect 只在 owner 非空时才走
            // 「禁用 owner + 记下 *modal_owner」这条模态路径（user32/dialog.c:582-601）；owner = NULL
            // 时这个框就是一个无归属的顶层窗口 —— 主窗口照旧可点（还能点出第二个确认框），
            // WM_TRANSIENT_FOR 也没有，层级与焦点全靠窗口管理器「尽力而为」。
            int answer = MessageBox(hwndMain, lc_str.msg_confirm_exit_app, lc_str.confirm_exit,
                                    MB_YESNO | MB_ICONQUESTION);
            // 放在 MessageBox 之后才复位：确认框期间的重复 WM_CLOSE 一律被上面的标志位吃掉
            exitConfirmPending = false;
            if (answer == IDYES) {
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_CLOSE: {
            // 这里**绝不能**同步建对话框：菜单循环可能正跑着并持有本线程的捕获（见本文件上方那段
            // 说明），此时建出来的确认框，按钮会永远收不到点击（键盘却正常）。
            // 正确做法：先 EndMenu() 把菜单循环请出去，再隔一次消息派发去问。
            if (!exitConfirmPending) {
                exitConfirmPending = true;
                EndMenu();   // 没有菜单在跟踪时是空操作（NtUserEndMenu 里 top_popup 为空就直接返回）
                PostMessage(hwnd, MSG_DEFERRED_EXIT_CONFIRM, 0, 0);
            }
            return 0;
        }       
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nmhdr = (NMHDR*)lParam;
            if (nmhdr->hwndFrom == hwndContentView) {
                return contentViewNotify(nmhdr);
            }
            else if (nmhdr->hwndFrom == hwndTreeview) {
                return treeviewNotify(nmhdr);
            }
            else return 0;
        }
        case WM_DPICHANGED: {
            updateGuiFont();
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void navigateToFileNode(struct FileNode* node) {
    if (node) {
        clearAddrButtons();
        setCurrPathFileNode(node);
        navigateRefresh();
    }
}

void navigateToPath(wchar_t* path) {
    if (path) {
        clearAddrButtons();
        setCurrPathFromString(path);
        navigateRefresh();
    }
}

void navigateUp() {
    if (currPathFileNode->parent) {
        clearAddrButtons();
        setCurrPathFileNode(currPathFileNode->parent);
        navigateRefresh();
    }
}

void navigateRefresh() {
    if (currPathFileNode) {
        buildChildNodes(currPathFileNode, false);

        // S15：标题栏显示完整路径（原来只有最后一级目录名，用户无从判断所在位置）。
        // 「此电脑」「书签」等虚拟节点没有文件系统路径，退回显示节点名。
        wchar_t fullPath[MAX_PATH] = {0};
        getFileNodePath(currPathFileNode, fullPath);
        SetWindowText(hwndMain, fullPath[0] != L'\0' ? fullPath : currPathFileNode->name);

        updateAddrButtons();
        refreshContentView();
    }
}

void openFileNode(struct FileNode* node) {
    if (node->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        wchar_t parentPath[MAX_PATH] = {0};
        getFileNodePath(node, path);
        getFileNodePath(node->parent, parentPath);
        
        // 检查是否有自定义文件关联
        wchar_t* ext = wcsrchr(node->name, L'.');
        wchar_t program[MAX_PATH] = {0};
        bool openedByAssoc = false;
        if (ext && getFileAssociation(ext, program, MAX_PATH)) {
            // 使用自定义关联程序打开
            HINSTANCE hInst = ShellExecute(hwndMain, L"open", program, path, parentPath, SW_SHOW);
            // 关联程序启动失败（返回值 <= 32）时必须落回常规流程：
            // 否则双击静默无反应，"打开方式"弹窗也永远回不来
            openedByAssoc = ((INT_PTR)hInst > 32);
        }
        if (!openedByAssoc) {
            // 和 Explorer 一样：先让 Wine 处理（Windows exe / Linux 程序都支持）
            HINSTANCE hInst = ShellExecute(hwndMain, L"open", path, NULL, parentPath, SW_SHOW);
            if ((INT_PTR)hInst <= 32) {
                // Wine 也打不开，显示"打开方式"对话框
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
    }
    else navigateToFileNode(node);
}

static void saveShowHidden(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = g_showHiddenFiles ? 1 : 0;
        RegSetValueEx(hkey, L"ShowHidden", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

static void loadShowHidden(void) {
    HKEY hkey;
    DWORD val = 0;
    DWORD size = sizeof(val);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegQueryValueEx(hkey, L"ShowHidden", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hkey);
    }
    g_showHiddenFiles = val != 0;
}

static void toggleShowHidden(void) {
    g_showHiddenFiles = !g_showHiddenFiles;
    saveShowHidden();
    if (hMenuView) {
        CheckMenuItem(hMenuView, ID_VIEW_SHOW_HIDDEN,
            MF_BYCOMMAND | (g_showHiddenFiles ? MF_CHECKED : MF_UNCHECKED));
    }
    navigateRefresh();
}

// Switch language: save to registry, prompt restart (takes effect on next launch)
static void switchLanguage(const wchar_t* lang) {
    saveLanguageToRegistry(lang);
    wcscpy_s(currentLocale, 16, lang);
    updateLangMenuCheckmarks();

    MessageBoxW(hwndMain,
        L"Language setting saved. It will take effect on next launch.\n\n\u8bed\u8a00\u8bbe\u7f6e\u5df2\u4fdd\u5b58\uff0c\u4e0b\u6b21\u542f\u52a8\u65f6\u751f\u6548\u3002",
        L"WFM", MB_OK | MB_ICONINFORMATION);
}

static void updateLangMenuCheckmarks(void) {
    if (!hMenuLang) return;
    UINT check;
    if (wcsncmp(currentLocale, L"zh", 2) == 0)      check = ID_LANG_ZH;
    else if (wcsncmp(currentLocale, L"pt", 2) == 0) check = ID_LANG_PT;
    else if (wcsncmp(currentLocale, L"ru", 2) == 0) check = ID_LANG_RU;
    else                                             check = ID_LANG_EN;
    CheckMenuRadioItem(hMenuLang, ID_LANG_EN, ID_LANG_RU, check, MF_BYCOMMAND);
}

static void createMainMenu() {
    HMENU hmFile = CreatePopupMenu();
    AppendMenu(hmFile, MF_STRING, ID_FILE_NEW_WINDOW, lc_str.new_window);
    AppendMenu(hmFile, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmFile, MF_STRING, ID_FILE_EXIT, lc_str.exit);
    
    HMENU hmEdit = CreatePopupMenu();
    hMenuEdit = hmEdit;
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_CUT, lc_str.cut);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_COPY, lc_str.copy);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_PASTE, lc_str.paste);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_PASTE_SHORTCUT, lc_str.paste_shortcut);
    AppendMenu(hmEdit, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_CLEAR_CLIPBOARD, lc_str.clear_clipboard);
    AppendMenu(hmEdit, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmEdit, MF_STRING, ID_EDIT_SELECT_ALL, lc_str.select_all);
    updatePasteMenuState();
    
    HMENU hmView = CreatePopupMenu();
    hMenuView = hmView;
    AppendMenu(hmView, MF_STRING, ID_VIEW_LARGEICONS, lc_str.large_icons);
    AppendMenu(hmView, MF_STRING, ID_VIEW_SMALLICONS, lc_str.small_icons);
    AppendMenu(hmView, MF_STRING, ID_VIEW_LIST, lc_str.list);
    AppendMenu(hmView, MF_STRING, ID_VIEW_DETAILS, lc_str.details);
    
    AppendMenu(hmView, MF_SEPARATOR, 0, NULL);
    AppendMenu(hmView, MF_STRING, ID_VIEW_SHOW_HIDDEN, lc_str.show_hidden_files);

    HMENU hmFolderSort = CreatePopupMenu();
    hMenuFolderSort = hmFolderSort;
    AppendMenu(hmFolderSort, MF_STRING, ID_VIEW_FOLDER_CLASSIC, lc_str.folder_pos_classic);
    AppendMenu(hmFolderSort, MF_STRING, ID_VIEW_FOLDER_TOP, lc_str.folder_pos_top);
    AppendMenu(hmFolderSort, MF_STRING, ID_VIEW_FOLDER_BOTTOM, lc_str.folder_pos_bottom);
    AppendMenu(hmFolderSort, MF_STRING, ID_VIEW_FOLDER_PLAIN, lc_str.folder_pos_plain);
    AppendMenu(hmView, MF_POPUP, (UINT_PTR)hmFolderSort, lc_str.folder_position);

    // 大图标视图设置：图标尺寸与文件名行数都只作用于大图标视图，因此归到
    // 「大图标视图」父项下 —— 直接叫「图标大小」会被理解成对所有视图生效。
    HMENU hmIconView = CreatePopupMenu();
    hMenuIconView = hmIconView;

    HMENU hmIconSize = CreatePopupMenu();
    hMenuIconSize = hmIconSize;
    AppendMenu(hmIconSize, MF_STRING, ID_VIEW_ICONSIZE_32, L"32");
    AppendMenu(hmIconSize, MF_STRING, ID_VIEW_ICONSIZE_48, L"48");
    AppendMenu(hmIconSize, MF_STRING, ID_VIEW_ICONSIZE_64, L"64");
    AppendMenu(hmIconSize, MF_STRING, ID_VIEW_ICONSIZE_96, L"96");
    AppendMenu(hmIconSize, MF_STRING, ID_VIEW_ICONSIZE_128, L"128");
    AppendMenu(hmIconView, MF_POPUP, (UINT_PTR)hmIconSize, lc_str.icon_size);

    HMENU hmLines = CreatePopupMenu();
    hMenuLines = hmLines;
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_AUTO, lc_str.label_lines_auto);
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_1, L"1");
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_2, L"2");
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_3, L"3");
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_4, L"4");
    AppendMenu(hmLines, MF_STRING, ID_VIEW_LINES_5, L"5");
    AppendMenu(hmIconView, MF_POPUP, (UINT_PTR)hmLines, lc_str.label_lines);

    AppendMenu(hmView, MF_POPUP, (UINT_PTR)hmIconView, lc_str.icon_view);

    // 详细信息视图设置：磁盘占用显示（图形条 / 仅总容量 / 不显示）只作用于「大小」
    // 列，而这一列只有详细信息视图才有；图标视图/列表视图下它连显示的地方都没有，
    // 所以整组挂在这个点明范围的父项下，非详细信息视图时父项置灰。
    HMENU hmDetailsView = CreatePopupMenu();
    hMenuDetailsView = hmDetailsView;

    HMENU hmDriveBar = CreatePopupMenu();
    hMenuDriveBar = hmDriveBar;
    AppendMenu(hmDriveBar, MF_STRING, ID_VIEW_DRIVE_BAR, lc_str.drive_usage_bar_graph);
    AppendMenu(hmDriveBar, MF_STRING, ID_VIEW_DRIVE_BAR_TOTAL, lc_str.drive_usage_bar_total);
    AppendMenu(hmDriveBar, MF_STRING, ID_VIEW_DRIVE_BAR_NONE, lc_str.drive_usage_bar_none);
    AppendMenu(hmDetailsView, MF_POPUP, (UINT_PTR)hmDriveBar, lc_str.drive_usage_bar);

    AppendMenu(hmView, MF_POPUP, (UINT_PTR)hmDetailsView, lc_str.details_view);

    AppendMenu(hmView, MF_STRING, ID_VIEW_CLEAR_ICON_CACHE, lc_str.clear_icon_cache);
#ifdef USE_LIBCDIO
    HMENU hmMount = CreatePopupMenu();
    AppendMenu(hmMount, MF_STRING, ID_MOUNT_LOCATE_ISO, lc_str.locate_iso);
    AppendMenu(hmMount, MF_STRING, ID_MOUNT_UNMOUNT_ISO, lc_str.unmount_iso);
#endif

    HMENU hmHelp = CreatePopupMenu();
    AppendMenu(hmHelp, MF_STRING, ID_HELP_ABOUT, lc_str.about);

    // Language submenu (the word "Language" itself is NOT translated)
    HMENU hmLang = CreatePopupMenu();
    hMenuLang = hmLang;
    AppendMenu(hmLang, MF_STRING, ID_LANG_EN, L"English");
    AppendMenu(hmLang, MF_STRING, ID_LANG_ZH, L"\u4e2d\u6587");
    AppendMenu(hmLang, MF_STRING, ID_LANG_PT, L"Portugu\u00eas");
    AppendMenu(hmLang, MF_STRING, ID_LANG_RU, L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439");
    
    HMENU hmMain = CreateMenu();
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmFile, lc_str.file);
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmEdit, lc_str.edit);
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmView, lc_str.view);
#ifdef USE_LIBCDIO
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmMount, lc_str.mount);
#endif
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmLang, L"Language");
    AppendMenu(hmMain, MF_POPUP, (UINT_PTR)hmHelp, lc_str.help);
    
    SetMenu(hwndMain, hmMain);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;   // Win32 下恒为 NULL
    (void)lpCmdLine;       // 改用 GetCommandLineW()/CommandLineToArgvW 解析
    (void)nCmdShow;
    // 启动计时：环境变量非空才启用（见 startupMark）。放在最前面，这样
    // SetProcessDPIAware 自己的耗时也能被量到。
    startupTraceOn = (GetEnvironmentVariableA("WFM_STARTUP_TRACE", NULL, 0) != 0);
    startupTraceT0 = startupTracePrev = GetTickCount();
    startupMark("WinMain entry");
    SetProcessDPIAware();
    startupMark("SetProcessDPIAware");
    // COM 初始化：lnk 图标解析（IShellLink，content_view.c）与创建快捷方式
    // （IShellLink，file_actions.c）都依赖它。真 Windows 上未初始化 apartment
    // 的 CoCreateInstance 会直接失败（Wine 容忍裸用），这里统一补上。
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    startupMark("CoInitializeEx (lazy)");
    int numArgs;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &numArgs);

    // 解析命令行参数（第一个参数作为导航路径）
    wchar_t navigatePathBuf[MAX_PATH] = {0};
    wchar_t* navigatePath = NULL;
    if (args && numArgs > 1) {
        wcscpy_s(navigatePathBuf, MAX_PATH, args[1]);
        navigatePath = navigatePathBuf;
    }
    if (args) LocalFree(args);
    // Language: registry > system locale
    wchar_t localeName[16] = {0};
    if (!loadLanguageFromRegistry(localeName, sizeof(localeName))) {
        GetSystemDefaultLocaleName(localeName, 16);
    }
    loadLCStrings(localeName);
    wcscpy_s(currentLocale, 16, localeName);
    
    globalHInstance = hInstance;
    preloadIcons();
    startupMark("preloadIcons");
    
    loadBookmarks();
    loadAutoOpenBookmark();
    startupMark("bookmarks + settings");

    NONCLIENTMETRICS ncm = {0};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    hGuiFont = CreateFontIndirect(&ncm.lfMessageFont);
    startupMark("gui font");

    WNDCLASSEX wcx = {0};
    wcx.cbSize = sizeof(wcx);
    wcx.style = CS_HREDRAW | CS_VREDRAW;
    wcx.lpfnWndProc = &MainWndProc;
    wcx.cbClsExtra = 0;
    wcx.cbWndExtra = 0;
    wcx.hInstance = hInstance;
    wcx.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN));
    wcx.hCursor = LoadCursor(hInstance, IDC_ARROW);
    wcx.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wcx.lpszClassName = mainWndClass;
    wcx.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN));

    if (!RegisterClassEx(&wcx)) return 0;
    
    initFileNodes();
    startupMark("initFileNodes");

    HWND hwndDesktop = GetDesktopWindow();
    RECT desktopRect;
    GetWindowRect(hwndDesktop, &desktopRect);
    int hwndWidth = (desktopRect.right - desktopRect.left) * 0.8f;
    int hwndHeight = (desktopRect.bottom - desktopRect.top) * 0.8f;
    
    hwndMain = CreateWindowEx(0, mainWndClass, L"", WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW, 
                              0, 0, hwndWidth, hwndHeight, NULL, NULL, hInstance, NULL);
    if (!hwndMain) return 0;
    startupMark("CreateWindowEx(main)");
    
    createMainMenu();
    updateLangMenuCheckmarks();
    startupMark("  createMainMenu");
    createToolbar();
    startupMark("  createToolbar");
    createNavbar();
    startupMark("  createNavbar");
    createTreeview();
    startupMark("  createTreeview");
    createSizebar();
    startupMark("  createSizebar");
    createContentView();
    startupMark("  createContentView");
    createStatusbar();
    startupMark("child controls");

    SendMessage(hwndToolbar, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndTreeview, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndContentView, WM_SETFONT, (WPARAM)hGuiFont, 0);
    SendMessage(hwndStatusbar, WM_SETFONT, (WPARAM)hGuiFont, 0);
    startupMark("WM_SETFONT x4");
    
    // 视图设置要在 setViewStyle 之前读好：大图标视图首次布局就要用到
    loadIconViewSettings();
    loadDriveBarMode();
    setViewStyle(loadViewStyle());
    loadShowHidden();
    loadFolderSortMode();
    updateViewMenuCheckmarks();
    updateFolderSortMenuCheckmarks();
    updateIconViewMenuCheckmarks();
    if (hMenuView) {
        CheckMenuItem(hMenuView, ID_VIEW_SHOW_HIDDEN,
            MF_BYCOMMAND | (g_showHiddenFiles ? MF_CHECKED : MF_UNCHECKED));
    }
    updateDriveBarMenuCheckmarks();
    int treeviewWidth = hwndWidth * 0.2f;
    SetWindowPos(hwndTreeview, NULL, 0, 0, treeviewWidth, 0, SWP_NOZORDER | SWP_NOMOVE);    
    startupMark("view settings");
    
    if (navigatePath) {
        navigateToPath(navigatePath);
    }
    else {
        navigateRefresh();
        // 「启动时打开」的收藏只在没有显式启动路径时生效：命令行/「打开新窗口」
        // 传进来的路径是用户的明确意图，不该被收藏覆盖掉
        openAutoOpenBookmark();
    }
    startupMark("navigate: enum + icons");

    ShowWindow(hwndMain, SW_SHOW);
    startupMark("ShowWindow");
    UpdateWindow(hwndMain);
    startupMark("UpdateWindow (first paint)");

    // 启动时对齐一次剪贴板相关 UI：菜单里的「粘贴 / 粘贴快捷方式 / 清空剪贴板」、
    // 工具栏粘贴按钮、状态栏来源指示，全部由 CF_HDROP 是否可用来决定。
    // createMainMenu 里已经设过菜单那一份，这里再统一对齐一次，确保窗口首次绘制
    // 时三处状态一致（不依赖 WM_ACTIVATE 是否已经到达）。
    onClipboardChanged();

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 消息循环已退出、不会再有重绘引用，销毁 preloadIcons 缓存的 HICON
    freeUIcons();

#ifdef USE_LIBCDIO
    libcdio_free();
#endif
    
    return (int)msg.wParam;
}