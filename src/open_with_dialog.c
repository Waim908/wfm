#include "main.h"

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

static wchar_t* selectedProgram = NULL;
static bool shouldRemoveAssoc = false;
static wchar_t dialogExistingProgram[MAX_PATH] = {0};

bool getFileAssociation(const wchar_t* ext, wchar_t* program, int programSize) {
    if (!ext || !program || programSize <= 0) return false;
    
    HKEY hkey;
    wchar_t regPath[128] = {0};
    swprintf_s(regPath, 128, L"SOFTWARE\\Winlator\\WFM\\FileAssociations\\%ls", ext);
    
    if (RegOpenKeyEx(HKEY_CURRENT_USER, regPath, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD size = programSize * sizeof(wchar_t);
        DWORD type = REG_SZ;
        if (RegQueryValueEx(hkey, L"Program", NULL, &type, (BYTE*)program, &size) == ERROR_SUCCESS) {
            RegCloseKey(hkey);
            return (program[0] != L'\0');
        }
        RegCloseKey(hkey);
    }
    return false;
}

void saveFileAssociation(const wchar_t* ext, const wchar_t* program) {
    if (!ext || !program) return;
    
    HKEY hkey;
    wchar_t regPath[128] = {0};
    swprintf_s(regPath, 128, L"SOFTWARE\\Winlator\\WFM\\FileAssociations\\%ls", ext);
    
    if (RegCreateKeyEx(HKEY_CURRENT_USER, regPath, 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(hkey, L"Program", 0, REG_SZ, (BYTE*)program,
                     (wcslen(program) + 1) * sizeof(wchar_t));
        RegCloseKey(hkey);
    }
}

void removeFileAssociation(const wchar_t* ext) {
    if (!ext) return;
    
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\FileAssociations", 0, KEY_WRITE, &hkey) == ERROR_SUCCESS) {
        RegDeleteKey(hkey, ext);
        RegCloseKey(hkey);
    }
}

static INT_PTR CALLBACK OpenWithDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(GetParent(hwndDlg), &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL,
                (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2,
                (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2,
                0, 0, SWP_NOZORDER | SWP_NOSIZE);
            
            SetWindowText(hwndDlg, lc_str.open_with);
            SetWindowText(GetDlgItem(hwndDlg, IDC_LABEL), (wchar_t*)lParam);
            SetWindowText(GetDlgItem(hwndDlg, IDC_BTN_BROWSE), lc_str.browse);
            SetWindowText(GetDlgItem(hwndDlg, IDC_CHECK_ALWAYS), lc_str.always_use);
            SetWindowText(GetDlgItem(hwndDlg, IDOK), lc_str.ok);
            SetWindowText(GetDlgItem(hwndDlg, IDCANCEL), lc_str.cancel);
            
            // 如果已有关联程序，显示在输入框中并勾选"始终使用"
            if (dialogExistingProgram[0]) {
                SetWindowText(GetDlgItem(hwndDlg, IDC_EDIT_PROGRAM), dialogExistingProgram);
                CheckDlgButton(hwndDlg, IDC_CHECK_ALWAYS, BST_CHECKED);
            }
            
            SendMessage(hwndDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hwndDlg, IDC_EDIT_PROGRAM), TRUE);
            return (INT_PTR)TRUE;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDC_BTN_BROWSE: {
                    wchar_t fileName[MAX_PATH] = {0};
                    OPENFILENAME ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwndDlg;
                    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = fileName;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    ofn.lpstrTitle = lc_str.open_with;
                    
                    if (GetOpenFileName(&ofn)) {
                        SetWindowText(GetDlgItem(hwndDlg, IDC_EDIT_PROGRAM), fileName);
                    }
                    break;
                }
                case IDOK: {
                    HWND hwndEdit = GetDlgItem(hwndDlg, IDC_EDIT_PROGRAM);
                    int len = GetWindowTextLength(hwndEdit);
                    
                    if (len > 0) {
                        // 用户输入了程序路径
                        selectedProgram = calloc(len + 1, sizeof(wchar_t));
                        SendMessage(hwndEdit, WM_GETTEXT, len + 1, (LPARAM)selectedProgram);
                        
                        bool alwaysUse = IsDlgButtonChecked(hwndDlg, IDC_CHECK_ALWAYS) == BST_CHECKED;
                        if (alwaysUse) {
                            // 勾选了"始终使用"，保存关联
                            EndDialog(hwndDlg, IDYES);
                        } else {
                            // 没勾选，只是一次性打开
                            EndDialog(hwndDlg, IDOK);
                        }
                    } else {
                        // 输入框为空，询问是否取消关联
                        if (dialogExistingProgram[0]) {
                            // 已有关联，询问是否删除
                            int msgResult = MessageBox(hwndDlg, 
                                L"清空路径将取消默认打开程序设置，是否继续？",
                                lc_str.open_with, MB_YESNO | MB_ICONQUESTION);
                            if (msgResult == IDYES) {
                                selectedProgram = NULL;
                                shouldRemoveAssoc = true;
                                EndDialog(hwndDlg, IDYES);
                            }
                        } else {
                            // 没有关联，提示输入程序路径
                            MessageBox(hwndDlg, L"请输入程序路径", lc_str.open_with, MB_OK | MB_ICONINFORMATION);
                        }
                    }
                    break;
                }
                case IDCANCEL:
                    selectedProgram = NULL;
                    shouldRemoveAssoc = false;
                    EndDialog(hwndDlg, IDCANCEL);
                    break;
            }
            break;
        }
    }
    return (INT_PTR)FALSE;
}

wchar_t* showOpenWithDialog(const wchar_t* filePath, const wchar_t* ext, bool* outRemoveAssoc) {
    selectedProgram = NULL;
    shouldRemoveAssoc = false;
    dialogExistingProgram[0] = L'\0';
    if (outRemoveAssoc) *outRemoveAssoc = false;
    
    wchar_t label[512] = {0};
    wchar_t* fileName = wcsrchr(filePath, L'\\');
    if (fileName) fileName++;
    else fileName = (wchar_t*)filePath;
    
    // 检查是否已有关联，如果有则显示在输入框中
    if (ext) {
        getFileAssociation(ext, dialogExistingProgram, MAX_PATH);
    }
    
    swprintf_s(label, 512, L"%ls\n%ls", lc_str.open_with_label, fileName);
    
    INT_PTR result = DialogBoxParam(globalHInstance, MAKEINTRESOURCE(IDD_OPEN_WITH),
                                    hwndMain, OpenWithDialogProc, (LPARAM)label);
    
    if (result == IDYES) {
        // IDYES: 勾选了"始终使用"或确认删除关联
        if (shouldRemoveAssoc) {
            // 用户清空了路径，取消关联
            if (outRemoveAssoc) *outRemoveAssoc = true;
            return NULL;
        } else if (selectedProgram) {
            // 勾选了"始终使用"，保存关联
            if (ext) {
                saveFileAssociation(ext, selectedProgram);
            }
            return selectedProgram;
        }
    } else if (result == IDOK && selectedProgram) {
        // IDOK: 一次性打开，不保存关联
        return selectedProgram;
    }
    
    if (selectedProgram) free(selectedProgram);
    return NULL;
}
