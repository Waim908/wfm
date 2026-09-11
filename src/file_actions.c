#include "main.h"
#ifdef USE_LIBCDIO
#include "libcdio_loader.h"
#endif

#define ID_EVENT_PRELOADER 100
#define PRELOADER_PERIOD 120
#define CEILING(x, y) ((x+(y-1))/y)

enum Msg {
    MSG_CLOSE = WM_APP,
    MSG_NAVIGATE_REFRESH
};

enum FileAction {
    ACTION_NONE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_MOVE,
    ACTION_ISO_EXTRACT
};

struct ActionData {
    enum FileAction action;
    wchar_t** srcPaths;
    int numSrcPaths;
    wchar_t* dstPath;
    bool cancel;
};

static HWND hwndDlg;
static HICON preloaderIcons[8] = {0};
static int preloaderIconIndex = 0;
static wchar_t** clipboard = NULL;
static int clipboardSize = 0;
static bool clipboardIsCut = false;
static struct ActionData* actionData = NULL;

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

static void animatePreloader() {
    SendDlgItemMessage(hwndDlg, IDC_PRELOADER, STM_SETICON, (WPARAM)preloaderIcons[preloaderIconIndex], 0);
    preloaderIconIndex = (preloaderIconIndex + 1) % 8;
}

void clearClipboard() {
    if (clipboard) {
        for (int i = 0; i < clipboardSize; i++) free(clipboard[i]);
        MEMFREE(clipboard);
    }
    clipboardSize = 0;
}

static void freeActionData() {
    if (!actionData) return;
    
    // 对于复制操作，srcPaths 指向 clipboard，不应该被释放
    // 对于剪切/移动操作，srcPaths 也指向 clipboard，但操作完成后应该清空 clipboard
    // 对于删除操作，srcPaths 是单独分配的，需要被释放
    // 对于 ISO 提取操作，srcPaths 也是单独分配的，需要被释放
    
    if (actionData->action == ACTION_DELETE || actionData->action == ACTION_ISO_EXTRACT) {
        if (actionData->srcPaths) {
            for (int i = 0; i < actionData->numSrcPaths; i++) {
                if (actionData->srcPaths[i]) {
                    free(actionData->srcPaths[i]);
                }
            }
            free(actionData->srcPaths);
        }
    }
    else if (actionData->action == ACTION_MOVE) {
        // 移动操作完成后清空 clipboard
        clearClipboard();
        clipboardIsCut = false;
    }
    // 对于 ACTION_COPY，不释放 srcPaths（因为指向 clipboard）
    
    if (actionData->dstPath) {
        free(actionData->dstPath);
    }
    free(actionData);
    actionData = NULL;
}

INT_PTR CALLBACK FileActionDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);

    switch (msg) {
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(GetParent(hwndDlg), &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL, (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2, (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            
            for (int i = 0; i < 8; i++) {
                preloaderIcons[i] = (HICON)LoadImage(globalHInstance, MAKEINTRESOURCE(IDI_PRELOADER_1 + i), IMAGE_ICON, 64, 64, 0);
            }
            
            HWND hwndLabel = GetDlgItem(hwndDlg, IDC_LABEL);
            switch (actionData->action) {
                case ACTION_DELETE: {
                    SetWindowText(hwndDlg, lc_str.deleting_files);
                    SetWindowText(hwndLabel, lc_str.msg_deleting_files);
                    break;
                }
                case ACTION_COPY: {
                    SetWindowText(hwndDlg, lc_str.copying_files);
                    SetWindowText(hwndLabel, lc_str.msg_copying_files);
                    break;
                }
                case ACTION_MOVE: {
                    SetWindowText(hwndDlg, lc_str.moving_files);
                    SetWindowText(hwndLabel, lc_str.msg_moving_files);
                    break;
                }
                case ACTION_ISO_EXTRACT: {
                    SetWindowText(hwndDlg, lc_str.extracting_files);
                    SetWindowText(hwndLabel, lc_str.msg_extracting_files);
                    break;
                }
                case ACTION_NONE:
                    return (INT_PTR)FALSE;
            }
            return (INT_PTR)TRUE;
        }
        case WM_TIMER: {
            if (wParam == ID_EVENT_PRELOADER) animatePreloader();
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDCANCEL) {
                if (MessageBox(hwndDlg, lc_str.msg_cancel_file_operation, lc_str.cancel, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    actionData->cancel = true;
                }
            }
            break;
        }
        case MSG_CLOSE: {
            freeActionData();
            DestroyWindow(hwndDlg);
            hwndDlg = NULL;
            navigateRefresh();
            break;
        }
        case MSG_NAVIGATE_REFRESH: {
            navigateRefresh();
            break;
        }
    }

    return (INT_PTR)FALSE;
}

#ifdef USE_LIBCDIO
static void extractSingleISOFile(void* handle, bool isCDImage, iso9660_stat_t* isoStat, wchar_t* dstPath) {
    char filename[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, dstPath, -1, filename, MAX_PATH, NULL, NULL);
    
    FILE* outFile = fopen(filename, "wb");
    if (!outFile) return;
    
    const uint64_t totalSize = isoStat->total_size;
    const uint64_t isoBlocks = CDIO_EXTENT_BLOCKS(totalSize);

    // 缓冲移出循环：每轮都会被完整覆写，原来在循环内做 2KB 清零纯属浪费
    char buffer[ISO_BLOCKSIZE];
    uint64_t written = 0;

    for (uint64_t i = 0; i < isoBlocks; i++) {
        const lsn_t lsn = isoStat->lsn + i;

        if (isCDImage) {
            if (ptr_cdio_read_data_sectors((CdIo_t*)handle, buffer, lsn, ISO_BLOCKSIZE, 1) != 0) goto end;
        }
        else if (ptr_iso9660_iso_seek_read((iso9660_t*)handle, buffer, lsn, 1) != ISO_BLOCKSIZE) goto end;

        // 最后一块只写入 ISO 记录的真实长度，而不是写满整块再 ftruncate。
        // ftruncate 的返回值原先未检查，失败时会静默留下带填充的截断文件。
        uint64_t remain = totalSize - written;
        size_t chunk = remain < (uint64_t)ISO_BLOCKSIZE ? (size_t)remain : (size_t)ISO_BLOCKSIZE;
        if (chunk == 0) break;
        if (fwrite(buffer, 1, chunk, outFile) != chunk) goto end;
        written += chunk;
    }
    
    fflush(outFile);
    
end:    
    fclose(outFile);
}

static void extractAllISOFiles(void* handle, bool isCDImage, char* srcPath, wchar_t* dstPath) {
    CdioISO9660FileList_t* isoFileList = isCDImage ? ptr_iso9660_fs_readdir((CdIo_t*)handle, srcPath) : 
                                                     ptr_iso9660_ifs_readdir((iso9660_t*)handle, srcPath);
    if (!isoFileList) return;
    
    CdioListNode_t* isoNode;
    char srcName[MAX_PATH] = {0};
    wchar_t dstName[MAX_PATH] = {0};
    char fullSrcPath[MAX_PATH] = {0};
    wchar_t fullDstPath[MAX_PATH] = {0};
    
    int jolietLevel = isCDImage ? ptr_cdio_get_joliet_level((CdIo_t*)handle) : ptr_iso9660_ifs_get_joliet_level((iso9660_t*)handle);
    
    // 使用与原版相同的遍历方式
    _CDIO_LIST_FOREACH(isoNode, isoFileList) {
        // 检查取消标志
        if (actionData && actionData->cancel) {
            break;
        }
        
        iso9660_stat_t* isoStat = (iso9660_stat_t*)ptr__cdio_list_node_data(isoNode);
        if (strcmp(isoStat->filename, ".") == 0 || strcmp(isoStat->filename, "..") == 0) continue;
        
        memset(srcName, 0, MAX_PATH);
        ptr_iso9660_name_translate_ext(isoStat->filename, srcName, jolietLevel);
        
        joinUnixPaths(srcPath, srcName, fullSrcPath, MAX_PATH);
        
        MultiByteToWideChar(CP_ACP, 0, srcName, -1, dstName, MAX_PATH);
        joinPaths(dstPath, dstName, fullDstPath, MAX_PATH);
        
        if (isoStat->type == _STAT_DIR) {
            CreateDirectory(fullDstPath, NULL);
            extractAllISOFiles(handle, isCDImage, fullSrcPath, fullDstPath);
        }
        else if (isoStat->type == _STAT_FILE) {
            extractSingleISOFile(handle, isCDImage, isoStat, fullDstPath);
        }
    }

    ptr_iso9660_filelist_free(isoFileList);
}

#endif /* USE_LIBCDIO */

static DWORD WINAPI fileActionTask(void* param) {
    struct ActionData* actionData = (struct ActionData*)param;
    
    if (actionData->action == ACTION_ISO_EXTRACT) {
#ifdef USE_LIBCDIO
        if (!libcdio_is_loaded() && !libcdio_load()) {
            MessageBoxW(NULL, L"Failed to load libcdio.dll", L"Error", MB_OK | MB_ICONERROR);
            SendMessage(hwndDlg, MSG_CLOSE, 0, 0);
            return 0;
        } else {
            wchar_t* srcPath = actionData->srcPaths[0];
            bool isCDImage = !hasFileExtension(srcPath, L"iso");
            
            char filename[MAX_PATH] = {0};
            WideCharToMultiByte(CP_ACP, 0, srcPath, -1, filename, MAX_PATH, NULL, NULL);

            if (isCDImage) {
                CdIo_t* cdio = ptr_cdio_open(filename, DRIVER_UNKNOWN);
                if (!cdio) {
                    MessageBoxW(NULL, L"Failed to open CD image file.", L"Error", MB_OK | MB_ICONERROR);
                    SendMessage(hwndDlg, MSG_CLOSE, 0, 0);
                    return 0;
                }
                ptr_cdio_set_arg(cdio, "joliet-level", "1");
                extractAllISOFiles(cdio, true, "/", actionData->dstPath);
                ptr_cdio_destroy(cdio);
            }
            else {
                iso9660_t* iso = ptr_iso9660_open_ext(filename, ISO_EXTENSION_JOLIET);
                if (!iso) {
                    MessageBoxW(NULL, L"Failed to open ISO file.", L"Error", MB_OK | MB_ICONERROR);
                    SendMessage(hwndDlg, MSG_CLOSE, 0, 0);
                    return 0;
                }
                extractAllISOFiles(iso, false, "/", actionData->dstPath);
                ptr_iso9660_close(iso);
            }
        }
#else
        MessageBoxW(NULL, lc_str.msg_no_libcdio, L"WFM", MB_OK | MB_ICONWARNING);
        SendMessage(hwndDlg, MSG_CLOSE, 0, 0);
        return 0;
#endif
    }
    else {
        DWORD lastTime = GetTickCount();
            
        for (int i = 0; i < actionData->numSrcPaths && !actionData->cancel; i++) {
            if (!actionData->srcPaths || !actionData->srcPaths[i]) continue;
            
            if (actionData->action == ACTION_DELETE) {
                SHFILEOPSTRUCT sfo = {0};
                sfo.hwnd = hwndDlg;
                sfo.wFunc = FO_DELETE;
                // FOF_ALLOWUNDO：可用时送入回收站（Wine 的 shlfileop.c 会检查 is_trash_available()，
                // 不可用时自动退化为永久删除，故在 Winlator 上无副作用）
                sfo.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_ALLOWUNDO;
                sfo.pTo = NULL;
                sfo.pFrom = actionData->srcPaths[i];
                
                int res = SHFileOperation(&sfo);
                if (res != 0) break;
            }
            else if (actionData->action == ACTION_COPY || actionData->action == ACTION_MOVE) {
                if (!actionData->dstPath) break;
                
                SHFILEOPSTRUCT sfo = {0};
                sfo.hwnd = hwndDlg;
                sfo.wFunc = actionData->action == ACTION_COPY ? FO_COPY : FO_MOVE;
                sfo.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
                sfo.pTo = actionData->dstPath;
                sfo.pFrom = actionData->srcPaths[i];

                int res = SHFileOperation(&sfo);
                if (res != 0) break;            
            }

            DWORD currTime = GetTickCount();
            if ((currTime - lastTime) >= 3000) {
                SendMessage(hwndDlg, MSG_NAVIGATE_REFRESH, 0, 0);
                lastTime = currTime;
            }
        }
    }
    
    SendMessage(hwndDlg, MSG_CLOSE, 0, 0);
    return 0;
}

static wchar_t** createPathsFromFileNodes(struct FileNode** nodes, int count) {
    if (!nodes || count <= 0) return NULL;

    wchar_t** paths = calloc(count, sizeof(wchar_t*));
    if (!paths) return NULL;
    
    wchar_t tmp[MAX_PATH] = {0};
    for (int i = 0; i < count; i++) {
        if (!nodes[i]) continue;

        getFileNodePath(nodes[i], tmp);
        int len = wcslen(tmp);
        wchar_t* path = calloc(len + 2, sizeof(wchar_t));
        if (!path) {
            // 分配失败：释放已分配的部分，整体返回 NULL，避免部分为 NULL 的数组
            for (int j = 0; j < i; j++) free(paths[j]);
            free(paths);
            return NULL;
        }
        wcscpy_s(path, len + 2, tmp);
        path[len+0] = L'\0';
        path[len+1] = L'\0';         
        paths[i] = path;
    }
    
    return paths;
}

void deleteFiles(struct FileNode** nodes, int count) {
    wchar_t msg[128] = {0};
    if (count == 1) {
        swprintf_s(msg, 128, lc_str.msg_confirm_delete_item, nodes[0]->name);
    }
    else swprintf_s(msg, 128, lc_str.msg_confirm_delete_multiple_items, count);

    if (MessageBox(hwndMain, msg, lc_str.confirm_delete, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        actionData = calloc(1, sizeof(struct ActionData));
        if (!actionData) return;
        
        actionData->srcPaths = createPathsFromFileNodes(nodes, count);
        if (!actionData->srcPaths) {
            free(actionData);
            actionData = NULL;
            return;
        }
        actionData->numSrcPaths = count;
        actionData->action = ACTION_DELETE;
        
        hwndDlg = CreateDialogParam(globalHInstance, MAKEINTRESOURCE(IDD_FILE_ACTION), hwndMain, &FileActionDialogProc, 0);
        if (!hwndDlg) {
            freeActionData();
            return;
        }
        SetTimer(hwndDlg, ID_EVENT_PRELOADER, PRELOADER_PERIOD, NULL);
        CreateThread(NULL, 0, fileActionTask, actionData, 0, NULL);
        ShowWindow(hwndDlg, SW_SHOW);
    }
}

void copyFiles(struct FileNode** nodes, int count) {
    clearClipboard();
    clipboard = createPathsFromFileNodes(nodes, count);
    // 分配失败时不能留下「计数非 0 但指针为 NULL」的状态，否则粘贴会解引用空指针
    clipboardSize = clipboard ? count : 0;
    clipboardIsCut = false;
}

void cutFiles(struct FileNode** nodes, int count) {
    clearClipboard();
    clipboard = createPathsFromFileNodes(nodes, count);
    clipboardSize = clipboard ? count : 0;
    clipboardIsCut = true;
}

void pasteFiles(wchar_t* dstDir) {
    if (clipboardSize == 0) return;
    if (!dstDir || dstDir[0] == L'\0') return;
    
    actionData = calloc(1, sizeof(struct ActionData));
    if (!actionData) return;
    
    int len = wcslen(dstDir);
    actionData->dstPath = calloc(len + 2, sizeof(wchar_t));
    if (!actionData->dstPath) {
        free(actionData);
        actionData = NULL;
        return;
    }
    wcscpy_s(actionData->dstPath, len + 2, dstDir);
    actionData->dstPath[len+0] = L'\0';
    actionData->dstPath[len+1] = L'\0';
    
    actionData->action = clipboardIsCut ? ACTION_MOVE : ACTION_COPY;
    actionData->srcPaths = clipboard;
    actionData->numSrcPaths = clipboardSize;
    
    hwndDlg = CreateDialogParam(globalHInstance, MAKEINTRESOURCE(IDD_FILE_ACTION), hwndMain, &FileActionDialogProc, 0);
    if (!hwndDlg) {
        freeActionData();
        return;
    }
    SetTimer(hwndDlg, ID_EVENT_PRELOADER, PRELOADER_PERIOD, NULL);
    CreateThread(NULL, 0, fileActionTask, actionData, 0, NULL);
    ShowWindow(hwndDlg, SW_SHOW);
}

static void createShortcut(wchar_t* srcPath, wchar_t* dstPath) {
    HRESULT hres;

    IShellLinkW* isl;
    hres = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (LPVOID*)&isl);
    if (SUCCEEDED(hres)) {
        wchar_t workingDir[MAX_PATH] = {0};
        getParentDirFromPath(srcPath, workingDir);
        
        IShellLinkW_SetPath(isl, srcPath);
        IShellLinkW_SetWorkingDirectory(isl, workingDir);
        IShellLinkW_SetDescription(isl, L"");
    
        IPersistFile* ipf;
        hres = IShellLinkW_QueryInterface(isl, &IID_IPersistFile, (void **)&ipf);

        if (SUCCEEDED(hres)) {
            IPersistFile_Save(ipf, dstPath, TRUE);
            IPersistFile_Release(ipf);
        }
        
        IShellLinkW_Release(isl);
    }
}

void pasteShortcuts(wchar_t* dstDir) {
    if (clipboardSize == 0) return;
    
    wchar_t dstPath[MAX_PATH] = {0};
    wchar_t basename[80] = {0};
    
    for (int i = 0; i < clipboardSize; i++) {
        wchar_t* srcPath = clipboard[i];
        if (wcscmp(srcPath, L".lnk") != 0) {
            getBasenameFromPath(srcPath, basename, 80, true);
            swprintf_s(dstPath, MAX_PATH, L"%ls\\%ls.lnk", dstDir, basename);
            createShortcut(srcPath, dstPath);           
        }
    }
    
    if (clipboardIsCut) clearClipboard();
    navigateRefresh();
}

void createDesktopShortcuts(struct FileNode** nodes, int count) {
    wchar_t** srcPaths = createPathsFromFileNodes(nodes, count);
    if (!srcPaths) return;

    wchar_t* desktopPath = getDesktopPath();
    wchar_t dstPath[MAX_PATH] = {0};
    wchar_t basename[80] = {0};
    
    for (int i = 0; i < count; i++) {
        wchar_t* srcPath = srcPaths[i];
        if (wcscmp(srcPath, L".lnk") != 0) {
            getBasenameFromPath(srcPath, basename, 80, true);
            swprintf_s(dstPath, MAX_PATH, L"%ls\\%ls.lnk", desktopPath, basename);
            createShortcut(srcPath, dstPath);           
        }
        free(srcPaths[i]);
    }
    
    free(srcPaths); 
}

void extractFilesFromISOImage(wchar_t* isoPath, wchar_t* dstPath) {
#ifndef USE_LIBCDIO
    MessageBoxW(NULL, lc_str.msg_no_libcdio, L"WFM", MB_OK | MB_ICONINFORMATION);
    return;
#endif

    actionData = calloc(1, sizeof(struct ActionData));
    if (!actionData) return;
    
    int len = wcslen(isoPath);
    actionData->srcPaths = calloc(1, sizeof(wchar_t*));
    if (!actionData->srcPaths) {
        free(actionData);
        actionData = NULL;
        return;
    }
    actionData->srcPaths[0] = calloc(len + 2, sizeof(wchar_t));
    if (!actionData->srcPaths[0]) {
        free(actionData->srcPaths);
        free(actionData);
        actionData = NULL;
        return;
    }
    wcscpy_s(actionData->srcPaths[0], len + 2, isoPath);
    actionData->numSrcPaths = 1;
    
    len = wcslen(dstPath);
    actionData->dstPath = calloc(len + 2, sizeof(wchar_t));
    if (!actionData->dstPath) {
        free(actionData->srcPaths[0]);
        free(actionData->srcPaths);
        free(actionData);
        actionData = NULL;
        return;
    }
    wcscpy_s(actionData->dstPath, len + 2, dstPath);
    actionData->dstPath[len+0] = L'\0';
    actionData->dstPath[len+1] = L'\0';
    
    actionData->action = ACTION_ISO_EXTRACT;
    
    hwndDlg = CreateDialogParam(globalHInstance, MAKEINTRESOURCE(IDD_FILE_ACTION), hwndMain, &FileActionDialogProc, 0);
    if (!hwndDlg) {
        freeActionData();
        return;
    }
    SetTimer(hwndDlg, ID_EVENT_PRELOADER, PRELOADER_PERIOD, NULL);
    CreateThread(NULL, 0, fileActionTask, actionData, 0, NULL);
    ShowWindow(hwndDlg, SW_SHOW);
}