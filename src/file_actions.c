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
    MSG_NAVIGATE_REFRESH,
    // 粘贴遇到同名项：worker 线程用 SendMessage 请求 UI 线程弹模态框并等结果。
    // 模态框必须在 UI 线程建（worker 线程建窗体会和主消息循环抢消息），而
    // SendMessage 是同步的，参数直接指向 worker 栈上的结构体即可。
    MSG_CONFLICT_PROMPT
};

enum FileAction {
    ACTION_NONE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_MOVE,
    ACTION_ISO_EXTRACT
};

// 同名冲突的处理策略。ASK 表示还没问过；用户勾选「对全部冲突项使用相同操作」
// 后，本批次后续冲突直接套用已选策略，不再询问。
enum ConflictChoice {
    CONFLICT_ASK = 0,
    CONFLICT_REPLACE,
    CONFLICT_SKIP,
    CONFLICT_KEEP_BOTH,
    CONFLICT_CANCEL
};

// 传给冲突对话框的上下文。text 由 worker 线程填好，choice/applyToAll 由
// 对话框写回（SendMessage 返回时即已生效）。
struct ConflictPrompt {
    wchar_t text[512];
    bool allowApplyAll;
    // 源与目标其实是同一个文件时置起：「替换」在这里等于拿自己覆盖自己，
    // 不会产生任何变化，把按钮置灰免得给出一个什么都不做的选项。
    bool hideReplace;
    enum ConflictChoice choice;
    bool applyToAll;
};

struct ActionData {
    enum FileAction action;
    wchar_t** srcPaths;
    int numSrcPaths;
    wchar_t* dstPath;
    bool cancel;
    // 收尾汇总用（仅复制/移动会累加）
    int succeededCount;
    int skippedCount;
    int failedCount;
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

// 有界求长：最多扫 limit 个字符，返回值等于 limit 说明该段没有 \0 结尾
// （畸形数据），调用方据此停止解析，避免越过剪贴板分配块读内存
static size_t boundedWLen(const wchar_t* s, size_t limit) {
    size_t l = 0;
    while (l < limit && s[l]) l++;
    return l;
}

static size_t boundedLen(const char* s, size_t limit) {
    size_t l = 0;
    while (l < limit && s[l]) l++;
    return l;
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
        SIZE_T blockBytes = df ? GlobalSize(h) : 0;
        if (df && blockBytes > df->pFiles) {
            // 路径串区域的容量上限（字节）；解析一律限制在此范围内
            SIZE_T payload = blockBytes - df->pFiles;
            if (df->fWide) {
                const wchar_t* s = (const wchar_t*)((const char*)df + df->pFiles);
                size_t maxChars = payload / sizeof(wchar_t);
                const wchar_t* end = s + maxChars;
                for (const wchar_t* p = s; p < end && *p; ) {
                    size_t l = boundedWLen(p, end - p);
                    if (l == (size_t)(end - p)) break;  // 没有 \0 结尾，数据不完整
                    count++;
                    p += l + 1;
                }
                if (count > 0) {
                    paths = calloc(count, sizeof(wchar_t*));
                    if (paths) {
                        int i = 0;
                        for (const wchar_t* p = s; *p && i < count; ) {
                            size_t l = boundedWLen(p, end - p);
                            if (l == (size_t)(end - p)) break;
                            wchar_t* copy = calloc(l + 2, sizeof(wchar_t)); // 双 \0 结尾
                            if (!copy) break;
                            memcpy(copy, p, l * sizeof(wchar_t));   // calloc 已补两个 \0
                            paths[i++] = copy;
                            p += l + 1;
                        }
                        count = i;
                    }
                    else count = 0;
                }
            }
            else {
                // ANSI 版本（个别老程序放入的）：转换成宽字符
                const char* s = (const char*)df + df->pFiles;
                const char* end = s + payload;
                for (const char* p = s; p < end && *p; ) {
                    size_t l = boundedLen(p, end - p);
                    if (l == (size_t)(end - p)) break;
                    count++;
                    p += l + 1;
                }
                if (count > 0) {
                    paths = calloc(count, sizeof(wchar_t*));
                    if (paths) {
                        int i = 0;
                        for (const char* p = s; *p && i < count; ) {
                            size_t l = boundedLen(p, end - p);
                            if (l == (size_t)(end - p)) break;
                            int wl = MultiByteToWideChar(CP_ACP, 0, p, (int)l, NULL, 0);
                            // 转换结果最多 wl 个宽字符（不含 \0），分配 wl+1 补成双 \0
                            wchar_t* copy = calloc(wl + 2, sizeof(wchar_t));
                            if (!copy || wl == 0) {
                                free(copy);
                                break;
                            }
                            int got = MultiByteToWideChar(CP_ACP, 0, p, (int)l, copy, wl);
                            if (got <= 0) {
                                free(copy);
                                break;
                            }
                            copy[got] = L'\0';      // 首个 \0
                            copy[got + 1] = L'\0';  // 补成双 \0
                            paths[i++] = copy;
                            p += l + 1;
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

    // 移动完成后按 Explorer 语义清空剪贴板（剪切+粘贴是一次性操作）；
    // 用户取消时不清，保留剪切内容以便重试
    if (actionData->action == ACTION_MOVE && !actionData->cancel) clearClipboard();

    if (actionData->dstPath) {
        free(actionData->dstPath);
    }
    free(actionData);
    actionData = NULL;
}

// ===== 同名冲突处理（资源管理器语义） =====
// 逐条检查目标是否已有同名项，有冲突就弹 IDD_CONFLICT 让用户选
// 替换 / 跳过 / 保留两者，并可勾选「对全部冲突项使用相同操作」。
//
// 不用 SHFileOperation 自带的确认是有原因的（wine/dlls/shell32/shlfileop.c）：
// 它只是「是/否/全是」消息框，且「全是」仅在单批源文件多于一个时才出现
// （:1190 的 SHELL_ConfirmDialogW(..., op->bManyItems)），更关键的是选「否」
// 会 return DE_OPCANCELLED，被 copy_move_files() 的循环 break 掉（:1223）——
// 那会中止整批剩余文件，而不是跳过当前这一个。FOF_RENAMEONCOLLISION 在
// Wine 里更是 FIXME 后直接忽略（check_flags，:1382），「保留两者」指望不上系统。

// 冲突对话框与调用方之间的唯一通道。worker 线程 SendMessage 时把结构体
// 挂在上面，UI 线程在对话框的 WM_COMMAND 里写回选择。同一时刻只可能有一个
// 文件操作在跑（actionData 本身就是单例），所以单槽位足够。
static struct ConflictPrompt* activeConflictPrompt = NULL;

static INT_PTR CALLBACK ConflictDialogProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);

    switch (msg) {
        case WM_INITDIALOG: {
            RECT rect, rect1;
            GetWindowRect(hwndMain, &rect);
            GetClientRect(hwndDlg, &rect1);
            SetWindowPos(hwndDlg, NULL,
                (rect.right + rect.left) / 2 - (rect1.right - rect1.left) / 2,
                (rect.bottom + rect.top) / 2 - (rect1.bottom - rect1.top) / 2,
                0, 0, SWP_NOZORDER | SWP_NOSIZE);

            // 先按「取消」记账：用右上角 X 关窗不会走 WM_COMMAND，
            // 靠这个初值兜底，语义与点「取消」一致
            if (activeConflictPrompt) activeConflictPrompt->choice = CONFLICT_CANCEL;

            SetWindowText(hwndDlg, lc_str.conflict_title);
            SetWindowText(GetDlgItem(hwndDlg, IDC_CONFLICT_TEXT),
                          activeConflictPrompt ? activeConflictPrompt->text : L"");
            SetWindowText(GetDlgItem(hwndDlg, IDC_BTN_REPLACE), lc_str.conflict_replace);
            SetWindowText(GetDlgItem(hwndDlg, IDC_BTN_SKIP), lc_str.conflict_skip);
            SetWindowText(GetDlgItem(hwndDlg, IDC_BTN_KEEP_BOTH), lc_str.conflict_keep_both);
            SetWindowText(GetDlgItem(hwndDlg, IDC_CHECK_APPLY_ALL), lc_str.conflict_apply_all);
            SetWindowText(GetDlgItem(hwndDlg, IDCANCEL), lc_str.cancel);

            // 只剩最后一个条目时「对全部」没有意义，隐藏（资源管理器同样不显示）
            if (activeConflictPrompt && !activeConflictPrompt->allowApplyAll) {
                ShowWindow(GetDlgItem(hwndDlg, IDC_CHECK_APPLY_ALL), SW_HIDE);
            }
            // 源就是目标：「替换」无从下手（覆盖自己＝不变），置灰并把焦点移到
            // 「保留两者」——这里真正有意义的选择是它。默认按钮本来就是替换，
            // 停在灰按钮上会让回车变成空操作。
            HWND hwndReplace = GetDlgItem(hwndDlg, IDC_BTN_REPLACE);
            HWND hwndFocus = hwndReplace;
            if (activeConflictPrompt && activeConflictPrompt->hideReplace) {
                EnableWindow(hwndReplace, FALSE);
                hwndFocus = GetDlgItem(hwndDlg, IDC_BTN_KEEP_BOTH);
                // 默认按钮也要跟着挪，否则回车落在置灰的「替换」上等于没反应。
                // DefDlgProc 会顺带把 BS_DEFPUSHBUTTON 样式换过去（wine/dlls/
                // user32/defdlg.c:117 的 DEFDLG_SetDefId），外观一并正确。
                SendMessage(hwndDlg, DM_SETDEFID, IDC_BTN_KEEP_BOTH, 0);
            }
            SetFocus(hwndFocus);
            return (INT_PTR)FALSE;   // 焦点已自行设置，不必让系统再选默认按钮
        }
        case WM_COMMAND: {
            if (!activeConflictPrompt) break;

            enum ConflictChoice choice = CONFLICT_ASK;
            switch (LOWORD(wParam)) {
                case IDC_BTN_REPLACE:   choice = CONFLICT_REPLACE;   break;
                case IDC_BTN_SKIP:      choice = CONFLICT_SKIP;      break;
                case IDC_BTN_KEEP_BOTH: choice = CONFLICT_KEEP_BOTH; break;
                case IDCANCEL:          choice = CONFLICT_CANCEL;    break;
                default: break;
            }
            if (choice == CONFLICT_ASK) break;

            activeConflictPrompt->choice = choice;
            activeConflictPrompt->applyToAll =
                IsDlgButtonChecked(hwndDlg, IDC_CHECK_APPLY_ALL) == BST_CHECKED;
            EndDialog(hwndDlg, LOWORD(wParam));
            break;
        }
        case WM_CLOSE:
            EndDialog(hwndDlg, IDCANCEL);
            break;
    }
    return (INT_PTR)FALSE;
}

// 让 UI 线程弹冲突对话框并取回结果。对话框建不出来时退化为「跳过这一项」，
// 而不是把整批操作取消掉。
static void promptConflict(HWND hwndDlg, const wchar_t* name, bool isFolder,
                           bool allowApplyAll, bool hideReplace, struct ConflictPrompt* out) {
    memset(out, 0, sizeof(*out));
    swprintfTrunc(out->text, 512,
                  isFolder ? lc_str.conflict_existing_folder : lc_str.conflict_existing_file,
                  name);
    out->allowApplyAll = allowApplyAll;
    out->hideReplace = hideReplace;
    out->choice = CONFLICT_SKIP;

    activeConflictPrompt = out;
    SendMessage(hwndDlg, MSG_CONFLICT_PROMPT, 0, 0);
    activeConflictPrompt = NULL;
}

static bool isDirectoryPath(const wchar_t* path) {
    DWORD attrs = GetFileAttributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 把文件名拆成主干和扩展名（扩展名含点号）。以点号开头的隐藏文件（.gitignore）
// 整个当主干；没有扩展名时 ext 是空串。
static void splitNameExt(const wchar_t* name, wchar_t* stem, int stemCch,
                         wchar_t* ext, int extCch) {
    if (!name || !stem || !ext || stemCch <= 0 || extCch <= 0) return;

    wcsncpy_s(stem, (size_t)stemCch, name, _TRUNCATE);
    ext[0] = L'\0';

    wchar_t* lastDot = wcsrchr(stem, L'.');
    if (lastDot && lastDot != stem) {
        wcsncpy_s(ext, (size_t)extCch, lastDot, _TRUNCATE);
        *lastDot = L'\0';
    }
}

// 候选名可用则写进 out 并返回 true。两个条件：拼出来放得下双 \0（pTo 是按
// 双 \0 结尾的多字符串解析的），且当前不存在。名字长到被截断时，拼出的路径
// 一定顶满 outCch，会被长度检查挡下——绝不退化成「写进截断路径」。
static bool tryAcceptCandidate(wchar_t* dstDir, wchar_t* candidate,
                               wchar_t* out, int outCch) {
    joinPaths(dstDir, candidate, out, outCch);
    size_t len = wcslen(out);
    if (len == 0 || len + 1 >= (size_t)outCch) return false;
    if (isPathExists(out)) return false;
    out[len + 1] = L'\0';
    return true;
}

// 生成「保留两者」的目标路径：name.ext -> "name (1).ext"、"name (2).ext" …
// 编号从 1 起，与资源管理器一致（它把首份副本叫 image (1).png）。
static bool makeUniquePath(wchar_t* dstDir, const wchar_t* name, wchar_t* out, int outCch) {
    if (!dstDir || !name || !out || outCch <= 0) return false;

    wchar_t stem[MAX_PATH] = {0};
    wchar_t ext[MAX_PATH] = {0};
    splitNameExt(name, stem, MAX_PATH, ext, MAX_PATH);

    for (int n = 1; n < 10000; n++) {
        wchar_t candidate[MAX_PATH] = {0};
        swprintfTrunc(candidate, MAX_PATH, L"%ls (%d)%ls", stem, n, ext);
        if (tryAcceptCandidate(dstDir, candidate, out, outCch)) return true;
    }
    return false;
}

// 同目录留副本的目标路径："name - 副本.ext"，再冲突则 "name - 副本 (2).ext" …
// 这是资源管理器往自己所在目录粘贴时的命名（英文 … - Copy.ext），和对话框里
// 「保留两者」用的 "name (1).ext" 不是同一套，故单独一个函数。
static bool makeCopyPath(wchar_t* dstDir, const wchar_t* name, wchar_t* out, int outCch) {
    if (!dstDir || !name || !out || outCch <= 0) return false;
    if (!lc_str.copy_suffix || lc_str.copy_suffix[0] == L'\0') {
        return makeUniquePath(dstDir, name, out, outCch);
    }

    wchar_t stem[MAX_PATH] = {0};
    wchar_t ext[MAX_PATH] = {0};
    splitNameExt(name, stem, MAX_PATH, ext, MAX_PATH);

    for (int n = 1; n < 10000; n++) {
        wchar_t candidate[MAX_PATH] = {0};
        if (n == 1) {
            swprintfTrunc(candidate, MAX_PATH, L"%ls - %ls%ls", stem, lc_str.copy_suffix, ext);
        }
        else {
            swprintfTrunc(candidate, MAX_PATH, L"%ls - %ls (%d)%ls", stem, lc_str.copy_suffix, n, ext);
        }
        if (tryAcceptCandidate(dstDir, candidate, out, outCch)) return true;
    }
    return false;
}

// 复制/移动单个条目，含同名冲突处理。返回 false 表示这一条没做成
// （跳过或失败，已分别记入计数器）；用户中止整批时置 actionData->cancel。
static bool copyMoveOneItem(struct ActionData* ad, int index, HWND hwndDlg,
                            enum ConflictChoice* applyAll) {
    const wchar_t* srcPath = ad->srcPaths[index];

    // 默认目标就是目标目录本身；「保留两者」时换成显式的新文件名（见下）。
    // target 全零初始化，再显式补上第二个 \0 —— SHFileOperation 的 pTo
    // 要求双 \0 结尾的多字符串，缺了它真 Windows 会读越界。
    wchar_t target[MAX_PATH] = {0};
    int dstLen = (int)wcslen(ad->dstPath);
    if (dstLen <= 0 || dstLen + 1 >= MAX_PATH) {
        ad->failedCount++;
        return false;
    }
    wcsncpy_s(target, MAX_PATH, ad->dstPath, _TRUNCATE);
    target[dstLen + 1] = L'\0';

    DWORD extraFlags = 0;

    // 取不出名字的情况（剪贴板里是盘根 "C:\" 这类）不做冲突检测：
    // 整条交给 SHFileOperation 按原样处理，别让冲突层把原本能做的事挡掉
    wchar_t name[MAX_PATH] = {0};
    getBasenameFromPath(srcPath, name, MAX_PATH, false);

    wchar_t existing[MAX_PATH] = {0};
    if (name[0] != L'\0') joinPaths(ad->dstPath, name, existing, MAX_PATH);

    if (name[0] != L'\0' && isPathExists(existing)) {
        // 源与目标其实是同一个文件（往自己所在目录里粘贴）。这种情况下 SHFileOperation
        // 自己也会以 DE_SAMEFILE 直接失败（wine/dlls/shell32/shlfileop.c:1186 用
        // wcscmp 比对后返回），所以得在这里判掉：
        //   移动 → 本来就没有任何可做的事，也不会丢数据，按资源管理器静默跳过；
        //   复制 → 和普通同名冲突一样问一次，只是「保留两者」改用资源管理器的
        //          「名字 - 副本.ext」命名（见 makeCopyPath）。
        bool sameItem = (wcsicmp(existing, srcPath) == 0);

        if (sameItem && ad->action == ACTION_MOVE) {
            ad->skippedCount++;
            return false;
        }

        enum ConflictChoice choice = *applyAll;
        if (choice == CONFLICT_ASK) {
            struct ConflictPrompt prompt;
            promptConflict(hwndDlg, name, isDirectoryPath(existing),
                           index + 1 < ad->numSrcPaths, sameItem, &prompt);
            choice = prompt.choice;
            if (prompt.applyToAll) *applyAll = choice;
        }

        if (choice == CONFLICT_CANCEL) {
            ad->cancel = true;
            return false;
        }
        if (choice == CONFLICT_SKIP) {
            ad->skippedCount++;
            return false;
        }
        if (choice == CONFLICT_KEEP_BOTH) {
            bool ok = sameItem ? makeCopyPath(ad->dstPath, name, target, MAX_PATH)
                               : makeUniquePath(ad->dstPath, name, target, MAX_PATH);
            if (!ok) {
                ad->failedCount++;
                return false;
            }
            // 走 FOF_MULTIDESTFILES：此时 pTo 是「目标文件名」而不是目标目录，
            // 这正是该标志的文档语义（pTo 与 pFrom 一一对应），真 Windows 同样成立
            extraFlags = FOF_MULTIDESTFILES;
        }
        else {
            // CONFLICT_REPLACE：target 保持目标目录，覆盖交给 SHFileOperation
            // （文件走 CopyFileW/MoveFileEx+REPLACE_EXISTING，文件夹则是合并内容，
            //   与资源管理器的「替换」一致）
            if (sameItem) {
                // 源就是目标，覆盖自己等于什么都不做（SHFileOperation 也会以
                // DE_SAMEFILE 失败）。对话框里这个按钮已经置灰，这里是兜底：
                // 记成「跳过」而不是「失败」，别把一个空操作报成错误。
                ad->skippedCount++;
                return false;
            }
        }
    }

    SHFILEOPSTRUCT sfo = {0};
    sfo.hwnd = hwndDlg;
    sfo.wFunc = ad->action == ACTION_COPY ? FO_COPY : FO_MOVE;
    sfo.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | extraFlags;
    sfo.pTo = target;
    sfo.pFrom = ad->srcPaths[index];

    if (SHFileOperation(&sfo) != 0) {
        // 单条失败不再中止整批（资源管理器也是做完剩下的再报告）
        ad->failedCount++;
        return false;
    }
    ad->succeededCount++;
    return true;
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
            // 汇总数值要在 freeActionData 之前取出来
            int succeeded = actionData ? actionData->succeededCount : 0;
            int skipped = actionData ? actionData->skippedCount : 0;
            int failed = actionData ? actionData->failedCount : 0;
            bool cancelled = actionData ? actionData->cancel : false;

            freeActionData();
            DestroyWindow(hwndDlg);
            hwndDlg = NULL;
            navigateRefresh();
            // 强制同步整窗重绘：确认框/进度窗关闭留下的残影不能指望
            // 系统后续的激活重绘来清除（Winlator 上表现为短暂花屏）
            RedrawWindow(hwndMain, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);

            // 有跳过/失败才提示（顺利完成不打扰，与资源管理器一致）。
            // 放在进度窗销毁之后弹，避免两层窗挤在同一时刻创建销毁（Winlator 花屏）
            if (!cancelled && (skipped > 0 || failed > 0)) {
                wchar_t msg[256] = {0};
                swprintfTrunc(msg, 256, lc_str.msg_file_op_summary, succeeded, skipped, failed);
                MessageBox(hwndMain, msg, lc_str.conflict_title, MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        case MSG_CONFLICT_PROMPT: {
            // 由 worker 线程请求：冲突可能发生在进度窗延迟显示之前，
            // 先让它现身，冲突框才有稳定的归属窗口
            KillTimer(hwndDlg, ID_EVENT_SHOW);
            ShowWindow(hwndDlg, SW_SHOW);
            DialogBoxParam(globalHInstance, MAKEINTRESOURCE(IDD_CONFLICT), hwndDlg,
                           &ConflictDialogProc, 0);
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
        // 「对全部冲突项使用相同操作」在本批次内的记忆；CONFLICT_ASK 表示逐条询问
        enum ConflictChoice applyAll = CONFLICT_ASK;

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
                copyMoveOneItem(actionData, i, hwndDlg, &applyAll);
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
        if (!hasFileExtension(srcPath, L"lnk")) {
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
        if (!hasFileExtension(srcPath, L"lnk")) {
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