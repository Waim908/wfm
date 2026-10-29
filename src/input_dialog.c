#include "main.h"

struct Options {
    wchar_t* title;
    wchar_t* label;
    wchar_t* defaultText;
    enum InputSelectMode mode;
};

static struct Options options = {0};
static wchar_t* result = NULL;

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

INT_PTR CALLBACK InputDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;   // 本对话框不依赖 LPARAM 初始化数据
    switch (msg) {      
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK) {
                HWND hwndEdit = GetDlgItem(hwndDlg, IDC_EDIT);
                int len = GetWindowTextLength(hwndEdit);
                if (len > 0) {
                    result = calloc((size_t)len + 1, sizeof(wchar_t));
                    if (!result) {           // S15：分配失败时必须中止，否则 WM_GETTEXT 会写入 NULL
                        EndDialog(hwndDlg, IDCANCEL);
                        return (INT_PTR)TRUE;
                    }
                    SendMessage(hwndEdit, WM_GETTEXT, len + 1, (LPARAM)result);
                }
                EndDialog(hwndDlg, LOWORD(wParam));
                return (INT_PTR)TRUE;
            } 
            else if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwndDlg, LOWORD(wParam));
                return (INT_PTR)TRUE;
            }
            else if (LOWORD(wParam) == IDC_BTN_CLEAR_EDIT) {
                // 清空输入框并把焦点交回去：重命名长文件名时「整个换掉」比划着删快
                HWND hwndEdit = GetDlgItem(hwndDlg, IDC_EDIT);
                SetWindowText(hwndEdit, L"");
                SendMessage(hwndDlg, WM_NEXTDLGCTL, (WPARAM)hwndEdit, TRUE);
                return (INT_PTR)TRUE;
            }
            break;
        }
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(GetParent(hwndDlg), &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL, (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2,
                         (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2,
                         0, 0, SWP_NOZORDER | SWP_NOSIZE);

            SetWindowText(hwndDlg, options.title);
            SetWindowText(GetDlgItem(hwndDlg, IDC_LABEL), options.label);
            SetWindowText(GetDlgItem(hwndDlg, IDOK), lc_str.ok);
            SetWindowText(GetDlgItem(hwndDlg, IDCANCEL), lc_str.cancel);
            
            HWND hwndEdit = GetDlgItem(hwndDlg, IDC_EDIT);
            if (options.defaultText) SetWindowText(hwndEdit, options.defaultText);

            SendMessage(hwndDlg, WM_NEXTDLGCTL, (WPARAM)hwndEdit, TRUE);

            // 打开时选中哪一段。BASENAME 只在「重命名」用：扩展名不选中，输入新名字时
            // 它会自动保留（Windows 资源管理器就是这个行为）；全选会把扩展名一起换掉。
            if (options.mode == INPUT_SELECT_ALL) {
                SendMessage(hwndEdit, EM_SETSEL, 0, -1);
            }
            else if (options.mode == INPUT_SELECT_BASENAME) {
                const wchar_t* dot = options.defaultText ? wcsrchr(options.defaultText, L'.') : NULL;
                // 没有扩展名、或者点就在开头（.gitignore 这类）时，点之前没有可留的主名，
                // 这种情况整个选中。
                int end = (dot && dot != options.defaultText)
                        ? (int)(dot - options.defaultText)
                        : (int)wcslen(options.defaultText ? options.defaultText : L"");
                if (end > 0) SendMessage(hwndEdit, EM_SETSEL, 0, end);
            }
            break;
        }
    }

    return (INT_PTR)FALSE;  
}

wchar_t* InputDialog(wchar_t* title, wchar_t* label, wchar_t* defaultText, enum InputSelectMode mode) {
    options.title = title;
    options.label = label;
    options.defaultText = defaultText;
    options.mode = mode;
    result = NULL;
    
    DialogBox(globalHInstance, MAKEINTRESOURCE(IDD_INPUT), hwndMain, &InputDialogProc);
    
    options.title = NULL;
    options.label = NULL;
    options.defaultText = NULL;
    options.mode = INPUT_SELECT_NONE;
    return result;
}