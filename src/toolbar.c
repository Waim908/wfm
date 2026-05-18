#include "main.h"

#define TBBUTTON_COMMAND_OFFSET (WM_APP + 100)
#define NUM_BUTTONS 8

struct ToolButton {
    wchar_t* text;
    int bmpId;
    void(*proc)();
    bool separate;
};

extern void onBookmarkButtonClick();

struct ToolButton buttons[] = {
    {NULL, 0, &onMenuItemUpClick, true},
    {NULL, 1, &onMenuItemCopyClick, false},
    {NULL, 2, &onMenuItemCutClick, false},
    {NULL, 3, &onMenuItemPasteClick, false},
    {NULL, 4, &onMenuItemDeleteClick, true},
    {NULL, 5, &onMenuItemNewFolderClick, false},
    {NULL, 6, &onMenuItemNewFileClick, false},
    {NULL, 7, &onBookmarkButtonClick, true}
};

static WNDPROC OrigWndProc;

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

HWND hwndToolbar = NULL;

LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return OrigWndProc(hwnd, msg, wParam, lParam);
}

void createToolButtons() {
    buttons[0].text = lc_str.up;
    buttons[1].text = lc_str.copy;
    buttons[2].text = lc_str.cut;
    buttons[3].text = lc_str.paste;
    buttons[4].text = lc_str.delete;
    buttons[5].text = lc_str.new_folder;
    buttons[6].text = lc_str.new_file;
    buttons[7].text = lc_str.bookmark;

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

void createToolbar() {
    hwndToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, NULL, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE | TBSTYLE_FLAT | CCS_NORESIZE, 
                                 0, 0, 0, 0, hwndMain, NULL, globalHInstance, NULL);
    createToolButtons();
    
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndToolbar, GWLP_WNDPROC, (LONG_PTR)ToolbarWndProc);
    UpdateWindow(hwndToolbar);
}

void toolbarCommand(int command) {
    (buttons[command - TBBUTTON_COMMAND_OFFSET].proc)();
}