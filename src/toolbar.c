#include "main.h"

#define TBBUTTON_COMMAND_OFFSET (WM_APP + 100)
#define NUM_BUTTONS 10

struct ToolButton {
    wchar_t* text;
    int bmpId;
    void(*proc)();
    bool separate;
};

extern void onBookmarkButtonClick();
static void onCmdButtonClick();
static void onExplorerButtonClick();

struct ToolButton buttons[] = {
    {NULL, 0, &onMenuItemUpClick, true},
    {NULL, 1, &onMenuItemCopyClick, false},
    {NULL, 2, &onMenuItemCutClick, false},
    {NULL, 3, &onMenuItemPasteClick, false},
    {NULL, 4, &onMenuItemDeleteClick, true},
    {NULL, 5, &onMenuItemNewFolderClick, false},
    {NULL, 6, &onMenuItemNewFileClick, false},
    {NULL, 7, &onBookmarkButtonClick, true},
    {NULL, 8, &onCmdButtonClick, false},
    {NULL, 9, &onExplorerButtonClick, false}
};

static WNDPROC OrigWndProc;

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndToolbar = NULL;

LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return OrigWndProc(hwnd, msg, wParam, lParam);
}

void createToolButtons() {
    buttons[0].text = lc_str.tb_up;
    buttons[1].text = lc_str.tb_copy;
    buttons[2].text = lc_str.tb_cut;
    buttons[3].text = lc_str.tb_paste;
    buttons[4].text = lc_str.tb_delete;
    buttons[5].text = lc_str.tb_new_folder;
    buttons[6].text = lc_str.tb_new_file;
    buttons[7].text = lc_str.tb_bookmark;
    buttons[8].text = L"CMD";
    buttons[9].text = L"Explorer";

    SendMessage(hwndToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
    SendMessage(hwndToolbar, TB_SETINDENT, 2, 0);

    HIMAGELIST hImageList = ImageList_Create(16, 16, ILC_COLOR32, NUM_BUTTONS, 0);

    ImageList_AddIcon(hImageList, uiIcons[ICON_UP]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_COPY]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_CUT]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_PASTE]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_DELETE]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_NEW_FOLDER]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_NEW_FILE]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_BOOKMARK]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_CMD]);
    ImageList_AddIcon(hImageList, uiIcons[ICON_EXPLORER]);
    
    SendMessage(hwndToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImageList);

    TBBUTTON tbbSeparator = {0};
    tbbSeparator.fsStyle = BTNS_SEP;

    TBBUTTON tbButton;
    tbButton.fsState = TBSTATE_ENABLED;
    tbButton.fsStyle = BTNS_BUTTON;

    for (int i = 0, j = 0; i < NUM_BUTTONS; i++, j++) {
        tbButton.iBitmap = buttons[i].bmpId;
        tbButton.idCommand = TBBUTTON_COMMAND_OFFSET + i;
        tbButton.iString = (INT_PTR)buttons[i].text;

        SendMessage(hwndToolbar, TB_INSERTBUTTON, j, (LPARAM)&tbButton);

        if (buttons[i].separate) SendMessage(hwndToolbar, TB_INSERTBUTTON, ++j, (LPARAM)&tbbSeparator);
    }   
}

// 用绝对路径启动系统程序：Winlator 等环境的 PATH 不一定包含
// C:\windows\system32，裸名字的 ShellExecute 会静默失败（返回值 <= 32），
// 表现为按钮点击毫无反应。先试系统目录绝对路径，失败退回裸名字，
// 仍失败弹提示，绝不让点击无声无息。
// dir/params 允许为 NULL：「此电脑」等虚拟节点没有文件系统路径，
// 此时直接用系统程序的默认位置启动，而不是拒绝执行。
static void shellLaunchSystemApp(BOOL useWindowsDir, const wchar_t* exeName, const wchar_t* params, const wchar_t* dir) {
    wchar_t exePath[MAX_PATH] = {0};
    UINT len = useWindowsDir ? GetWindowsDirectoryW(exePath, MAX_PATH)
                             : GetSystemDirectoryW(exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) exePath[0] = L'\0';
    else {
        wcscat_s(exePath, MAX_PATH, L"\\");
        wcscat_s(exePath, MAX_PATH, exeName);
    }

    HINSTANCE hInst = ShellExecute(hwndMain, L"open", exePath[0] ? exePath : exeName, params, dir, SW_SHOW);
    if ((INT_PTR)hInst <= 32 && exePath[0]) {
        hInst = ShellExecute(hwndMain, L"open", exeName, params, dir, SW_SHOW);
    }
    if ((INT_PTR)hInst <= 32) {
        wchar_t msg[MAX_PATH + 128] = {0};
        swprintfTrunc(msg, MAX_PATH + 128, lc_str.msg_cannot_launch_system_app,
                      exePath[0] ? exePath : exeName);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONERROR);
    }
}

static void onCmdButtonClick() {
    extern struct FileNode* currPathFileNode;
    if (!currPathFileNode) return;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    // 虚拟节点（此电脑/收藏等）path 为空串 → 不带工作目录启动
    shellLaunchSystemApp(FALSE, L"cmd.exe", NULL, path[0] ? path : NULL);
}

static void onExplorerButtonClick() {
    extern struct FileNode* currPathFileNode;
    if (!currPathFileNode) return;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    // explorer.exe 需要将路径作为参数传递；虚拟节点则不带参数，
    // 让 explorer 用自己的默认位置（如“我的电脑”）
    shellLaunchSystemApp(TRUE, L"explorer.exe", path[0] ? path : NULL, NULL);
}

void createToolbar() {
    hwndToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, NULL, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE | TBSTYLE_FLAT | CCS_NORESIZE, 
                                 0, 0, 0, 0, hwndMain, NULL, globalHInstance, NULL);
    createToolButtons();
    
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndToolbar, GWLP_WNDPROC, (LONG_PTR)ToolbarWndProc);
    UpdateWindow(hwndToolbar);
}

void toolbarCommand(int command) {
    int index = command - TBBUTTON_COMMAND_OFFSET;
    if (index < 0 || index >= NUM_BUTTONS) return;
    (buttons[index].proc)();
}