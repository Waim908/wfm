#include "main.h"
#ifdef USE_LIBCDIO
#include "libcdio_loader.h"
#endif

#define ID_EVENT_PRELOADER 100
#define PRELOADER_PERIOD 120
#define ID_EVENT_SHOW 101
// 进度对话框延迟显示：删除/复制少量文件往往几百毫秒内就完成，
// 立即显示只会得到一闪而过的小窗，以及在 Winlator 上跟随而来的
// 主窗口花屏/残影（确认框关闭 → 小窗创建销毁 → 整表重绘挤在一起）。
// 操作在延迟内完成时，MSG_CLOSE 直接销毁隐藏窗口，进度窗全程不可见。
#define SHOW_DELAY_MS 300
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
static struct ActionData* actionData = NULL;
static UINT cfDropEffect = 0;   // "Preferred DropEffect" 注册格式 id 缓存

extern HINSTANCE globalHInstance;
extern HWND hwndMain;

static void animatePreloader() {
    SendDlgItemMessage(hwndDlg, IDC_PRELOADER, STM_SETICON, (WPARAM)preloaderIcons[preloaderIconIndex], 0);
    preloaderIconIndex = (preloaderIconIndex + 1) % 8;
}

// ===== 系统剪贴板（CF_HDROP）实现 =====
// 与 Windows 资源管理器同款格式：文件列表放 CF_HDROP，剪切/复制语义放
// "Preferred DropEffect" 注册格式。Wine 的剪贴板数据由 wineserver 全局
// 保存（dlls/user32/clipboard.c 的 marshal_data 对 CF_HDROP 走 default
// 分支原样转发，读取进程再 unmarshal_data 本地重建），因此跨进程可用：
// 多个 wfm 窗口之间、乃至 wfm 与资源管理器之间都能互相复制粘贴。

static UINT dropEffectFormatId() {
    if (!cfDropEffect) cfDropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return cfDropEffect;
}

static void freePathList(wchar_t** paths, int count) {
    if (!paths) return;
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

// 把文件路径列表写入系统剪贴板。paths 各条目为单 \0 结尾即可（内部按
// multi-sz 拼接）；cut 决定 DropEffect 写 DROPEFFECT_MOVE 还是 _COPY。
static bool setClipboardFiles(wchar_t** paths, int count, bool cut) {
    if (!paths || count <= 0) return false;

    // multi-sz 总长：每条含结尾 \0，末尾再补一个 \0 表示列表结束
    size_t totalChars = 1;
    for (int i = 0; i < count; i++) totalChars += wcslen(paths[i]) + 1;

    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + totalChars * sizeof(wchar_t));
    if (!hDrop) return false;

    DROPFILES* df = (DROPFILES*)GlobalLock(hDrop);
    if (!df) {
        GlobalFree(hDrop);
        return false;
    }
    ZeroMemory(df, sizeof(DROPFILES));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* out = (wchar_t*)((char*)df + df->pFiles);
    for (int i = 0; i < count; i++) {
        size_t l = wcslen(paths[i]);
        memcpy(out, paths[i], l * sizeof(wchar_t));
        out += l;
        *out++ = L'\0';
    }
    *out = L'\0';
    GlobalUnlock(hDrop);

    HGLOBAL hEffect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!hEffect) {
        GlobalFree(hDrop);
        return false;
    }
    DWORD* eff = (DWORD*)GlobalLock(hEffect);
    if (!eff) {
        GlobalFree(hEffect);
        GlobalFree(hDrop);
        return false;
    }
    *eff = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    GlobalUnlock(hEffect);

    bool ok = false;
    if (OpenClipboard(hwndMain)) {
        EmptyClipboard();
        // SetClipboardData 成功后内存归系统所有；失败才需要自己释放
        if (SetClipboardData(CF_HDROP, hDrop)) {
            if (!SetClipboardData(dropEffectFormatId(), hEffect)) GlobalFree(hEffect);
            ok = true;
        }
        else GlobalFree(hDrop);
        CloseClipboard();
    }
    else {
        GlobalFree(hDrop);
        GlobalFree(hEffect);
    }
    return ok;
}

// 从系统剪贴板读出文件路径列表。每个条目按 SHFileOperation 的要求以
// 双 \0 结尾，由调用方（或 freePathList）逐条释放；返回 NULL 表示
// 剪贴板上没有可用文件。*outCut 依据 Preferred DropEffect 判断，与
// Explorer 一致（未放 DropEffect 的来源一律视为复制）。
static wchar_t** readClipboardFiles(int* outCount, bool* outCut) {
    *outCount = 0;
    *outCut = false;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(hwndMain)) return NULL;

    wchar_t** paths = NULL;
    int count = 0;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) {
        DROPFILES* df = (DROPFILES*)GlobalLock(h);
        if (df) {
            if (df->fWide) {
                const wchar_t* s = (const wchar_t*)((const char*)df + df->pFiles);
                for (const wchar_t* p = s; *p; p += wcslen(p) + 1) count++;
                if (count > 0) {
                    paths = calloc(count, sizeof(wchar_t*));
                    if (paths) {
                        int i = 0;
                        for (const wchar_t* p = s; *p && i < count; p += wcslen(p) + 1) {
                            size_t l = wcslen(p);
                            wchar_t* copy = calloc(l + 2, sizeof(wchar_t)); // 双 \0 结尾
                            if (!copy) break;
                            memcpy(copy, p, l * sizeof(wchar_t));   // calloc 已补两个 \0
                            paths[i++] = copy;
                        }
                        count = i;
                    }
                    else count = 0;
                }
            }
            else {
                // ANSI 版本（个别老程序放入的）：转换成宽字符
                const char* s = (const char*)df + df->pFiles;
                for (const char* p = s; *p; p += strlen(p) + 1) count++;
                if (count > 0) {
                    paths = calloc(count, sizeof(wchar_t*));
                    if (paths) {
                        int i = 0;
                        for (const char* p = s; *p && i < count; p += strlen(p) + 1) {
                            int wl = MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
                            wchar_t* copy = calloc(wl + 1, sizeof(wchar_t));
                            if (!copy) break;
                            MultiByteToWideChar(CP_ACP, 0, p, -1, copy, wl);
                            copy[wl] = L'\0';   // 已写入一个 \0，再补一个成双 \0
                            paths[i++] = copy;
                        }
                        count = i;
                    }
                    else count = 0;
                }
            }
            GlobalUnlock(h);
        }
    }

    if (dropEffectFormatId()) {
        HANDLE he = GetClipboardData(dropEffectFormatId());
        if (he) {
            DWORD* eff = (DWORD*)GlobalLock(he);
            if (eff) {
                *outCut = (*eff & DROPEFFECT_MOVE) != 0;
                GlobalUnlock(he);
            }
        }
    }
    CloseClipboard();

    if (count <= 0) {
        freePathList(paths, count);
        return NULL;
    }
    *outCount = count;
    return paths;
}

bool clipboardHasItems() {
    return IsClipboardFormatAvailable(CF_HDROP);
}

int getClipboardCount() {
    int count = 0;
    bool cut;
    wchar_t** paths = readClipboardFiles(&count, &cut);
    freePathList(paths, count);
    return count;
}

bool isClipboardCut() {
    bool cut = false;
    UINT fmt = dropEffectFormatId();
    if (fmt && IsClipboardFormatAvailable(fmt) && OpenClipboard(hwndMain)) {
        HANDLE he = GetClipboardData(fmt);
        if (he) {
            DWORD* eff = (DWORD*)GlobalLock(he);
            if (eff) {
                cut = (*eff & DROPEFFECT_MOVE) != 0;
                GlobalUnlock(he);
            }
        }
        CloseClipboard();
    }
    return cut;
}

// 返回指向 static 缓冲的指针，仅适合立即使用（调用方都是取完即用）
wchar_t* getClipboardFirstPath() {
    static wchar_t first[MAX_PATH];
    first[0] = L'\0';
    int count = 0;
    bool cut;
    wchar_t** paths = readClipboardFiles(&count, &cut);
    if (paths && count > 0) wcsncpy_s(first, MAX_PATH, paths[0], _TRUNCATE);
    freePathList(paths, count);
    return paths ? first : NULL;
}

void clearClipboard() {
    // 仅当剪贴板上还是文件数据时才清空，避免误伤其他程序放入的内容
    if (IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(hwndMain)) {
        EmptyClipboard();
        CloseClipboard();
    }
    onClipboardChanged();
}

static void freeActionData() {
    if (!actionData) return;

    // 各操作的 srcPaths 都是独立分配的（粘贴时从系统剪贴板读出，
    // 删除/ISO 提取时由 createPathsFromFileNodes 生成），统一释放
    if (actionData->srcPaths) {
        for (int i = 0; i < actionData->numSrcPaths; i++) {
            free(actionData->srcPaths[i]);
        }
        free(actionData->srcPaths);
    }

    // 移动完成后按 Explorer 语义清空剪贴板（剪切+粘贴是一次性操作）
    if (actionData->action == ACTION_MOVE) clearClipboard();

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

            // 不在创建后立即 ShowWindow，由定时器延迟弹出（见 SHOW_DELAY_MS）
            SetTimer(hwndDlg, ID_EVENT_SHOW, SHOW_DELAY_MS, NULL);
            return (INT_PTR)TRUE;
        }
        case WM_TIMER: {
            if (wParam == ID_EVENT_SHOW) {
                KillTimer(hwndDlg, ID_EVENT_SHOW);
                ShowWindow(hwndDlg, SW_SHOW);
            }
            else if (wParam == ID_EVENT_PRELOADER) animatePreloader();
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
            // 强制同步整窗重绘：确认框/进度窗关闭留下的残影不能指望
            // 系统后续的激活重绘来清除（Winlator 上表现为短暂花屏）
            RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
    }
}

void copyFiles(struct FileNode** nodes, int count) {
    wchar_t** paths = createPathsFromFileNodes(nodes, count);
    if (!paths) return;
    setClipboardFiles(paths, count, false);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    onClipboardChanged();
}

void cutFiles(struct FileNode** nodes, int count) {
    wchar_t** paths = createPathsFromFileNodes(nodes, count);
    if (!paths) return;
    setClipboardFiles(paths, count, true);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    onClipboardChanged();
}

void pasteFiles(wchar_t* dstDir) {
    int count = 0;
    bool isCut = false;
    wchar_t** paths = readClipboardFiles(&count, &isCut);
    if (!paths) return;
    if (!dstDir || dstDir[0] == L'\0') {
        freePathList(paths, count);
        return;
    }

    actionData = calloc(1, sizeof(struct ActionData));
    if (!actionData) {
        freePathList(paths, count);
        return;
    }

    int len = wcslen(dstDir);
    actionData->dstPath = calloc(len + 2, sizeof(wchar_t));
    if (!actionData->dstPath) {
        free(actionData);
        actionData = NULL;
        freePathList(paths, count);
        return;
    }
    wcscpy_s(actionData->dstPath, len + 2, dstDir);
    actionData->dstPath[len+0] = L'\0';
    actionData->dstPath[len+1] = L'\0';

    actionData->action = isCut ? ACTION_MOVE : ACTION_COPY;
    actionData->srcPaths = paths;   // 所有权移交，freeActionData 统一释放
    actionData->numSrcPaths = count;

    hwndDlg = CreateDialogParam(globalHInstance, MAKEINTRESOURCE(IDD_FILE_ACTION), hwndMain, &FileActionDialogProc, 0);
    if (!hwndDlg) {
        freeActionData();
        return;
    }
    SetTimer(hwndDlg, ID_EVENT_PRELOADER, PRELOADER_PERIOD, NULL);
    CreateThread(NULL, 0, fileActionTask, actionData, 0, NULL);
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
    int count = 0;
    bool isCut = false;
    wchar_t** paths = readClipboardFiles(&count, &isCut);
    if (!paths) return;

    wchar_t dstPath[MAX_PATH] = {0};
    wchar_t basename[80] = {0};

    for (int i = 0; i < count; i++) {
        wchar_t* srcPath = paths[i];
        if (wcscmp(srcPath, L".lnk") != 0) {
            getBasenameFromPath(srcPath, basename, 80, true);
            swprintf_s(dstPath, MAX_PATH, L"%ls\\%ls.lnk", dstDir, basename);
            createShortcut(srcPath, dstPath);
        }
    }
    freePathList(paths, count);

    if (isCut) clearClipboard();
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
}