#include "main.h"

#define TBBUTTON_COMMAND_OFFSET (WM_APP + 100)
#define NUM_BUTTONS 8

struct ToolButton {
    wchar_t* text;
    int bmpId;
    void(*proc)();
    bool separate;
    bool showText;
};

extern void onBookmarkButtonClick();

struct ToolButton buttons[] = {
    {NULL, 0, &onMenuItemUpClick, true, false},
    {NULL, 1, &onMenuItemCopyClick, false, false},
    {NULL, 2, &onMenuItemCutClick, false, false},
    {NULL, 3, &onMenuItemPasteClick, false, false},
    {NULL, 4, &onMenuItemDeleteClick, true, false},
    {NULL, 5, &onMenuItemNewFolderClick, false, false},
    {NULL, 6, &onMenuItemNewFileClick, false, false},
    {NULL, -1, &onBookmarkButtonClick, true, true}
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
    buttons[7].text = L"Bookmark";

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
    
    // Create a bitmap for bookmark button with text
    HDC hdc = GetDC(hwndToolbar);
    HBITMAP hBmpBookmark = CreateBitmap(16, 16, 1, 32, NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmpBookmark);
    
    // Fill with button face color
    COLORREF btnFace = GetSysColor(COLOR_BTNFACE);
    HBRUSH hBrush = CreateSolidBrush(btnFace);
    RECT rect = {0, 0, 16, 16};
    FillRect(hdcMem, &rect, hBrush);
    DeleteObject(hBrush);
    
    // Draw bookmark text
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, GetSysColor(COLOR_BTNTEXT));
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);
    
    // Draw "B" character
    RECT textRect = {0, 0, 16, 16};
    DrawTextW(hdcMem, L"B", 1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    
    SelectObject(hdcMem, hOldFont);
    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(hwndToolbar, hdc);
    
    ImageList_Add(hImageList, hBmpBookmark, NULL);
    DeleteObject(hBmpBookmark);
    
    SendMessage(hwndToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImageList);

    TBBUTTON tbbSeparator = {0};
    tbbSeparator.fsStyle = BTNS_SEP;

    TBBUTTON tbButton;
    tbButton.fsState = TBSTATE_ENABLED;

    for (int i = 0, j = 0; i < NUM_BUTTONS; i++, j++) {
        tbButton.iBitmap = buttons[i].bmpId;
        tbButton.idCommand = TBBUTTON_COMMAND_OFFSET + i;
        tbButton.iString = (INT_PTR)buttons[i].text;

        if (buttons[i].showText) {
            tbButton.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
        } else {
            tbButton.fsStyle = BTNS_BUTTON;
        }

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