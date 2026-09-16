#include "main.h"
#include <commoncontrols.h>   // IImageList：读 shell 的共享镜像列表用
#include <wctype.h>           // towlower：图标去重键统一转小写

#define COLUMN_NAME_IDX 0
#define COLUMN_TYPE_IDX 1
#define COLUMN_SIZE_IDX 2
#define COLUMN_DATE_IDX 3
#define COLUMN_PATH_IDX 4


// 当前挂在内容区上的图像列表。正常情况下就是自建图标库里的那一份；只有自建列表
// 建不起来（或刚被释放）时才会是 shell 的系统列表。
static HIMAGELIST currentImageList = NULL;

// 图标渲染的底层实现位于文件末尾的 PE / ICO 解码区，自建图标库要用，先声明。
//
// 渲染流水线以**像素缓冲**为中间态（BYTE* = 32bpp straight-alpha，top-down），
// 而不是 HICON：HICON 每过一趟都要付 GetIconInfo 的位图深拷贝 + CreateIconIndirect
// 的再复制，一个图标累积起来是 MB 级的搬运。只有最后上屏前才建一次 HICON。
static HICON createShellIconBest(int sysIcon, int desired, int* outNativeSize);
static BYTE* extractIcoIconPixels(const wchar_t* icoPath, int desired, int* outSize);
static BYTE* extractPeIconPixels(const wchar_t* pePath, int groupIndex, int desired, int* outSize);
static HICON extractIconFromPeIndexed(const wchar_t* pePath, int groupIndex, int cxDesired, int cyDesired);
static BYTE* scalePixelsOwned(BYTE* pixels, int srcSize, int dstSize);
static HICON createIconFromPixels(const BYTE* pixels, int width, int height);
static BYTE* getIconPixelsSquare(HICON hIcon, int* outSize);
static bool stampShortcutOverlay(BYTE* dst, int outSize);
static void initGdiplus(void);                  // 后台解码 PNG 帧要用；起线程前必须已在主线程调过
static void iconQueueRender(int id, int size);  // 排一条来源给后台渲染（主线程）
static void iconRenderPump(void);               // 接管后台渲好的图标（主线程）
static void iconRenderInvalidate(void);         // 清缓存：作废在途任务与已完成结果


// 图标补齐 / 预取（实现见文件末尾同名一节）。绘制路径发现有待补的图标时调它，
// 因此滚动、键盘翻页、改窗口大小都自动触发，不需要单独接滚动消息。
//
// **不再做「整屏一起出现」了**。曾经的做法是：可见区里只要还剩一项没渲染好，就把整屏
// 图标一律画成空位，等本屏补齐后再整体重画一次。那个做法在**一屏项数多**的视图里是灾难：
// 大图标一屏二十来项尚可，详细视图一屏五六十到上百项，而补齐每轮只有 8ms 预算、滚动一到
// 就被打断 —— 于是「只要还有几项没补上，整屏连文件夹图标都没有」。现在的语义是
// **有就画、缺就补、补到就把缺过的区间重画一次**。
static bool iconFillPosted;       // 同一时刻只挂一条补齐消息，见 scheduleIconFill
static void scheduleIconFill(void);
static bool iconFillStep(void);   // 补一轮可见区图标；返回 true = 还没补完
static void iconFillVisibleSync(int budgetMs);   // 在绘制之前同步补本屏
static void prewarmSharedIcons(void);            // 目录加载时把「共享来源」一次性渲染进池
static void requestIconFillForVisible(void);     // 绘制入口：可见区有缺图标就安排补齐
static void scheduleIconPrefetch(void);
static bool iconPrefetchStep(void);   // 预取可见区两侧的邻域；返回 true = 还有可预取的
static int visibleIconSpan(void);     // 一屏可见项数；池容量按它定（见 getIconPool）
static void visibleItemRange(int* outFirst, int* outLast);   // 一屏对应的项范围（唯一权威）
static bool preScrollHandleVScroll(UINT code);   // 滚动前先渲好目标窗口（见文件末尾同名一节）
static void preScrollCancelRetry(void);          // 撤销「押后期间重试」的定时器

// 本屏连续多少轮毫无进展（剩下的项这几轮渲染不出来）。到 ICON_FILL_STALL_ROUNDS 就
// **停止自续投递**省 CPU —— 不标记任何项、也不影响绘制，下一次绘制/滚动会重新安排补齐。
static int iconFillStall;
// 上一次「因为补进了新图标而重画缺图标区间」的时刻。重画限流用：补齐每轮都重画会吃掉
// 大半个 8ms 预算，反而让补齐更慢。
static DWORD lastIconRepaintTick;
// 上一次真正「把补齐排上队」（挂定时器或投消息）的时刻。给调度闸做看门狗用，
// 见 scheduleIconFill 开头。
static DWORD iconFillDispatchTick;

// 绘制路径上报的「这里缺图标」见证（见 getIconSlotForPaint / iconFillStep）。
// 哨兵：iconMissLast < iconMissFirst 表示本趟还没人上报。
static bool iconPaintMissed;   // 屏幕上确实存在「本来画得出来、现在却还是空格子」的位置
static int iconMissFirst;
static int iconMissLast = -1;

// 滚动期「就地渲染」的额度，每次 WM_PAINT 重开口子（见 getIconSlotForPaint）：
// 只有滚动驱动的绘制才允许在绘制路径里渲染，额度按本屏跨度给 —— 要够把本屏渲完。
static int iconPaintRenderBudget;
static DWORD iconPaintRenderDeadline;

// 「正在处理滚动消息」。滚动消息内部会同步重画（listview.c 的 scroll_list → UpdateWindow），
// 那次绘制会重入进本窗口过程；绘制路径据此认出「这一帧是滚动驱动的」。比「距 lastScrollTick
// 多少毫秒」准：异步凑上来的绘制不会被误判成滚动驱动。
static bool inScrollMessage;

// 补齐循环的重入闸：iconFillIds 是静态去重缓冲，重入会让两层互相覆盖（外层那轮就会漏渲/渲错来源）。
static bool inIconFillRange;

// 「先渲好目标窗口、再让控件滚」的押后状态（见文件末尾的 preScrollHandleVScroll）：
static int pendingScrollTop = -1;    // 已经准备好、但还没落地的目标顶项（-1 = 没有押后）
static DWORD pendingScrollSince;     // 本次押后的起点（超过 ICON_SCROLL_HOLD_MAX_MS 就放弃押后）
static bool iconScrollTimerPending;  // 已经挂了一个「押后期间重试」的定时器
static bool inPreScroll;             // 我们自己在把滚动交给控件（别再拦，否则会自锁）
static bool preScrollFromTimer;      // 这次调用来自「押后期间的重试」，不是真实的拖动消息（别当快拖）

// 滚动静默：滚动类消息（拖滚动条 / 滚轮 / 翻页键 / 改窗口大小）刚发生过的时间戳。
// 补齐必须据此让位 —— 见 scheduleIconFill 里为什么这事关「拖动滚动条卡住」。
static DWORD lastScrollTick;
static bool iconFillTimerPending;      // 已经挂了一个「等滚动静默」的定时器
static bool iconPrefetchTimerPending;  // 已经挂了一个预取定时器
static int iconPrefetchDoneSize;       // 该图标尺寸的池已预取收工（0 = 允许预取）
// 不用 1 这种小号：Wine 的 listview 内部也用 SetTimer（滚动条按住时的连续滚动，
// 见 comctl32/listview.c:4079，它的 id 是 (UINT_PTR)infoPtr），号段留开更省心。
#define TIMER_ICON_FILL            0x7FFF
#define TIMER_ICON_PREFETCH        0x7FFE
#define TIMER_ICON_SCROLL          0x7FFD
#define ICON_FILL_SCROLL_QUIET_MS  180   // 滚动停多久之后才继续补图标
#define ICON_FILL_BUDGET_MS        8     // 一趟补齐/预取最多占用多少毫秒
#define ICON_PREFETCH_INTERVAL_MS  40    // 预取两趟之间歇多久（走定时器，最低优先级）
#define ICON_SYNC_BUDGET_MS        150   // 导航/换尺寸时**同步**补首屏的时间预算
#define ICON_SCROLL_SYNC_BUDGET_MS 8     // 离散滚动一步之后同步补新露出区域的预算
#define ICON_PREWARM_BUDGET_MS     80    // 目录加载时预热「共享来源」的时间预算
#define ICON_PREWARM_MAX           64    // 一次预热最多新增多少共享槽（另受「最多占半个池」约束）
#define ICON_PAINT_RENDER_MIN      8     // 一次绘制「就地渲染」的额度下限
#define ICON_PAINT_RENDER_CAP      16    // 额度上限：它直接等于「拖动时每帧多卡多久」，不能放开（见 WM_PAINT）
#define ICON_PAINT_RENDER_MS       25    // 一次绘制的就地渲染累计时间上限
#define ICON_SCROLL_PAINT_WINDOW_MS 60   // 绘制距最近一次滚动消息多久之内算「滚动驱动的绘制」
#define ICON_FILL_STALL_ROUNDS     4     // 本屏连续这么多轮毫无进展就停止自续投递，见 iconFillRange
#define ICON_REPAINT_MIN_MS        40    // 两趟「补进新图标后重画」之间至少隔这么久
#define ICON_RETRY_BACKOFF_ROUNDS  3     // 某项渲染/登记失败后退避几轮再试（见 itemIconReady）
#define ICON_SCROLL_PREPARE_MS     8     // 一次「滚动前预渲染」的时间片（见 preScrollHandleVScroll）
#define ICON_SCROLL_HOLD_MAX_MS    60    // 拖动时最多把内容押后多久；超过就照滚（宁可闪，不能粘住）
#define ICON_SCROLL_FAST_DRAG_MS   50    // 相邻两条 WM_VSCROLL 间隔小于它＝用户在快拖：跟手优先，预渲染让路
#define ICON_SCROLL_HOLD_MAX_MISS  12    // 目标窗口缺口超过这么多就不押后（渲不完，押着只会白扣滞后）
#define ICON_SCROLL_FINAL_MS       80    // 松手 / 翻页这类一次性滚动的预渲染预算
#define ICON_SCROLL_RETRY_MS       15    // 押后期间的重试间隔（走 WM_TIMER，优先级最低）

// ---- 后台图标渲染（见文件末尾「后台图标渲染」一节）----
// 图标解码的成本大头是**文件 I/O 与 PE 加载**（LoadLibraryExW 映射 + 枚举 RT_GROUP_ICON），
// 不是像素多少。留在 UI 线程上就只能靠「一帧渲几条」的额度去摊 —— 一帧要渲的独占来源一多，
// 超出的格子那一帧必然空（「滚动时随机几个图标消失又立马显示」）。搬到独立线程后：绘制帧
// 只查池、永不等待解码；解码在后台连续跑、吞吐随核数上升；渲好的经完成队列回主线程入池。
#define ICON_RENDER_QUEUE_MAX      192   // 待渲染请求上限（满了这一轮不排，下一轮再来）
#define ICON_RENDER_DONE_MAX       64    // 待接管结果上限（满了渲染线程会等，**不丢结果**）
#define ICON_RENDER_IDLE_MS        30    // 没活干时的等待时长（有活会被 SetEvent 立刻唤醒）


// IID_IImageList 不在 mingw 的 libuuid 里，按 wine include/commoncontrols.idl
// 的 uuid 本地定义
static const IID wfm_IID_IImageList =
    { 0x46eb5926, 0x582e, 0x4017, { 0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50 } };

// 状态栏节流：搜索期间每批（100 项）都刷新一次状态栏没有意义，限制到约 5 次/秒。
// 最终值由 MSG_SEARCH_DONE 里的 updateStatusbar() 保证正确。
static DWORD lastStatusbarTick = 0;



// 添加扩展名图标缓存


enum Msg {
    MSG_ADD_ITEMS_BATCH = WM_APP,
    MSG_SEARCH_DONE,
    MSG_NAVIGATE_TO_PATH,
    MSG_ICON_FILL,
    // 后台渲染线程交回一个图标（见末尾「后台图标渲染」一节）。用 PostMessage 而不是让渲染
    // 线程直接碰池：ImageList 只能在创建它的线程（也就是 UI 线程）上操作。
    MSG_ICON_RENDER_DONE,
    // 后台计数线程交回一个目录的条目数（见「后台文件夹条目数计数」一节）。同样只能 PostMessage：
    // 结果要写回 FileNode 并让 ListView 重画，两者都只有 UI 线程能做。
    MSG_FOLDER_COUNT_DONE
};

enum ContextMenuType {
    MENU_SINGLE,
    MENU_MULTIPLE,
    MENU_EMPTY
};

struct ListItem {
    int icon;
    struct FileNode* node;
    wchar_t type[64];
    wchar_t formattedSize[64];
    wchar_t formattedDate[32];
    bool loaded;
    bool isHidden;
    // 渲染 / 登记的退避计数（见 itemIconReady）：
    //   >0 = 这一项最近失败过（来源表暂时满、这一刻挑不出可淘汰的槽、shell 碰巧没成），
    //        接下来这么多轮不再尝试渲染，每轮递减。
    //   ==0 = 正常状态，可以尝试。
    // **只用来节流「要不要去补」**，绝不参与「这一格画不画」：池里有槽就一定画得出来。
    // 曾经这里还有一个 iconGaveUp，被绘制路径当成「直接画空」的判据 —— 而它只在滚动消息里
    // 被清零，表现出来就是「滚动时随机几个图标先消失、紧接着又出现」。已彻底删除。
    unsigned char iconTries;
    uint64_t size;
    wchar_t* path;
    FILETIME modifiedTime;
    uint64_t driveTotalBytes;
    uint64_t driveFreeBytes;
    // 这一格有没有一条**在途**的「文件夹条目数」计数请求（0 = 没有）。
    // 它同时是结果回主线程时的认领凭据：只有 items[idx].countRequestId 与后台交回来的
    // requestId 相等，才说明这一格还是当初发出去的那一格（见「后台文件夹条目数计数」一节）。
    unsigned countRequestId;
};

// 搜索线程持有的只读节点池。
// 搜索线程绝不调用 buildChildNodes（那会 free 掉 UI 线程正在使用的节点），
// 而是用 FindFirstFile 在本地建立一棵只属于自己的树，退出时统一释放。
struct SearchNodePool {
    struct FileNode** nodes;
    int count;
    int capacity;
};

struct SearchData {
    wchar_t keyword[64];  // 拥有自己的副本，避免悬空指针
    wchar_t rootPath[MAX_PATH];  // 搜索起点路径，创建线程前由 UI 线程抓取
    volatile bool active;
    volatile bool canceled;
    HANDLE threadHandle;  // 线程句柄，用于等待线程退出
    struct SearchNodePool* pool;      // 线程建立的只读节点池
    struct FileNode** results;        // 结果数组（MSG_SEARCH_DONE 时移交 UI 线程）
    int resultCount;
};

struct SearchCache {
    wchar_t path[MAX_PATH];
    wchar_t keyword[64];
    bool showHidden;               // 缓存生成时的隐藏文件开关：切换开关后缓存必须失效
    struct FileNode** results;
    int count;
    struct SearchNodePool* pool;   // results 指向的节点由该池拥有
    time_t timestamp;
};

static struct SearchCache searchCache = {0};

// 搜索进行中用户又发起了新搜索：先取消当前搜索，关键词暂存在这里，
// 等 MSG_SEARCH_DONE 清理完 searchData 后自动重新发起（模式同 pendingNavigatePath）
static wchar_t pendingSearchKeyword[64] = {0};

// ===== 搜索节点池 =====
// 池中记录搜索线程分配过的每一个节点，释放时不依赖链表结构，
// 避免某条链忘记登记造成泄漏。
static void freeSearchPool(struct SearchNodePool* pool) {
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) {
        struct FileNode* node = pool->nodes[i];
        if (!node) continue;
        free(node->name);
        free(node);
    }
    free(pool->nodes);
    free(pool);
}

static struct FileNode* allocSearchNode(struct SearchNodePool* pool, const wchar_t* name, enum FileType type) {
    if (!pool) return NULL;
    struct FileNode* node = calloc(1, sizeof(struct FileNode));
    if (!node) return NULL;
    node->name = wcsdup(name);
    if (!node->name) { free(node); return NULL; }
    node->type = type;
    // calloc 出来的 0 会被当成「目录里有 0 项」，必须显式改成「未知」
    node->childItemCount = CHILD_ITEM_COUNT_UNKNOWN;

    if (pool->count >= pool->capacity) {
        int newCap = pool->capacity ? pool->capacity * 2 : 64;
        struct FileNode** tmp = realloc(pool->nodes, newCap * sizeof(struct FileNode*));
        if (!tmp) { free(node->name); free(node); return NULL; }
        pool->nodes = tmp;
        pool->capacity = newCap;
    }
    pool->nodes[pool->count++] = node;
    return node;
}

struct BatchItems {
    struct FileNode** nodes;
    int count;
    int capacity;
};

struct ContextMenuItem {
    wchar_t* text;
    void(*proc)();
    wchar_t* cmdData;
    bool disabled;
};

#ifdef USE_LIBCDIO
static void onMenuItemLoadISOImageClick();
void onMenuItemUnloadISOImageClick();
#endif
static void onMenuItemShowIconClick();
static void onMenuItemOpenFileLocationClick();
static void onMenuItemOpenLinkTargetClick();
static void onMenuItemOpenWithClick();
static void onMenuItemClearClipboardClick();
static bool isInSearchMode();
// 解析 lnk 的目标路径（实现与 lnk 图标提取放在一起，见文件末尾）
static bool resolveLnkTargetPath(const wchar_t* lnkPath, wchar_t* targetPath, int targetCch);
// 解析 lnk 自带的图标位置（ICON_LOCATION，没有则退到目标文件本身）
static bool resolveLnkIconLocation(const wchar_t* lnkPath, wchar_t* iconPath, int iconPathCch, int* iconIndex);

static struct ContextMenuItem cmiOpen = {NULL, &onMenuItemOpenClick, NULL, false};
static struct ContextMenuItem cmiEdit = {NULL, &onMenuItemEditClick, NULL, false};
static struct ContextMenuItem cmiCut = {NULL, &onMenuItemCutClick, NULL, false};
static struct ContextMenuItem cmiCopy = {NULL, &onMenuItemCopyClick, NULL, false};
static struct ContextMenuItem cmiCreateShortcut = {NULL, &onMenuItemCreateShortcutClick, NULL, false};
static struct ContextMenuItem cmiDelete = {NULL, &onMenuItemDeleteClick, NULL, false};
static struct ContextMenuItem cmiRename = {NULL, &onMenuItemRenameClick, NULL, false};
static struct ContextMenuItem cmiPaste = {NULL, &onMenuItemPasteClick, NULL, false};
static struct ContextMenuItem cmiPasteShortcut = {NULL, &onMenuItemPasteShortcutClick, NULL, false};
static struct ContextMenuItem cmiClearClipboard = {NULL, &onMenuItemClearClipboardClick, NULL, false};
static struct ContextMenuItem cmiNewFolder = {NULL, &onMenuItemNewFolderClick, NULL, false};
static struct ContextMenuItem cmiNewFile = {NULL, &onMenuItemNewFileClick, NULL, false};
#ifdef USE_LIBCDIO
static struct ContextMenuItem cmiLoadISOImage = {NULL, &onMenuItemLoadISOImageClick, NULL, false};
static struct ContextMenuItem cmiUnloadISOImage = {NULL, &onMenuItemUnloadISOImageClick, NULL, false};
#endif
static struct ContextMenuItem cmiShowIcon = {NULL, &onMenuItemShowIconClick, NULL, false};
static struct ContextMenuItem cmiOpenWith = {NULL, &onMenuItemOpenWithClick, NULL, false};
static struct ContextMenuItem cmiOpenFileLocation = {NULL, &onMenuItemOpenFileLocationClick, NULL, false};
static struct ContextMenuItem cmiOpenLinkTarget = {NULL, &onMenuItemOpenLinkTargetClick, NULL, false};
static void onMenuItemImportRegClick();
static struct ContextMenuItem cmiImportReg = {NULL, &onMenuItemImportRegClick, NULL, false};

static WNDPROC OrigWndProc;
static struct ListItem* items = NULL;
static int numItems = 0;
static int itemsCapacity = 0;
static enum ViewStyle viewStyle = STYLE_DETAILS;
static HMENU hContextMenu;
static char sortColumnIdx = COLUMN_NAME_IDX;
static bool sortAscending = true;
// 文件夹位置策略的当前值。默认「经典」= 原版 WFM 行为：分组随升降序翻转
// （升序文件夹在前、降序在后），与注册表缺失该项时的默认值保持一致
// （见 loadFolderSortMode）。
static enum FolderSortMode folderSortMode = FOLDER_SORT_CLASSIC;

// ---- 大图标视图设置 ----
// 格子布局参数。TOP_PAD/MARGIN_X 对齐 Wine listview 自身的标签留白常量
// (ICON_TOP_PADDING=4、TRAILING_LABEL_PADDING 等)：即使自绘不生效、由 Wine
// 兜底绘制，BOTTOM_PAD 也必须覆盖它「图标高 + 6px 上留白 + 文字高 + 1px」
// 的空间需求，否则它会把最后一行换成省略号。
#define ICONVIEW_MARGIN_X     8
#define ICONVIEW_TOP_PAD      4
#define ICONVIEW_ICON_GAP     4
#define ICONVIEW_BOTTOM_PAD   8

// 图标尺寸：32 用系统大图像列表；更大尺寸自建缩放图像列表（Wine 没有
// 超大系统图标，只能从 32px 源图标缩放，尺寸越大越模糊）。
static int iconViewIconSize = 32;
// 文件名行数：0 = 无限（格子高度按当前目录里最长的文件名自动测算），
// 1..5 = 固定行数。格子高度通过 LVM_SETICONSPACING 控制，行数 N 对应
// 高度 = 图标高 + 间距 + N×行高。
static int iconViewLabelLines = 0;
// DrawTextW 的实际行进距离（默认不含外部行距）。updateIconViewLayout
// 测出后存下来，绘制端用它夹住文字区底边，保证 N 行就是 N 行。
static int iconViewLineHeight = 16;

// 详细信息视图中驱动器"大小"列的磁盘占用显示模式（见 enum DriveBarMode）
static int driveBarMode = DRIVE_BAR_GRAPH;

// 驱动器"大小"列的文字：按显示模式生成
static void formatDriveSizeText(struct ListItem* item) {
    switch (driveBarMode) {
        case DRIVE_BAR_TOTAL:
            formatFileSize(item->driveTotalBytes, item->formattedSize);
            break;
        case DRIVE_BAR_NONE:
            item->formattedSize[0] = L'\0';
            break;
        case DRIVE_BAR_GRAPH:
        default:
            formatDriveSpace(item->driveTotalBytes, item->driveFreeBytes, item->formattedSize, 64);
            break;
    }
}

// ========== 自建图标库 ==========
//
// 【为什么不拿 shell 的系统镜像列表（SIC）当图标容器】
// SIC 的缓存键是「图标来源文件 + 资源序号」。每缓存一个新的来源文件，它就往**全部 5 个**
// 共享镜像列表各追加一帧（32/16/48/16/256）—— 那帧 256×256 单独就是 256 KB，实测每个
// exe/lnk 约 367 KB。它是进程级、**没有任何回收 API**（SIC_Destroy 未导出），于是
// 「逛一遍 exe 多的目录内存涨几十 MB、点『清除图标缓存』也降不下来」是必然的。
//
// 【改法：shell 只当「查图标坐标」的目录服务】
//   SHGetFileInfoW(路径, 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION)
// 走的是 IExtractIconW::GetIconLocation（shell32_main.c 的对应分支），**完全不碰 SIC**；
// 拿到「图标文件 + 资源序号」后用 PrivateExtractIconsW 自己按尺寸提取。
// 实测（宿主 wine 11.17）：该路径下 5 个共享列表计数零增长，且提取出的 32px 图标与
// shell 自己渲染的图标**像素完全一致**。
//
// 【三层结构】
//   ① 图标来源表 iconSources[]：id → 「图标文件 + 序号 / shell 索引 / .ico 路径」。
//      共享项（目录、驱动器、按扩展名的普通文件）永不淘汰；独占项（exe/lnk/.ico）
//      按路径各占一项 —— 每项只是几十字节的字符串，和 SIC 的 367 KB/项不在一个量级。
//      表只增不删，这保证 item->icon 这个 id 永远有效，重绘时不必回查路径。
//   ② 显示列表池：按显示尺寸各一份 ImageList，槽位用 LRU 原地替换，容量按字节封顶。
//      位图（每张 size²×4）才是有可能吃掉几十 MB 的东西，这里给它设死上限。
//   ③ item->icon = ①的 id（0 = 尚未解析）。重绘时经 ② 换成当前尺寸的槽位索引。
//
// 槽位数取「字节预算 / 每张字节」，再按**当前一屏可见项数 × 2** 抬一次，最后用
// ICON_SLOT_MAX_BYTES 封顶。
// 为什么必须显式按一屏抬：字节预算与可见项数虽然同阶（都 ∝ 1/size²），比例常数却
// 差得多 —— 128px 下预算只算得出 32 槽（被下限抬到 64），而 1080p 大图标视图一屏
// 就有 70~80 项（span 还按 (cols+1)×(rows+2) 算了溢出余量）。池装不下一屏时，
// 「渲染一个、顶掉一个」是数学必然：本屏永远凑不齐 → 认输阀 → 随机格子缺图标。
// ×2 的含义是「一屏在池里 + 一屏留给预取跟随」，这也是「在屏图标不可能被顶掉」的前提。
//
// 之所以只要「槽数 ≥ 一屏可见项数」就够：Wine 每帧重绘都会为**每个可见项**重新回调
// LVN_GETDISPINFO 取 iImage（listview.c 的 LISTVIEW_DrawItem → LISTVIEW_GetItemW），
// 被回收的槽位下次绘制会自然重新申请；此时 LRU 选中的受害者必然是已滚出视野的那个。
//
// 预算取 2 MB（原 8 MB）：手机上大图标视图一帧就 64 KB（128px），128 个槽 = 8 MB，
// 「看 128 个不同的 exe 就能摸到顶」——这是用户实际感知到的主要增长源。降到 2 MB 后
// 16px 详细视图仍能放 2k 张（够几屏滚动 + 来回浏览）。
// 硬顶 48 MB 是「一屏 ×2」这条正确性的代价上限：4K + 128px 大图标下 span×2 能到
// 700 余槽（≈46 MB），到这里就不再涨；普通 1080p 大图标约 160 槽 = 10 MB。
#define ICON_SLOT_BUDGET_BYTES (2u * 1024u * 1024u)
#define ICON_SLOT_MIN_COUNT    64
#define ICON_SLOT_MAX_BYTES    (48u * 1024u * 1024u)
#define ICON_SOURCE_INIT       8192    // 来源表初始容量
#define ICON_SOURCE_MAX        65536   // 来源表扩容上限（去重键可寻址的独占项上限）
#define ICON_KEYMAP_INIT       16384   // 2 的幂；= 2 × ICON_SOURCE_INIT，半装载率
#define ICON_POOL_MAX          8
#define ICON_SHELL_UNKNOWN     (-1000)  // shellIndex 的「还没查过」哨兵（0 是合法的列表索引）

// 来源类型
enum IconSrcKind {
    ICONSRC_FILE  = 0,  // (iconFile, iconIndex) → PrivateExtractIconsW
    ICONSRC_SHELL = 1,  // shell 系统镜像列表索引（只读，不追加，不增长 SIC）
    ICONSRC_ICO   = 2   // iconFile 是 .ico，走自带的 ICO 解码器（PrivateExtractIconsW 不认 .ico）
};

// 来源标志
enum IconSrcFlags {
    ICONSRC_SHARED  = 1u << 0,  // 共享项：槽位不参与 LRU 淘汰
    ICONSRC_OVERLAY = 1u << 1   // .lnk：渲染后要合成快捷方式角标
};

struct IconSource {
    wchar_t* key;        // 去重键（已转小写）：独占=完整路径，共享=扩展名或 \x01..\x04 前缀的固定标识
    wchar_t* iconFile;   // ICONSRC_FILE / ICONSRC_ICO 的图标文件
    wchar_t* typeName;   // 顺带缓存的类型名（loadItemData 用，独占项也填）
    int iconIndex;       // ICONSRC_FILE 的资源序号；ICONSRC_SHELL 时是列表索引
    int shellIndex;      // 兜底用的 shell 系统列表索引（ICON_SHELL_UNKNOWN = 还没查过，见 renderShellFallback）
    unsigned kind;
    unsigned flags;
    // 这条来源已排给后台渲染、结果还没回来。**只由主线程读写**：渲染线程拿的是入队时
    // 复制进任务结构的快照，所以它与 iconStoreGrow 的 realloc 之间没有任何竞态。
    volatile LONG renderPending;
};

static struct IconSource* iconSources = NULL;
static int iconSourceCount = 0;
static int iconSourceCap = ICON_SOURCE_INIT;   // iconSources 已分配容量（可扩到 ICON_SOURCE_MAX）
static int* iconKeyMap = NULL;       // 键哈希 → id（0 = 空槽）
static int iconKeyMapSize = ICON_KEYMAP_INIT;  // 去重索引槽数：必须始终 ≥ 2 × 来源数
static int iconKeyMapMask = ICON_KEYMAP_INIT - 1;

struct IconPool {
    int size;                 // 边长
    HIMAGELIST himl;
    int* slotId;              // 槽 → 图标 id（-1 = 空闲）
    unsigned* slotTick;       // 槽的最后使用序号（LRU 判据）
    int slotCount;            // 容量上限（按字节预算反推）
    int filled;               // 已分配到的槽数（AddIcon 的行进指针）
    unsigned tick;
    int* idSlot;              // 图标 id → 槽（-1 = 不在池内）
    int idSlotCap;
};

static struct IconPool iconPools[ICON_POOL_MAX];

// setViewStyle 会自己调用 updateIconViewLayout —— 这样即便 refreshContentView 因
// 「搜索进行中」而提前返回，切换视图也仍会重算格子尺寸。refreshContentView 靠这个
// 标志跳过它自己那次布局，免得同一次切换把「遍历全表做 GDI 文本测量 + Arrange +
// 全表失效」原样做两遍（大图标视图下这是 N 次 DrawTextW(DT_CALCRECT)）。
static bool skipLayoutInRefresh = false;

static struct FileNode** selectedItems = NULL;
static int numSelectedItems = 0;

static struct ContextMenuItem* menuItems = NULL;
static int numMenuItems = 0;

static struct SearchData* searchData;

// 延迟导航缓冲区（避免菜单回调深调用链导致栈溢出）
static wchar_t pendingNavigatePath[MAX_PATH] = {0};
static wchar_t pendingSelectName[MAX_PATH] = {0};

// Icon viewer dialog globals
#define MAX_ICON_GROUPS 16
#define MAX_ICONS_PER_GROUP 16
static HWND hwndIconViewer = NULL;
static int iconGroupCount = 0;
static int currentGroupIndex = 0;
static int currentIconIndex = 0;
static wchar_t iconViewerTitle[MAX_PATH] = {0};
static wchar_t iconViewerFileName[MAX_PATH] = {0};

struct IconGroup {
    HICON icons[MAX_ICONS_PER_GROUP];
    int sizes[MAX_ICONS_PER_GROUP];
    int iconCount;
};
static struct IconGroup iconGroups[MAX_ICON_GROUPS];
static void cleanupIconGroups(void);

extern struct FileNode* currPathFileNode;
extern HINSTANCE globalHInstance;
extern HWND hwndMain;
extern HMENU hMenuView;
extern HMENU hMenuFolderSort;
extern HMENU hMenuIconView;
extern HMENU hMenuIconSize;
extern HMENU hMenuLines;
extern HMENU hMenuDriveBar;

HWND hwndContentView = NULL;

static void fillFileInfo(struct FileNode* node, struct ListItem* item) {
    // 直接使用已保存的文件属性，无需再次调用 API
    item->size = node->size;
    item->isHidden = node->isHidden;
    memcpy(&item->modifiedTime, &node->modifiedTime, sizeof(FILETIME));
}

// ---- 自建图标库：来源表 + 显示列表池 ----

// 显示尺寸：大图标视图用设置里的尺寸，其余视图（详细/列表/小图标）都是 16。
static int currentIconDisplaySize(void) {
    return (viewStyle == STYLE_LARGE_ICON) ? iconViewIconSize : 16;
}

// 键的哈希。**必须忽略大小写**：登记时存进去的是 lowerDup() 的结果、槽位也是按它的
// 哈希算的，而查找时传进来的是原始大小写的路径/扩展名（Windows 路径几乎总含大写）。
// 若这里按原样计算，查找就会从**另一个槽位**开始线性探测、撞到空槽就判「未登记」——
// 于是每次解析（每次导航都会重建 items）都给同一个文件新增一条重复来源，来源表被同一
// 批文件反复填满；撞到 ICON_SOURCE_MAX 之后，新出现的文件就再也登记不进去 → 那个文件
// 永远没有图标。表现为「小概率某个文件压根不显示图标」，且用得越久越容易遇到。
static unsigned iconKeyHash(const wchar_t* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned)(towlower((wint_t)*s) & 0xFFFF); h *= 16777619u; }
    return h;
}

// 小写副本。路径与扩展名在 Windows 上都是大小写不敏感的，统一小写后
// 「同一个图标的两个键」不会因为大小写差异各占一项。
static wchar_t* lowerDup(const wchar_t* s) {
    if (!s) return NULL;
    size_t n = wcslen(s);
    wchar_t* p = malloc((n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    for (size_t i = 0; i <= n; i++) p[i] = (wchar_t)towlower((wint_t)s[i]);
    return p;
}

// 按去重键查 id（0 = 未登记）。开放寻址线性探测；表只增不删，所以不必处理墓碑。
static int iconKeyLookup(const wchar_t* key) {
    if (!iconKeyMap || !key || !key[0]) return 0;
    unsigned i = iconKeyHash(key) & (unsigned)iconKeyMapMask;
    for (int probe = 0; probe < iconKeyMapSize; probe++) {
        int id = iconKeyMap[i];
        if (id == 0) return 0;
        if (iconSources && wcsicmp(iconSources[id - 1].key, key) == 0) return id;
        i = (i + 1) & (unsigned)iconKeyMapMask;
    }
    return 0;
}

// 扩容来源表，连带把去重索引一起翻倍并重哈希。
//
// 为什么必须有：来源表是**进程级**的，每浏览到一个新的 exe/lnk/ico 就按完整路径登记
// 一条独占项，而且没有任何回收路径（池只回收槽位，不回收来源）。表满之后
// iconSourceAdd 返回 0 → 新出现的文件再也登记不进去 → 那个文件没有图标。这正是
// 「文件一多就随机缺图标」的另一半成因（另一半是认输阀写永久标记，见 iconFillRange）。
//
// 不做「按引用计数回收独占项」的原因：一条来源的 id 同时被 item->icon 与池的 idSlot
// 引用，收回它就要同时作废这两处，而开放寻址的哈希表又不支持删除（会打断探测链）。
// 收益（极限容量）远小于引入新 bug 的风险 —— 扩容到 ICON_SOURCE_MAX 就够了。
static bool iconStoreGrow(void) {
    if (iconSourceCap >= ICON_SOURCE_MAX) return false;

    int newCap = iconSourceCap * 2;
    if (newCap > ICON_SOURCE_MAX) newCap = ICON_SOURCE_MAX;

    // 去重索引必须始终 ≥ 2 × 来源数：探测是开放寻址，索引一旦填满，插入时的
    // `while (iconKeyMap[i] != 0) i = (i + 1) & mask;` 会变成**死循环**。
    int newMapSize = iconKeyMapSize * 2;
    int* newMap = calloc((size_t)newMapSize, sizeof(int));
    if (!newMap) return false;

    struct IconSource* grown = realloc(iconSources, (size_t)newCap * sizeof(struct IconSource));
    if (!grown) { free(newMap); return false; }

    iconSources = grown;
    iconSourceCap = newCap;

    // 重哈希：键的哈希与表大小无关，按新掩码重新落位即可（id 保持不变，
    // 所以 items[].icon 与池里的 idSlot 都不会失效）。
    for (int id = 1; id <= iconSourceCount; id++) {
        unsigned i = iconKeyHash(iconSources[id - 1].key) & (unsigned)(newMapSize - 1);
        while (newMap[i] != 0) i = (i + 1) & (unsigned)(newMapSize - 1);
        newMap[i] = id;
    }
    free(iconKeyMap);
    iconKeyMap = newMap;
    iconKeyMapSize = newMapSize;
    iconKeyMapMask = newMapSize - 1;
    return true;
}

// 登记一条来源，返回 id（>0）。iconFile 按原样保存（不能转小写：Wine 侧
// 解析路径时可能大小写敏感），只有去重键统一小写。
// 返回 0 = 登记失败（键无效 / 表已到 ICON_SOURCE_MAX / 分配失败）——调用方必须把它
// 当作**瞬态**失败处理：表满之后清一次图标缓存就能恢复，绝不能标成永久无图标。
static int iconSourceAdd(const wchar_t* key, unsigned kind, unsigned flags,
                         const wchar_t* iconFile, int iconIndex,
                         const wchar_t* typeName) {
    if (!key || !key[0]) return 0;
    if (iconSourceCount >= iconSourceCap && !iconStoreGrow()) return 0;
    if (!iconKeyMap) {
        iconKeyMap = calloc((size_t)iconKeyMapSize, sizeof(int));
        if (!iconKeyMap) return 0;
    }
    if (!iconSources) {
        iconSources = calloc((size_t)iconSourceCap, sizeof(struct IconSource));
        if (!iconSources) return 0;
    }

    struct IconSource* s = &iconSources[iconSourceCount];
    memset(s, 0, sizeof(*s));
    s->key = lowerDup(key);
    if (!s->key) return 0;
    if (iconFile && iconFile[0]) {
        s->iconFile = wcsdup(iconFile);
        if (!s->iconFile) { free(s->key); s->key = NULL; return 0; }
    }
    if (typeName && typeName[0]) s->typeName = wcsdup(typeName);
    s->iconIndex = iconIndex;
    s->shellIndex = ICON_SHELL_UNKNOWN;
    s->kind = kind;
    s->flags = flags;

    int id = ++iconSourceCount;
    unsigned i = iconKeyHash(s->key) & (unsigned)iconKeyMapMask;
    while (iconKeyMap[i] != 0) i = (i + 1) & (unsigned)iconKeyMapMask;
    iconKeyMap[i] = id;
    return id;
}

static int iconIdTypeNameValid(int id) {
    return id > 0 && id <= iconSourceCount;
}

static const wchar_t* iconIdTypeName(int id) {
    if (!iconIdTypeNameValid(id)) return NULL;
    return iconSources[id - 1].typeName;
}

// 取（必要时创建）指定尺寸的显示列表池
static struct IconPool* getIconPool(int size) {
    if (size <= 0) return NULL;
    for (int i = 0; i < ICON_POOL_MAX; i++)
        if (iconPools[i].himl && iconPools[i].size == size) return &iconPools[i];

    struct IconPool* p = NULL;
    for (int i = 0; i < ICON_POOL_MAX; i++)
        if (!iconPools[i].himl) { p = &iconPools[i]; break; }
    if (!p) return NULL;

    // 槽数先算出来 —— grow 要按它缩放（见下）
    unsigned frameBytes = (unsigned)size * (unsigned)size * 4u;
    int slots = (int)(ICON_SLOT_BUDGET_BYTES / frameBytes);
    if (slots < ICON_SLOT_MIN_COUNT) slots = ICON_SLOT_MIN_COUNT;

    // 再按「一屏跨度 × 2」抬一次（见 ICON_SLOT_* 处的说明）。控件还没建好时
    // visibleIconSpan() 返回 0，那就只按字节预算算 —— 之后导航重建池时会按真实跨度算。
    int span = visibleIconSpan();
    if (span > 0 && slots < span * 2) slots = span * 2;

    // 硬顶：槽数上界。它是「在屏图标不可能被顶掉」这条正确性的代价上限，
    // 超大窗口 + 大图标下 span×2 可能比字节预算大一个数量级。
    int maxSlots = (int)(ICON_SLOT_MAX_BYTES / frameBytes);
    if (maxSlots < ICON_SLOT_MIN_COUNT) maxSlots = ICON_SLOT_MIN_COUNT;
    if (slots > maxSlots) slots = maxSlots;

    // grow 的语义（Wine comctl32/imagelist.c 的 IMAGELIST_InternalExpandBitmaps）：
    // 新建时 cMaxImage = cInitial + 1，位图**立刻**按这个容量分配；之后每次扩容
    // nNewCount = cMaxImage + max(nImageCount, cGrow) + 1，且是重建整块位图再 BitBlt
    // 拷贝旧内容。逐个 AddIcon 时 nImageCount 恒为 1，所以扩容步长就等于 cGrow ——
    // 它同时决定「重建次数」和「容量过冲」：grow=16 容下 1000 张要重建约 57 次；
    // grow=128 则在第 34 张图标时一次冲到 162 张（128px 下比实际用量多占 7 MB）。
    // 取「槽数/4，夹在 8..32」：大池（16px 下 2k 槽）用 32 摊薄重建次数，过冲 ≤ 33 帧
    // 只有 33 KB；小池（128px 下 64 槽）用 16，过冲 ≤ 16 帧 = 1 MB —— 池本身越小，
    // 越不能让它被固定步长的过冲放大。
    // cInitial 取 16：位图一建列表就按容量分配，128px 下起步从 2.1 MB 降到 1.1 MB。
    int grow = slots / 4;
    if (grow < 8) grow = 8;
    if (grow > 32) grow = 32;

    HIMAGELIST himl = ImageList_Create(size, size, ILC_COLOR32, 16, grow);
    if (!himl) return NULL;

    p->slotId = malloc((size_t)slots * sizeof(int));
    p->slotTick = calloc((size_t)slots, sizeof(unsigned));
    if (!p->slotId || !p->slotTick) {
        // 池建不起来就别留半成品：宁可退回「不画图标」，也不要一个没有上限的列表
        free(p->slotId); p->slotId = NULL;
        free(p->slotTick); p->slotTick = NULL;
        ImageList_Destroy(himl);
        return NULL;
    }
    for (int i = 0; i < slots; i++) p->slotId[i] = -1;
    p->size = size;
    p->himl = himl;
    p->slotCount = slots;
    p->filled = 0;
    p->tick = 0;
    p->idSlot = NULL;
    p->idSlotCap = 0;
    return p;
}

// 当前视图该用哪个池
static struct IconPool* currentIconPool(void) {
    return getIconPool(currentIconDisplaySize());
}

// 释放除 keep 之外的所有池。必须在 ListView 已改挂 keep->himl（或系统列表）之后
// 调用，否则会释放控件仍持有的列表句柄。
static void pruneIconPools(struct IconPool* keep) {
    for (int i = 0; i < ICON_POOL_MAX; i++) {
        struct IconPool* p = &iconPools[i];
        if (!p->himl || p == keep) continue;
        ImageList_Destroy(p->himl);
        free(p->slotId);
        free(p->slotTick);
        free(p->idSlot);
        memset(p, 0, sizeof(*p));
    }
}

// 释放全部自建列表与来源表。调用方必须保证此刻控件已改挂系统列表。
static void resetIconStore(void) {
    pruneIconPools(NULL);
    if (iconSources) {
        for (int i = 0; i < iconSourceCount; i++) {
            free(iconSources[i].key);
            free(iconSources[i].iconFile);
            free(iconSources[i].typeName);
        }
        free(iconSources);
        iconSources = NULL;
    }
    iconSourceCount = 0;
    iconSourceCap = ICON_SOURCE_INIT;
    free(iconKeyMap);
    iconKeyMap = NULL;
    iconKeyMapSize = ICON_KEYMAP_INIT;
    iconKeyMapMask = ICON_KEYMAP_INIT - 1;

    // 池都没了，补齐/预取的状态一并作废
    iconPrefetchDoneSize = 0;
    iconPaintMissed = false;
    iconMissFirst = 0;
    iconMissLast = -1;
    iconFillStall = 0;
    lastIconRepaintTick = 0;
    iconFillDispatchTick = 0;

    // 押后滚动也一并作废（连同它的重试定时器）：池都没了，「目标窗口已就位」这个判据
    // 已经没有意义，留着只会让下一次拖动一上来就撞上押后上限。
    pendingScrollTop = -1;
    if (iconScrollTimerPending) {
        if (hwndContentView) KillTimer(hwndContentView, TIMER_ICON_SCROLL);
        iconScrollTimerPending = false;
    }
}

// 主路径渲染失败时的最后手段：按**来源文件**查一次 shell 的系统列表索引并缓存。
//
// 为什么必须有它：注册表里的关联可能指向一个本 prefix 里并不存在的文件
// （实测 Wine 里 .html → C:\Program Files (x86)\Internet Explorer\iexplore.exe，
// 该文件不存在）——ICONLOCATION 会「成功」地给出这个坐标，但 PrivateExtractIconsW
// 提不出任何东西。没有这条退路的话，这类文件会画成空白，而旧实现（直接取
// SYSICONINDEX）是能画出通用图标的。
//
// 只在失败路径上发生一次（结果缓存在 s->shellIndex），所以正常浏览时它一次都不会
// 走到，也就不会让 SIC 随「浏览过的文件数」增长。
static BYTE* renderShellFallbackPixels(struct IconSource* s, int desired, int* outSize) {
    if (s->shellIndex == ICON_SHELL_UNKNOWN) {
        s->shellIndex = -1;
        if (s->iconFile && s->iconFile[0]) {
            struct FileInfo fi = {0};
            getFileInfo(s->iconFile, TYPE_FILE, false, &fi);
            s->shellIndex = fi.icon;
        }
    }
    if (s->shellIndex < 0) return NULL;

    int nativeSize = 0;
    HICON h = createShellIconBest(s->shellIndex, desired, &nativeSize);
    if (!h) return NULL;
    BYTE* pixels = getIconPixelsSquare(h, outSize);
    DestroyIcon(h);
    return pixels;
}

// 按尺寸渲染一条来源。返回的 HICON 由调用方 DestroyIcon()；失败返回 NULL。
//
// 全流程以像素缓冲为中间态：解码出**原生帧**像素 → 一次性缩到目标尺寸 → 只建一次
// HICON。旧实现是「解码 → 建 HICON → getIconPixels 把它拆回像素 → 缩放 → 再建
// HICON」，中间那两个 HICON 里外各复制一遍位图（Wine 的 GetIconInfo 是
// NtUserGetIconInfo → copy_bitmap×2，每次都现场深拷贝），一个 128px 图标就要搬
// 约 1 MB 像素、约 30 次 win32u/GDI 调用 —— 那是「点进去慢一下」的主因。
static HICON renderIconSource(int id, int size) {
    if (!iconIdTypeNameValid(id) || size <= 0) return NULL;
    struct IconSource* s = &iconSources[id - 1];

    // shell 来源只有一条路可走（系统列表只吐 HICON），单独处理：原生尺寸正好等于
    // 目标且不需要叠角标时直接返回，省掉全部像素往返。
    if (s->kind == ICONSRC_SHELL) {
        int nativeSize = 0;
        HICON h = createShellIconBest(s->iconIndex, size, &nativeSize);
        if (!h) return NULL;
        if (nativeSize == size && !(s->flags & ICONSRC_OVERLAY)) return h;
        int pxSize = 0;
        BYTE* px = getIconPixelsSquare(h, &pxSize);
        DestroyIcon(h);
        if (!px) return NULL;
        BYTE* scaled = scalePixelsOwned(px, pxSize, size);
        if (!scaled) return NULL;
        if (s->flags & ICONSRC_OVERLAY) stampShortcutOverlay(scaled, size);
        HICON out = createIconFromPixels(scaled, size, size);
        free(scaled);
        return out;
    }

    int srcSize = 0;
    BYTE* src = NULL;
    if (s->kind == ICONSRC_ICO) {
        src = extractIcoIconPixels(s->iconFile, size, &srcSize);
    }
    else if (s->iconFile && s->iconFile[0]) {
        // 自带 PE 解码器，**不能**用 PrivateExtractIconsW 取首帧：
        // 后者内部的择帧规则是「不大于目标里最大」（user32/cursoricon.c 的
        // CURSORICON_FindBestIcon），选中后再由 create_icon_frame 拉伸到目标尺寸。
        // 于是「目标 128、文件里只有 16/32/48/256」时会选中 48 再放大 2.67 倍
        // —— 实测这就是大图标视图发糊的原因。自带解码器用的是 isBetterIconEntry
        // 的「不小于目标里最小，都不够大才取最大」，会直接挑中 256 原生帧，再由
        // 调用方平滑缩到目标尺寸（缩永远比放大清楚）。
        // 序号语义与 shell 一致：负数 = 资源 ID，目录/驱动器的图标就是 shell32.dll
        // 的负 ID（已在 extractPeIconPixels 里按 ID 解析）。
        src = extractPeIconPixels(s->iconFile, s->iconIndex, size, &srcSize);
        // 图标组序号越界（注册表里的坐标常带一个文件里并不存在的序号）：退回主图标
        if (!src && s->iconIndex != 0)
            src = extractPeIconPixels(s->iconFile, 0, size, &srcSize);
    }

    // 上面全落空：退回 shell 系统列表（只读，见 renderShellFallbackPixels 的说明）
    if (!src) src = renderShellFallbackPixels(s, size, &srcSize);
    if (!src) return NULL;

    // 原生尺寸就是目标尺寸时 scalePixelsOwned 直接把缓冲交回（省一次整幅 memcpy）
    BYTE* scaled = scalePixelsOwned(src, srcSize, size);
    if (!scaled) return NULL;

    // 角标叠在最终尺寸上；合成失败不影响主图标，只是少个箭头
    if (s->flags & ICONSRC_OVERLAY) stampShortcutOverlay(scaled, size);

    HICON h = createIconFromPixels(scaled, size, size);
    free(scaled);
    return h;
}

// 池内查已有槽位（**不渲染**）。-1 = 这条来源还不占槽。
// 绘制路径必须走这条：渲染一个图标要解 PE / 缩放，成本是毫秒级，一旦落在
// WM_PAINT 里，首帧就要等整屏图标提完（Wine 先擦白再画，就是「点进去闪一下」）。
static int poolSlotLookup(struct IconPool* p, int id) {
    if (!p || !iconIdTypeNameValid(id)) return -1;
    if (id - 1 >= p->idSlotCap) return -1;   // 容量没覆盖到 = 肯定没登记过
    if (p->idSlot[id - 1] < 0) return -1;
    int slot = p->idSlot[id - 1];
    // slotId / slotTick 是裸数组，slot 只能来自我们自己的登记 —— 这里再兜一道：
    // 簿记一旦出错（池被换掉、槽数缩过），宁可当「这条来源没有槽」，也不要越界写。
    if (slot >= p->slotCount) return -1;
    p->slotTick[slot] = ++p->tick;           // 命中即刷新 LRU 时间戳
    return slot;
}

// 把**已经渲染好的** HICON 放进池：分配槽位、维护双向映射、池满时 LRU 淘汰。
// 只做簿记、不渲染 —— 渲染归调用方（同步的 renderIconSource，或后台线程）。
// 单独拆出来的原因：后台渲好的图标也要走同一套簿记（见 iconRenderPump）。
static int poolStoreIcon(struct IconPool* p, int id, HICON hicon) {
    if (!p || !hicon || !iconIdTypeNameValid(id)) return -1;

    // 反向映射数组要覆盖到这个 id（后台路径不经过 poolSlotFor，不能指望它扩过容）
    if (p->idSlotCap < iconSourceCount) {
        int newCap = iconSourceCount + 64;
        int* tmp = realloc(p->idSlot, (size_t)newCap * sizeof(int));
        if (!tmp) return -1;
        for (int i = p->idSlotCap; i < newCap; i++) tmp[i] = -1;
        p->idSlot = tmp;
        p->idSlotCap = newCap;
    }

    int slot;
    if (p->filled < p->slotCount) {
        slot = ImageList_AddIcon(p->himl, hicon);
        if (slot >= 0 && slot < p->slotCount && slot >= p->filled) p->filled = slot + 1;
    }
    else {
        // 池满：在**非共享**槽里挑最久没用过的原地替换。ReplaceIcon 索引不变，
        // 所以控件里已挂的列表句柄与 iImage 的语义都不受影响。
        // 共享项（目录/驱动器/固定节点/扩展名）会反复出现，优先不动它们。
        int victim = -1;
        for (int i = 0; i < p->slotCount; i++) {
            int owner = p->slotId[i];
            if (owner <= 0) continue;
            if (iconSources[owner - 1].flags & ICONSRC_SHARED) continue;
            if (victim < 0 || p->slotTick[i] < p->slotTick[victim]) victim = i;
        }
        // 整池都被共享项占着时（共享槽不参与淘汰，所以它只会随浏览过的扩展名种类单调
        // 增长；大图标 128px 的池也只有几十个槽）必须退让一步、连共享项一起挑，否则
        // 这个独占项（exe/lnk/ico）永远挤不进池，调用方还会把它标成永久失败 ——
        // 那正是「小概率某个文件压根不显示图标」。被顶掉的共享项下次绘制会重新渲染，
        // 代价只是一次 shell 坐标查询。
        if (victim < 0) {
            for (int i = 0; i < p->slotCount; i++) {
                if (p->slotId[i] <= 0) continue;
                if (victim < 0 || p->slotTick[i] < p->slotTick[victim]) victim = i;
            }
        }
        if (victim < 0) return -1;
        // 被顶掉的图标要同时作废它的反向映射，否则它会一直命中一个装了别人图案的槽位；
        // 它的在途标记也要清掉，这样下次被绘制时才会重新排给后台渲染。
        int oldOwner = p->slotId[victim];
        if (oldOwner > 0 && oldOwner <= p->idSlotCap) p->idSlot[oldOwner - 1] = -1;
        if (oldOwner > 0 && oldOwner <= iconSourceCount && iconSources)
            iconSources[oldOwner - 1].renderPending = 0;
        slot = ImageList_ReplaceIcon(p->himl, victim, hicon);
    }
    // 槽位必须落在池内。ImageList_AddIcon 理论上只返回有效索引，但下面两行是**裸数组写**，
    // 簿记一旦出错就是堆越界写 —— 宁可这次不画，也不要写坏池。
    if (slot < 0 || slot >= p->slotCount) return -1;

    p->slotId[slot] = id;
    p->slotTick[slot] = ++p->tick;
    p->idSlot[id - 1] = slot;
    return slot;
}

// 取某条来源在池里的槽位。
//   allowSync = true  → 查不到就**当场渲染**（绘制路径额度内的分支用，保住那一帧不留空格）
//   allowSync = false → 查不到就**排给后台渲染**并返回 -1（补齐 / 预取用，不占 UI 线程）
static int poolSlotFor(struct IconPool* p, int id, bool allowSync) {
    if (!p || !iconIdTypeNameValid(id)) return -1;

    int ready = poolSlotLookup(p, id);
    if (ready >= 0) return ready;

    // 异步：解码交给后台线程，这一帧先空着。渲好后 iconRenderPump() 会建 HICON、分配槽位、
    // 把缺过图标的区间重画一次 —— 于是补齐/预取不再占用 UI 线程的任何时间。
    if (!allowSync) { iconQueueRender(id, p->size); return -1; }

    HICON hicon = renderIconSource(id, p->size);
    if (!hicon) return -1;
    int slot = poolStoreIcon(p, id, hicon);
    DestroyIcon(hicon);
    return slot;
}


// ===== 后台图标渲染 =====
//
// 为什么要有它：图标解码是毫秒级的**同步**操作，而它的成本大头是文件 I/O 与 PE 加载，不是像素
// 多少。留在 UI 线程上时，一帧能渲几条只能靠额度摊 —— 超出的格子那一帧必然空，等补齐渲好再
// 重画一次，用户看到的就是「滚动时随机几个图标消失又立马显示」。搬到独立线程后：
//   · 绘制帧只查池、永不等待解码 → 拖动不再被解码拖住；
//   · 解码在后台连续跑、吞吐随核数上升（原来是与绘制严格串行的单线程）；
//   · 渲好的经完成队列回主线程入池，再由既有的「补到就重画缺过的区间」画上屏。
//
// 线程边界（本节最重要的约束）：
//   · 后台**只做纯解码**：解 PE/ICO 的帧 → 缩放到目标尺寸 → 叠角标。读文件、分配内存、算像素。
//   · 后台**绝不**碰这三样：shell 图标 API、ImageList、iconSources。前两者只能在 UI 线程；
//     来源表则可能被 iconStoreGrow 的 realloc 搬走 —— 所以入队时把字段**快照**进任务结构，
//     渲染线程只认这份快照，与来源表彻底解耦。
//   · 主线程负责：建 HICON、分配槽位、维护 LRU、重画。
// 代价（如实说）：图标可能晚几十毫秒才出现 —— 换掉了原来「被截掉就一帧空白」的行为。
struct IconRenderJob {
    int id;
    int size;
    unsigned generation;    // 与当前代次不符的结果一律丢弃（中途清过缓存）
    unsigned kind;
    unsigned flags;
    int iconIndex;
    wchar_t iconFile[MAX_PATH];
};

struct IconRenderDone {
    int id;
    int size;
    unsigned generation;
    unsigned flags;
    BYTE* pixels;           // 已缩放到 size 的 32bpp 方形像素；NULL = 后台拿不到（需主线程兜底）
};

static struct IconRenderJob  iconRenderJobs[ICON_RENDER_QUEUE_MAX];
static int iconRenderJobHead, iconRenderJobTail, iconRenderJobCount;
static struct IconRenderDone iconRenderDone[ICON_RENDER_DONE_MAX];
static int iconRenderDoneHead, iconRenderDoneTail, iconRenderDoneCount;
static CRITICAL_SECTION iconRenderLock;
static bool iconRenderLockReady = false;
static unsigned iconRenderGeneration = 1;   // 清缓存时 ++：作废所有在途任务
static HANDLE iconRenderThread = NULL;
static HANDLE iconRenderWake = NULL;
static volatile LONG iconRenderStop = 0;

// 后台的纯解码：与 renderIconSource 的 PE/ICO 分支同源，只是不碰 shell、不建 HICON。
// 返回**已缩放到目标尺寸**的像素缓冲（调用方负责 free）；NULL = 这条来源后台解不出来。
static BYTE* renderIconPixelsOffThread(const struct IconRenderJob* job) {
    if (!job || job->size <= 0 || !job->iconFile[0]) return NULL;

    int srcSize = 0;
    BYTE* src = NULL;
    if (job->kind == ICONSRC_ICO) {
        src = extractIcoIconPixels(job->iconFile, job->size, &srcSize);
    }
    else {
        src = extractPeIconPixels(job->iconFile, job->iconIndex, job->size, &srcSize);
        // 图标组序号越界（注册表里的坐标常带一个文件里并不存在的序号）：退回主图标
        if (!src && job->iconIndex != 0)
            src = extractPeIconPixels(job->iconFile, 0, job->size, &srcSize);
    }
    if (!src) return NULL;

    // 原生尺寸就是目标尺寸时 scalePixelsOwned 直接把缓冲交回（省一次整幅 memcpy）
    BYTE* scaled = scalePixelsOwned(src, srcSize, job->size);
    if (!scaled) return NULL;

    // 角标叠在最终尺寸上；合成失败不影响主图标，只是少个箭头
    if (job->flags & ICONSRC_OVERLAY) stampShortcutOverlay(scaled, job->size);
    return scaled;
}

static DWORD WINAPI iconRenderTask(LPVOID param) {
    (void)param;
    for (;;) {
        if (InterlockedCompareExchange(&iconRenderStop, 0, 0)) break;

        struct IconRenderJob job;
        bool have = false;
        EnterCriticalSection(&iconRenderLock);
        if (iconRenderJobCount > 0) {
            job = iconRenderJobs[iconRenderJobHead];
            iconRenderJobHead = (iconRenderJobHead + 1) % ICON_RENDER_QUEUE_MAX;
            iconRenderJobCount--;
            have = true;
        }
        LeaveCriticalSection(&iconRenderLock);

        if (!have) {
            // 没活干就睡；有活会被 SetEvent 立刻叫醒。睡醒再验一次退出标志。
            WaitForSingleObject(iconRenderWake, ICON_RENDER_IDLE_MS);
            continue;
        }

        BYTE* pixels = renderIconPixelsOffThread(&job);

        // 结果入队。**满了就等着，绝不丢弃** —— 丢掉的那条对应的 renderPending 就再也清不掉，
        // 那条来源从此不会被排进来（变成永久缺图标）。
        for (;;) {
            EnterCriticalSection(&iconRenderLock);
            if (iconRenderDoneCount < ICON_RENDER_DONE_MAX) {
                struct IconRenderDone* d = &iconRenderDone[iconRenderDoneTail];
                d->id = job.id;
                d->size = job.size;
                d->generation = job.generation;
                d->flags = job.flags;
                d->pixels = pixels;
                iconRenderDoneTail = (iconRenderDoneTail + 1) % ICON_RENDER_DONE_MAX;
                iconRenderDoneCount++;
                LeaveCriticalSection(&iconRenderLock);
                break;
            }
            LeaveCriticalSection(&iconRenderLock);
            if (InterlockedCompareExchange(&iconRenderStop, 0, 0)) { free(pixels); return 0; }
            Sleep(2);
        }

        HWND hwnd = hwndContentView;
        if (hwnd && IsWindow(hwnd)) PostMessage(hwnd, MSG_ICON_RENDER_DONE, 0, 0);
    }
    return 0;
}

// 惰性创建渲染线程。必须在**主线程**调用：它顺带保证 GDI+ 已初始化（PNG 压缩帧的解码要走
// GDI+，而 GdiplusStartup 只应由单一线程初始化一次）。
static void iconRenderStart(void) {
    if (iconRenderThread) return;

    if (!iconRenderLockReady) {
        InitializeCriticalSection(&iconRenderLock);
        iconRenderLockReady = true;
    }
    initGdiplus();

    if (!iconRenderWake) iconRenderWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!iconRenderWake) return;

    iconRenderStop = 0;
    iconRenderThread = CreateThread(NULL, 0, iconRenderTask, NULL, 0, NULL);
    if (!iconRenderThread) {
        CloseHandle(iconRenderWake);
        iconRenderWake = NULL;
    }
}

// 排一条来源给后台渲染。**只由主线程调用**；已在途的（renderPending）不重复排。
// 没有 iconFile 的来源（ICONSRC_SHELL）后台做不了，直接返回让调用方走同步路径。
static void iconQueueRender(int id, int size) {
    if (id <= 0 || size <= 0 || id > iconSourceCount || !iconSources) return;

    struct IconSource* s = &iconSources[id - 1];
    if (s->renderPending) return;
    if (!s->iconFile || !s->iconFile[0]) return;
    if (s->kind != ICONSRC_FILE && s->kind != ICONSRC_ICO) return;

    iconRenderStart();
    if (!iconRenderThread) return;

    EnterCriticalSection(&iconRenderLock);
    if (iconRenderJobCount >= ICON_RENDER_QUEUE_MAX) { LeaveCriticalSection(&iconRenderLock); return; }
    struct IconRenderJob* j = &iconRenderJobs[iconRenderJobTail];
    memset(j, 0, sizeof(*j));
    j->id = id;
    j->size = size;
    j->generation = iconRenderGeneration;
    j->kind = s->kind;
    j->flags = s->flags;
    j->iconIndex = s->iconIndex;
    // 路径必须在这里**复制**进任务：iconSources 会被 iconStoreGrow 的 realloc 搬走，
    // 而且 clearIconCaches 会整个释放来源表 —— 渲染线程只认这份快照。
    wcsncpy(j->iconFile, s->iconFile, MAX_PATH - 1);
    j->iconFile[MAX_PATH - 1] = L'\0';
    iconRenderJobTail = (iconRenderJobTail + 1) % ICON_RENDER_QUEUE_MAX;
    iconRenderJobCount++;
    // 在途标记与入队放在同一个临界区里：两者必须原子，否则渲染线程可能在标记盖上之前
    // 就完成并被主线程接管（清掉的标记随后又被盖上 → 这条来源再也不排队 = 永久缺图标）。
    s->renderPending = 1;
    LeaveCriticalSection(&iconRenderLock);

    SetEvent(iconRenderWake);
}

// 接管后台渲好的图标：建 HICON → 入池 → 把缺过图标的区间重画一次。
// **只由主线程调用**（ImageList 与来源表都只有主线程能碰）。
static void iconRenderPump(void) {
    if (!iconRenderThread || !iconRenderLockReady) return;

    bool adopted = false;
    for (;;) {
        struct IconRenderDone done;
        EnterCriticalSection(&iconRenderLock);
        if (iconRenderDoneCount <= 0) { LeaveCriticalSection(&iconRenderLock); break; }
        done = iconRenderDone[iconRenderDoneHead];
        iconRenderDone[iconRenderDoneHead].pixels = NULL;
        iconRenderDoneHead = (iconRenderDoneHead + 1) % ICON_RENDER_DONE_MAX;
        iconRenderDoneCount--;
        LeaveCriticalSection(&iconRenderLock);

        // 代次不符（中途清过缓存）：这条结果的 id 可能已被新来源复用，直接丢
        if (done.generation != iconRenderGeneration) { free(done.pixels); continue; }

        // 尺寸不符（用户切了图标尺寸）：这条结果对不上任何在用池，丢掉
        struct IconPool* p = currentIconPool();
        if (!p || p->himl != currentImageList || p->size != done.size) {
            free(done.pixels);
            if (done.id > 0 && done.id <= iconSourceCount && iconSources)
                iconSources[done.id - 1].renderPending = 0;
            continue;
        }

        if (done.id > 0 && done.id <= iconSourceCount && iconSources)
            iconSources[done.id - 1].renderPending = 0;

        if (!done.pixels) {
            // 后台解不出来（PE/ICO 里没有可用帧、或那个文件本 prefix 里根本不存在）。这条来源
            // 还有一条出路：shell 系统列表兜底 —— 那是主线程才能碰的 API，且只在失败路径上
            // 发生一次（结果缓存在 s->shellIndex），不会成为热点。
            HICON hFb = renderIconSource(done.id, done.size);
            if (hFb) {
                if (poolStoreIcon(p, done.id, hFb) >= 0) adopted = true;
                DestroyIcon(hFb);
            }
            continue;
        }

        HICON h = createIconFromPixels(done.pixels, done.size, done.size);
        free(done.pixels);
        if (!h) continue;
        if (poolStoreIcon(p, done.id, h) >= 0) adopted = true;
        DestroyIcon(h);
    }

    // 与 iconFillRange 里那处重画同一个套路：RedrawItems **不擦背景**（所以不闪），
    // 限流 40ms 免得把消息队列塞满重绘。
    if (adopted && hwndContentView) {
        DWORD now = GetTickCount();
        if (now - lastIconRepaintTick >= ICON_REPAINT_MIN_MS) {
            lastIconRepaintTick = now;
            int first = iconMissFirst, last = iconMissLast;
            if (first < 0) first = 0;
            if (last >= numItems) last = numItems - 1;
            if (last >= first && numItems > 0) ListView_RedrawItems(hwndContentView, first, last);
        }
    }
}

// 清缓存：作废在途任务与已完成结果。来源表马上要被整个释放，旧结果的 id 会指向新来源，
// 那会让一个文件显示成别人的图标 —— 所以要在 resetIconStore() 之前调。
static void iconRenderInvalidate(void) {
    if (!iconRenderLockReady) return;
    EnterCriticalSection(&iconRenderLock);
    iconRenderGeneration++;
    iconRenderJobCount = 0;
    iconRenderJobHead = iconRenderJobTail = 0;
    while (iconRenderDoneCount > 0) {
        free(iconRenderDone[iconRenderDoneHead].pixels);
        iconRenderDone[iconRenderDoneHead].pixels = NULL;
        iconRenderDoneHead = (iconRenderDoneHead + 1) % ICON_RENDER_DONE_MAX;
        iconRenderDoneCount--;
    }
    LeaveCriticalSection(&iconRenderLock);
}

// ---------------------------------------------------------------------------
// 后台「文件夹条目数」计数
// ---------------------------------------------------------------------------
//
// 「大小」列对文件夹显示条目数（如「12 项」）。这件事**不白给**：
//   · Windows / Wine **没有「只查数量」的 API** —— 要知道一个目录里有几项，只能把它枚举一遍
//     （FindFirstFile + FindNextFile）。这和「修改日期」那种从 WIN32_FIND_DATA 顺手拷 8 字节
//     完全不是一个量级，它是真的一次目录读取。
//   · 所以**绝不能**在 loadItemData 里同步做：它的调用点里有两个（itemIconReady /
//     iconFillRangeInner）就落在**绘制帧**上，而「绘制路径只查缓存、绝不现算」是图标子系统
//     那几轮教训换来的规矩。
// 结构照抄「后台图标渲染」：请求 / 完成两个环形队列 + 事件唤醒 + PostMessage 通知主线程。
//
// 结果怎样安全地认回「是哪一行」：任务只带 {requestId, idx}，而 requestId 被**记在
// items[idx] 这一格上**（ListItem.countRequestId）。回主线程时三条一起验：idx 仍在范围内、
// items[idx].countRequestId 等于发出去的那个、且该 node 还是目录。中途换目录 / 重排 / 刷新都会
// 重建 items[]（memset 成 0）→ 验证必然失败 → 结果丢弃。这是**自愈**的：新的一格
// countRequestId 是 0，下一次 loadItemData 会重新排队。所以既不需要「代次」全局变量，也不会有
// 图标那边「在途标记清不掉 → 永久缺图标」的隐患。
#define FOLDER_COUNT_QUEUE_MAX  64   // 在途请求上限（只有可见行会进来，够用）
#define FOLDER_COUNT_DONE_MAX  128
#define FOLDER_COUNT_IDLE_MS   500
// 数到这个数就停，显示「9999+ 项」：十万项的目录不该为了一个数字把后台线程占住。
#define FOLDER_COUNT_CAP      9999
// 查过了但枚举失败（权限 / 路径过长）：记成这个值，别再反复排队
#define CHILD_ITEM_COUNT_FAILED (-2)

struct FolderCountJob {
    unsigned requestId;
    int idx;
    wchar_t path[MAX_PATH];
};

struct FolderCountDone {
    unsigned requestId;
    int idx;
    int count;          // < 0 = 枚举失败
};

static struct FolderCountJob  folderCountJobs[FOLDER_COUNT_QUEUE_MAX];
static int folderCountJobHead, folderCountJobTail, folderCountJobCount;
static struct FolderCountDone folderCountDone[FOLDER_COUNT_DONE_MAX];
static int folderCountDoneHead, folderCountDoneTail, folderCountDoneCount;
static CRITICAL_SECTION folderCountLock;
static bool folderCountLockReady = false;
static HANDLE folderCountThread = NULL;
static HANDLE folderCountWake = NULL;
static volatile LONG folderCountStop = 0;
// 只增不减；0 保留给「本格没有在途请求」。用无符号是为了让回绕有定义（int 自增溢出是 UB），
// 回绕本身无害 —— 认领只看相等，而 0 已被显式跳过。
static unsigned folderCountRequestSeq = 0;

// 数一个目录里的条目数（后台线程唯一做的事）。**只读**，不递归。
// 过滤规则必须与 buildChildNodes 一致（跳过 . / ..，按 g_showHiddenFiles 决定是否含隐藏项），
// 否则这一格显示的项数会和列表里的行数对不上。
static int countItemsInFolder(const wchar_t* dir) {
    if (!dir || !dir[0]) return -1;

    size_t len = wcslen(dir);
    // 与 buildChildNodes 同一套 MAX_PATH 守卫：先判长度再拼接
    if (len + 2 >= MAX_PATH) return -1;

    wchar_t pattern[MAX_PATH] = {0};
    wcscpy_s(pattern, MAX_PATH, dir);
    wcscat_s(pattern, MAX_PATH, dir[len - 1] == L'\\' ? L"*" : L"\\*");

    WIN32_FIND_DATA wfd;
    HANDLE handle = FindFirstFile(pattern, &wfd);
    if (handle == INVALID_HANDLE_VALUE) return -1;

    int count = 0;
    do {
        if (wfd.cFileName[0] == L'.' && (wfd.cFileName[1] == L'\0' ||
            (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))) continue;
        if (!g_showHiddenFiles && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        count++;
        if (count >= FOLDER_COUNT_CAP) break;    // 到顶就不再往下数
    }
    while (FindNextFile(handle, &wfd));
    FindClose(handle);
    return count;
}

static DWORD WINAPI folderCountTask(LPVOID param) {
    (void)param;
    for (;;) {
        if (InterlockedCompareExchange(&folderCountStop, 0, 0)) break;

        struct FolderCountJob job;
        bool have = false;
        EnterCriticalSection(&folderCountLock);
        if (folderCountJobCount > 0) {
            job = folderCountJobs[folderCountJobHead];
            folderCountJobHead = (folderCountJobHead + 1) % FOLDER_COUNT_QUEUE_MAX;
            folderCountJobCount--;
            have = true;
        }
        LeaveCriticalSection(&folderCountLock);

        if (!have) {
            // 没活就睡；有活会被 SetEvent 立刻叫醒。睡醒再验一次退出标志。
            WaitForSingleObject(folderCountWake, FOLDER_COUNT_IDLE_MS);
            continue;
        }

        int count = countItemsInFolder(job.path);

        // 结果入队。**满了就等，绝不丢弃** —— 丢掉的那条对应的 countRequestId 就再也清不掉，
        // 那一格从此不会再排队（永久空白）。同 iconRenderTask 的规则。
        for (;;) {
            EnterCriticalSection(&folderCountLock);
            if (folderCountDoneCount < FOLDER_COUNT_DONE_MAX) {
                struct FolderCountDone* d = &folderCountDone[folderCountDoneTail];
                d->requestId = job.requestId;
                d->idx = job.idx;
                d->count = count;
                folderCountDoneTail = (folderCountDoneTail + 1) % FOLDER_COUNT_DONE_MAX;
                folderCountDoneCount++;
                LeaveCriticalSection(&folderCountLock);
                break;
            }
            LeaveCriticalSection(&folderCountLock);
            if (InterlockedCompareExchange(&folderCountStop, 0, 0)) return 0;
            Sleep(2);
        }

        HWND hwnd = hwndContentView;
        if (hwnd && IsWindow(hwnd)) PostMessage(hwnd, MSG_FOLDER_COUNT_DONE, 0, 0);
    }
    return 0;
}

// 惰性起线程。**只在主线程调用**。
static void folderCountStart(void) {
    if (folderCountThread) return;

    if (!folderCountLockReady) {
        InitializeCriticalSection(&folderCountLock);
        folderCountLockReady = true;
    }
    if (!folderCountWake) folderCountWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!folderCountWake) return;

    folderCountStop = 0;
    folderCountThread = CreateThread(NULL, 0, folderCountTask, NULL, 0, NULL);
    if (!folderCountThread) {
        CloseHandle(folderCountWake);
        folderCountWake = NULL;
    }
}

// 给第 idx 项（目录）排一次计数。**只由主线程调用**（要写 items[idx].countRequestId）。
// 已经有结果 / 已有在途请求 / 队列满 → 什么都不做，下一次 loadItemData 会再来（自愈）。
static void folderCountRequest(int idx) {
    if (!items || idx < 0 || idx >= numItems) return;
    struct ListItem* item = &items[idx];
    struct FileNode* node = item->node;
    if (!node || node->type != TYPE_DIR) return;
    if (node->childItemCount != CHILD_ITEM_COUNT_UNKNOWN || item->countRequestId != 0) return;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(node, path);
    if (path[0] == L'\0') return;

    folderCountStart();
    if (!folderCountThread) return;

    unsigned requestId = ++folderCountRequestSeq;
    if (requestId == 0) requestId = ++folderCountRequestSeq;    // 跳过保留值 0

    EnterCriticalSection(&folderCountLock);
    if (folderCountJobCount >= FOLDER_COUNT_QUEUE_MAX) { LeaveCriticalSection(&folderCountLock); return; }
    struct FolderCountJob* j = &folderCountJobs[folderCountJobTail];
    memset(j, 0, sizeof(*j));
    j->requestId = requestId;
    j->idx = idx;
    // 路径必须在这里**复制**进任务：FileNode 树会在换目录时整个释放，后台线程只认这份快照。
    wcsncpy(j->path, path, MAX_PATH - 1);
    j->path[MAX_PATH - 1] = L'\0';
    folderCountJobTail = (folderCountJobTail + 1) % FOLDER_COUNT_QUEUE_MAX;
    folderCountJobCount++;
    // 「本格有在途请求」的标记必须与入队放在同一临界区：否则后台可能在标记盖上之前就完成，
    // 主线程随即把结果吃掉 —— 而标记随后才盖上，那一格就再也不会排队了。
    item->countRequestId = requestId;
    LeaveCriticalSection(&folderCountLock);

    SetEvent(folderCountWake);
}

// 接管后台数好的结果：写回 FileNode → 让那一行重新格式化 → 重画。**只由主线程调用**。
static void folderCountPump(void) {
    if (!folderCountThread || !folderCountLockReady) return;

    int firstRedraw = -1, lastRedraw = -1;
    for (;;) {
        struct FolderCountDone done;
        EnterCriticalSection(&folderCountLock);
        if (folderCountDoneCount <= 0) { LeaveCriticalSection(&folderCountLock); break; }
        done = folderCountDone[folderCountDoneHead];
        folderCountDoneHead = (folderCountDoneHead + 1) % FOLDER_COUNT_DONE_MAX;
        folderCountDoneCount--;
        LeaveCriticalSection(&folderCountLock);

        // 三条一起验：换过目录 / 重排过 / 刷新过 → 这一格已经不是当初那一格，直接丢掉
        if (!items || done.idx < 0 || done.idx >= numItems) continue;
        if (items[done.idx].countRequestId != done.requestId) continue;
        struct FileNode* node = items[done.idx].node;
        if (!node || node->type != TYPE_DIR) continue;

        items[done.idx].countRequestId = 0;
        if (done.count < 0) {
            node->childItemCount = CHILD_ITEM_COUNT_FAILED;   // 查过了、没结果，不再重试
            continue;
        }
        node->childItemCount = done.count;
        // 让这一行下次取显示信息时重新格式化（loadItemData 会读到刚写回的 childItemCount）。
        // 代价只是一行的重算：图标是缓存命中，时间/大小都是纯计算。
        items[done.idx].loaded = false;
        if (firstRedraw < 0) firstRedraw = done.idx;
        lastRedraw = done.idx;
    }

    // RedrawItems 不擦背景（所以不闪），一次 pump 只重画一条连续区间，不需要再限流。
    if (firstRedraw >= 0 && hwndContentView)
        ListView_RedrawItems(hwndContentView, firstRedraw, lastRedraw);
}

// 解析并登记条目对应的图标来源，返回图标 id（0 = 失败）。
//
// 这是全局唯一碰「shell 图标 API」的地方：
//   - 独占项（exe / lnk / .ico）按完整路径登记 —— 它们每个文件都可能有不同图案；
//   - 共享项（目录、驱动器、其他扩展名）按扩展名/固定标识登记，数量有界；
//   - 坐标一律优先走 SHGFI_ICONLOCATION（不碰 SIC），只有它拿不到时才退回
//     shell 的系统列表索引（ICONSRC_SHELL，只读、不追加、不增长 SIC）。
// 顺带把类型名一起缓存进来源项，loadItemData 因此不再需要自己调 getFileInfo。
static int resolveIconId(struct ListItem* item) {
    if (!item || !item->node) return 0;
    struct FileNode* node = item->node;
    if (iconIdTypeNameValid(item->icon)) return item->icon;
    // 退避中：这一项上一轮解析 / 登记失败过，这一轮直接放弃。那两个 shell 调用
    // （ICONLOCATION、系统列表索引）是毫秒级的，退避期内每帧重试会把绘制和补齐预算一起拖垮。
    // 退避计数由补齐循环递减，绘制路径只读、不会走到这条分支。
    if (item->iconTries > 0) return 0;

    wchar_t key[MAX_PATH] = {0};
    bool shared = true;
    bool overlay = false;
    unsigned kind = ICONSRC_FILE;

    // 键先算出来 —— 命中就不必碰任何 shell API（大目录里绝大多数条目走这条路）
    if (node->type == TYPE_DRIVE) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        key[0] = L'\x02'; key[1] = path[0]; key[2] = L'\0';
    }
    else if (node->type == TYPE_DIR) {
        key[0] = L'\x01'; key[1] = L'\0';
    }
    else if (node->type == TYPE_FILE) {
        const wchar_t* ext = wcsrchr(node->name, L'.');
        bool exclusive = ext && (wcsicmp(ext, L".exe") == 0 || wcsicmp(ext, L".lnk") == 0
                                 || wcsicmp(ext, L".ico") == 0);
        if (exclusive) {
            wchar_t path[MAX_PATH] = {0};
            getFileNodePath(node, path);
            wcsncpy_s(key, MAX_PATH, path, _TRUNCATE);
            shared = false;
            overlay = (wcsicmp(ext, L".lnk") == 0);
        }
        else if (ext) {
            wcsncpy_s(key, MAX_PATH, ext, _TRUNCATE);
        }
        else {
            key[0] = L'\x03'; key[1] = L'\0';   // 无扩展名
        }
    }
    else {
        // 桌面 / 个人目录 / 计算机 等固定节点：按类型共享
        key[0] = L'\x04'; key[1] = (wchar_t)(L'a' + (int)node->type); key[2] = L'\0';
    }

    int id = iconKeyLookup(key);
    if (id > 0) { item->icon = id; return id; }

    wchar_t file[MAX_PATH] = {0};
    wchar_t typeName[80] = {0};
    int index = 0;
    int shellIndex = -1;

    if (node->type == TYPE_DRIVE) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        wcsncpy_s(typeName, 80, isCDDrivePath(path) ? lc_str.cd_drive : lc_str.local_drive, _TRUNCATE);
        SHFILEINFO sfi = {0};
        if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION) && sfi.szDisplayName[0]) {
            wcsncpy_s(file, MAX_PATH, sfi.szDisplayName, _TRUNCATE);
            index = sfi.iIcon;
        }
        else {
            struct FileInfo fi = {0};
            getFileInfo(path, TYPE_DRIVE, false, &fi);
            shellIndex = fi.icon;
        }
    }
    else if (node->type == TYPE_DIR) {
        // 所有目录共用一张「文件夹」图标（与原实现的 folderIconCachedForStyle 语义一致）。
        // 目录的图标与路径无关，所以不必按路径去查，一个常量键就够。
        wcsncpy_s(typeName, 80, lc_str.folder, _TRUNCATE);
        // USEFILEATTRIBUTES 分支只看扩展名/属性，不看路径 —— 传个哑名字即可，
        // 传空串在部分实现上会退化成「查当前目录」而失败。
        SHFILEINFO sfi = {0};
        if (SHGetFileInfoW(L"a", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                           SHGFI_ICONLOCATION | SHGFI_USEFILEATTRIBUTES) && sfi.szDisplayName[0]) {
            wcsncpy_s(file, MAX_PATH, sfi.szDisplayName, _TRUNCATE);
            index = sfi.iIcon;
        }
        else {
            // 退路：system 列表里的目录类型图标（只读，不追加）
            struct FileInfo fi = {0};
            getFileInfo(L"a", TYPE_DIR, false, &fi);
            shellIndex = fi.icon;
        }
    }
    else if (node->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        const wchar_t* ext = wcsrchr(node->name, L'.');
        bool isExe = ext && wcsicmp(ext, L".exe") == 0;
        bool isLnk = ext && wcsicmp(ext, L".lnk") == 0;
        bool isIco = ext && wcsicmp(ext, L".ico") == 0;

        if (isExe || isLnk) {
            wcsncpy_s(typeName, 80, isExe ? lc_str.application : lc_str.shortcut, _TRUNCATE);
            if (isLnk) {
                // .lnk 必须自己读 lnk 里的 ICON_LOCATION，**不能**用
                // SHGFI_ICONLOCATION：Wine 对快捷方式直接返回 shell32.dll 的通用
                // 文档图标（实测 file=SHELL32.dll idx=0），于是最后画出来的就只剩
                // 我们合成的那个角标。详见 resolveLnkIconLocation 的说明。
                wchar_t iconPath[MAX_PATH] = {0};
                int iconIndex = 0;
                if (resolveLnkIconLocation(path, iconPath, MAX_PATH, &iconIndex)) {
                    wcsncpy_s(file, MAX_PATH, iconPath, _TRUNCATE);
                    index = iconIndex;
                    const wchar_t* iext = wcsrchr(iconPath, L'.');
                    // 图标来源本身是 .ico：走自带 ICO 解码器（PE 解码器与
                    // PrivateExtractIconsW 都不认 .ico）
                    if (iext && wcsicmp(iext, L".ico") == 0) kind = ICONSRC_ICO;
                }
                else {
                    // lnk 损坏 / 目标是 URL 等：退回 lnk 自己的注册表坐标，
                    // 再不行 renderIconSource 还有 shell 兜底。
                    SHFILEINFO sfi = {0};
                    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION) && sfi.szDisplayName[0]) {
                        wcsncpy_s(file, MAX_PATH, sfi.szDisplayName, _TRUNCATE);
                        index = sfi.iIcon;
                    }
                }
            }
            else {
                SHFILEINFO sfi = {0};
                if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION) && sfi.szDisplayName[0]) {
                    wcsncpy_s(file, MAX_PATH, sfi.szDisplayName, _TRUNCATE);
                    index = sfi.iIcon;
                }
                if (!file[0]) {
                    // ICONLOCATION 落空：exe 用文件自己（自带 PE 解码器兜底），
                    // 同样不必再碰 shell，也就不会增长 SIC。
                    wcsncpy_s(file, MAX_PATH, path, _TRUNCATE);
                }
            }
        }
        else if (isIco) {
            // .ico 必须按文件取真图案：注册表里 .ico 常映射到 shell32 的通用图标，
            // 走 ICONLOCATION 会得到「不是这个文件」的图案。用自带的 ICO 解码器。
            kind = ICONSRC_ICO;
            wcsncpy_s(file, MAX_PATH, path, _TRUNCATE);
            wchar_t upper[30] = {0};
            if (ext[1]) strToUpper(ext + 1, upper, 30);
            swprintfTrunc(typeName, 80, lc_str.fmt_file, upper);
        }
        else {
            // 普通文件：类型名在本地拼（与 getFileInfo 的 default 分支同一套规则 ——
            // exe/lnk 上面的分支已经处理掉，这里只会是「<大写扩展名> 文件」，无扩展名
            // 则是「文件」），图标取注册表登记的坐标。
            //
            // 刻意**不**预调 getFileInfo：它唯一的产出是 SYSICONINDEX，而
            // USEFILEATTRIBUTES 分支每碰到一个新扩展名就往 SIC 里塞一项（≈367 KB，
            // 主要是那份 256×256 JUMBO 帧）。坐标拿不到时才退到它，当作最后手段。
            wcsncpy_s(typeName, 80, lc_str.file, _TRUNCATE);
            if (ext && ext[1]) {
                wchar_t upper[30] = {0};
                strToUpper(ext + 1, upper, 30);
                swprintfTrunc(typeName, 80, lc_str.fmt_file, upper);
            }
            SHFILEINFO sfi = {0};
            if (SHGetFileInfoW(path, FILE_ATTRIBUTE_ARCHIVE, &sfi, sizeof(sfi),
                               SHGFI_ICONLOCATION | SHGFI_USEFILEATTRIBUTES) && sfi.szDisplayName[0]) {
                wcsncpy_s(file, MAX_PATH, sfi.szDisplayName, _TRUNCATE);
                index = sfi.iIcon;
            }
            else {
                struct FileInfo fi = {0};
                getFileInfo(path, TYPE_FILE, false, &fi);
                shellIndex = fi.icon;
            }
        }
    }
    else {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        struct FileInfo fi = {0};
        getFileInfo(path, node->type, false, &fi);
        wcsncpy_s(typeName, 80, fi.typeName, _TRUNCATE);
        shellIndex = fi.icon;
    }

    if (file[0]) {
        id = iconSourceAdd(key, kind,
                           (shared ? ICONSRC_SHARED : 0) | (overlay ? ICONSRC_OVERLAY : 0),
                           file, index, typeName);
    }
    else if (shellIndex >= 0) {
        id = iconSourceAdd(key, ICONSRC_SHELL, ICONSRC_SHARED | (overlay ? ICONSRC_OVERLAY : 0),
                           NULL, shellIndex, typeName);
    }
    else {
        id = 0;
    }

    item->icon = id;
    // 登记失败（来源表到顶 / 分配失败 / 坐标与 shell 索引都没拿到）一律按**瞬态**处理：
    // 只做退避计数，不写任何永久标记。那两个 shell 调用完全可能只是**这一次**没成
    // （Wine 的 shell 正忙、内存紧张、注册表项临时读不到），写死一次就是那个文件在本目录内
    // 永远没有图标 —— 那正是「随机文件缺图标」最直接的来源。
    // 退避也**不影响绘制**：只要池里有这条来源的槽，getIconSlotForPaint 照样画出来。
    if (id <= 0 && item->iconTries == 0) item->iconTries = ICON_RETRY_BACKOFF_ROUNDS;
    return id;
}

// 条目在当前显示列表里的槽位（-1 = 这次画不出图标）
static int getIconSlot(struct ListItem* item, bool allowSync) {
    if (!item) return -1;
    int id = resolveIconId(item);
    if (id <= 0) return -1;
    struct IconPool* p = currentIconPool();
    if (!p) return -1;
    // 只在这个池确实挂在控件上时才给出索引：池创建失败时 refreshContentView 会
    // 退回系统列表，那时的 iImage 语义与池内的槽位不对应。
    if (p->himl != currentImageList) return -1;
    return poolSlotFor(p, id, allowSync);
}

// 绘制路径专用的槽位查询：默认**只查已渲染好的槽**，不在这里渲染。唯一的例外是
// 「紧跟着滚动消息的那几次绘制」—— 那时允许就地渲染有界的几个（见函数体内说明），因为
// Wine 的 scroll_list() 在 ScrollWindowEx 之后会同步 UpdateWindow，那一帧必须当场就是
// 完整的，否则新露出的那一条一定带着空格子结束。
//
// 为什么不能在这里渲染：渲染一个图标要解 PE / 缩放（或查 shell），是毫秒级的活。它一旦
// 落在 WM_PAINT 里，首帧就得等整屏图标提完 —— 而 Wine 是先 WM_ERASEBKGND 把客户区擦成
// 窗口底色再发 WM_PAINT，用户看到的就是「点进去先闪一下白」。所以绘制只管画，缺图标的项
// 先空着；渲染由 iconFillStep() 在绘制之外补，补进来的由 iconFillRange 重画缺过的区间。
//
// 这个函数同时是绘制路径唯一知道「哪些项还没有图标」的地方，所以补齐的调度也从这里发 ——
// 滚动/翻页/改窗口大小之后自然会有绘制，也就自然会重新安排补齐，不必再去接
// WM_VSCROLL / WM_MOUSEWHEEL / WM_SIZE。
static int getIconSlotForPaint(struct ListItem* item) {
    if (!item) return -1;
    struct IconPool* p = currentIconPool();
    // 只在这个池确实挂在控件上时才给出索引：池创建失败时 refreshContentView 会
    // 退回系统列表，那时的 iImage 语义与池内的槽位不对应。
    if (!p || p->himl != currentImageList) return -1;

    // **有就画**：池里已经有这一项的槽就直接画出来。
    //
    // 这条判据里**不掺任何**「本轮要不要去补」的状态。退避计数（iconTries）只决定补齐要不要
    // 再试一次，绝不能决定这一格画不画 —— 曾经这里有一行
    // `if (item->iconFailed || item->iconGaveUp) return -1;`，把补齐的调度标记当成了绘制
    // 判据：于是池里明明有槽（文件夹、普通文件的共享槽几乎总在池里）也被画成空位，而那个
    // 标记**只在滚动消息到达时**被清零，两者的时间差就表现为「滚动时随机几个图标先消失、
    // 紧接着又出现」。判据改成「池里有没有槽」之后，有槽的图标在任何一帧都画得出来。
    if (iconIdTypeNameValid(item->icon)) {
        int slot = poolSlotLookup(p, item->icon);
        if (slot >= 0) return slot;
    }

    // 池里确实没有。**滚动驱动的绘制是唯一的例外**：就地渲染出来，这一帧直接画全。
    //
    // 为什么必须破这个例：Wine 的 comctl32/listview.c 里 scroll_list() 是
    //     ScrollWindowEx(..., SW_ERASE | SW_INVALIDATE);
    //     UpdateWindow(infoPtr->hwndSelf);        ← 同步 WM_PAINT
    // —— 新露出的那一条**在控件自己处理滚动消息的过程中就被画掉了**。把它留到滚动之后的
    // iconFillVisibleSync 去补，那一帧必然已经带着空格子结束，补上后再重画一次，用户看到的
    // 就是「图标消失一下又立刻出现」。
    //
    // 为什么在这里渲染不会把「先擦白」的老毛病带回来：那一帧脏的只是新露出的**一条**（它
    // 刚被 SW_ERASE 擦过，本来就得等这一次绘制），渲染这几毫秒不会让任何已有内容消失。
    // 导航时那种「整屏被 InvalidateRect(TRUE) 擦白」的情形则由额度挡住 —— 只有紧跟着滚动
    // 消息的绘制才有额度（iconPaintRenderBudget 在 WM_PAINT 里按 lastScrollTick 判定）。
    //
    // 额度按**本屏跨度**给（见 WM_PAINT），不是一个小常数：拖滑块时每条 WM_VSCROLL 都会把整屏
    // 重画一次，额度必须够把这一帧真正要画的来源渲完，否则被截掉的那几条就是空格子。
    // 额度用尽只会发生在「一屏全是各不相同的独占来源」这种极端情形，那时走下面的异步补齐。
    DWORD nowTick = GetTickCount();
    if (iconPaintRenderBudget > 0 &&
        (inScrollMessage || nowTick - lastScrollTick <= ICON_SCROLL_PAINT_WINDOW_MS) &&
        nowTick < iconPaintRenderDeadline) {
        int slot = getIconSlot(item, true);   // 额度内允许当场渲染：保住这一帧不留空格
        if (slot >= 0) {
            iconPaintRenderBudget--;
            return slot;
        }
    }

    // 池里确实没有、额度也用尽（或不在滚动期）：上报「这里缺图标」的见证（补齐据此把本轮
    // 范围扩到绘制真正看到过的项上），并安排一轮补齐。
    if (items && item >= items && item < items + numItems) {
        int idx = (int)(item - items);
        if (idx < iconMissFirst) iconMissFirst = idx;
        if (idx > iconMissLast) iconMissLast = idx;
        iconPaintMissed = true;
    }
    scheduleIconFill();
    return -1;
}

// 计算并应用大图标视图的格子尺寸（LVM_SETICONSPACING）。
// 宽度按当前目录最长文件名的单行宽度自适应（夹在图标宽与上限之间），
// 高度按该宽度下实际换行数自适应 —— 短名目录紧凑，长名目录宽而不高。
// 行数固定（1..5）时高度 = N×行高，超出部分绘制时加省略号；
// 「无限」时任何长度的文件名都完整显示。
static void updateIconViewLayout(void) {
    if (viewStyle != STYLE_LARGE_ICON || !hwndContentView) return;

    const int marginX = ICONVIEW_MARGIN_X;
    const int iconLabelGap = ICONVIEW_ICON_GAP;
    const int bottomPad = ICONVIEW_BOTTOM_PAD;

    int lineHeight = 16;
    int textWidth = iconViewIconSize;  // 文字区宽度，至少与图标同宽
    int textHeight = lineHeight;       // 至少留一行，空目录也不至于挤成 0

    HDC hdc = GetDC(hwndContentView);
    if (hdc) {
        HFONT font = (HFONT)SendMessage(hwndContentView, WM_GETFONT, 0, 0);
        HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : NULL;

        TEXTMETRICW tm;
        // 行高必须与 DrawTextW 的实际行进距离一致：DrawText 默认不含外部
        // 行距（没加 DT_EXTERNALLEADING 时每行只前进 tmHeight）。若按
        // tmHeight+tmExternalLeading 算 N 行高度，N≥4 时累计省下的行距会
        // 凑出一整行，绘制时就多画一行（4 行显示成 5 行）。
        if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0)
            lineHeight = tm.tmHeight;

        // 文字区宽度上限用行高的整数倍，随 DPI/字体缩放联动
        const int maxTextWidth = lineHeight * 10;
        // 安全上限：超过 64 行的文件名（正常路径不可能出现）停止拉高格子
        const int maxAutoTextHeight = lineHeight * 64;

        if (items && numItems > 0) {
            // 先取最长文件名的单行宽度，夹在 [图标宽, 上限] 之间作为文字区宽度
            int maxNameWidth = 0;
            for (int i = 0; i < numItems; i++) {
                if (!items[i].node || !items[i].node->name) continue;
                RECT rc = {0, 0, 0, 0};
                DrawTextW(hdc, items[i].node->name, -1, &rc, DT_CALCRECT | DT_NOPREFIX);
                if (rc.right > maxNameWidth) maxNameWidth = rc.right;
            }
            bool widthClamped = maxNameWidth > maxTextWidth;
            textWidth = maxNameWidth;
            if (widthClamped) textWidth = maxTextWidth;
            if (textWidth < iconViewIconSize) textWidth = iconViewIconSize;

            if (iconViewLabelLines > 0) {
                textHeight = iconViewLabelLines * lineHeight;
            }
            else if (widthClamped) {
                // 只有宽度触顶时名字才可能换行，需要逐个测换行高度取最大值。
                // 未触顶时 textWidth ≥ 任何名字的单行宽度，全部单行，直接跳过。
                // 测量宽度与绘制宽度严格一致（都是 textWidth），测量标志与绘制
                // 标志一致，保证画的时候永远不需要省略号。
                for (int i = 0; i < numItems; i++) {
                    if (!items[i].node || !items[i].node->name) continue;
                    RECT rc = {0, 0, textWidth, 0};
                    int h = DrawTextW(hdc, items[i].node->name, -1, &rc,
                                      DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
                    if (h > textHeight) textHeight = h;
                    if (textHeight >= maxAutoTextHeight) { textHeight = maxAutoTextHeight; break; }
                }
            }
        }
        else if (iconViewLabelLines > 0) {
            textHeight = iconViewLabelLines * lineHeight;
        }

        if (oldFont) SelectObject(hdc, oldFont);
        ReleaseDC(hwndContentView, hdc);
    }

    iconViewLineHeight = lineHeight;   // 供绘制端夹文字区底边用（取不到 DC 时也是回退值）

    int cx = textWidth + marginX * 2;
    int cy = iconViewIconSize + iconLabelGap + textHeight + bottomPad;

    ListView_SetIconSpacing(hwndContentView, cx, cy);
    ListView_Arrange(hwndContentView, LVA_DEFAULT);
    InvalidateRect(hwndContentView, NULL, TRUE);
}

static void saveIconViewSettings(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)iconViewIconSize;
        RegSetValueEx(hkey, L"IconViewIconSize", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = (DWORD)iconViewLabelLines;
        RegSetValueEx(hkey, L"IconViewLabelLines", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

void loadIconViewSettings(void) {
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD size = sizeof(val);
        if (RegQueryValueEx(hkey, L"IconViewIconSize", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS
            && (val == 32 || val == 48 || val == 64 || val == 96 || val == 128)) {
            iconViewIconSize = (int)val;
        }
        size = sizeof(val);
        if (RegQueryValueEx(hkey, L"IconViewLabelLines", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS
            && val <= 5) {
            iconViewLabelLines = (int)val;
        }
        RegCloseKey(hkey);
    }
}

void setIconViewIconSize(int size) {
    if (size != 32 && size != 48 && size != 64 && size != 96 && size != 128) size = 32;
    if (size != iconViewIconSize) {
        iconViewIconSize = size;
        saveIconViewSettings();
        // 仅大图标视图的显示尺寸会变；其他视图固定 16px，不受影响。
        // 条目里的 item->icon 是「图标来源 id」，与尺寸无关 —— 换个尺寸只是换个
        // 显示列表池，不必清任何缓存。refreshContentView 会把新尺寸的池挂上，
        // 然后把不再使用的池释放掉（旧池此时已从控件上摘下，销毁是安全的）。
        if (viewStyle == STYLE_LARGE_ICON) refreshContentView();
    }
    updateIconViewMenuCheckmarks();
}

void setIconViewLabelLines(int lines) {
    if (lines < 0 || lines > 5) lines = 0;
    if (lines != iconViewLabelLines) {
        iconViewLabelLines = lines;
        saveIconViewSettings();
        if (viewStyle == STYLE_LARGE_ICON) updateIconViewLayout();
    }
    updateIconViewMenuCheckmarks();
}

void updateIconViewMenuCheckmarks(void) {
    // 图标尺寸 / 文件名行数只作用于大图标视图，非大图标视图下把父项「大图标视图」
    // 整体置灰，免得用户以为它们对当前视图也生效。
    // 灰 popup 项用的是「popup 项的 wID 就是子菜单句柄」这条约定：Wine 侧
    // win32u/menu.c 的 MENU_InsertItem 把 info->wID 原样存下，user32 的
    // AppendMenuW(MF_POPUP) 传的就是子菜单句柄；而 MENU_ShowSubPopup 对
    // fState 带 MF_GRAYED/MF_DISABLED 的 popup 直接 return（menu.c:3383），
    // 所以灰掉父项就足以让它弹不开，不必再逐个子项置灰。
    if (hMenuView && hMenuIconView) {
        EnableMenuItem(hMenuView, (UINT)(UINT_PTR)hMenuIconView,
                       MF_BYCOMMAND | (viewStyle == STYLE_LARGE_ICON ? MF_ENABLED : MF_GRAYED));
    }
    if (hMenuIconSize) {
        UINT check;
        switch (iconViewIconSize) {
            case 48:  check = ID_VIEW_ICONSIZE_48;  break;
            case 64:  check = ID_VIEW_ICONSIZE_64;  break;
            case 96:  check = ID_VIEW_ICONSIZE_96;  break;
            case 128: check = ID_VIEW_ICONSIZE_128; break;
            default:  check = ID_VIEW_ICONSIZE_32;  break;
        }
        CheckMenuRadioItem(hMenuIconSize, ID_VIEW_ICONSIZE_32, ID_VIEW_ICONSIZE_128, check, MF_BYCOMMAND);
    }
    if (hMenuLines) {
        UINT check;
        switch (iconViewLabelLines) {
            case 1:  check = ID_VIEW_LINES_1;  break;
            case 2:  check = ID_VIEW_LINES_2;  break;
            case 3:  check = ID_VIEW_LINES_3;  break;
            case 4:  check = ID_VIEW_LINES_4;  break;
            case 5:  check = ID_VIEW_LINES_5;  break;
            default: check = ID_VIEW_LINES_AUTO; break;
        }
        CheckMenuRadioItem(hMenuLines, ID_VIEW_LINES_AUTO, ID_VIEW_LINES_5, check, MF_BYCOMMAND);
    }
}

void updateDriveBarMenuCheckmarks(void) {
    if (!hMenuDriveBar) return;
    UINT check;
    switch (driveBarMode) {
        case DRIVE_BAR_TOTAL: check = ID_VIEW_DRIVE_BAR_TOTAL; break;
        case DRIVE_BAR_NONE:  check = ID_VIEW_DRIVE_BAR_NONE;  break;
        default:              check = ID_VIEW_DRIVE_BAR;       break;
    }
    CheckMenuRadioItem(hMenuDriveBar, ID_VIEW_DRIVE_BAR, ID_VIEW_DRIVE_BAR_NONE, check, MF_BYCOMMAND);
}

static void saveDriveBarMode(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)driveBarMode;
        RegSetValueEx(hkey, L"ShowDriveBarMode", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

void loadDriveBarMode(void) {
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD size = sizeof(val);
        if (RegQueryValueEx(hkey, L"ShowDriveBarMode", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS
            && val <= (DWORD)DRIVE_BAR_NONE) {
            driveBarMode = (int)val;
        }
        else {
            // 迁移旧版布尔值 ShowDriveBar：0（隐藏）→ 不显示，1 → 图形条
            size = sizeof(val);
            if (RegQueryValueEx(hkey, L"ShowDriveBar", NULL, NULL, (BYTE*)&val, &size) == ERROR_SUCCESS)
                driveBarMode = val ? DRIVE_BAR_GRAPH : DRIVE_BAR_NONE;
        }
        RegCloseKey(hkey);
    }
}

void setDriveBarMode(int mode) {
    if (mode < DRIVE_BAR_GRAPH || mode > DRIVE_BAR_NONE) mode = DRIVE_BAR_GRAPH;
    if (mode != driveBarMode) {
        driveBarMode = mode;
        saveDriveBarMode();
        // 已加载的驱动器条目缓存了大小文字，按新模式重算
        for (int i = 0; i < numItems; i++) {
            if (items[i].node && items[i].node->type == TYPE_DRIVE && items[i].driveTotalBytes > 0)
                formatDriveSizeText(&items[i]);
        }
        InvalidateRect(hwndContentView, NULL, TRUE);
    }
    updateDriveBarMenuCheckmarks();
}

// 长路径截短显示：状态栏/菜单宽度有限，超长路径会被硬截断而看不到
// 关键部分。保留末尾完整的路径段（最容易辨认的部分），前面加 "..." 表示省略。
// 例：C:\very\long\prefix\winlator\bin（maxChars=20）→ ...\winlator\bin
static void compactPathTail(const wchar_t* path, wchar_t* out, int outSize, int maxChars) {
    out[0] = L'\0';
    if (!path || maxChars < 8) return;

    size_t len = wcslen(path);
    if (len <= (size_t)maxChars) {
        swprintfTrunc(out, outSize, L"%ls", path);
        return;
    }

    // "..." 前缀占 3 字符，剩下 maxChars-3 留给路径尾部。
    // 从右往左找出最靠左的、能放下的组件起点（即前一个字符是 '\'）。
    const wchar_t* keep = NULL;
    for (const wchar_t* p = path + len; p > path; p--) {
        if (p[-1] == L'\\' && (len - (p - path)) + 3 <= (size_t)maxChars) keep = p;
    }

    if (keep) swprintfTrunc(out, outSize, L"...%ls", keep);
    else {
        // 连最后一个组件都放不下：硬截取末尾 maxChars-3 个字符
        swprintfTrunc(out, outSize, L"...%ls", path + len - (maxChars - 3));
    }
}

// 中段截短：长路径只显示尾部会丢掉盘符，这里保留头部路径段（通常是盘符，
// 如 "C:\"）和末尾路径段，中间用 "...\..." 连接。
// 例：C:\Storage\0000-1111\Android\data\app\cache（maxChars=40）→ C:\...\data\app\cache
static void compactPathMid(const wchar_t* path, wchar_t* out, int outSize, int maxChars) {
    out[0] = L'\0';
    if (!path || maxChars < 8) return;

    size_t len = wcslen(path);
    if (len <= (size_t)maxChars) {
        swprintfTrunc(out, outSize, L"%ls", path);
        return;
    }

    const wchar_t* firstSlash = wcschr(path, L'\\');
    // 没有分隔符（纯文件名）或头部就占满预算：退化为尾部截短
    if (!firstSlash || (firstSlash - path) + 7 > maxChars) {
        compactPathTail(path, out, outSize, maxChars);
        return;
    }

    // "C:\" 头部 3 字符 + "...\\" 4 字符，其余留给尾部
    int tailBudget = maxChars - (int)(firstSlash - path) - 1 - 4;
    wchar_t tail[MAX_PATH] = {0};
    compactPathTail(firstSlash + 1, tail, MAX_PATH, tailBudget);
    if (tail[0]) swprintfTrunc(out, outSize, L"%.*ls...\\%ls", (int)(firstSlash - path) + 1, path, tail);
    else swprintfTrunc(out, outSize, L"%.*ls...", (int)(firstSlash - path) + 1, path);
}

// 取首个剪贴板条目的显示信息：截短后的源目录 + 首个文件名
static void buildClipboardSourceDisplay(wchar_t* outDir, int dirSize, int dirMaxChars,
                                        wchar_t* outName, int nameSize, int nameMaxChars) {
    outDir[0] = L'\0';
    if (outName) outName[0] = L'\0';

    wchar_t* firstPath = getClipboardFirstPath();
    if (!firstPath) return;

    wchar_t srcDir[MAX_PATH] = {0};
    getParentDirFromPath(firstPath, srcDir);
    compactPathMid(srcDir, outDir, dirSize, dirMaxChars);

    if (outName) {
        wchar_t basename[MAX_PATH] = {0};
        getBasenameFromPath(firstPath, basename, MAX_PATH, false);
        compactPathTail(basename, outName, nameSize, nameMaxChars);
    }
}

static void updateStatusbar() {
    wchar_t statusText[32] = {0};
    swprintf_s(statusText, 32, L"%d %ls", numItems, lc_str.items);

    // 剪贴板有内容时在条目数后面附上来源指示（操作类型 / 条目数 /
    // 首个文件名 / 源目录），复制或剪切后即使切到别的目录也能看到待粘贴的是什么
    if (clipboardHasItems()) {
        static wchar_t fullText[MAX_PATH + 192] = {0};
        wchar_t clipText[MAX_PATH + 160] = {0};
        wchar_t compactDir[MAX_PATH] = {0};
        wchar_t compactName[MAX_PATH] = {0};
        // 前半段 "N items | " 约占 20 字符，路径预算放宽到 60，
        // 窗口不够宽时仍由状态栏自身裁剪兜底
        buildClipboardSourceDisplay(compactDir, MAX_PATH, 60, compactName, MAX_PATH, 24);
        swprintfTrunc(clipText, MAX_PATH + 160, lc_str.clipboard_info,
                      isClipboardCut() ? lc_str.cut : lc_str.copy,
                      getClipboardCount(), compactName, compactDir);
        swprintfTrunc(fullText, MAX_PATH + 192, L"%ls | %ls", statusText, clipText);
        setStatusbarText(fullText);
    }
    else setStatusbarText(statusText);
}

// 剪贴板内容变化后的 UI 同步：状态栏指示、工具栏粘贴按钮、编辑菜单。
// file_actions 在复制/剪切/移动完成（清空剪贴板）时调用。
void onClipboardChanged() {
    updateStatusbar();
    setPasteButtonEnabled(clipboardHasItems());
    updatePasteMenuState();
}

static void freeMenuItems() {
    if (menuItems) {
        for (int i = 0; i < numMenuItems; i++) {
            if (menuItems[i].text) {
                free(menuItems[i].text);
                menuItems[i].text = NULL;
            }
            if (menuItems[i].cmdData) {
                free(menuItems[i].cmdData);
                menuItems[i].cmdData = NULL;
            }
        }
        free(menuItems);
        menuItems = NULL;
    }
    numMenuItems = 0;
}

void clearContentView() {    
    ListView_SetItemCountEx(hwndContentView, 0, 0);
    // 删除所有现有列（在非REPORT视图下清除列，避免残留）
    HWND hHeader = ListView_GetHeader(hwndContentView);
    if (hHeader) {
        int numCols = Header_GetItemCount(hHeader);
        for (int i = numCols - 1; i >= 0; i--) {
            ListView_DeleteColumn(hwndContentView, i);
        }
    } else {
        // 无法获取表头时，尝试删除搜索模式添加的第4列
        ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
    }

    if (items) {
        for (int i = 0; i < numItems; i++) {
            if (items[i].path) {
                free(items[i].path);
                items[i].path = NULL;
            }
        }
        free(items);
        items = NULL;
    }
    numItems = 0;
    itemsCapacity = 0;

    // 清除搜索缓存，防止文件树重建后指针悬空
    free(searchCache.results);
    searchCache.results = NULL;
    searchCache.count = 0;
    // 搜索结果所引用的节点由搜索节点池拥有，与结果同生命周期
    freeSearchPool(searchCache.pool);
    searchCache.pool = NULL;

    // 注意：图标缓存不再在此清空，以保持跨导航的加速效果
    // 只有在视图样式切换时才需要重建图像列表
    freeMenuItems();

    // Cleanup icon viewer resources
    cleanupIconGroups();
}

// 后台等待线程的参数：command 的所有权在线程结束前归它所有。
struct CommandWaitData {
    wchar_t* command;
};

// 在后台线程里执行并等待 cmd.exe 退出。
// 旧实现在 UI 线程上 WaitForSingleObject(INFINITE)：只要自定义命令启动的程序
// 不退出，整个 WFM 就完全无响应（连窗口都不能拖动）。
static DWORD WINAPI commandWaitTask(void* param) {
    struct CommandWaitData* data = (struct CommandWaitData*)param;

    SHELLEXECUTEINFO shExecInfo = {0};
    shExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExecInfo.hwnd = hwndMain;
    shExecInfo.lpVerb = NULL;
    shExecInfo.lpFile = L"C:\\windows\\system32\\cmd.exe";
    shExecInfo.lpParameters = data->command;
    shExecInfo.lpDirectory = NULL;
    shExecInfo.nShow = SW_SHOW;
    shExecInfo.hInstApp = NULL;

    // ShellExecuteEx 失败时 hProcess 为 NULL，直接传给 WaitForSingleObject 无意义
    if (ShellExecuteEx(&shExecInfo) && shExecInfo.hProcess) {
        WaitForSingleObject(shExecInfo.hProcess, INFINITE);
        CloseHandle(shExecInfo.hProcess);
    }

    free(data->command);
    free(data);
    return 0;
}

// 接管 command 的所有权（成功创建线程时由线程释放）
static void execCommandLine(wchar_t* command) {
    struct CommandWaitData* data = calloc(1, sizeof(struct CommandWaitData));
    if (!data) {
        free(command);
        return;
    }
    data->command = command;

    HANDLE thread = CreateThread(NULL, 0, commandWaitTask, data, 0, NULL);
    if (!thread) {
        free(command);
        free(data);
        return;
    }
    CloseHandle(thread);
}

LRESULT CALLBACK ContentViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 滚动类消息一律记时间戳：图标补齐据此让位，避免和拖动抢消息队列
    // （`switch` 下面不动它们，继续交给列表控件的默认处理）。
    // LVM_SCROLL 也算：外部（手势 / 外壳 / 应用自身）可以直接给控件发这条消息让它滚动，
    // 走的是同一条 scroll_list → 同步重画的路子；不记的话那一帧就没人当场补图标。
    // —— 滚动之前先把这个目标窗口的图标渲好，再让控件滚 ——
    // Wine 的 scroll_list() 是 ScrollWindowEx(整块 rcList) + 同步 UpdateWindow：滚动消息一进
    // 控件，新露出的一屏**当场**就被画掉了。等它画完再去补图标，那一帧必然已经带着空格子结束
    // —— 这正是「快拖时随机几个图标先消失、紧接着又出现」。顺序必须反过来（见
    // preScrollHandleVScroll）。它返回 true = 这条消息已经处理完（已经滚过，或者正押着）。
    bool preScrolled = false;
    if (msg == WM_VSCROLL && !inPreScroll && items && numItems > 0)
        preScrolled = preScrollHandleVScroll((UINT)LOWORD(wParam));

    bool scrollMsg = (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
                      msg == WM_MOUSEHWHEEL || msg == WM_KEYDOWN || msg == WM_SIZE ||
                      msg == LVM_SCROLL);
    if (scrollMsg) {
        lastScrollTick = GetTickCount();
        // 滚动 = 时机重来的机会。这里只碰**可见区**（O(可见项数)，与目录规模无关）：
        //   ① 把可见项的退避计数清零，让停手后的补齐立刻再试一次；
        //   ② 对可见项各查一次池 —— 命中会刷新槽的 LRU 时间戳，从而**保护在屏图标不被淘汰**。
        // ② 必须由我们主动做：控件在快速滚动时只重绘脏区，某些可见项这一帧压根没被绘制过，
        // 它们的槽时间戳就显得「很久没用」，池满时被挑作 LRU 受害者顶掉 —— 下一帧那一格就是
        // 空白，补齐再渲染回来，看上去正是「闪一下」。
        struct IconPool* p = currentIconPool();
        if (p && p->himl == currentImageList && items && numItems > 0) {
            int first, last;
            visibleItemRange(&first, &last);
            for (int i = first; i < last; i++) {
                struct ListItem* it = &items[i];
                it->iconTries = 0;
                if (iconIdTypeNameValid(it->icon)) poolSlotLookup(p, it->icon);
            }
        }
    }

    // 连续拖动（按住滚动条滑块）事后不做同步补齐：拖动期间每个鼠标移动都来一条
    // WM_VSCROLL，每条都付一次渲染代价的话，拖动本身就卡了。它交给「滚动静默 + 异步
    // 补齐」；拖动期间绘制照常画池里已有的图标，缺的先空着。
    // 离散滚动（滚轮 / 翻页键 / 行滚动 / 改尺寸）只有一步，趁机把新露出来的补齐。
    bool syncFillAfter = scrollMsg && !(msg == WM_VSCROLL && LOWORD(wParam) == SB_THUMBTRACK);

    // 绘制入口：发现可见区还有缺图标的项就安排一轮补齐（见 requestIconFillForVisible）。
    // 注意它**不**决定「这一帧画不画图标」—— 绘制一律「有就画」，缺的先空着，
    // 补齐每有进展就把缺过的区间重画一次。
    if (msg == WM_PAINT) {
        // 本次绘制重开「就地渲染」额度（见 getIconSlotForPaint）。
        //
        // 这是「滚动那一帧」的兜底渲染额度。拖滑块时每一条 WM_VSCROLL 都把整屏同步重画一次
        // （Wine 的 scroll_list = ScrollWindowEx(整块 rcList) + UpdateWindow），额度就是那一帧
        // 最多肯花多少时间在渲染上 —— 它**直接等于「拖动时每帧多卡多久」**。
        //
        // 它已经被「先渲后滚」的预渲染（preScrollHandleVScroll）挤到次要位置：慢拖时目标窗口
        // 早在滚动之前就渲好了，这里一条都不用渲。只有当用户**快拖**（预渲染主动让路，见
        // ICON_SCROLL_FAST_DRAG_MS）、或缺口大到押后也没用时，这一帧才真得靠它自己画。
        //
        // 所以额度必须收住，两个极端都错：曾经是写死的 10 个 / 15ms（一屏来源多于它就被截掉
        // → 「快拖随机几个图标缺失」）；后来放大成「一屏跨度 / 100ms」（不掉了，但拖动粘住）。
        // 现在取中间值，且**上限** ICON_PAINT_RENDER_CAP：快拖那一帧够画本屏去重后的来源
        // （.dll 这类共享项一个 id 点亮整屏，通常个位数到十几个），又不会让拖动变粘。
        DWORD now = GetTickCount();
        bool scrollDriven = inScrollMessage || (now - lastScrollTick <= ICON_SCROLL_PAINT_WINDOW_MS);
        if (scrollDriven) {
            int quota = visibleIconSpan();
            if (quota < ICON_PAINT_RENDER_MIN) quota = ICON_PAINT_RENDER_MIN;
            if (quota > ICON_PAINT_RENDER_CAP) quota = ICON_PAINT_RENDER_CAP;
            iconPaintRenderBudget = quota;
            iconPaintRenderDeadline = now + ICON_PAINT_RENDER_MS;
        }
        else {
            // 导航 / 改尺寸那种整屏脏的绘制：不在绘制路径里渲染，交给首屏同步补齐
            // （prewarmSharedIcons + iconFillVisibleSync）—— 保住「点进去立刻出列表」。
            iconPaintRenderBudget = 0;
            iconPaintRenderDeadline = now;
        }
        requestIconFillForVisible();
    }
    // 两个一次性定时器：TIMER_ICON_FILL 是「等滚动静默」之后重新安排补齐（此时
    // lastScrollTick 已经过期，scheduleIconFill 会真的把消息投出去）；TIMER_ICON_PREFETCH
    // 是邻域预取的下一趟。定时器 id 不匹配就原样落给列表控件处理。
    if (msg == WM_TIMER && (wParam == TIMER_ICON_FILL || wParam == TIMER_ICON_PREFETCH ||
                            wParam == TIMER_ICON_SCROLL)) {
        KillTimer(hwnd, (UINT_PTR)wParam);
        if (wParam == TIMER_ICON_FILL) {
            iconFillTimerPending = false;
            iconFillDispatchTick = GetTickCount();
            scheduleIconFill();
        }
        else if (wParam == TIMER_ICON_SCROLL) {
            // 押后拖动的重试：鼠标停在原地时不会有新的 WM_VSCROLL 进来（滑块不动，滚动条就
            // 不发消息），那条目标窗口就永远等不到「就位」。这一趟走 WM_TIMER 而不是
            // PostMessage —— PostMessage 的优先级高于输入消息，自续环会把鼠标消息压在后面，
            // 那正是「拖动粘住」的成因（见 scheduleIconFill 里的同类说明）。
            iconScrollTimerPending = false;
            if (pendingScrollTop >= 0 && !inPreScroll) {
                preScrollFromTimer = true;      // 这不是真实消息，别被当成「快拖」而撤销押后
                preScrollHandleVScroll((UINT)SB_THUMBTRACK);
                preScrollFromTimer = false;
            }
        }
        else {
            iconPrefetchTimerPending = false;
            // 押后拖动期间不预取：这一刻预取的目标（可见区两侧邻域）下一帧就过时了，
            // 只会和「渲染目标窗口」抢时间。
            if (pendingScrollTop >= 0) scheduleIconPrefetch();
            else if (iconPrefetchStep()) scheduleIconPrefetch();
        }
        return 0;
    }

    switch (msg) {
        case MSG_ADD_ITEMS_BATCH: {
            if (searchData != NULL && searchData->active) {
                struct BatchItems* batch = (struct BatchItems*)lParam;
                int newCount = numItems + batch->count;

                if (newCount > itemsCapacity) {
                    int newCapacity = itemsCapacity == 0 ? 1000 : itemsCapacity * 2;
                    if (newCapacity < newCount) newCapacity = newCount;
                    struct ListItem* tmp = realloc(items, newCapacity * sizeof(struct ListItem));
                    if (!tmp) break;
                    items = tmp;
                    itemsCapacity = newCapacity;
                }
                
                for (int i = 0; i < batch->count; i++) {
                    struct ListItem* item = &items[numItems + i];
                    // realloc 出来的新区是未初始化的，icon / iconTries 必须清零：iconTries 的
                    // 垃圾值非 0 会让这一项一进来就处于退避状态（白白拖几轮不补），而 icon 的
                    // 垃圾值若恰好落在已登记的来源范围内，还会让它显示成**别的文件**的图标。
                    // 这正好是随机、小概率的。
                    memset(item, 0, sizeof(struct ListItem));
                    item->node = batch->nodes[i];
                    item->loaded = false;
                    fillFileInfo(batch->nodes[i], item);
                }
                
                numItems = newCount;
                ListView_SetItemCountEx(hwndContentView, numItems, LVSICF_NOINVALIDATEALL);
                scheduleIconFill();   // 搜索结果是分批追加的，每批都安排一次补齐

                DWORD now = GetTickCount();
                if (now - lastStatusbarTick >= 200) {
                    lastStatusbarTick = now;
                    updateStatusbar();
                }
            }
            break;
        }
        case MSG_ICON_FILL: {
            // 先清调度闸再补：iconFillStep 开头的 UpdateWindow 会触发绘制，绘制发现仍有
            // 缺图标就会再安排一次，所以这里必须允许重新挂号。
            iconFillPosted = false;
            iconFillDispatchTick = GetTickCount();
            iconRenderPump();   // 兜底：万一某次完成通知没投到，这条路径也能接管结果
            folderCountPump();  // 同上，给文件夹条目数也留一条兜底路径
            if (iconFillStep()) scheduleIconFill();
            break;
        }
        case MSG_ICON_RENDER_DONE: {
            // 后台渲好了一批：建 HICON、入池、把缺过图标的区间重画一次。
            iconRenderPump();
            break;
        }
        case MSG_FOLDER_COUNT_DONE: {
            // 后台数好了一批文件夹的条目数：写回节点、让那几行重新格式化并重画。
            folderCountPump();
            break;
        }
        case MSG_SEARCH_DONE: {
            if (!searchData) break;   // 防御：正常情况下该消息只在 searchData 非空时投递
            bool canceled = searchData->canceled;
            searchData->active = false;
            if (searchData->threadHandle) {
                CloseHandle(searchData->threadHandle);
                searchData->threadHandle = NULL;
            }

            if (!canceled && searchData->results) {
                // 在 UI 线程接管搜索结果的数组与节点池的所有权
                free(searchCache.results);
                freeSearchPool(searchCache.pool);
                searchCache.results = searchData->results;
                searchCache.pool = searchData->pool;
                searchCache.count = searchData->resultCount;
                searchCache.timestamp = time(NULL);
                searchCache.showHidden = g_showHiddenFiles;
                wcsncpy_s(searchCache.path, MAX_PATH, searchData->rootPath, _TRUNCATE);
                wcscpy_s(searchCache.keyword, 64, searchData->keyword);
                searchData->results = NULL;
                searchData->pool = NULL;
            }
            else {
                free(searchData->results);
                freeSearchPool(searchData->pool);
                searchData->results = NULL;
                searchData->pool = NULL;
            }

            free(searchData);
            searchData = NULL;
            if (pendingSearchKeyword[0]) {
                // 消费挂起的新关键词：searchData 已清理完毕，这里可以直接重新发起。
                // 不再走 refreshContentView()，马上就会被新搜索的 clearContentView 覆盖。
                wchar_t keyword[64] = {0};
                wcscpy_s(keyword, 64, pendingSearchKeyword);
                pendingSearchKeyword[0] = L'\0';
                searchFor(keyword);
            }
            else if (canceled) {
                refreshContentView();
            }
            else updateStatusbar();
            break;
        }
        case MSG_NAVIGATE_TO_PATH: {
            if (pendingNavigatePath[0]) {
                // 搜索线程可能还没退出，等待MSG_SEARCH_DONE处理完再导航
                if (searchData != NULL) {
                    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
                    break;
                }
                navigateToPath(pendingNavigatePath);
                // 在新目录中查找并选中目标文件
                if (pendingSelectName[0]) {
                    for (int i = 0; i < numItems; i++) {
                        if (items[i].node && items[i].node->name &&
                            wcscmp(items[i].node->name, pendingSelectName) == 0) {
                            ListView_SetItemState(hwndContentView, -1, 0, LVIS_SELECTED);
                            ListView_SetItemState(hwndContentView, i, LVIS_SELECTED, LVIS_SELECTED);
                            ListView_EnsureVisible(hwndContentView, i, FALSE);
                            SetFocus(hwndContentView);
                            break;
                        }
                    }
                    pendingSelectName[0] = L'\0';
                }
                pendingNavigatePath[0] = L'\0';
            }
            break;
        }
        case WM_NOTIFY: {
            // Force full redraw when SIZE column is resized
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr->code == HDN_ITEMCHANGEDW || hdr->code == HDN_ITEMCHANGEDA) {
                NMHEADERW* nmh = (NMHEADERW*)hdr;
                if (nmh->iItem == COLUMN_SIZE_IDX) {
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            break;
        }
    }
    // 滚动消息全程插旗：scroll_list() 处理过程中就会同步 UpdateWindow，那次绘制会重入进来，
    // 必须能被认出是「滚动驱动的绘制」（见 getIconSlotForPaint 与 WM_PAINT 处的额度）。
    if (scrollMsg) inScrollMessage = true;
    // preScrolled：这条滚动已经在 preScrollHandleVScroll 里处理过了（它自己发过滚动消息：
    // 渲染好目标窗口之后才把滚动交给控件）。
    LRESULT result = preScrolled ? 0 : OrigWndProc(hwnd, msg, wParam, lParam);
    if (scrollMsg) inScrollMessage = false;

    // 滚完之后对**新的**可见区再做一遍上面那件事。滚动前那一次扫的是旧范围，而快速拖动时
    // 新滚进来的项可能根本没被这一帧画到（控件只重绘脏区），它的槽时间戳就显得很旧 ——
    // 池满时被挑作 LRU 受害者顶掉，下一帧那一格就是空白、补齐再渲回来 = 「闪一下」。
    if (scrollMsg) {
        struct IconPool* p = currentIconPool();
        if (p && p->himl == currentImageList && items && numItems > 0) {
            int first, last;
            visibleItemRange(&first, &last);
            for (int i = first; i < last; i++) {
                struct ListItem* it = &items[i];
                it->iconTries = 0;
                if (iconIdTypeNameValid(it->icon)) poolSlotLookup(p, it->icon);
            }
        }
    }

    // 滚动/改尺寸之后，控件已经把新露出来的那一条失效掉了，但重绘还没发生。趁这个空档
    // 把新露出来的那一屏补齐 —— 重绘到来时就是完整的，不会「滚过去一半有图标一半空白」。
    if (syncFillAfter && !preScrolled) iconFillVisibleSync(ICON_SCROLL_SYNC_BUDGET_MS);

    return result;
}

void updateSelectedItems() {
    MEMFREE(selectedItems);
    numSelectedItems = 0;
    if (!items || numItems <= 0) return;

    int total = ListView_GetSelectedCount(hwndContentView);
    if (total <= 0) return;

    // 一次分配：旧实现逐项 realloc，全选 N 项是 O(N^2)。
    struct FileNode** tmp = calloc((size_t)total, sizeof(struct FileNode*));
    if (!tmp) return;
    selectedItems = tmp;

    for (int i = 0; i < numItems && numSelectedItems < total; i++) {
        if (ListView_GetItemState(hwndContentView, i, LVIS_SELECTED) & LVIS_SELECTED) {
            selectedItems[numSelectedItems++] = items[i].node;
        }
    }

    // 防御：如果控件上报的选中数大于实际能取到的条目（或个别条目的 node 为空），
    // 在这里把空槽压缩掉，保证下游所有 selectedItems[i]->... 的解引用都安全。
    int write = 0;
    for (int i = 0; i < numSelectedItems; i++) {
        if (selectedItems[i]) selectedItems[write++] = selectedItems[i];
    }
    numSelectedItems = write;
}

static void addContextMenuItem(HMENU hMenu, int id, struct ContextMenuItem* cmItem, bool separate) {
    if (!cmItem->text) return;
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_DATA | MIIM_ID | MIIM_STATE;
    item.fType = MFT_STRING;
    item.fState = cmItem->disabled ? MFS_DISABLED : MFS_ENABLED;
    item.dwTypeData = cmItem->text;
    item.cch = wcslen(cmItem->text);
    item.wID = id;
    item.dwItemData = (ULONG_PTR)cmItem;

    InsertMenuItem(hMenu, -1, TRUE, &item);

    if (separate) {
        item.fMask = MIIM_TYPE;
        item.fType = MFT_SEPARATOR;
        InsertMenuItem(hMenu, -1, TRUE, &item);
    }
}

// ===== 自定义菜单命令的安全替换 =====
// 文件名可能包含 " & ^ | % 等字符（Linux/Winlator 文件名允许，Wine 会原样映射），
// 直接拼接就等于命令注入。这里按「模板是否已经给占位符加引号」分别处理：
//   - 模板已加引号（如官方模板 ... x "%FILE%" ...）：引号内无法转义 " 和 %，
//     遇到这类字符就放弃整个菜单项，其余原样替换（保持既有模板完全兼容）；
//   - 模板未加引号：值里含空白或 cmd 元字符时补一层引号，避免参数被拆分或注入。
static bool placeholderIsQuoted(const wchar_t* cmd, const wchar_t* placeholder) {
    const wchar_t* p = wcsstr(cmd, placeholder);
    return p && p != cmd && p[-1] == L'"';
}

static wchar_t* buildCmdArg(const wchar_t* cmd, const wchar_t* placeholder, const wchar_t* value) {
    if (!value) return NULL;
    if (wcspbrk(value, L"\"%")) return NULL;

    bool quoted = placeholderIsQuoted(cmd, placeholder);
    if (quoted || !wcspbrk(value, L" \t&|<>^()")) return wcsdup(value);

    size_t len = wcslen(value);
    wchar_t* out = malloc((len + 3) * sizeof(wchar_t));
    if (!out) return NULL;
    out[0] = L'"';
    memcpy(out + 1, value, len * sizeof(wchar_t));
    out[len + 1] = L'"';
    out[len + 2] = L'\0';
    return out;
}

// 把注册表里的命令模板替换成最终命令行；任一处无法安全替换就返回 NULL
static wchar_t* buildContextMenuCommand(const wchar_t* tmpl, const wchar_t* filePath,
                                        const wchar_t* basename, const wchar_t* dirPath) {
    if (!tmpl) return NULL;
    wchar_t* cmdData = wcsdup(tmpl);
    if (!cmdData) return NULL;

    const wchar_t* specs[3][2] = {
        { L"%FILE%", filePath },
        { L"%BASENAME%", basename },
        { L"%DIR%", dirPath }
    };

    for (int s = 0; s < 3; s++) {
        if (!wcsstr(cmdData, specs[s][0])) continue;
        wchar_t* arg = buildCmdArg(cmdData, specs[s][0], specs[s][1]);
        if (!arg) { free(cmdData); return NULL; }
        wchar_t* replaced = strReplace(cmdData, specs[s][0], arg, true);
        free(arg);
        // strReplace 在分配失败时返回原串（而不是 NULL）。这种情况下占位符没被替换，
        // 留在命令行里的 %FILE% 会被 cmd.exe 当环境变量展开 —— 必须整条放弃。
        if (replaced == cmdData) { free(cmdData); return NULL; }
        cmdData = replaced;
    }
    return cmdData;
}

// 文件名含 " 或 % 时无法安全构造 cmd 命令行（引号内无法转义，% 会被 cmd 展开）。
// 菜单项仍然显示，点击时给出明确提示 —— 不能让整项从菜单里凭空消失。
static void onUnsafeCmdItemClick() {
    MessageBox(hwndMain, L"文件名含有引号或 % 字符，无法安全用于该命令模板",
               lc_str.alert, MB_OK | MB_ICONINFORMATION);
}

static void createContextMenuFromRegistry(int* id) {
    freeMenuItems();
    HKEY hkeyContextMenu, hkeyItem;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\ContextMenu", &hkeyContextMenu) != ERROR_SUCCESS) return;
    if (!selectedItems || numSelectedItems < 1 || !selectedItems[0]) {
        RegCloseKey(hkeyContextMenu);
        return;
    }

    WCHAR itemName[64] = {0};
    WCHAR subitemName[100] = {0};
    WCHAR itemValue[MAX_PATH] = {0};
    DWORD i, j, itemNameLen, itemValueLen;

    i = 0;
    while (i < 10) {
        // RegEnumKey 的 lpcchName 单位是「字符」，不是字节
        itemNameLen = (DWORD)(sizeof(itemName) / sizeof(itemName[0]));
        if (RegEnumKey(hkeyContextMenu, i++, itemName, itemNameLen) != ERROR_SUCCESS) break;
        if (RegOpenKey(hkeyContextMenu, itemName, &hkeyItem) == ERROR_SUCCESS) {
            MENUITEMINFO item = {0};
            item.cbSize = sizeof(MENUITEMINFO);
            item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
            item.fType = MFT_STRING;
            item.dwTypeData = itemName;
            item.cch = itemNameLen;
            item.wID = ++(*id);
            
            HMENU hSubmenu = CreatePopupMenu();
            
            // 结果条目文本与当前路径只取一次，循环内复用
            wchar_t filePath[MAX_PATH] = {0};
            wchar_t dirPath[MAX_PATH] = {0};
            wchar_t basename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0], filePath);
            getBasenameFromPath(filePath, basename, MAX_PATH, true);
            getFileNodePath(selectedItems[0]->parent, dirPath);

            j = 0;
            while (j < 10) {
                // name 长度单位是字符，data 长度单位是字节（Wine: RegEnumValueW 内部按
                // info->NameLength/sizeof(WCHAR) 比较 val_count，按字节比较 *count）
                itemNameLen = (DWORD)(sizeof(subitemName) / sizeof(subitemName[0]));
                itemValueLen = sizeof(itemValue);
                if (RegEnumValue(hkeyItem, j++, subitemName, &itemNameLen, NULL, NULL, (LPBYTE)itemValue, &itemValueLen) != ERROR_SUCCESS) break;

                // 文件名含 " 或 % 时无法安全替换：保留菜单项（点击时提示原因），
                // 但绝不执行被文件名篡改过的命令
                wchar_t* cmdData = buildContextMenuCommand(itemValue, filePath, basename, dirPath);
                if (numMenuItems >= 100) { free(cmdData); break; }

                struct ContextMenuItem* tmp = realloc(menuItems, (numMenuItems + 1) * sizeof(struct ContextMenuItem));
                if (!tmp) { free(cmdData); break; }
                menuItems = tmp;

                wchar_t* text = wcsdup(subitemName);
                if (!text) { free(cmdData); break; }

                struct ContextMenuItem* cmItem = &menuItems[numMenuItems];
                cmItem->text = text;
                cmItem->proc = cmdData ? NULL : onUnsafeCmdItemClick;
                cmItem->cmdData = cmdData;
                numMenuItems++;

                addContextMenuItem(hSubmenu, (*id)++, cmItem, false);
            }
            
            item.hSubMenu = hSubmenu;
            
            InsertMenuItem(hContextMenu, -1, TRUE, &item);
            
            item.fMask = MIIM_TYPE;
            item.fType = MFT_SEPARATOR;
            InsertMenuItem(hContextMenu, -1, TRUE, &item);
            
            RegCloseKey(hkeyItem);
        }
    }
    
    RegCloseKey(hkeyContextMenu);
}

#ifdef USE_LIBCDIO
static void createCDDriveContextMenu(int* id) {
    HMENU hSubmenu = CreatePopupMenu();
    
    wchar_t currentISOPath[MAX_PATH] = {0};
    LONG currentISOPathLen = sizeof(currentISOPath);   // RegQueryValue 的长度单位是字节
    HKEY hkey;
    bool hasISO = false;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        LONG ret = RegQueryValue(hkey, NULL, currentISOPath, &currentISOPathLen);
        // Wine 在值不存在时返回 ERROR_SUCCESS 且给出空串，必须同时判断内容
        hasISO = (ret == ERROR_SUCCESS && currentISOPath[0] != L'\0');
        RegCloseKey(hkey);
    }
    
    // 缓冲区要同时容纳前缀、完整 ISO 路径（最长 MAX_PATH）和尖括号。
    // 旧实现是 wchar_t itemText[64] 却告诉 CRT 有 MAX_PATH，属必然触发的栈溢出。
    wchar_t itemText[MAX_PATH + 64] = {0};
    swprintfTrunc(itemText, MAX_PATH + 64, L"%ls <%ls>",
                  lc_str.load_iso_image, hasISO ? currentISOPath : lc_str.no_media);

    cmiLoadISOImage.text = itemText;
    addContextMenuItem(hSubmenu, (*id)++, &cmiLoadISOImage, false);
    
    cmiLoadISOImage.text = NULL;
    addContextMenuItem(hSubmenu, (*id)++, &cmiUnloadISOImage, false);
    
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE | MIIM_ID | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    swprintfTrunc(itemText, MAX_PATH + 64, L"%ls [X:]", lc_str.cd_drive);
    item.dwTypeData = itemText;
    item.cch = wcslen(itemText);
    item.wID = ++(*id);    
    
    item.hSubMenu = hSubmenu;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
    
    item.fMask = MIIM_TYPE;
    item.fType = MFT_SEPARATOR;
    InsertMenuItem(hContextMenu, -1, TRUE, &item);    
}
#endif /* USE_LIBCDIO */


// 在菜单末尾插入一条分隔线。addContextMenuItem 的 separate 只能把分隔线加在
// 条目「之后」，需要「之前」时用这个（与 createCDDriveContextMenu 里的写法一致）。
static void addContextMenuSeparator(HMENU hMenu) {
    MENUITEMINFO item = {0};
    item.cbSize = sizeof(MENUITEMINFO);
    item.fMask = MIIM_TYPE;
    item.fType = MFT_SEPARATOR;
    InsertMenuItem(hMenu, -1, TRUE, &item);
}

static void createContextMenu(enum ContextMenuType type) {
    HMENU hMenu = CreatePopupMenu();
    hContextMenu = hMenu;

    // ID 从 1 开始：TPM_RETURNCMD 用返回值 0 表示"未选中任何项"（同 navbar.c）
    int id = 1;

    // 「清空剪贴板」在三种菜单形态里都出现，文字统一在这儿挂上
    // （addContextMenuItem 见到 text 为 NULL 会静默跳过这一项）
    cmiClearClipboard.text = lc_str.clear_clipboard;

    if (type == MENU_SINGLE || type == MENU_MULTIPLE) {
        if (type == MENU_SINGLE) {
            if (selectedItems[0]->type == TYPE_FILE) {
                addContextMenuItem(hMenu, id++, &cmiOpen, false);
                addContextMenuItem(hMenu, id++, &cmiEdit, true);
                addContextMenuItem(hMenu, id++, &cmiOpenWith, true);
                addContextMenuItem(hMenu, id++, &cmiShowIcon, true);
                #ifdef USE_LIBCDIO
                createCDDriveContextMenu(&id);
                #endif
                createContextMenuFromRegistry(&id);
            }
            else addContextMenuItem(hMenu, id++, &cmiOpen, true);
        }
        addContextMenuItem(hMenu, id++, &cmiCut, false);
        addContextMenuItem(hMenu, id++, &cmiCopy, true);
        addContextMenuItem(hMenu, id++, &cmiCreateShortcut, false);
        addContextMenuItem(hMenu, id++, &cmiDelete, false);
        
        if (type == MENU_SINGLE) {
            bool inSearch = isInSearchMode();
            addContextMenuItem(hMenu, id++, &cmiRename, inSearch);
            // 搜索模式下显示"定位到文件所在路径"
            if (inSearch) {
                addContextMenuItem(hMenu, id++, &cmiOpenFileLocation, false);
            }
            // .lnk 显示"定位到目标文件路径"：解析快捷方式的目标，跳到目标所在目录；
            // .reg 文件显示导入到注册表菜单项。扩展名互斥，用 else if 避免多算一次 id
            if (selectedItems[0]->type == TYPE_FILE) {
                wchar_t filePath[MAX_PATH] = {0};
                getFileNodePath(selectedItems[0], filePath);
                if (hasFileExtension(filePath, L"lnk")) {
                    addContextMenuItem(hMenu, id++, &cmiOpenLinkTarget, false);
                }
                else if (hasFileExtension(filePath, L"reg")) {
                    addContextMenuItem(hMenu, id++, &cmiImportReg, false);
                }
            }
        }

        // 剪贴板组：放弃待粘贴的内容（只清空 CF_HDROP，文件本身不动）。
        // 单选的扩展名分支各走一条，分隔线统一在这儿加，免得每个分支都管一次
        addContextMenuSeparator(hMenu);
        cmiClearClipboard.disabled = !clipboardHasItems();
        addContextMenuItem(hMenu, id++, &cmiClearClipboard, false);
    }
    else {
        // 空白处右键：剪贴板为空时「粘贴」置灰；有内容时在菜单项上
        // 直接标出操作类型 / 条目数 / 源目录，让用户知道要粘贴的是什么
        bool hasClip = clipboardHasItems();
        static wchar_t pasteText[MAX_PATH + 192] = {0};
        if (hasClip) {
            wchar_t compactDir[MAX_PATH] = {0};
            wchar_t compactName[MAX_PATH] = {0};
            // 菜单项太宽会撑出屏幕，预算比状态栏收得更紧
            buildClipboardSourceDisplay(compactDir, MAX_PATH, 36, compactName, MAX_PATH, 20);
            swprintfTrunc(pasteText, MAX_PATH + 192, lc_str.clipboard_info,
                          isClipboardCut() ? lc_str.cut : lc_str.copy,
                          getClipboardCount(), compactName, compactDir);
            cmiPaste.text = pasteText;
        }
        else cmiPaste.text = lc_str.paste;
        cmiPaste.disabled = !hasClip;
        cmiPasteShortcut.disabled = !hasClip;
        cmiClearClipboard.disabled = !hasClip;
        addContextMenuItem(hMenu, id++, &cmiPaste, false);
        // 分隔线原本挂在「粘贴快捷方式」之后，现在移到剪贴板组末尾，
        // 让「粘贴 / 粘贴快捷方式 / 清空剪贴板」连成一组
        addContextMenuItem(hMenu, id++, &cmiPasteShortcut, false);
        addContextMenuItem(hMenu, id++, &cmiClearClipboard, true);
        #ifdef USE_LIBCDIO
        createCDDriveContextMenu(&id);
        #endif
        addContextMenuItem(hMenu, id++, &cmiNewFolder, false);
        addContextMenuItem(hMenu, id++, &cmiNewFile, false);
    }

    POINT cursor;
    GetCursorPos(&cursor);
    // 必须用 TPM_RETURNCMD 同步取回选中项（同 navbar.c/treeview.c 的写法）。
    // Wine 选中菜单项后是 PostMessage 投递 WM_COMMAND（win32u/menu.c），在
    // TrackPopupMenu 返回之后才被派发 —— 若返回后就销毁菜单，WM_COMMAND
    // 到达时 hContextMenu 已为 NULL，分发永远失败，右键菜单全部静默无反应。
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, cursor.x, cursor.y, 0, hwndContentView, NULL);

    // dwItemData 会随菜单一起销毁，必须在 DestroyMenu 前取回。
    // GetMenuItemInfo 按命令 ID 查找时 Wine 会递归子菜单（挂载镜像/注册表命令都在子菜单里）。
    struct ContextMenuItem* cmItem = NULL;
    if (cmd) {
        MENUITEMINFO item = {0};
        item.cbSize = sizeof(MENUITEMINFO);
        item.fMask = MIIM_DATA;
        if (GetMenuItemInfo(hMenu, cmd, FALSE, &item)) cmItem = (struct ContextMenuItem*)item.dwItemData;
    }
    hContextMenu = NULL;
    DestroyMenu(hMenu);

    if (!cmItem) return;

    if (cmItem->cmdData) {
        // 缓冲区按命令实际长度分配：固定 MAX_PATH 会静默丢弃较长的自定义命令
        size_t cmdLen = wcslen(cmItem->cmdData) + 4;
        wchar_t* command = malloc(cmdLen * sizeof(wchar_t));
        if (command) {
            wcscpy_s(command, cmdLen, L"/C ");
            wcscat_s(command, cmdLen, cmItem->cmdData);
            execCommandLine(command);   // 所有权移交（内部线程负责释放）
        }
        navigateRefresh();
    }
    else if (cmItem->proc) cmItem->proc();
}

// 绘制时反复 CreateSolidBrush/DeleteObject 会带来可观的 GDI 开销，这里按颜色复用
static HBRUSH brushDriveFree = NULL;
static HBRUSH brushDriveLow = NULL;
static HBRUSH brushDriveMid = NULL;
static HBRUSH brushDriveHigh = NULL;
static HBRUSH brushIconSelLabel = NULL;   // 大图标视图选中态的标签底色

static HBRUSH getUiBrush(HBRUSH* slot, COLORREF color) {
    if (!*slot) *slot = CreateSolidBrush(color);
    return *slot;
}

// 按需加载单个条目的图标 / 类型名 / 大小 / 日期。
// 只在 LVN_GETDISPINFO（即可见行）里调用 —— 排序阶段绝不能调用它，
// 否则虚拟列表会被物化，排序时就把整个目录的图标都解析一遍。
static void loadItemData(struct ListItem* item, int idx) {
    if (!item || !item->node || item->loaded) return;
    struct FileNode* node = item->node;

    // 图标与类型名都由自建图标库一次性给出：resolveIconId 内部按「路径 / 扩展名 /
    // 固定标识」去重，命中已登记项时连 shell 都不碰，所以这里的代价与目录规模无关。
    item->icon = resolveIconId(item);
    const wchar_t* typeName = iconIdTypeName(item->icon);
    wcsncpy_s(item->type, 64, typeName ? typeName : L"", _TRUNCATE);

    if (node->type == TYPE_FILE) {
        formatFileSize(item->size, item->formattedSize);
    }
    else if (node->type == TYPE_DIR) {
        // 「大小」列对文件夹显示**条目数**。
        // 有缓存值就直接格式化（纯计算）；没有则排队给后台线程数，这一帧先留空 ——
        // 绝不能在这里同步枚举目录：本函数的调用点有两个在绘制帧上（itemIconReady /
        // iconFillRangeInner），详见「后台文件夹条目数计数」一节。
        // 只有详细信息视图有「大小」这一列，图标/列表视图不必白白去枚举目录。
        if (node->childItemCount >= 0) {
            if (node->childItemCount >= FOLDER_COUNT_CAP)
                swprintfTrunc(item->formattedSize, 64, L"%d+ %ls", node->childItemCount, lc_str.items);
            else
                swprintfTrunc(item->formattedSize, 64, L"%d %ls", node->childItemCount, lc_str.items);
        }
        else if (viewStyle == STYLE_DETAILS) {
            folderCountRequest(idx);
        }
    }
    else if (node->type == TYPE_DRIVE) {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(node, path);
        wchar_t rootPath[4] = {0};
        swprintfTrunc(rootPath, 4, L"%lc:\\", path[0]);
        ULARGE_INTEGER freeBytesAvail, totalBytes, freeBytesTotal;
        if (GetDiskFreeSpaceExW(rootPath, &freeBytesAvail, &totalBytes, &freeBytesTotal)) {
            item->driveTotalBytes = totalBytes.QuadPart;
            item->driveFreeBytes = freeBytesAvail.QuadPart;
            formatDriveSizeText(item);
        }
    }

    // 修改日期：文件与**文件夹**都显示。目录的 ftLastWriteTime 在枚举时就已经拿到
    // （FindNextFile 一并填好，见 file_node.c），所以这里对文件夹做的事与对文件完全一样 ——
    // 两次时间转换 + 一次格式化，不多调任何 API，代价与「可见行数」成正比而不是目录规模。
    // 固定节点（驱动器 / 桌面 / 文档 / 用户 / 计算机 / 书签）没有真实的修改时间，不显示。
    if ((node->type == TYPE_FILE || node->type == TYPE_DIR) && !isZeroFileTime(&item->modifiedTime)) {
        SYSTEMTIME systemTime = {0};
        FILETIME localFiletime;
        if (FileTimeToLocalFileTime(&item->modifiedTime, &localFiletime) && FileTimeToSystemTime(&localFiletime, &systemTime)) {
            formatModifiedDate(systemTime.wMonth, systemTime.wDay, systemTime.wYear, systemTime.wHour, systemTime.wMinute, item->formattedDate, 32);
        }
    }

    item->loaded = true;
}

// 大图标视图条目自绘。Wine 的 ListView 默认只给非选中项画一行截断的文件名
// （选中后才展开多行），这与 Windows 不一致，也无法靠调整格子高度改变，
// 所以完全接管绘制：图标居中在上，文件名多行换行铺在下方。
// ListView 的文本色：只要没人调用过 LVM_SETTEXTCOLOR，Wine 的 LVM_GETTEXTCOLOR
// 返回的就是 CLR_DEFAULT（commctrl.h 里 0xff000000）。而 GDI 的 SetTextColor 只取
// 低 24 位，CLR_DEFAULT 于是成了纯黑 —— 浅色主题下看着没问题，深色主题下就是
// 「深色背景 + 黑字」，等于看不见。
// Wine 自己绘制条目时对 CLR_DEFAULT 的处理是回落系统色（comctl32/listview.c 的
// prepaint_setup：`if (textcolor == CLR_DEFAULT) textcolor = clrWindowText`，而
// comctl32 的系统色缓存又直接来自 GetSysColor），这里照同一条规则解析，
// 大图标视图的文字颜色就和其他视图保持一致。
static COLORREF getContentTextColor(void) {
    COLORREF c = ListView_GetTextColor(hwndContentView);
    return (c == (COLORREF)CLR_DEFAULT) ? GetSysColor(COLOR_WINDOWTEXT) : c;
}

static void drawLargeIconItem(NMCUSTOMDRAW* nmcd, struct ListItem* item) {
    HDC hdc = nmcd->hdc;
    int itemIndex = (int)nmcd->dwItemSpec;

    // 不信任通知携带的矩形（Wine 各版本给的矩形不一致），直接取完整格子
    RECT rc;
    if (!ListView_GetItemRect(hwndContentView, itemIndex, &rc, LVIR_BOUNDS))
        rc = nmcd->rc;

    // 选中/焦点位以**控件**为准，通知里的 uItemState 只当补充。
    //
    // Wine 的 uItemState 来自 comctl32/listview.c 的 customdraw_fill()，而那份
    // state 是 LISTVIEW_GetItemT() 在 owner-data 分支里拼出来的：先把 state 清 0
    // （listview.c:6705），回调 LVN_GETDISPINFO 取应用侧数据，最后再把「由控件
    // 自己负责」的那两位或回去（6768 焦点 / 6776 选中）。这条链任何一环没走到
    // 最后，通知里就是 0 —— 表现就是「明明选中了，蓝底却没画出来」，而且因为
    // 触发条件与绘制时机相关，看上去是**概率性**丢的。
    //
    // LVM_GETITEMSTATE 走的是同一个函数的收尾分支，但在 owner-data 下它是纯内存
    // 查询（mask 只有 LVIF_STATE，不满足 6709/6710 的回调条件，不会反过来调我们
    // 的 LVN_GETDISPINFO），直接给出控件记录的真实选中/焦点位。只在该位缺席时补问
    // 一次，正常路径一次额外调用都不多花。
    bool selected = (nmcd->uItemState & CDIS_SELECTED) != 0;
    bool focused = (nmcd->uItemState & CDIS_FOCUS) != 0;
    if (!selected) {
        selected = (ListView_GetItemState(hwndContentView, itemIndex, LVIS_SELECTED)
                    & LVIS_SELECTED) != 0;
    }
    if (!focused) {
        focused = (ListView_GetItemState(hwndContentView, itemIndex, LVIS_FOCUSED)
                   & LVIS_FOCUSED) != 0;
    }

    // 图标：水平居中放在格子顶部（TOP_PAD 与 Wine 的 ICON_TOP_PADDING 一致）
    int iconSize = iconViewIconSize;
    int iconX = rc.left + ((rc.right - rc.left) - iconSize) / 2;
    int iconY = rc.top + ICONVIEW_TOP_PAD;
    if (currentImageList && item->loaded) {
        // item->icon 是自建图标库的 id，这里换成当前显示列表（iconViewIconSize 那本）
        // 里的槽位索引。只查已渲染好的槽（见 getIconSlotForPaint）；缺失的项由
        // iconFillStep() 补上后重画这一格。
        int imageIdx = getIconSlotForPaint(item);
        if (imageIdx >= 0) {
            ImageList_DrawEx(currentImageList, imageIdx, hdc, iconX, iconY, 0, 0,
                             CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
        }
    }

    // 文件名：图标下方，文字区宽度与 updateIconViewLayout 的测量宽度严格
    // 一致（格子宽 - 2×MARGIN_X），换行结果必然相同，预留高度必然够用。
    // 固定行数模式把底边夹在恰好 N 行：格子自带的底部留白如果也留给
    // DrawTextW，DT_EDITCONTROL 会把多出来的那几像素画成多出的一行
    RECT labelRc = rc;
    labelRc.top = iconY + iconSize + ICONVIEW_ICON_GAP;
    labelRc.left = rc.left + ICONVIEW_MARGIN_X;
    labelRc.right = rc.right - ICONVIEW_MARGIN_X;
    if (iconViewLabelLines > 0)
        labelRc.bottom = labelRc.top + iconViewLabelLines * iconViewLineHeight;

    if (selected) {
        RECT selRc = labelRc;
        selRc.left -= 2;
        selRc.right += 2;
        FillRect(hdc, &selRc, getUiBrush(&brushIconSelLabel, GetSysColor(COLOR_HIGHLIGHT)));
    }

    SetBkMode(hdc, TRANSPARENT);
    if (selected) SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
    else if (item->isHidden) SetTextColor(hdc, RGB(160, 160, 160));  // 与其他视图的隐藏文件颜色一致
    else SetTextColor(hdc, getContentTextColor());

    // 无限行模式：不带 DT_END_ELLIPSIS、带 DT_NOCLIP —— Wine 的 DrawTextW
    // 只在 rect 装不下剩余文字时才画省略号，这里直接让它没有任何截断手段；
    // 固定行数模式：超出 N 行的部分正常加省略号
    UINT fmt = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL;
    if (iconViewLabelLines > 0) fmt |= DT_END_ELLIPSIS;
    else fmt |= DT_NOCLIP;

    HFONT hOldFont = SelectObject(hdc, hGuiFont);
    DrawTextW(hdc, item->node->name, -1, &labelRc, fmt);
    SelectObject(hdc, hOldFont);

    if (focused) DrawFocusRect(hdc, &labelRc);
}

LRESULT contentViewNotify(NMHDR* nmhdr) {
    switch (nmhdr->code) {
        case NM_CUSTOMDRAW: {
            NMLVCUSTOMDRAW* lvcd = (NMLVCUSTOMDRAW*)nmhdr;
            switch (lvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    int idx = (int)lvcd->nmcd.dwItemSpec;
                    // 大图标视图：完全自绘条目（Wine 默认对非选中项只画一行文件名）
                    if (viewStyle == STYLE_LARGE_ICON) {
                        if (idx >= 0 && idx < numItems && items[idx].node) {
                            if (!items[idx].loaded) loadItemData(&items[idx], idx);
                            drawLargeIconItem(&lvcd->nmcd, &items[idx]);
                            return CDRF_SKIPDEFAULT;
                        }
                        return CDRF_DODEFAULT;
                    }
                    if (driveBarMode == DRIVE_BAR_GRAPH && idx >= 0 && idx < numItems
                        && items[idx].node->type == TYPE_DRIVE && items[idx].driveTotalBytes > 0) {
                        return CDRF_NOTIFYSUBITEMDRAW;
                    }
                    // 隐藏文件用灰色文字
                    if (idx >= 0 && idx < numItems && items[idx].isHidden) {
                        lvcd->clrText = RGB(160, 160, 160);
                    }
                    return CDRF_DODEFAULT;
                }
                case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                    int idx = (int)lvcd->nmcd.dwItemSpec;
                    if (idx >= 0 && idx < numItems && items[idx].node->type == TYPE_DRIVE
                        && items[idx].driveTotalBytes > 0 && lvcd->iSubItem == COLUMN_SIZE_IDX) {
                        
                        struct ListItem* item = &items[idx];
                        HDC hdc = lvcd->nmcd.hdc;
                        RECT rc = lvcd->nmcd.rc;
                        
                        // Margin inside the cell
                        InflateRect(&rc, -2, -1);

                        // 选中态：整行高亮是控件铺的底，而这一格被我们 SKIPDEFAULT 接管，
                        // 必须自己跟着铺高亮色 —— 否则整行的蓝底会正好在这一格断开
                        // （长度条所在的是"大小"列，位于行的中段，最显眼）。
                        // 判定同 drawLargeIconItem：先看通知里的 uItemState，缺席时补问控件；
                        // 子项阶段 Wine 不会再刷 uItemState（listview.c 的子项循环只更新
                        // nmcd.rc 与 iSubItem），所以拿到的仍是条目级状态，含 CDIS_SELECTED。
                        bool sel = (lvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                        if (!sel) {
                            sel = (ListView_GetItemState(hwndContentView, idx, LVIS_SELECTED)
                                   & LVIS_SELECTED) != 0;
                        }

                        // 选中时这一格不画长度条，整行铺同一片高亮色：长度条的浅蓝/黄/红
                        // 与高亮蓝撞色后边界根本看不出来，而压在条上的那段文字在深色浅色
                        // 两头都不好读。取消选中后长度条自动回来（总量/剩余仍在类型与
                        // 大小列里）。这样选中一行就是一条连续的蓝底，不再断在中间。
                        if (sel) {
                            FillRect(hdc, &rc, getUiBrush(&brushIconSelLabel, GetSysColor(COLOR_HIGHLIGHT)));
                        }
                        else {
                            // Background bar: light gray for free space
                            FillRect(hdc, &rc, getUiBrush(&brushDriveFree, RGB(230, 235, 240)));

                            // Used space bar
                            double usedPct = (double)(item->driveTotalBytes - item->driveFreeBytes)
                                           / (double)item->driveTotalBytes;
                            if (usedPct < 0.0) usedPct = 0.0;
                            if (usedPct > 1.0) usedPct = 1.0;
                            int usedWidth = (int)((rc.right - rc.left) * usedPct);

                            if (usedWidth > 0) {
                                RECT usedRc = rc;
                                usedRc.right = rc.left + usedWidth;

                                // Color: blue (<80%), yellow (80-90%), red (>90%)
                                COLORREF barColor;
                                if (usedPct < 0.8) barColor = RGB(100, 181, 246);
                                else if (usedPct < 0.9) barColor = RGB(255, 213, 79);
                                else barColor = RGB(239, 154, 154);

                                HBRUSH* slot = (barColor == RGB(100, 181, 246)) ? &brushDriveLow :
                                               (barColor == RGB(255, 213, 79))  ? &brushDriveMid : &brushDriveHigh;
                                FillRect(hdc, &usedRc, getUiBrush(slot, barColor));
                            }
                        }

                        // Text overlay
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(50, 50, 50));
                        HFONT hOldFont = SelectObject(hdc, hGuiFont);
                        DrawTextW(hdc, item->formattedSize, -1, &rc,
                                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
                        SelectObject(hdc, hOldFont);
                        
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            return CDRF_DODEFAULT;
        }
        case LVN_GETDISPINFO: {
            NMLVDISPINFO* nmlvdi = (NMLVDISPINFO*)nmhdr;
            UINT mask = nmlvdi->item.mask;
            if (!items || nmlvdi->item.iItem < 0 || nmlvdi->item.iItem >= numItems) break;
            struct ListItem* item = &items[nmlvdi->item.iItem];
            
            if (!item->loaded) loadItemData(item, nmlvdi->item.iItem);
            if (!item->node) break;

            // 不回应 LVIF_STATE：owner-data 模式下选中/焦点状态由控件自行维护。
            // 旧实现无条件把 state 清 0（且从不设置 stateMask），语义是错的 ——
            // 这里保持不修改，由控件决定。

            if (mask & LVIF_IMAGE) {
                // item->icon 是自建图标库的 id（与显示尺寸无关），这里换成当前显示
                // 列表里的槽位索引 —— 所有视图样式走同一条路，不再区分大/小图标列表。
                // 只查已渲染好的槽（见 getIconSlotForPaint），缺失的项由 iconFillStep()
                // 补上后重画；绝不在这里渲染，否则首帧要等整屏图标提完（闪白）。
                nmlvdi->item.iImage = getIconSlotForPaint(item);
            }
            
            if (mask & LVIF_TEXT) {
                switch (nmlvdi->item.iSubItem) {
                    case COLUMN_NAME_IDX:
                        nmlvdi->item.pszText = item->node->name;
                        break;
                    case COLUMN_TYPE_IDX:
                        nmlvdi->item.pszText = item->type;
                        break;
                    // 这一格的三元判断是**显示层的门**，与 loadItemData 里的格式化门是两回事 ——
                    // 格式化过的东西在这里被换成 L""，屏幕上就是空白（改 loadItemData 必须同步改这里）。
                    // 大小：文件＝容量、驱动器＝容量条（自绘时这里只当文本兜底）、文件夹＝条目数
                    // （后台数完才填，没数出来时 formattedSize 是空串，显示空白）。
                    case COLUMN_SIZE_IDX:
                        nmlvdi->item.pszText = (item->node->type == TYPE_FILE || item->node->type == TYPE_DRIVE
                                                || item->node->type == TYPE_DIR) ? item->formattedSize : L"";
                        break;
                    // 日期：文件与**文件夹**都显示 —— 目录的 ftLastWriteTime 枚举时就已拿到（见
                    // file_node.c），代价与文件完全相同。固定节点（驱动器 / 桌面 / 文档 / 用户 /
                    // 计算机 / 书签）没有真实修改时间，保持空白（formattedDate 空串）。
                    case COLUMN_DATE_IDX:
                        nmlvdi->item.pszText = (item->node->type == TYPE_FILE || item->node->type == TYPE_DIR)
                                               ? item->formattedDate : L"";
                        break;
                    case COLUMN_PATH_IDX: {
                        if (!item->path) {
                            wchar_t path[MAX_PATH] = {0};
                            getFileNodePath(item->node, path);
                            item->path = wcsdup(path);
                        }
                        nmlvdi->item.pszText = item->path;
                        break;
                    }                       
                }
            }           
            break;
        }
        case LVN_ITEMCHANGED: {
            // 大图标视图的蓝底是**我们自绘**的（见 drawLargeIconItem），而 Wine 在
            // 选中状态变化时只失效「它自己算出来的」那个项矩形。位置数据刚变过时
            // （例如刚进目录、首帧绘制还没跑完）那个矩形可能还没算对，于是出现
            // 「点了但蓝底不出来，过一会儿/下次重绘才补上」——看着就是概率性丢失，
            // 严重时第二下点击（打开）都到了蓝底还没画出来，像是单击直接进去。
            // 这里按我们自己取的格子矩形再失效一次：只重画一个格子，代价可忽略。
            NMLISTVIEW* nmlv = (NMLISTVIEW*)nmhdr;
            if (viewStyle == STYLE_LARGE_ICON && nmlv->iItem >= 0
                && nmlv->iItem < numItems) {
                RECT rc;
                if (ListView_GetItemRect(hwndContentView, nmlv->iItem, &rc, LVIR_BOUNDS))
                    InvalidateRect(hwndContentView, &rc, TRUE);
            }
            break;
        }
        case NM_RCLICK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;

            // 不限制 iSubItem：启用整行选中后，右键落在哪一列都应当弹出菜单
            if (nmia->iItem != -1) {
                updateSelectedItems();
                
                bool show = true;
                for (int i = 0; i < numSelectedItems; i++) {
                    if (!(selectedItems[i]->type == TYPE_FILE || selectedItems[i]->type == TYPE_DIR)) {
                        show = false;
                        break;
                    }
                }
                if (show) createContextMenu(numSelectedItems == 1 ? MENU_SINGLE : MENU_MULTIPLE);
            }
            else createContextMenu(MENU_EMPTY);         
            break;
        }
        case NM_DBLCLK: {
            NMITEMACTIVATE* nmia = (NMITEMACTIVATE*)nmhdr;
            if (nmia->iItem == -1) break;
            
            if (!items || nmia->iItem >= numItems) break;
            struct ListItem* item = &items[nmia->iItem];            
            openFileNode(item->node);
            break;
        }
        case LVN_COLUMNCLICK: {
            LPNMLISTVIEW plvInfo = (LPNMLISTVIEW)nmhdr;

            if (plvInfo->iSubItem == sortColumnIdx) {
                sortAscending = !sortAscending;
            }
            else {
                sortColumnIdx = plvInfo->iSubItem;
                sortAscending = true;
            }

            refreshContentView();
            break;
        }       
    }

    return 0;   
}

// 按路径字符串建立一条属于搜索线程的节点链，返回最深一级（搜索起点）。
// 拆分方式与 setCurrPathFromString 保持一致：第一段是盘符。
static struct FileNode* createSearchRootNode(struct SearchNodePool* pool, const wchar_t* path) {
    if (!pool || !path || path[0] == L'\0') return NULL;

    wchar_t tmp[MAX_PATH] = {0};
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);

    struct FileNode* leaf = NULL;
    wchar_t* saveptr = NULL;
    wchar_t* token = wcstok(tmp, L"\\", &saveptr);
    int i = 0;
    while (token) {
        enum FileType type = (i++ == 0) ? TYPE_DRIVE : TYPE_DIR;
        struct FileNode* node = allocSearchNode(pool, token, type);
        if (!node) return NULL;
        node->parent = leaf;
        leaf = node;
        token = wcstok(NULL, L"\\", &saveptr);
    }
    return leaf;
}

// 只读枚举目录项并在搜索节点池里建链 —— 绝不修改（更不能释放）UI 线程的文件树。
static struct FileNode* enumChildrenForSearch(struct SearchNodePool* pool, struct FileNode* parent) {
    if (!pool || !parent) return NULL;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(parent, path);
    if (path[0] == L'\0') return NULL;
    if (wcslen(path) + 2 >= MAX_PATH) return NULL;
    wcscat_s(path, MAX_PATH, path[wcslen(path) - 1] == L'\\' ? L"*" : L"\\*");

    WIN32_FIND_DATA wfd = {0};
    HANDLE handle = FindFirstFile(path, &wfd);
    if (handle == INVALID_HANDLE_VALUE) return NULL;

    struct FileNode* first = NULL;
    struct FileNode* last = NULL;
    do {
        if (wfd.cFileName[0] == L'.' && (wfd.cFileName[1] == L'\0' ||
            (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))) continue;
        if (!g_showHiddenFiles && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;

        bool isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        struct FileNode* child = allocSearchNode(pool, wfd.cFileName, isDir ? TYPE_DIR : TYPE_FILE);
        if (!child) continue;
        child->parent = parent;
        child->isHidden = (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        if (isDir) {
            child->hasChildDirs = true;
        }
        else {
            LARGE_INTEGER filesize;
            filesize.LowPart = wfd.nFileSizeLow;
            filesize.HighPart = wfd.nFileSizeHigh;
            child->size = filesize.QuadPart;
        }
        // 与 file_node.c 的 buildChildNodes 保持一致：日期对目录也取（FindNextFile 已经填好）。
        memcpy(&child->modifiedTime, &wfd.ftLastWriteTime, sizeof(FILETIME));

        if (!first) first = child;
        if (last) last->sibling = child;
        last = child;
    }
    while (FindNextFile(handle, &wfd));
    FindClose(handle);
    return first;
}

static DWORD WINAPI searchTask(void* param) {
    struct SearchData* searchData = (struct SearchData*)param;
    const int maxStackSize = 50;
    struct FileNode* stack[maxStackSize];
    int stackSize = 0;

    struct SearchNodePool* pool = calloc(1, sizeof(struct SearchNodePool));
    wchar_t keyword[64] = {0};
    strToLower(searchData->keyword, keyword, 64);

    // 批大小 = 搜索线程与 UI 线程的同步粒度：每批一次 SendMessage，而它是同步的，
    // 搜索线程必须等 UI 线程处理完（items 扩容 + SetItemCountEx + 状态栏节流）才返回。
    // 100 太密：10000 项上限下要同步 100 次。400 把它降到 25 次，同时结果仍是
    // 分批渐进出现的，用户感知不到差别。
    const int BATCH_SIZE = 400;
    struct BatchItems batch;
    batch.capacity = BATCH_SIZE;
    batch.nodes = malloc(batch.capacity * sizeof(struct FileNode*));
    batch.count = 0;

    int cacheCapacity = 1000;
    struct FileNode** cacheResults = malloc(cacheCapacity * sizeof(struct FileNode*));
    int cacheCount = 0;
    int addedCount = 0;   // 本线程自己的计数，不再读 UI 线程的 numItems

    if (!pool || !batch.nodes || !cacheResults) {
        free(batch.nodes);
        free(cacheResults);
        searchData->pool = pool;
        SendMessage(hwndContentView, MSG_SEARCH_DONE, 0, 0);
        return 0;
    }

    // 搜索起点：用线程自己建立的节点链，完全不依赖 UI 线程正在使用的文件树
    struct FileNode* root = createSearchRootNode(pool, searchData->rootPath);
    if (root) {
        struct FileNode* rootChildren = enumChildrenForSearch(pool, root);
        if (rootChildren) stack[stackSize++] = rootChildren;
    }

    while (stackSize > 0 && addedCount < 10000 && searchData->active) {
        struct FileNode* node = stack[--stackSize];
        while (node && searchData->active) {
            if (wcsstrIgnoreCase(node->name, keyword)) {
                batch.nodes[batch.count++] = node;

                if (cacheCount >= cacheCapacity) {
                    int newCap = cacheCapacity * 2;
                    struct FileNode** tmp = realloc(cacheResults, newCap * sizeof(struct FileNode*));
                    if (!tmp) break;
                    cacheResults = tmp;
                    cacheCapacity = newCap;
                }
                cacheResults[cacheCount++] = node;
                
                if (batch.count >= BATCH_SIZE) {
                    SendMessage(hwndContentView, MSG_ADD_ITEMS_BATCH, 0, (LPARAM)&batch);
                    addedCount += batch.count;
                    batch.count = 0;
                }
            }
            
            if (addedCount >= 10000) break;
            
            if (node->type == TYPE_DIR && stackSize < maxStackSize) {
                struct FileNode* children = enumChildrenForSearch(pool, node);
                if (children) stack[stackSize++] = children;
            }
            node = node->sibling;       
        }
    }
    
    if (batch.count > 0 && searchData->active) {
        SendMessage(hwndContentView, MSG_ADD_ITEMS_BATCH, 0, (LPARAM)&batch);
        addedCount += batch.count;
    }
    
    free(batch.nodes);

    // 结果与节点池一起交给 UI 线程处理（MSG_SEARCH_DONE 里决定并入缓存还是释放）
    if (searchData->active) {
        searchData->results = cacheResults;
        searchData->resultCount = cacheCount;
    }
    else {
        free(cacheResults);
        searchData->results = NULL;
        searchData->resultCount = 0;
    }
    searchData->pool = pool;

    SendMessage(hwndContentView, MSG_SEARCH_DONE, 0, 0);
    return 0;
}

static bool isSearchCacheValid(const wchar_t* path, const wchar_t* keyword) {
    if (searchCache.count == 0) return false;
    if (wcscmp(searchCache.path, path) != 0) return false;
    if (wcscmp(searchCache.keyword, keyword) != 0) return false;
    // 搜索线程按当时的 g_showHiddenFiles 过滤：开关变了，旧结果就不再可信
    if (searchCache.showHidden != g_showHiddenFiles) return false;
    time_t now = time(NULL);
    if (now - searchCache.timestamp > 30) return false;
    return true;
}

void createLVColumns();

// 取消当前搜索（不等待线程退出，避免死锁）
static void cancelSearch() {
    if (searchData == NULL) return;
    if (searchData->active) {
        searchData->active = false;
        searchData->canceled = true;
    }
}

void searchFor(wchar_t* keyword) {
    if (wcslen(keyword) == 0) return;
    // 如果有搜索正在进行或正在清理中，取消并把新关键词挂起：
    // 等 MSG_SEARCH_DONE 清理完 searchData 后自动用新关键词重新发起。
    // 旧实现直接 return，用户必须再点一次搜索按钮才生效。
    if (searchData != NULL) {
        if (searchData->active) {
            searchData->active = false;
            searchData->canceled = true;
        }
        // 截断拷贝：搜索框文本超长时 wcscpy_s 会触发约束处理器而非静默截断
        wcsncpy_s(pendingSearchKeyword, 64, keyword, _TRUNCATE);
        return;
    }
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    
    if (isSearchCacheValid(path, keyword)) {
        // 先保存缓存数据到局部变量，因为clearContentView会清空缓存
        int cachedCount = searchCache.count;
        struct FileNode** cachedResults = searchCache.results;
        // 结果节点由节点池拥有，必须连同池一起移出，否则会被 clearContentView 释放
        struct SearchNodePool* cachedPool = searchCache.pool;
        searchCache.results = NULL;
        searchCache.count = 0;
        searchCache.pool = NULL;

        clearContentView();
        createLVColumns();

        LVCOLUMN column = {0};
        column.mask = LVCF_WIDTH | LVCF_TEXT;
        column.cx = 250;
        column.pszText = lc_str.path;
        ListView_InsertColumn(hwndContentView, COLUMN_PATH_IDX, &column);

        items = malloc(cachedCount * sizeof(struct ListItem));
        if (items) {
            itemsCapacity = cachedCount;
            numItems = cachedCount;

            for (int i = 0; i < cachedCount; i++) {
                struct ListItem* item = &items[i];
                memset(item, 0, sizeof(struct ListItem));
                item->node = cachedResults[i];
                item->path = NULL;
                item->loaded = false;
                fillFileInfo(cachedResults[i], item);
            }
        }
        else {
            itemsCapacity = 0;
            numItems = 0;
        }

        // 重新缓存（指针仍然有效，因为currPathFileNode未变）
        searchCache.results = cachedResults;
        searchCache.count = cachedCount;
        searchCache.pool = cachedPool;
        searchCache.timestamp = time(NULL);

        ListView_SetItemCountEx(hwndContentView, numItems, 0);
        updateStatusbar();
        return;
    }

    clearContentView();
    createLVColumns();

    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;
    column.cx = 250;
    column.pszText = lc_str.path;
    ListView_InsertColumn(hwndContentView, COLUMN_PATH_IDX, &column);
    UpdateWindow(hwndContentView);
    
    searchData = calloc(1, sizeof(struct SearchData));
    if (!searchData) return;
    wcscpy_s(searchData->keyword, 64, keyword);
    // 搜索起点路径在此处（UI 线程）抓取，搜索线程不再读取 UI 线程的文件树
    wcsncpy_s(searchData->rootPath, MAX_PATH, path, _TRUNCATE);
    searchData->active = true;
    searchData->canceled = false;
    searchData->results = NULL;
    searchData->resultCount = 0;
    searchData->pool = NULL;
    searchData->threadHandle = CreateThread(NULL, 0, searchTask, searchData, 0, NULL);
    if (!searchData->threadHandle) {
        // 线程创建失败：自己清理，否则 refreshContentView 会一直被 searchData 挡住
        free(searchData);
        searchData = NULL;
    }
}

static void saveViewStyle(void);

void setViewStyle(enum ViewStyle newViewStyle) {
    LONG_PTR wndstyle = GetWindowLongPtr(hwndContentView, GWL_STYLE);
    wndstyle &= ~LVS_TYPEMASK;
    // 图标视图必须带 LVS_AUTOARRANGE：Wine 的 WM_SIZE 处理只有在该样式下
    // 才会随窗口宽度变化重排图标，否则窗口放大后右侧留白、缩小后要横向滚动
    wndstyle &= ~LVS_AUTOARRANGE;

    switch (newViewStyle) {
        case STYLE_LARGE_ICON:
            wndstyle |= LVS_ICON | LVS_AUTOARRANGE;
            break;
        case STYLE_SMALL_ICON:
            wndstyle |= LVS_SMALLICON | LVS_AUTOARRANGE;
            break;
        case STYLE_LIST:
            wndstyle |= LVS_LIST;
            break;
        case STYLE_DETAILS:
            wndstyle |= LVS_REPORT;
            break;
    }

    SetWindowLongPtr(hwndContentView, GWL_STYLE, wndstyle);
    // 强制 ListView 识别样式变更并重新布局
    SetWindowPos(hwndContentView, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 这里不再需要清图标缓存：item->icon 现在是「图标来源 id」，与显示尺寸无关
    // （旧实现存的是系统图像列表索引，大/小列表索引空间不同 —— 那才是 README 里
    // 「图标混淆」的根因）。换视图只需换一份显示列表池，来源表原样复用。
    viewStyle = newViewStyle;
    // 布局统一由下面这次调用负责，让 refreshContentView 跳过它自己那次
    // （正常情况下两者等价，纯属重复；见 skipLayoutInRefresh 的说明）
    skipLayoutInRefresh = true;
    refreshContentView();
    skipLayoutInRefresh = false;

    // 图标视图需要重新排列：样式切换时 LISTVIEW_StyleChanged 会按旧的条目数排布。
    // 大图标视图改用 updateIconViewLayout：先按设置定格子尺寸再 Arrange。
    if (viewStyle == STYLE_LARGE_ICON || viewStyle == STYLE_SMALL_ICON) {
        if (viewStyle == STYLE_LARGE_ICON) {
            updateIconViewLayout();
            // Arrange 之后要重设一次 ItemCount 触发 LISTVIEW_UpdateScroll 修正滚动
            // 范围。原实现里这一步紧跟在 refreshContentView 的那次布局之后，而这次
            // 布局被 skipLayoutInRefresh 跳过了，所以在这里补上，保持顺序不变。
            ListView_SetItemCountEx(hwndContentView, numItems, 0);
        }
        else {
            ListView_Arrange(hwndContentView, LVA_DEFAULT);
            InvalidateRect(hwndContentView, NULL, FALSE);
        }
    }

    // 持久化视图样式到注册表
    saveViewStyle();
    // 更新菜单栏选中标记。updateIconViewMenuCheckmarks 顺带按新视图切换
    // 「大图标视图」父项的可用/置灰状态，切换视图必须补这一次刷新。
    updateViewMenuCheckmarks();
    updateIconViewMenuCheckmarks();
}

static void saveViewStyle(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)viewStyle;
        RegSetValueEx(hkey, L"ViewStyle", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

enum ViewStyle loadViewStyle(void) {
    HKEY hkey;
    DWORD val = (DWORD)STYLE_DETAILS;
    DWORD size = sizeof(val);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegQueryValueEx(hkey, L"ViewStyle", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hkey);
    }
    if (val > STYLE_DETAILS) val = (DWORD)STYLE_DETAILS;
    return (enum ViewStyle)val;
}

void updateViewMenuCheckmarks(void) {
    if (!hMenuView) return;
    UINT first = ID_VIEW_LARGEICONS;
    UINT last  = ID_VIEW_DETAILS;
    UINT check;
    switch (viewStyle) {
        case STYLE_LARGE_ICON:  check = ID_VIEW_LARGEICONS; break;
        case STYLE_SMALL_ICON:  check = ID_VIEW_SMALLICONS; break;
        case STYLE_LIST:        check = ID_VIEW_LIST;       break;
        case STYLE_DETAILS:
        default:                check = ID_VIEW_DETAILS;    break;
    }
    CheckMenuRadioItem(hMenuView, first, last, check, MF_BYCOMMAND);
}

static void saveFolderSortMode(void) {
    HKEY hkey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)folderSortMode;
        RegSetValueEx(hkey, L"FolderSortMode", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hkey);
    }
}

void loadFolderSortMode(void) {
    HKEY hkey;
    // 未配置时默认「经典」——与 folderSortMode 的静态初值保持一致
    DWORD val = (DWORD)FOLDER_SORT_CLASSIC;
    DWORD size = sizeof(val);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegQueryValueEx(hkey, L"FolderSortMode", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hkey);
    }
    // 注册表可能被写入了越界值或根本不是 REG_DWORD，这里统一回落到默认值
    if (val > (DWORD)FOLDER_SORT_PLAIN) val = (DWORD)FOLDER_SORT_CLASSIC;
    folderSortMode = (enum FolderSortMode)val;
}

void updateFolderSortMenuCheckmarks(void) {
    if (!hMenuFolderSort) return;
    UINT check;
    switch (folderSortMode) {
        case FOLDER_SORT_TOP:    check = ID_VIEW_FOLDER_TOP;     break;
        case FOLDER_SORT_BOTTOM: check = ID_VIEW_FOLDER_BOTTOM;  break;
        case FOLDER_SORT_PLAIN:  check = ID_VIEW_FOLDER_PLAIN;   break;
        case FOLDER_SORT_CLASSIC:
        default:                 check = ID_VIEW_FOLDER_CLASSIC; break;
    }
    CheckMenuRadioItem(hMenuFolderSort, ID_VIEW_FOLDER_CLASSIC, ID_VIEW_FOLDER_PLAIN,
                       check, MF_BYCOMMAND);
}

void setFolderSortMode(enum FolderSortMode newMode) {
    if (newMode < FOLDER_SORT_CLASSIC || newMode > FOLDER_SORT_PLAIN) {
        newMode = FOLDER_SORT_CLASSIC;
    }
    folderSortMode = newMode;

    // 不需要重新枚举目录：refreshContentView() 会从 currPathFileNode 的子链表
    // 重建 items[] 并在末尾 sortItems()，所以策略切换是「就地重排」级别的开销。
    refreshContentView();

    saveFolderSortMode();
    updateFolderSortMenuCheckmarks();
}

void createLVColumns() {
    LVCOLUMN column = {0};
    column.mask = LVCF_WIDTH | LVCF_TEXT;

    column.cx = 220;
    column.pszText = lc_str.name;
    ListView_InsertColumn(hwndContentView, COLUMN_NAME_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.type;
    ListView_InsertColumn(hwndContentView, COLUMN_TYPE_IDX, &column);

    column.cx = 170;
    column.pszText = lc_str.size;
    ListView_InsertColumn(hwndContentView, COLUMN_SIZE_IDX, &column);

    column.cx = 100;
    column.pszText = lc_str.date;
    ListView_InsertColumn(hwndContentView, COLUMN_DATE_IDX, &column);
}

void createContentView() {
    hwndContentView = CreateWindowEx(0, WC_LISTVIEW, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_BORDER | LVS_OWNERDATA | LVS_REPORT | LVS_SHAREIMAGELISTS,
                                     0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);

    // 不启用 LVS_EX_DOUBLEBUFFER：它会把 Wine 的列表绘制切到"拦截 WM_ERASEBKGND +
    // 内存 DC"路径，在 Winlator 的 GL 合成环境下出现列表内容（含图标）不显示/不刷新。
    // 恢复原版绘制路径；但 LVS_EX_FULLROWSELECT 要留着（它跟 DOUBLEBUFFER 无关，
    // 不碰内存 DC，见下）。
    //
    // LVS_EX_FULLROWSELECT：详细视图里选中一行时整行连成一片高亮底色，与 Windows
    // 资源管理器一致。Wine 在**没有**该扩展样式时是按「单元格内容的宽度」画选中底色的
    // —— comctl32/listview.c 的 LISTVIEW_DrawItemPart：
    //     background = &rcSelect;                    // 选中 且 非 FULLROWSELECT
    //     rcSelect.right = min(Label.left + labelSize.cx, Label.right);
    // 也就是蓝色只铺到该格文字的末尾。于是名称列只蓝到文件名尾巴、后面的类型/大小/日期
    // 各自只蓝出自己那一小段文字 —— 看上去正是「蓝底中间断开、一格一截」。
    // 加上 FULLROWSELECT 后 Wine 改画 rcLabel（名称列 = 整列宽、其余列 = 列右边界，
    // 见 LISTVIEW_GetItemMetrics 里 `labelSize.cx = nItemWidth` 那条分支），各列连成整行。
    ListView_SetExtendedListViewStyle(hwndContentView, LVS_EX_FULLROWSELECT);

    cmiOpen.text = lc_str.open;
    cmiEdit.text = lc_str.edit;
    cmiCut.text = lc_str.cut;
    cmiCopy.text = lc_str.copy;
    cmiCreateShortcut.text = lc_str.create_shortcut;
    cmiDelete.text = lc_str.delete;
    cmiRename.text = lc_str.rename;
    cmiPaste.text = lc_str.paste;
    cmiPasteShortcut.text = lc_str.paste_shortcut;
    cmiNewFolder.text = lc_str.new_folder;
    cmiNewFile.text = lc_str.new_file;
#ifdef USE_LIBCDIO
    cmiLoadISOImage.text = NULL;
    cmiUnloadISOImage.text = lc_str.unload_iso_image;
#endif
    cmiShowIcon.text = lc_str.show_icon;
    cmiOpenWith.text = lc_str.open_with_menu;
    cmiOpenFileLocation.text = lc_str.open_file_location;
    cmiOpenLinkTarget.text = lc_str.open_link_target;
    cmiImportReg.text = lc_str.import_reg;
    
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndContentView, GWLP_WNDPROC, (LONG_PTR)ContentViewWndProc);
    createLVColumns();
    UpdateWindow(hwndContentView);
}

void onMenuItemUpClick() {
    navigateUp();
}

void onMenuItemOpenClick() {
    if (numSelectedItems == 1) openFileNode(selectedItems[0]);
}

static void onMenuItemOpenWithClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        wchar_t path[MAX_PATH] = {0};
        wchar_t parentPath[MAX_PATH] = {0};
        getFileNodePath(selectedItems[0], path);
        getFileNodePath(selectedItems[0]->parent, parentPath);
        
        wchar_t* ext = wcsrchr(selectedItems[0]->name, L'.');
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

void onMenuItemEditClick() {
    if (numSelectedItems == 1 && selectedItems[0]->type == TYPE_FILE) {
        static const wchar_t editorPath[] = L"C:\\windows\\notepad.exe";
        
        wchar_t path[MAX_PATH] = {0};
        wchar_t parameters[MAX_PATH + 8] = {0};
        getFileNodePath(selectedItems[0], path);
        if (wcschr(path, L'"')) return;   // 含引号的路径无法安全拼进命令行
        swprintfTrunc(parameters, MAX_PATH + 8, L"\"%ls\"", path);
        getFileNodePath(selectedItems[0]->parent, path);
        ShellExecute(hwndMain, L"open", editorPath, parameters, path, SW_SHOW);     
    }   
}

void onMenuItemCutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) cutFiles(selectedItems, numSelectedItems);    
}

void onMenuItemCopyClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) copyFiles(selectedItems, numSelectedItems);
}

void onMenuItemCreateShortcutClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) createDesktopShortcuts(selectedItems, numSelectedItems);  
}

void onMenuItemDeleteClick() {
    updateSelectedItems();
    if (numSelectedItems > 0) deleteFiles(selectedItems, numSelectedItems); 
}

void onMenuItemRenameClick() {
    if (numSelectedItems == 1) {
        wchar_t* result = InputDialog(lc_str.rename, lc_str.enter_new_name, selectedItems[0]->name, true);
        if (result) {
            wchar_t newFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0]->parent, newFilename);
            wcscat_s(newFilename, MAX_PATH, L"\\");
            wcscat_s(newFilename, MAX_PATH, result);
            free(result);
            
            wchar_t oldFilename[MAX_PATH] = {0};
            getFileNodePath(selectedItems[0], oldFilename);
            MoveFileW(oldFilename, newFilename);
            navigateRefresh();
        }
    }
}

void onMenuItemPasteClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteFiles(path);
}

void onMenuItemPasteShortcutClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    pasteShortcuts(path);   
}

// 放弃待粘贴的内容：「反悔不想粘贴了」。只清空系统剪贴板里的文件数据
// （CF_HDROP + Preferred DropEffect），文件本身原封不动，也不会碰其他程序
// 放进剪贴板的内容（clearClipboard 内部有格式判断）。
// 清空后 onClipboardChanged() 会把状态栏来源指示、工具栏粘贴按钮、编辑菜单
// 与右键菜单里这一项的置灰状态一并同步。
static void onMenuItemClearClipboardClick() {
    clearClipboard();
}

void onMenuItemNewFolderClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    
    wchar_t* result = InputDialog(lc_str.new_folder, lc_str.enter_folder_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);
        
        if (!isPathExists(path)) {
            CreateDirectory(path, NULL);
            navigateRefresh();          
        }
    }   
}

void onMenuItemNewFileClick() {
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(currPathFileNode, path);
    if (!isPathExists(path)) return;
    
    wchar_t* result = InputDialog(lc_str.new_file, lc_str.enter_file_name, NULL, false);
    if (result) {
        wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, result);
        free(result);
        
        if (!isPathExists(path)) {
            HANDLE handle = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            navigateRefresh();  
        }
    }       
}

void onMenuItemSelectAllClick() {
    ListView_SetItemState(hwndContentView, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(hwndContentView, -1, LVIS_SELECTED, LVIS_SELECTED);
    SetFocus(hwndContentView);
}

static bool isInSearchMode() {
    HWND hHeader = ListView_GetHeader(hwndContentView);
    if (!hHeader) return false;
    return Header_GetItemCount(hHeader) > COLUMN_PATH_IDX;
}

static void onMenuItemOpenFileLocationClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;

    struct FileNode* node = selectedItems[0];
    if (!node || !node->parent) return;

    // 仅支持文件和文件夹
    if (node->type != TYPE_FILE && node->type != TYPE_DIR) return;

    // 先保存路径字符串，因为导航后旧文件树会被释放，node指针失效
    wchar_t parentPath[MAX_PATH] = {0};
    getFileNodePath(node->parent, parentPath);
    wcscpy_s(pendingSelectName, MAX_PATH, node->name);

    // 路径为空或不存在则提示
    if (parentPath[0] == L'\0' || !isPathExists(parentPath)) {
        wchar_t msg[MAX_PATH + 64] = {0};
        swprintf_s(msg, MAX_PATH + 64, lc_str.bookmark_path_not_found,
                   parentPath[0] ? parentPath : node->name);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK);
        return;
    }

    // 如果搜索还在运行，先取消，否则refreshContentView会提前返回导致悬空指针
    cancelSearch();

    // 延迟导航：菜单回调深调用链会导致栈溢出，用PostMessage在回调返回后执行
    wcscpy_s(pendingNavigatePath, MAX_PATH, parentPath);
    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
}

// 定位 lnk 的目标：解析快捷方式真正指向的路径，跳到目标所在目录并选中目标本身。
// 目标是文件还是目录都定位到其父目录，与「打开文件所在位置」的语义保持一致。
static void onMenuItemOpenLinkTargetClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;

    wchar_t lnkPath[MAX_PATH] = {0};
    getFileNodePath(selectedItems[0], lnkPath);
    if (lnkPath[0] == L'\0') return;

    wchar_t targetPath[MAX_PATH] = {0};
    if (!resolveLnkTargetPath(lnkPath, targetPath, MAX_PATH)) {
        wchar_t msg[MAX_PATH + 64] = {0};
        swprintfTrunc(msg, MAX_PATH + 64, lc_str.msg_link_target_not_found, selectedItems[0]->name);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
        return;
    }

    // 后面要导航（旧文件树会被释放），所以路径与文件名都先算好、只留字符串
    wchar_t parentDir[MAX_PATH] = {0};
    getParentDirFromPath(targetPath, parentDir);
    // 目标是盘根下的文件时 getParentDirFromPath 只给出 "C:"，补上反斜杠：getFileNodePath
    // 给驱动器节点的形式是 "C:\"，setCurrPathFromString 也按这个形式解析
    if (parentDir[0] && parentDir[1] == L':' && parentDir[2] == L'\0') {
        parentDir[2] = L'\\';
        parentDir[3] = L'\0';
    }

    if (parentDir[0] == L'\0' || !isPathExists(parentDir)) {
        wchar_t msg[MAX_PATH + 64] = {0};
        swprintfTrunc(msg, MAX_PATH + 64, lc_str.msg_link_target_not_found, targetPath);
        MessageBox(hwndMain, msg, lc_str.alert, MB_OK | MB_ICONWARNING);
        return;
    }

    getBasenameFromPath(targetPath, pendingSelectName, MAX_PATH, false);

    // 搜索还在跑时先取消，否则 refreshContentView 会提前返回导致悬空指针
    cancelSearch();

    // 延迟导航：菜单回调深调用链直接导航会栈溢出，用 PostMessage 在回调返回后执行
    wcscpy_s(pendingNavigatePath, MAX_PATH, parentDir);
    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
}

void onBookmarkButtonClick() {
    addCurrentPathToBookmark();
}

static void onMenuItemImportRegClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;
    if (selectedItems[0]->type != TYPE_FILE) return;

    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(selectedItems[0], path);
    if (path[0] == L'\0') return;

    // 使用 regedit.exe /s 静默导入 .reg 文件
    // 先提示用户确认
    wchar_t msg[MAX_PATH + 64] = {0};
    swprintf_s(msg, MAX_PATH + 64, L"%ls\n\n%ls", selectedItems[0]->name, lc_str.import_reg);
    int result = MessageBox(hwndMain, msg, lc_str.import_reg, MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES) {
        if (!wcschr(path, L'"')) {   // 含引号的路径会逃逸引号，直接拒绝
            wchar_t params[MAX_PATH + 8] = {0};
            swprintfTrunc(params, MAX_PATH + 8, L"/s \"%ls\"", path);
            ShellExecute(hwndMain, L"open", L"regedit.exe", params, NULL, SW_SHOW);
        }
    }
}

#ifdef USE_LIBCDIO
static void onMenuItemLoadISOImageClick() {
    if (numSelectedItems != 1) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    wchar_t currentISOPath[MAX_PATH] = {0};
    HKEY hkey;
    getFileNodePath(selectedItems[0], currentISOPath);

    if (!isPathExists(currentISOPath) || !(hasFileExtension(currentISOPath, L"iso") ||
                                           hasFileExtension(currentISOPath, L"bin") ||
                                           hasFileExtension(currentISOPath, L"cue"))) {
        MessageBox(NULL, lc_str.msg_invalid_iso_image_file, lc_str.alert, MB_OK);
        return;
    }

    if (GetDriveTypeW(L"X:\\") == DRIVE_NO_ROOT_DIR) {
        MessageBox(hwndMain, lc_str.msg_x_drive_not_found, lc_str.alert, MB_OK | MB_ICONWARNING);
        return;
    }

    if (RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        RegSetValue(hkey, NULL, REG_SZ, currentISOPath, (wcslen(currentISOPath) + 1) * sizeof(wchar_t));
        RegCloseKey(hkey);
    }

    clearDirectory(L"X:");
    extractFilesFromISOImage(currentISOPath, L"X:\\");
}

void onMenuItemUnloadISOImageClick() {
    // S9：clearDirectory(L"X:") 是递归永久删除，而 README 指导用户把 X: 软链到真实目录。
    // 一次误点就会清空真实目录，因此这里必须先二次确认。
    if (MessageBox(hwndMain, lc_str.msg_confirm_unmount_iso, lc_str.unmount_iso,
                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    clearDirectory(L"X:");
    RegDeleteKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath");
    navigateRefresh();
}

void onMenuItemLocateISOImageClick() {
    wchar_t currentISOPath[MAX_PATH] = {0};
    if (!getCurrentISOPath(currentISOPath)) {
        MessageBox(NULL, lc_str.msg_no_mounted_image, lc_str.alert, MB_OK);
        return;
    }
    wchar_t parentDir[MAX_PATH] = {0};
    getParentDirFromPath(currentISOPath, parentDir);
    if (!isPathExists(parentDir)) {
        MessageBox(NULL, lc_str.msg_image_dir_not_found, lc_str.alert, MB_OK);
        return;
    }
    // 提取ISO文件名，导航后选中该文件
    wchar_t isoFileName[MAX_PATH] = {0};
    getBasenameFromPath(currentISOPath, isoFileName, MAX_PATH, false);
    wcscpy_s(pendingSelectName, MAX_PATH, isoFileName);
    
    // 如果搜索还在运行，先取消
    cancelSearch();
    
    // 延迟导航：使用消息机制确保导航后能选中文件
    wcscpy_s(pendingNavigatePath, MAX_PATH, parentDir);
    PostMessage(hwndContentView, MSG_NAVIGATE_TO_PATH, 0, 0);
}
#endif /* USE_LIBCDIO */

// ========== GDI+ PNG decoder (bypasses Wine's buggy PNG icon loading) ==========
#include <shlwapi.h>

/* GDI+ flat API - manually declared for C99 compatibility */
typedef int GpStatus;
typedef void GpBitmap;
typedef void GpImage;
typedef struct {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;
typedef struct { int dummy; } GdiplusStartupOutput;
#define WINGDIPAPI __stdcall
#define GDIPCONST const

/* GpStatus values */
enum { Ok = 0 };

/* Forward declarations for GDI+ flat API (gdiplus.dll) */
GpStatus WINGDIPAPI GdiplusStartup(ULONG_PTR*, GDIPCONST GdiplusStartupInput*, GdiplusStartupOutput*);
VOID     WINGDIPAPI GdiplusShutdown(ULONG_PTR);
GpStatus WINGDIPAPI GdipCreateBitmapFromStream(IStream*, GpBitmap**);
GpStatus WINGDIPAPI GdipCreateHICONFromBitmap(GpBitmap*, HICON*);
GpStatus WINGDIPAPI GdipDisposeImage(GpImage*);

static ULONG_PTR g_gdiplusToken = 0;

static void initGdiplus(void) {
    if (g_gdiplusToken) return;
    GdiplusStartupInput input;
    memset(&input, 0, sizeof(input));
    input.GdiplusVersion = 1;
    GdiplusStartup(&g_gdiplusToken, &input, NULL);
}

/* Decode PNG data directly using GDI+, bypassing Wine's load_png which has
   a bug where png_set_bgr() is missing for 24-bit RGB PNGs (R/B swapped).
   Returns an HICON or NULL on failure. Caller must DestroyIcon(). */
static HICON createIconFromPngData(const BYTE* data, DWORD dataSize) {
    if (!data || dataSize < 8) return NULL;
    // 上限校验：这是不受信任的 PNG 数据交给 GDI+ 解析的攻击面，先限制体积
    if (dataSize > 64u * 1024u * 1024u) return NULL;

    /* Verify PNG signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A */
    static const BYTE pngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(data, pngSig, 8) != 0) return NULL;

    initGdiplus();

    /* Create IStream from memory */
    IStream* pStream = SHCreateMemStream(data, dataSize);
    if (!pStream) return NULL;

    /* Decode PNG to GDI+ Bitmap */
    GpBitmap* pBitmap = NULL;
    GpStatus status = GdipCreateBitmapFromStream(pStream, &pBitmap);
    IStream_Release(pStream);

    if (status != Ok || !pBitmap) return NULL;

    /* Convert Bitmap to HICON */
    HICON hIcon = NULL;
    status = GdipCreateHICONFromBitmap(pBitmap, &hIcon);
    GdipDisposeImage((GpImage*)pBitmap);

    return (status == Ok) ? hIcon : NULL;
}

// ========== PE icon extraction (bypasses Wine's buggy icon APIs) ==========

#pragma pack(push, 2)
typedef struct {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    WORD nID;
} GRPICONDIRENTRY;

typedef struct {
    WORD idReserved;
    WORD idType;
    WORD idCount;
    GRPICONDIRENTRY idEntries[1];
} GRPICONDIR;
#pragma pack(pop)

// 把 RT_ICON / .ico 条目里的图像资源解成 32bpp straight-alpha 像素（top-down，
// 步长 = 边长×4），返回 malloc 缓冲（调用方 free()），失败返回 NULL。
//
// 这是 ICO 资源格式的内核：BITMAPINFOHEADER + 调色板 + XOR 位图 + AND 掩码。
// 以前的实现直接把它写进 DIB 段再 CreateIconIndirect 成 HICON，上层拿到 HICON 后
// 又用 GetIconInfo 把位图深拷贝出来才能缩放 —— 一个图标来回搬两趟整幅像素。
// 现在输出裸缓冲，上层全程在缓冲上选帧 / 缩放 / 叠角标，最后只建一次 HICON。
static BYTE* decodeIconDataToPixels(const BYTE* data, DWORD dataSize, int* outW, int* outH) {
    *outW = *outH = 0;
    if (!data || dataSize < sizeof(BITMAPINFOHEADER)) return NULL;

    const BITMAPINFOHEADER* bih = (const BITMAPINFOHEADER*)data;
    int width = bih->biWidth;
    // biHeight 是「XOR + AND 两幅」的合计高度，每幅各占一半。负数 = 自顶向下 DIB，
    // 这里不做特判：除 2 后仍是负数，下面的 height <= 0 会把它挡掉，交给 GDI+/Wine
    // 那条 HICON 路径（它们本来就按 biHeight 的符号选行序）。
    int height = bih->biHeight ? bih->biHeight / 2 : 0;
    int bpp = bih->biBitCount;

    // 只认标准 BITMAPINFOHEADER + BI_RGB，别的形态一律拒绝、交给兜底路径 —— 拒绝是
    // 安全的（后面还有 GDI+/Wine 的 HICON 路径），硬解才是危险的：
    //  · biSize 是 PNG 签名（0x474E5089）= Vista+ 的 PNG 压缩帧，后面根本没有调色板；
    //  · BITMAPCOREHEADER(12) / V4(108) / V5(124) 的字段布局与行步长规则都不同；
    //  · BI_BITFIELDS(3) 在头部后面多挂 3 个 DWORD 通道掩码，不认它就会把掩码当成
    //    像素读 —— 整幅颜色错位三个像素，正是「错误图标」的经典长相。
    if (bih->biSize != sizeof(BITMAPINFOHEADER) || bih->biCompression != BI_RGB) return NULL;

    // 尺寸上限：宽高直接来自被解析文件，必须限制，否则下面的乘法会溢出
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;

    const BYTE* xorData = data + sizeof(BITMAPINFOHEADER);
    size_t xorRowSize;

    // 行步长一律 DWORD 对齐（1/4/8bpp 是「位行」，每行补到 32 位）
    if (bpp == 32) {
        xorRowSize = (size_t)width * 4;
    } else if (bpp == 24) {
        xorRowSize = ((size_t)width * 3 + 3) / 4 * 4;
    } else if (bpp == 8) {
        xorRowSize = ((size_t)width + 3) / 4 * 4;
    } else if (bpp == 4) {
        xorRowSize = (((size_t)width + 1) / 2 + 3) / 4 * 4;
    } else if (bpp == 1) {
        xorRowSize = ((size_t)width + 31) / 32 * 4;
    } else {
        return NULL;
    }

    size_t xorTotalSize = xorRowSize * (size_t)height;
    size_t andRowSize = ((size_t)width + 31) / 32 * 4;
    size_t andTotalSize = andRowSize * (size_t)height;

    // 色表长度：biClrUsed 优先（上限 256），为 0 才用 2^bpp；>8bpp 没有色表。
    // 但 biClrUsed 有写错的文件（声明值小于实际写入的项数），所以再用「色表 + XOR +
    // 掩码必须整体落在资源内」反查一次：按声明值算不下就退回满色表。两个候选都不成立
    // 才放弃（边界检查全部用 size_t：旧实现用 int，宽高过大时 xorTotalSize 溢出为小值，
    // 检查通过后按巨大尺寸越界读像素）。
    size_t paletteSize = 0;
    {
        size_t declared = bih->biClrUsed
            ? (bih->biClrUsed > 256 ? 256 : (size_t)bih->biClrUsed)
            : (bpp > 8 ? 0 : (size_t)1 << bpp);
        size_t full = (bpp > 8) ? 0 : (size_t)1 << bpp;
        size_t candidates[2] = { declared, full };
        int candidateCount = (declared == full) ? 1 : 2;
        BOOL found = FALSE;
        for (int i = 0; i < candidateCount && !found; i++) {
            if (sizeof(BITMAPINFOHEADER) + candidates[i] * 4 + xorTotalSize + andTotalSize <= dataSize) {
                paletteSize = candidates[i] * 4;
                found = TRUE;
            }
        }
        if (!found) return NULL;
    }

    const BYTE* xorPixels = xorData + paletteSize;
    const BYTE* andData = xorPixels + xorTotalSize;

    BYTE* pixels = (BYTE*)malloc((size_t)width * height * 4);
    if (!pixels) return NULL;

    // Convert XOR data to 32bpp ARGB (source is bottom-up in ICO format)，
    // 同时记录是否真存在 alpha 通道（只有 32bpp 才可能有，其余格式一律靠掩码）
    BOOL hasAlpha = FALSE;
    for (int y = 0; y < height; y++) {
        const BYTE* srcRow = xorPixels + (size_t)(height - 1 - y) * xorRowSize;
        BYTE* dstRow = pixels + (size_t)y * width * 4;
        if (bpp == 32) {
            memcpy(dstRow, srcRow, (size_t)width * 4);
            for (int x = 0; x < width && !hasAlpha; x++)
                if (dstRow[x * 4 + 3]) hasAlpha = TRUE;
        } else if (bpp == 24) {
            for (int x = 0; x < width; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // B
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // R
                dstRow[x * 4 + 3] = 255;
            }
        } else if (bpp == 8) {
            const BYTE* palette = xorData;
            for (int x = 0; x < width; x++) {
                BYTE idx = srcRow[x];
                dstRow[x * 4 + 0] = palette[idx * 4 + 0]; // B
                dstRow[x * 4 + 1] = palette[idx * 4 + 1]; // G
                dstRow[x * 4 + 2] = palette[idx * 4 + 2]; // R
                dstRow[x * 4 + 3] = 255;
            }
        } else if (bpp == 4) {
            const BYTE* palette = xorData;
            for (int x = 0; x < width; x++) {
                BYTE val = srcRow[x / 2];
                BYTE idx = (x & 1) ? (val & 0x0F) : (val >> 4);
                dstRow[x * 4 + 0] = palette[idx * 4 + 0];
                dstRow[x * 4 + 1] = palette[idx * 4 + 1];
                dstRow[x * 4 + 2] = palette[idx * 4 + 2];
                dstRow[x * 4 + 3] = 255;
            }
        } else if (bpp == 1) {
            // 单色图标：色表恒为「黑, 白」，索引就是它的颜色（XOR 位 1 = 白）。
            // 必须自己解：这种帧在 Wine 里走 is_dib_monochrome 分支
            // （user32/cursoricon.c:156，bpp==1 且色表是黑+白），只建 2×高度的掩码位图、
            // **不建颜色位图** → GetIconInfo 的 hbmColor 为 NULL，我们的 HICON 兜底路径
            // 连尺寸都拿不到，最后只能退回通用图标。拒绝它 = 系统里所有单色图标全错。
            const BYTE* palette = xorData;
            for (int x = 0; x < width; x++) {
                BYTE bit = (srcRow[x / 8] >> (7 - (x % 8))) & 1;
                dstRow[x * 4 + 0] = palette[bit * 4 + 0];
                dstRow[x * 4 + 1] = palette[bit * 4 + 1];
                dstRow[x * 4 + 2] = palette[bit * 4 + 2];
                dstRow[x * 4 + 3] = 255;
            }
        }
    }

    // If no alpha (common Wine bug for multi-icon EXEs), use AND mask.
    //
    // 掩码不是一段独立数据，它就是**同一个 DIB 的后半幅**：行序与 XOR 位图完全一致
    // （biHeight 为正 = 自下而上，文件第 0 行是图像最下面一行）。以前这里漏了翻转
    // —— XOR 翻了、掩码没翻 —— 于是「本该透明的背景」按镜像行去查：查不到的那些
    // 格子保持不透明，露出 XOR 里的背景色（调色板 0 = 黑），这就是「exe 图标带黑边」
    // 的根因（AkelPad 的 48x48 帧最明显：左侧一整块黑三角 + 上下两排黑齿）。
    // Wine 侧同样把掩码当普通 DIB 直接交给 StretchDIBits（user32/cursoricon.c 的
    // create_icon_frame 只是 `bmi_copy->biHeight /= 2` 后原样拷掩码位），与 XOR 同序。
    if (!hasAlpha) {
        for (int y = 0; y < height; y++) {
            const BYTE* andRow = andData + (size_t)(height - 1 - y) * andRowSize;
            for (int x = 0; x < width; x++) {
                BOOL transparent = (andRow[x / 8] >> (7 - (x % 8))) & 1;

                int px = (y * width + x) * 4;
                if (transparent) {
                    pixels[px + 0] = 0;
                    pixels[px + 1] = 0;
                    pixels[px + 2] = 0;
                    pixels[px + 3] = 0;
                } else {
                    pixels[px + 3] = 255;
                }
            }
        }
    }

    *outW = width;
    *outH = height;
    return pixels;
}

// Create HICON from raw ICO image data（保留 HICON 形态的入口：图标查看器要用）。
static HICON createIconFromRawData(const BYTE* data, DWORD dataSize) {
    int width = 0, height = 0;
    BYTE* pixels = decodeIconDataToPixels(data, dataSize, &width, &height);
    if (!pixels) return NULL;
    HICON hIcon = createIconFromPixels(pixels, width, height);
    free(pixels);
    return hIcon;
}

// 枚举容量的独立上限：文件里的图标组数可以远超 MAX_ICON_GROUPS（那是图标查看
// 窗口的分组上限），而「按资源 ID 找组」必须能覆盖到靠后的组 —— shell32.dll 有
// 64 个组，且 IDI_SHELL_FOLDER 之类的 ID 并不总是排在最前。
#define ICON_GROUP_ENUM_MAX 128

struct GroupIconEnumData {
    HRSRC hRes[ICON_GROUP_ENUM_MAX];
    int ids[ICON_GROUP_ENUM_MAX];   // 对应的资源 ID（命名资源记 -1）
    int count;
};

static BOOL CALLBACK enumGroupIconProc(HMODULE hModule, LPCWSTR lpType, LPWSTR lpName, LONG_PTR lParam) {
    (void)lpType;   // 枚举时只关心 RT_GROUP_ICON，类型参数不使用
    struct GroupIconEnumData* data = (struct GroupIconEnumData*)lParam;
    if (data->count < ICON_GROUP_ENUM_MAX) {
        HRSRC hRes = FindResourceW(hModule, lpName, RT_GROUP_ICON);
        if (hRes) {
            // 整数 ID 就是 lpName 本身（MAKEINTRESOURCE 的形式）；命名资源记 -1
            data->ids[data->count] = IS_INTRESOURCE(lpName) ? (int)(ULONG_PTR)lpName : -1;
            data->hRes[data->count++] = hRes;
        }
    }
    return TRUE; // Continue enumerating
}

// Extract ALL icon groups from a PE file, each with all its icon sizes.
// Returns number of icon groups extracted. Populates iconGroups[].
static int extractAllIconGroupsFromPE(const wchar_t* filePath) {
    for (int i = 0; i < MAX_ICON_GROUPS; i++) {
        iconGroups[i].iconCount = 0;
        for (int j = 0; j < MAX_ICONS_PER_GROUP; j++) {
            iconGroups[i].icons[j] = NULL;
        }
    }
    if (!filePath) return 0;

    HMODULE hModule = LoadLibraryExW(filePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hModule) return 0;

    struct GroupIconEnumData enumData = {0};
    EnumResourceNamesW(hModule, RT_GROUP_ICON, enumGroupIconProc, (LONG_PTR)&enumData);

    int totalGroups = 0;
    for (int g = 0; g < enumData.count && totalGroups < MAX_ICON_GROUPS; g++) {
        HGLOBAL hGlob = LoadResource(hModule, enumData.hRes[g]);
        if (!hGlob) continue;

        const GRPICONDIR* grpDir = (const GRPICONDIR*)LockResource(hGlob);
        if (!grpDir) continue;

        int count = 0;
        WORD nIcons = grpDir->idCount;
        if (nIcons > MAX_ICONS_PER_GROUP) nIcons = MAX_ICONS_PER_GROUP;

        for (WORD i = 0; i < nIcons; i++) {
            const GRPICONDIRENTRY* entry = &grpDir->idEntries[i];

            HRSRC hIconRes = FindResourceW(hModule, MAKEINTRESOURCE(entry->nID), RT_ICON);
            if (!hIconRes) continue;

            DWORD iconResSize = SizeofResource(hModule, hIconRes);
            HGLOBAL hIconGlob = LoadResource(hModule, hIconRes);
            if (!hIconGlob) continue;

            const BYTE* iconData = (const BYTE*)LockResource(hIconGlob);
            if (!iconData || iconResSize == 0) continue;

            HICON hIcon = createIconFromRawData(iconData, iconResSize);
            if (!hIcon) {
                /* Try GDI+ PNG decoder first (bypasses Wine's load_png R/B swap bug) */
                hIcon = createIconFromPngData(iconData, iconResSize);
            }
            if (!hIcon) {
                /* Last resort: use Wine's API (may have color issues for some formats) */
                hIcon = CreateIconFromResourceEx((PBYTE)iconData, iconResSize,
                    TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
            }
            if (hIcon) {
                int size = entry->bWidth;
                if (size == 0) size = 256; // 0 means 256 in ICO format
                iconGroups[totalGroups].icons[count] = hIcon;
                iconGroups[totalGroups].sizes[count] = size;
                count++;
            }
        }

        if (count > 0) {
            iconGroups[totalGroups].iconCount = count;
            totalGroups++;
        }
    }

    FreeLibrary(hModule);
    return totalGroups;
}

// ========== 单图标提取（工具栏 CMD/Explorer 按钮、lnk 目标图标用） ==========

// ICO 条目的「最接近目标尺寸」选择：先取不小于目标的尺寸中最小的（精确命中
// 自然胜出），都不够大则取最大的；尺寸并列取位深更高的。bWidth==0 表示 256。
static bool isBetterIconEntry(int side, int depth, int bestSide, int bestDepth, int cxDesired) {
    int over = side - cxDesired;
    int bestOver = bestSide - cxDesired;
    if ((over >= 0) != (bestOver >= 0)) return over >= 0;
    if (over != bestOver) return (over >= 0) ? (over < bestOver) : (over > bestOver);
    return depth > bestDepth;
}

// 从 PE 文件提取第 groupIndex 个图标组（EnumResourceNamesW 的枚举顺序，与
// extractAllIconGroupsFromPE 一致）中最接近目标尺寸的图标，与批量提取一样
// 绕开 Wine 有颜色反转 bug 的 ExtractIconEx 系图标 API。工具栏启动时用
// index 0（主图标），lnk 解析用 GetIconLocation 给出的索引。
// 返回的 HICON 由调用方 DestroyIcon()，失败返回 NULL。
// groupIndex 有两种含义（与 shell 的约定一致）：
//   >= 0  RT_GROUP_ICON 的枚举序号（0 = 主图标，工具栏与 exe 用这个）
//   <  0  按资源 ID 查找，ID = -groupIndex —— 目录（IDI_SHELL_FOLDER=4，shell 给
//         出的就是 -4）、驱动器、注册过扩展名走的都是这种负数坐标
// 打开 PE 里的图标资源，返回模块句柄（调用方负责 FreeLibrary）+ 资源数据指针/长度，
// 并只按 desired 算出最合适的那一帧。失败返回 NULL。
//
// 单独抽出来是为了让「取 HICON」和「取像素」两条路共用同一套定位逻辑。注意数据指针
// 只在模块活着期间有效（LOAD_LIBRARY_AS_DATAFILE 是文件映射视图），调用方必须在拿到
// 的 hModule 上完成解码再 FreeLibrary。
static HMODULE peOpenIconResource(const wchar_t* pePath, int groupIndex, int desired,
                                  const BYTE** outData, DWORD* outSize) {
    *outData = NULL;
    *outSize = 0;
    if (!pePath || !pePath[0]) return NULL;

    HMODULE hModule = LoadLibraryExW(pePath, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hModule) return NULL;

    struct GroupIconEnumData enumData = {0};
    EnumResourceNamesW(hModule, RT_GROUP_ICON, enumGroupIconProc, (LONG_PTR)&enumData);

    if (groupIndex < 0) {
        int wantId = -groupIndex;
        groupIndex = -1;
        for (int i = 0; i < enumData.count; i++) {
            if (enumData.ids[i] == wantId) { groupIndex = i; break; }
        }
    }
    if (groupIndex < 0 || groupIndex >= enumData.count) {
        FreeLibrary(hModule);
        return NULL;
    }

    HRSRC hGroupRes = enumData.hRes[groupIndex];
    HGLOBAL hGroupGlob = hGroupRes ? LoadResource(hModule, hGroupRes) : NULL;
    const GRPICONDIR* grpDir = hGroupGlob ? (const GRPICONDIR*)LockResource(hGroupGlob) : NULL;
    if (!grpDir || grpDir->idType != 1 || grpDir->idCount == 0) {
        FreeLibrary(hModule);
        return NULL;
    }

    // 防畸形 PE：目录声明的条目数必须落在资源实际大小内，否则越界读
    DWORD groupSize = SizeofResource(hModule, hGroupRes);
    if (groupSize < 6 || (DWORD)grpDir->idCount > (groupSize - 6) / sizeof(GRPICONDIRENTRY)) {
        FreeLibrary(hModule);
        return NULL;
    }

    int bestIndex = -1;
    int bestSide = 0;
    int bestDepth = 0;
    for (WORD i = 0; i < grpDir->idCount; i++) {
        const GRPICONDIRENTRY* entry = &grpDir->idEntries[i];
        int w = entry->bWidth ? entry->bWidth : 256;
        int h = entry->bHeight ? entry->bHeight : 256;
        // 非方形帧直接跳过：解码器（要求 w == h）和 HICON 兜底（getIconPixelsSquare）
        // 都用不了它 —— 选中它这次渲染必然失败、最后退到通用图标。语料里这类帧都是
        // 「只有 76×24」的工具栏条（跳过前后结局都是退回兜底），但侧边若继续用
        // max(w,h) 参与比较，它还会在 desired 落进 (32, 76] 时**挤掉**真正可用的
        // 32×32 方形帧（isBetterIconEntry 优先「不小于目标」），所以要显式排除。
        if (w != h) continue;
        int side = w;
        if (bestIndex < 0 ||
            isBetterIconEntry(side, entry->wBitCount, bestSide, bestDepth, desired)) {
            bestIndex = i;
            bestSide = side;
            bestDepth = entry->wBitCount;
        }
    }
    if (bestIndex < 0) {
        FreeLibrary(hModule);
        return NULL;
    }

    const GRPICONDIRENTRY* entry = &grpDir->idEntries[bestIndex];
    HRSRC hIconRes = FindResourceW(hModule, MAKEINTRESOURCE(entry->nID), RT_ICON);
    HGLOBAL hIconGlob = hIconRes ? LoadResource(hModule, hIconRes) : NULL;
    const BYTE* iconData = hIconGlob ? (const BYTE*)LockResource(hIconGlob) : NULL;
    DWORD iconResSize = hIconRes ? SizeofResource(hModule, hIconRes) : 0;
    if (!iconData || iconResSize == 0) {
        FreeLibrary(hModule);
        return NULL;
    }

    *outData = iconData;
    *outSize = iconResSize;
    return hModule;
}

// 取 PE 图标组里最合适的那一帧，解成**原生尺寸**的 32bpp 像素缓冲（方形，边长写回
// *outSize）。失败返回 NULL。
//
// 三级 fallback 与批量提取一致：自带解码器（BITMAPINFOHEADER 资源）→ GDI+（现代 exe
// 的 256 JUMBO 常是 PNG 压缩帧，Wine 的 load_png 有 B/R 颠倒 bug 才走 GDI+）→
// Wine 的 CreateIconFromResourceEx。后两条只能吐 HICON，所以各多一次像素往返，
// 但它们只在「自带解码器啃不动」时才发生。
static BYTE* extractPeIconPixels(const wchar_t* pePath, int groupIndex, int desired, int* outSize) {
    *outSize = 0;
    if (!pePath || !pePath[0] || desired <= 0) return NULL;

    BYTE* pixels = NULL;
    const BYTE* data = NULL;
    DWORD len = 0;
    HMODULE hModule = peOpenIconResource(pePath, groupIndex, desired, &data, &len);

    if (hModule) {
        int w = 0, h = 0;
        pixels = decodeIconDataToPixels(data, len, &w, &h);
        if (pixels && w != h) {   // 非方形（畸形资源）：交给后面的 HICON 路线处理
            free(pixels);
            pixels = NULL;
        }

        if (pixels) {
            *outSize = w;
        }
        else {
            HICON hIcon = createIconFromPngData(data, len);
            // 0/0 = 按帧原生尺寸建，不许 Wine 替我们拉伸（它的择帧会挑小帧放大）
            if (!hIcon) hIcon = CreateIconFromResourceEx((PBYTE)data, len,
                                                         TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
            if (hIcon) {
                pixels = getIconPixelsSquare(hIcon, outSize);
                DestroyIcon(hIcon);
            }
        }
        FreeLibrary(hModule);
    }

    if (!pixels) {
        // 最后手段：Wine 自己的完整图标 API（按路径 + 序号/负 ID 自己找资源）。
        // 老实现把它当兜底，专门救「上面两条都啃不动」的位深/PNG 变体，保留同一层
        // 保险。代价是它会按 desired 拉伸（可能选到小帧再放大），所以只放在最后。
        HICON hIcon = NULL;
        if (PrivateExtractIconsW(pePath, groupIndex, desired, desired, &hIcon, NULL, 1, LR_DEFAULTCOLOR) && hIcon) {
            pixels = getIconPixelsSquare(hIcon, outSize);
            DestroyIcon(hIcon);
        }
    }

    return pixels;
}

static HICON extractIconFromPeIndexed(const wchar_t* pePath, int groupIndex, int cxDesired, int cyDesired) {
    if (!pePath || cxDesired <= 0 || cyDesired <= 0) return NULL;

    // 本文件所有调用点都是方形；非方形取宽（旧实现把它当拉伸目标，这里只做等比缩放）
    int size = 0;
    BYTE* pixels = extractPeIconPixels(pePath, groupIndex, cxDesired, &size);
    if (!pixels) return NULL;

    BYTE* scaled = scalePixelsOwned(pixels, size, cxDesired);
    if (!scaled) return NULL;

    HICON h = createIconFromPixels(scaled, cxDesired, cxDesired);
    free(scaled);
    return h;
}

// 兼容入口：主图标 = 枚举到的第一个图标组（工具栏 CMD/Explorer 按钮用）
HICON extractIconFromExe(const wchar_t* exePath, int cxDesired, int cyDesired) {
    return extractIconFromPeIndexed(exePath, 0, cxDesired, cyDesired);
}

#pragma pack(push, 1)
typedef struct {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    DWORD dwImageOffset;
} ICOFILEDIRENTRY;

typedef struct {
    WORD idReserved;
    WORD idType;
    WORD idCount;
} ICOFILEDIR;
#pragma pack(pop)

// 从独立 .ico 文件取最接近目标尺寸的那一帧，解成**原生尺寸**的像素缓冲（方形，边长
// 写回 *outSize）。条目数据与 PE 里的 RT_ICON 相同，PNG 压缩条目（Vista+ 常见）以
// PNG 签名开头，分别复用 decodeIconDataToPixels / createIconFromPngData。
// lnk 的 ICON_LOCATION 直接指向图标文件时走这条。失败返回 NULL。
static BYTE* extractIcoIconPixels(const wchar_t* icoPath, int desired, int* outSize) {
    *outSize = 0;
    if (!icoPath || desired <= 0) return NULL;

    HANDLE hFile = CreateFileW(icoPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    BYTE* buf = NULL;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize != INVALID_FILE_SIZE && fileSize >= sizeof(ICOFILEDIR) && fileSize <= 8u * 1024 * 1024)
        buf = (BYTE*)malloc(fileSize);

    BYTE* pixels = NULL;
    DWORD bytesRead = 0;
    if (buf && ReadFile(hFile, buf, fileSize, &bytesRead, NULL) && bytesRead == fileSize) {
        const ICOFILEDIR* dir = (const ICOFILEDIR*)buf;
        size_t tableSize = sizeof(ICOFILEDIR) + (size_t)dir->idCount * sizeof(ICOFILEDIRENTRY);
        if (dir->idReserved == 0 && dir->idType == 1 && dir->idCount > 0 &&
            (size_t)fileSize >= tableSize) {
            const ICOFILEDIRENTRY* entries = (const ICOFILEDIRENTRY*)(buf + sizeof(ICOFILEDIR));

            int bestIndex = -1;
            int bestSide = 0;
            int bestDepth = 0;
            for (WORD i = 0; i < dir->idCount; i++) {
                int w = entries[i].bWidth ? entries[i].bWidth : 256;
                int h = entries[i].bHeight ? entries[i].bHeight : 256;
                // 非方形帧跳过（理由同 peOpenIconResource 里的同名判断）：.ico 里
                // 常混着 76×24 这类工具栏条，而索引图标的文件往往同时带方形帧
                if (w != h) continue;
                int side = w;
                if (bestIndex < 0 ||
                    isBetterIconEntry(side, entries[i].wBitCount, bestSide, bestDepth, desired)) {
                    bestIndex = i;
                    bestSide = side;
                    bestDepth = entries[i].wBitCount;
                }
            }

            if (bestIndex >= 0) {
                DWORD off = entries[bestIndex].dwImageOffset;
                DWORD len = entries[bestIndex].dwBytesInRes;
                // 防畸形文件：条目声明的数据块必须整体落在文件内，否则越界读
                if (len > 0 && off <= fileSize && len <= fileSize - off) {
                    const BYTE* data = buf + off;
                    bool isPng = (len >= 8 && data[0] == 0x89 && memcmp(data + 1, "PNG", 3) == 0);

                    if (!isPng) {
                        int w = 0, h = 0;
                        pixels = decodeIconDataToPixels(data, len, &w, &h);
                        if (pixels && w == h) *outSize = w;
                        else { free(pixels); pixels = NULL; }
                    }
                    if (!pixels) {
                        // GDI+ 解 PNG（Wine 的 load_png 有 B/R 颠倒 bug），再兜底 Wine 的 API。
                        // 都按帧原生尺寸建 HICON，缩放留给上层 —— 不许 Wine 替我们择帧拉伸。
                        HICON hIcon = createIconFromPngData(data, len);
                        if (!hIcon) hIcon = CreateIconFromResourceEx((PBYTE)data, len,
                                                                     TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
                        if (hIcon) {
                            pixels = getIconPixelsSquare(hIcon, outSize);
                            DestroyIcon(hIcon);
                        }
                    }
                }
            }
        }
    }

    free(buf);
    CloseHandle(hFile);
    return pixels;
}

// 取 HICON 的 32bpp 像素副本（straight alpha）。来源缺 alpha（24 位/调色板
// 帧）时按 AND 掩码补透明度；ICONINFO.hbmMask 高度为图标两倍，上半是 AND
// 掩码。返回 malloc 缓冲（调用方 free），失败返回 NULL。
static BYTE* getIconPixels(HICON hIcon, int* outW, int* outH) {
    *outW = *outH = 0;
    if (!hIcon) return NULL;

    ICONINFO ii = {0};
    if (!GetIconInfo(hIcon, &ii)) return NULL;

    BYTE* pixels = NULL;
    int width = 0, height = 0;
    if (ii.hbmColor) {
        BITMAP bm = {0};
        if (GetObjectW(ii.hbmColor, sizeof(BITMAP), &bm)) {
            width = bm.bmWidth;
            height = bm.bmHeight;
        }
    }

    HDC hdcScreen = (width > 0 && height > 0) ? GetDC(NULL) : NULL;
    if (hdcScreen) {
        pixels = (BYTE*)malloc((size_t)width * height * 4);
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;   // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        if (pixels && !GetDIBits(hdcScreen, ii.hbmColor, 0, height, pixels, &bmi, DIB_RGB_COLORS)) {
            free(pixels);
            pixels = NULL;
        }

        if (pixels) {
            // 无 alpha 通道的源：按 AND 掩码补透明度
            bool hasAlpha = false;
            for (int i = 0; i < width * height && !hasAlpha; i++)
                if (pixels[i * 4 + 3]) hasAlpha = true;

            bool alphaReady = hasAlpha;
            if (!hasAlpha && ii.hbmMask) {
                // GetDIBits 会把调色板回写到 bmiColors（1bpp 是 2 项 RGBQUAD），
                // 而 BITMAPINFO 只带 1 项，直接传就是 4 字节栈溢出；这里留满 256 项
                struct {
                    BITMAPINFOHEADER header;
                    RGBQUAD colors[256];
                } maskBmi = {0};
                maskBmi.header.biSize = sizeof(BITMAPINFOHEADER);
                maskBmi.header.biWidth = width;
                maskBmi.header.biHeight = -2 * height;
                maskBmi.header.biPlanes = 1;
                maskBmi.header.biBitCount = 1;
                int andRow = (width + 31) / 32 * 4;
                BYTE* maskBits = (BYTE*)malloc((size_t)andRow * 2 * height);
                if (maskBits && GetDIBits(hdcScreen, ii.hbmMask, 0, 2 * height, maskBits,
                                          (BITMAPINFO*)&maskBmi, DIB_RGB_COLORS)) {
                    // 掩码全 1（整幅被掩掉）时补完仍是一片透明，所以这里统计是否
                    // 真补出过不透明像素：GetDIBits 成功 ≠ 结果可见
                    bool anyOpaque = false;
                    for (int y = 0; y < height; y++) {
                        const BYTE* andRowBits = maskBits + (size_t)y * andRow;
                        for (int x = 0; x < width; x++) {
                            int px = (y * width + x) * 4;
                            if ((andRowBits[x / 8] >> (7 - (x % 8))) & 1) {
                                pixels[px + 0] = 0;
                                pixels[px + 1] = 0;
                                pixels[px + 2] = 0;
                                pixels[px + 3] = 0;
                            }
                            else {
                                pixels[px + 3] = 255;
                                anyOpaque = true;
                            }
                        }
                    }
                    alphaReady = anyOpaque;
                }
                free(maskBits);
            }

            // 掩码补齐也失败时像素仍是全透明，合出来就是不可见图标——
            // 宁可返回失败让调用方走兜底
            if (!alphaReady) {
                free(pixels);
                pixels = NULL;
            }
        }
        ReleaseDC(NULL, hdcScreen);
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (!pixels) return NULL;
    *outW = width;
    *outH = height;
    return pixels;
}

// 快捷方式角标尺寸：素材帧整体缩放到图标边长的这个百分比后叠在左下角。
// 素材是一只完整的小角标（浅灰渐变圆角方块 + 深色箭头，含投影），占帧边长的
// 20/48≈42%（res/shortcut_overlay.ico 刻意按此比例生成）。75% 帧叠加后角标
// 实际约占图标边长的 31%（整帧叠加则是 50%，观感偏大）。
#define SHORTCUT_ARROW_PERCENT 75

// 角标的像素上限 = 素材的原生边长，即「角标永不被放大」。
// 按 75% 百分比算，96/128 槽位要叠到 72/96px，素材会被放大 1.5×/2× 而发糊。
// 夹在原生长边上正好：stamp == overlaySize 时合成代码连缩放都跳过（逐像素原样
// 叠加），是最清晰的一档；32/48/64 槽位按百分比算本就 ≤48，完全不受影响。
// 观感上可见角标（素材帧里含浅灰底板的部分约占 42%）在 64px 时 ≈32% 边长，
// 96px ≈21%，128px ≈16%，与 Windows 随尺寸递减的趋势一致。
#define SHORTCUT_ARROW_MAX_STAMP 48

// 整数倍盒式均值降采样（预乘后平均，避免透明像素拉暗边缘）。48px 素材缩到
// 12px（÷4）或 24px（÷2）都走这里；只有非整数倍（stock icon 兜底那一路）
// 才退到下面的双线性。
static BYTE* downsampleIconPixels(const BYTE* src, int srcSize, int dstSize) {
    if (srcSize <= 0 || dstSize <= 0 || srcSize % dstSize != 0) return NULL;
    int s = srcSize / dstSize;
    BYTE* out = (BYTE*)malloc((size_t)dstSize * dstSize * 4);
    if (!out) return NULL;
    for (int y = 0; y < dstSize; y++) {
        for (int x = 0; x < dstSize; x++) {
            unsigned sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            for (int dy = 0; dy < s; dy++) {
                for (int dx = 0; dx < s; dx++) {
                    const BYTE* p = src + ((size_t)(y * s + dy) * srcSize + x * s + dx) * 4;
                    unsigned a = p[3];
                    sumR += (unsigned)p[0] * a;
                    sumG += (unsigned)p[1] * a;
                    sumB += (unsigned)p[2] * a;
                    sumA += a;
                }
            }
            BYTE* q = out + ((size_t)y * dstSize + x) * 4;
            if (sumA) {
                q[0] = (BYTE)(sumR / sumA);
                q[1] = (BYTE)(sumG / sumA);
                q[2] = (BYTE)(sumB / sumA);
            }
            else q[0] = q[1] = q[2] = 0;
            q[3] = (BYTE)(sumA / (s * s));
        }
    }
    return out;
}

// 快捷方式角标像素（素材为 WFM 自带的 res/shortcut_overlay.ico，资源 ID
// IDI_SHORTCUT_OVERLAY；由 tools/gen_shortcut_overlay.sh 从 Tango 图标主题的
// emblem-symbolic-link 生成——该素材作者声明无条件供任何人使用，见脚本注释）。
// 取 48px 帧：48 能被 12、24 整除，缩小走整数倍盒式均值即可保持像素干净；
// 素材是完整的角标图案、固定在帧左下角、占 20/48，与「整帧缩到槽位 75% 后贴
// 左下角」的合成几何约定配套（见 SHORTCUT_ARROW_PERCENT）。
// 素材保留源图标的全部图层（渐变底板、内白描边、右下投影），只把箭头改成深色
// 实心：糊的根源是箭头本身半透明、与浅灰底板几乎同色，装饰层缩下来反而贡献了
// 立体感——别再删它们，也别再把箭头单独抠出来（试过，小尺寸下是个黑钩子）。
static const BYTE* getShortcutOverlayPixels(int* outSize) {
    static BYTE* cached = NULL;
    static int cachedSize = 0;
    *outSize = cachedSize;
    if (cached) return cached;

    // 从自身模块取资源：GetModuleHandleW(NULL) 即 wfm.exe，不依赖任何系统 DLL
    HICON hOverlay = LoadImageW(GetModuleHandleW(NULL),
                                MAKEINTRESOURCEW(IDI_SHORTCUT_OVERLAY),
                                IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
    if (hOverlay) {
        int w = 0, h = 0;
        BYTE* pixels = getIconPixels(hOverlay, &w, &h);
        DestroyIcon(hOverlay);   // LoadImageW 的 HICON 非 shared，需要销毁
        if (pixels && w == h) {
            cached = pixels;
            cachedSize = w;
        }
        else free(pixels);
    }

    // 兜底：资源缺失（正常构建不会发生）或素材取像素失败（尺寸不符 / 无 alpha
    // 且掩码补齐失败）时退回系统 stock icon。
    // 只有两条路都试过仍为空才算失败——否则缓存会永久停在 NULL，角标再也出不来。
    if (!cached) {
        // SHGetStockIconInfo 内部是 LoadIconW，返回共享 HICON，不能 DestroyIcon
        SHSTOCKICONINFO sii = {0};
        sii.cbSize = sizeof(sii);
        if (SUCCEEDED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) && sii.hIcon) {
            int w = 0, h = 0;
            BYTE* pixels = getIconPixels(sii.hIcon, &w, &h);
            if (pixels && w == h) {
                cached = pixels;
                cachedSize = w;
            }
            else free(pixels);
        }
    }
    *outSize = cachedSize;
    return cached;
}

// 双线性缩放 32bpp RGBA 像素（预乘后插值，透明像素不会拉出脏色边）。
// 用于把低分辨率目标图标平滑缩放到列表槽位尺寸——箭头角标随后按素材
// 原生帧叠加，不随目标图标一起被放大成马赛克。srcSize/dstSize 为边长。
static BYTE* scaleIconPixelsBilinear(const BYTE* src, int srcSize, int dstSize) {
    if (srcSize <= 0 || dstSize <= 0 || srcSize == dstSize) return NULL;
    // calloc 而不是 malloc：下面每个像素的四通道都会被赋值，但赋值点埋在两层
    // 循环 + 手算下标里，-fanalyzer 无法证明「全量写入」而会报
    // -Wanalyzer-use-of-uninitialized-value 假警告。清零让「缓冲区已初始化」
    // 这个事实对分析器显式成立，代价只是每帧一次 memset（≤128×128×4）。
    BYTE* out = (BYTE*)calloc((size_t)dstSize * dstSize * 4, 1);
    if (!out) return NULL;

    for (int y = 0; y < dstSize; y++) {
        double fy = (y + 0.5) * srcSize / dstSize - 0.5;
        if (fy < 0) fy = 0;
        if (fy > srcSize - 1) fy = srcSize - 1;
        int y0 = (int)fy;
        int y1 = (y0 + 1 < srcSize) ? y0 + 1 : srcSize - 1;
        double wy = fy - y0;

        for (int x = 0; x < dstSize; x++) {
            double fx = (x + 0.5) * srcSize / dstSize - 0.5;
            if (fx < 0) fx = 0;
            if (fx > srcSize - 1) fx = srcSize - 1;
            int x0 = (int)fx;
            int x1 = (x0 + 1 < srcSize) ? x0 + 1 : srcSize - 1;
            double wx = fx - x0;

            // 预乘 RGB 后做四角插值，再按合成 alpha 还原 straight alpha
            double r = 0, g = 0, b = 0, a = 0;
            const int px[2] = { x0, x1 };
            const int py[2] = { y0, y1 };
            const double wgt[2][2] = {
                { (1 - wx) * (1 - wy), wx * (1 - wy) },
                { (1 - wx) * wy,       wx * wy       },
            };
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    const BYTE* p = src + ((size_t)py[j] * srcSize + px[i]) * 4;
                    double wa = wgt[j][i] * p[3];
                    r += p[0] * wa;
                    g += p[1] * wa;
                    b += p[2] * wa;
                    a += wgt[j][i] * p[3];
                }
            }
            BYTE* q = out + ((size_t)y * dstSize + x) * 4;
            unsigned outA = (unsigned)(a + 0.5);
            if (outA) {
                int v;
                v = (int)(r / a + 0.5); q[0] = (BYTE)(v > 255 ? 255 : v);
                v = (int)(g / a + 0.5); q[1] = (BYTE)(v > 255 ? 255 : v);
                v = (int)(b / a + 0.5); q[2] = (BYTE)(v > 255 ? 255 : v);
            }
            else q[0] = q[1] = q[2] = 0;
            q[3] = (BYTE)(outA > 255 ? 255 : outA);
        }
    }
    return out;
}

// 32bpp straight-alpha 像素 → HICON。掩码留空（全 0，什么都不遮），透明度
// 完全交给 alpha 通道；CreateIconIndirect 会复制位图，调用方随后即可释放像素。
// 失败返回 NULL。
static HICON createIconFromPixels(const BYTE* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return NULL;

    HICON result = NULL;
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;   // top-down，与像素缓冲同序
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = NULL;
        HBITMAP hDib = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);
        if (hDib && dibBits) {
            memcpy(dibBits, pixels, (size_t)width * height * 4);
            HBITMAP hMaskBmp = CreateBitmap(width, height, 1, 1, NULL);
            if (hMaskBmp) {
                ICONINFO ci = {0};
                ci.fIcon = TRUE;
                ci.hbmColor = hDib;
                ci.hbmMask = hMaskBmp;
                result = CreateIconIndirect(&ci);
                DeleteObject(hMaskBmp);
            }
            DeleteObject(hDib);
        }
        ReleaseDC(NULL, hdcScreen);
    }
    return result;
}

// getIconPixels 的方形封装：非方形视为失败（本文件的图标全是方形）。
static BYTE* getIconPixelsSquare(HICON hIcon, int* outSize) {
    *outSize = 0;
    int width = 0, height = 0;
    BYTE* pixels = getIconPixels(hIcon, &width, &height);
    if (!pixels) return NULL;
    if (width != height) {
        free(pixels);
        return NULL;
    }
    *outSize = width;
    return pixels;
}

// 把方形像素缓冲缩放到 dstSize×dstSize。整数倍缩小先反复折半（每步都是整数倍盒式
// 均值），最后一步再收尾：256→48 若直接用点采样式双线性，会漏掉大部分源像素、细节
// 发花；非整数倍（含放大）走双线性，不会出现最近邻那种块状。
//
// **无论成功失败都消费 pixels**（尺寸相同则原样交回，省掉一次整幅 memcpy；需要重采样
// 时旧缓冲已被释放）。渲染路径要的就是这个语义：解码出的原生帧缓冲直接传进来，拿到
// 的即最终尺寸的缓冲 —— 中途不需要经过任何 HICON。
static BYTE* scalePixelsOwned(BYTE* pixels, int srcSize, int dstSize) {
    if (!pixels || srcSize <= 0 || dstSize <= 0) {
        free(pixels);
        return NULL;
    }
    if (srcSize == dstSize) return pixels;

    BYTE* cur = pixels;
    int curSize = srcSize;
    while (curSize >= dstSize * 2 && curSize % 2 == 0) {
        BYTE* half = downsampleIconPixels(cur, curSize, curSize / 2);
        if (!half) break;
        free(cur);
        cur = half;
        curSize /= 2;
    }

    BYTE* scaled = cur;
    if (curSize != dstSize) {
        scaled = (curSize > dstSize && curSize % dstSize == 0)
            ? downsampleIconPixels(cur, curSize, dstSize)
            : scaleIconPixelsBilinear(cur, curSize, dstSize);
        free(cur);
    }
    return scaled;
}

// 把自带的快捷方式箭头叠到像素缓冲的左下角（原地 src-over alpha 合成）。
//
// 调用时机：缓冲**已经缩到最终尺寸**。素材（res/shortcut_overlay.ico）只在这一步
// 缩放，与目标图标的分辨率无关，所以箭头的清晰度不会被目标图标的原生帧大小影响。
// outSize 是 16/32（列表槽位）或大图标视图的显示尺寸。
// 返回 false = 没叠上（素材取不到 / 尺寸畸形），调用方照旧用无角标的主图标。
static bool stampShortcutOverlay(BYTE* dst, int outSize) {
    if (!dst || outSize <= 0) return false;

    int overlaySize = 0;
    const BYTE* overlay = getShortcutOverlayPixels(&overlaySize);
    if (!overlay) return false;

    // 箭头尺寸：素材帧整体缩到图标边长的 75% 后叠在左下角，并按素材原生
    // 边长封顶（大图标视图下不让 48px 素材再被放大，见 SHORTCUT_ARROW_MAX_STAMP）
    int stamp = outSize * SHORTCUT_ARROW_PERCENT / 100;
    if (stamp > outSize) stamp = outSize;
    if (stamp > SHORTCUT_ARROW_MAX_STAMP) stamp = SHORTCUT_ARROW_MAX_STAMP;
    // 畸形图标（边长 1px）会让 stamp 算成 0，下一步 overlaySize % stamp 就是整数
    // 除零。角标画不出来不影响主图标，直接放弃。
    if (stamp < 1) return false;

    // 覆盖层缩放到叠加尺寸：整数倍用盒式均值（预乘后平均，像素干净），
    // 否则双线性。缩放只发生在素材帧这一步，与目标图标的分辨率无关
    const BYTE* scaled = overlay;
    BYTE* scaledBuf = NULL;
    if (stamp != overlaySize) {
        if (overlaySize % stamp == 0) scaledBuf = downsampleIconPixels(overlay, overlaySize, stamp);
        else scaledBuf = scaleIconPixelsBilinear(overlay, overlaySize, stamp);
        if (!scaledBuf) return false;
        scaled = scaledBuf;
    }

    // 标准 src-over alpha 合成，固定在左下角（glyph 在素材帧内自带定位）
    int dstSkip = outSize - stamp;
    for (int y = 0; y < stamp; y++) {
        for (int x = 0; x < stamp; x++) {
            const BYTE* s = scaled + ((size_t)y * stamp + x) * 4;
            BYTE* d = dst + ((size_t)(y + dstSkip) * outSize + x) * 4;
            unsigned aO = s[3];
            if (!aO) continue;
            unsigned aD = d[3];
            unsigned outA = aO + aD * (255 - aO) / 255;
            if (!outA) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; c++)
                d[c] = (BYTE)(((unsigned)s[c] * aO + (unsigned)d[c] * aD * (255 - aO) / 255) / outA);
            d[3] = (BYTE)outA;
        }
    }
    free(scaledBuf);
    return true;
}

// 解析 lnk 的图标来源，优先用 lnk 里存的 ICON_LOCATION（含图标组序号）；没有就
// 退到目标文件本身的主图标（GetPath）。
//
// 为什么不能拿 SHGFI_ICONLOCATION 代替：Wine 对快捷方式这条路径走的是
// IExtractIconW::GetIconLocation，实测直接吐出 shell32.dll 的通用文档图标
// （file=SHELL32.dll idx=0）——它根本不解析 lnk。于是图标链路上就只剩我们自己
// 合成的那个角标，看上去就是「快捷方式图标变成一个孤零零的箭头」。
// 图标里的 %var% 在这里统一展开（与 resolveLnkTargetPath 同一套理由）。
static bool resolveLnkIconLocation(const wchar_t* lnkPath, wchar_t* iconPath, int iconPathCch, int* iconIndex) {
    if (!iconPath || iconPathCch <= 0 || !iconIndex) return false;
    iconPath[0] = L'\0';
    *iconIndex = 0;
    if (!lnkPath || !lnkPath[0]) return false;

    bool ok = false;
    IShellLinkW* isl = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void**)&isl))) {
        IPersistFile* ipf = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(isl, &IID_IPersistFile, (void**)&ipf))) {
            if (SUCCEEDED(IPersistFile_Load(ipf, lnkPath, STGM_READ | STGM_SHARE_DENY_WRITE))) {
                wchar_t raw[MAX_PATH] = {0};
                int idx = 0;
                if (SUCCEEDED(IShellLinkW_GetIconLocation(isl, raw, MAX_PATH, &idx)) && raw[0]) {
                    ok = true;
                }
                else if (SUCCEEDED(IShellLinkW_GetPath(isl, raw, MAX_PATH, NULL, SLGP_RAWPATH)) && raw[0]) {
                    idx = 0;   // 目标文件自己的主图标
                    ok = true;
                }
                if (ok) {
                    // lnk 里常见 %windir%\system32\... 形式，先展开；放不下保留原样
                    wchar_t expanded[MAX_PATH] = {0};
                    DWORD n = ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
                    const wchar_t* finalPath = (n > 0 && n <= MAX_PATH && expanded[0]) ? expanded : raw;
                    wcsncpy_s(iconPath, (size_t)iconPathCch, finalPath, _TRUNCATE);
                    *iconIndex = idx;
                }
            }
            IPersistFile_Release(ipf);
        }
        IShellLinkW_Release(isl);
    }
    return ok;
}

// 解析 lnk 指向的目标路径（不取图标，只取路径）。用 SLGP_RAWPATH 拿 lnk 里存的
// 原始路径再自己展开 %var%：Wine 的 IShellLinkW_fnGetPath 完全忽略 fFlags、直接
// 返回 Load 时存下的 sPath，而真 Windows 下 SLGP_RAWPATH 正好也是不展开的形式，
// 两边都靠这一步统一。取不到路径（目标是 URL、MSI 广告式快捷方式、路径为空）
// 返回 false——注意 Wine 用 S_FALSE 表示「没有路径」，SUCCEEDED(S_FALSE) 为真，
// 所以必须靠内容判空，不能只看 HRESULT。
static bool resolveLnkTargetPath(const wchar_t* lnkPath, wchar_t* targetPath, int targetCch) {
    if (!targetPath || targetCch <= 0) return false;
    targetPath[0] = L'\0';
    if (!lnkPath || !lnkPath[0]) return false;

    bool ok = false;
    IShellLinkW* isl = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void**)&isl))) {
        IPersistFile* ipf = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(isl, &IID_IPersistFile, (void**)&ipf))) {
            if (SUCCEEDED(IPersistFile_Load(ipf, lnkPath, STGM_READ | STGM_SHARE_DENY_WRITE))) {
                wchar_t raw[MAX_PATH] = {0};
                if (SUCCEEDED(IShellLinkW_GetPath(isl, raw, MAX_PATH, NULL, SLGP_RAWPATH)) && raw[0]) {
                    // lnk 里常见 %windir%\system32\... 形式，先展开；放不下保留原样
                    wchar_t expanded[MAX_PATH] = {0};
                    DWORD n = ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
                    const wchar_t* finalPath = (n > 0 && n <= MAX_PATH && expanded[0]) ? expanded : raw;
                    wcsncpy_s(targetPath, (size_t)targetCch, finalPath, _TRUNCATE);
                    ok = true;
                }
            }
            IPersistFile_Release(ipf);
        }
        IShellLinkW_Release(isl);
    }
    return ok;
}

// ========== 大图标视图：按显示尺寸重新生成图标 ==========
//
// 旧的缩放路径是「拿系统 32px 列表里的图 → CopyImage 放大」，而 Wine 的
// CopyImage / CreateIconFromResourceEx / DrawIconEx 三条缩放路径全是最近邻
// （user32/cursoricon.c 的 stretch_bitmap 与 create_icon_frame 都不设拉伸模式，
// NtUserDrawIconEx 更是硬编码 STRETCH_DELETESCANS），32→128 就是 4 倍块复制。
// 这里改成两条更靠谱的来源：
//   ① exe / lnk —— 用 WFM 自带的解码器（手写 BMP + GDI+ 解 PNG）从文件里
//      按目标尺寸重新取，既拿得到原生 256 帧，也绕开 Wine 的 PNG R/B 反色 bug；
//   ② 其余（文件夹 / 驱动器 / .ico 等按类型给图标的）—— 从 shell 镜像列表里
//      挑一份源自己平滑缩放。Wine 的 shell 图标自带原生 256 帧
//      （resources/*.ico 里的 256 是原生的，不是放大出来的），所以缩下来比
//      放大上去清楚得多。
// 两条路的结果都是「目标尺寸的原生 HICON」，交给缩放图像列表按 1:1 贴图。

// 从 shell 镜像列表里取 sysIcon 对应的图标再平滑缩放到 size。Wine 的 SIC 对
// 五个列表是逐个追加的（iconcache.c 的 SIC_IconAppend），索引在五个列表里严格
// 同步，所以同一个 sysIcon 可以直接拿去查任意一个列表 —— 于是可以按尺寸挑源：
// 取「不小于目标的列表里最小的那个」，48 能直接命中原生的 48 帧，64/96/128 则
// 从 256 帧缩下来（缩小永远比放大清楚）。都够不着时退到最大的那个。
// 失败返回 NULL。
// shell 那 5 个共享镜像列表的尺寸在进程生命周期内不会变，而 SHGetImageList() 每次
// 都要现场造一个 IImageList 包装对象（Wine 侧是 SHGetImageList → 包装 + AddRef）。
// 原实现在**每渲染一张图标**时查 4 个列表 = 4 次 SHGetImageList + 4 次 GetIconSize
// + 4 次 Release；在 Winlator 上这些调用全要经 box86/box64 翻译，纯属浪费。
// 这里查一次存下来。**刻意不释放**：它们本来就是 shell 的进程级全局对象，
// SIC 也没有销毁 API，持有引用不会拖着任何东西不还。
static IImageList* shellScaledLists[4] = {0};
static int shellScaledSizes[4] = {0};
static bool shellScaledInit = false;

// 从 shell 的系统列表里取最合适的一张，**按原生尺寸**返回（不缩放），边长写回
// *outNativeSize。返回的 HICON 由调用方 DestroyIcon()，失败返回 NULL。
//
// 为什么不在这里直接把目标尺寸缩好：shell 的四个镜像列表尺寸是固定的
// （16/32/48/256），把「选哪张」与「缩到目标」拆开后，渲染路径可以用像素缓冲统一
// 缩一次（全程只有一次像素往返）；否则这里缩完还会被 getIconPixels 拆回像素，
// 白白多一趟整幅拷贝。
static HICON createShellIconBest(int sysIcon, int desired, int* outNativeSize) {
    *outNativeSize = 0;
    if (sysIcon < 0 || desired <= 0) return NULL;

    // 候选按尺寸从大到小：挑「不小于目标尺寸里最小的」那个，退而求其次用最大的。
    // SHIL_SMALL 也在候选里，16px 的详细视图因此能拿到 shell 的原生 16 帧，
    // 而不是把 32 帧缩下去（原生帧更锐）。
    static const int lists[] = { SHIL_JUMBO, SHIL_EXTRALARGE, SHIL_LARGE, SHIL_SMALL };
    const int numLists = (int)(sizeof(lists) / sizeof(lists[0]));

    if (!shellScaledInit) {
        shellScaledInit = true;   // 失败也只试一次：语义与原来「查不到就跳过」一致
        for (int i = 0; i < numLists; i++) {
            IImageList* piml = NULL;
            if (FAILED(SHGetImageList(lists[i], &wfm_IID_IImageList, (void**)&piml)) || !piml)
                continue;
            int cx = 0, cy = 0;
            if (FAILED(IImageList_GetIconSize(piml, &cx, &cy)) || cx <= 0 || cx != cy) {
                IImageList_Release(piml);
                continue;
            }
            shellScaledLists[i] = piml;
            shellScaledSizes[i] = cx;
        }
    }

    int bestIdx = -1, bestSize = 0;          // 不小于 desired 里最小的
    int biggestIdx = -1, biggestSize = 0;    // 兜底：最大的
    for (int i = 0; i < numLists; i++) {
        int cx = shellScaledSizes[i];
        if (cx <= 0 || !shellScaledLists[i]) continue;

        if (cx > biggestSize) { biggestSize = cx; biggestIdx = i; }
        if (cx >= desired && (bestIdx < 0 || cx < bestSize)) { bestSize = cx; bestIdx = i; }
    }
    if (bestIdx < 0) { bestIdx = biggestIdx; bestSize = biggestSize; }
    if (bestIdx < 0) return NULL;

    HICON src = NULL;
    HRESULT hr = IImageList_GetIcon(shellScaledLists[bestIdx], sysIcon, ILD_TRANSPARENT, &src);
    if (FAILED(hr) || !src) return NULL;

    *outNativeSize = bestSize;
    return src;
}

// ========== Icon viewer window ==========

static void cleanupIconGroups(void) {
    for (int g = 0; g < MAX_ICON_GROUPS; g++) {
        for (int i = 0; i < MAX_ICONS_PER_GROUP; i++) {
            if (iconGroups[g].icons[i]) {
                DestroyIcon(iconGroups[g].icons[i]);
                iconGroups[g].icons[i] = NULL;
            }
        }
        iconGroups[g].iconCount = 0;
    }
    iconGroupCount = 0;
}

static BOOL saveIconToFile(HICON hIcon, const wchar_t* filePath) {
    if (!hIcon || !filePath) return FALSE;

    ICONINFO ii = {0};
    if (!GetIconInfo(hIcon, &ii)) return FALSE;

    int iconWidth = 0, iconHeight = 0;

    if (ii.hbmColor) {
        BITMAP bm = {0};
        GetObjectW(ii.hbmColor, sizeof(BITMAP), &bm);
        iconWidth = bm.bmWidth;
        iconHeight = bm.bmHeight;
    }

    if (ii.hbmMask && iconWidth <= 0) {
        BITMAP bmMask = {0};
        GetObjectW(ii.hbmMask, sizeof(BITMAP), &bmMask);
        iconWidth = bmMask.bmWidth;
        iconHeight = bmMask.bmHeight;
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (iconWidth <= 0 || iconHeight <= 0) return FALSE;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = iconWidth;
    bmi.bmiHeader.biHeight = -iconHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* dibBits = NULL;
    HBITMAP hbmDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&dibBits, NULL, 0);
    if (!hbmDib || !dibBits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmDib);

    RECT rc = {0, 0, iconWidth, iconHeight};
    HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, hBr);
    DeleteObject(hBr);

    DrawIconEx(hdcMem, 0, 0, hIcon, iconWidth, iconHeight, 0, NULL, DI_NORMAL);
    GdiFlush();

    size_t colorSize = (size_t)iconWidth * (size_t)iconHeight * 4;
    BYTE* colorBits = (BYTE*)malloc(colorSize);
    if (colorBits) {
        memcpy(colorBits, dibBits, colorSize);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmDib);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (!colorBits) return FALSE;

    size_t maskSize = ((size_t)(iconWidth + 31) / 32) * 4 * (size_t)iconHeight;
    BYTE* maskBits = (BYTE*)calloc(1, maskSize);
    if (!maskBits) {
        free(colorBits);
        return FALSE;
    }

    // 行缓冲在打开文件之前分配：分配失败就整体失败，
    // 而不是退化写入导致生成的 .ico 上下颠倒却毫无提示
    size_t rowSize = (size_t)iconWidth * 4;
    BYTE* rowBuf = (BYTE*)malloc(rowSize);
    if (!rowBuf) {
        free(colorBits);
        free(maskBits);
        return FALSE;
    }

    BOOL result = FALSE;
    HANDLE hFile = CreateFileW(filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        DWORD dibSize = sizeof(BITMAPINFOHEADER);
        DWORD totalImageSize = dibSize + (DWORD)colorSize + (DWORD)maskSize;
        DWORD dataOffset = 6 + 16;

        WORD reserved = 0, type = 1, count = 1;
        WriteFile(hFile, &reserved, 2, &written, NULL);
        WriteFile(hFile, &type, 2, &written, NULL);
        WriteFile(hFile, &count, 2, &written, NULL);

        BYTE w = (iconWidth >= 256) ? 0 : (BYTE)iconWidth;
        BYTE h = (iconHeight >= 256) ? 0 : (BYTE)iconHeight;
        BYTE colors = 0, res = 0;
        WORD planes = 1, bpp = 32;
        WriteFile(hFile, &w, 1, &written, NULL);
        WriteFile(hFile, &h, 1, &written, NULL);
        WriteFile(hFile, &colors, 1, &written, NULL);
        WriteFile(hFile, &res, 1, &written, NULL);
        WriteFile(hFile, &planes, 2, &written, NULL);
        WriteFile(hFile, &bpp, 2, &written, NULL);
        WriteFile(hFile, &totalImageSize, 4, &written, NULL);
        WriteFile(hFile, &dataOffset, 4, &written, NULL);

        BITMAPINFOHEADER bih = {0};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = iconWidth;
        bih.biHeight = iconHeight * 2;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        WriteFile(hFile, &bih, sizeof(bih), &written, NULL);

        for (int row = 0; row < iconHeight; row++) {
            memcpy(rowBuf, colorBits + (size_t)(iconHeight - 1 - row) * rowSize, rowSize);
            WriteFile(hFile, rowBuf, (DWORD)rowSize, &written, NULL);
        }

        WriteFile(hFile, maskBits, (DWORD)maskSize, &written, NULL);

        CloseHandle(hFile);
        result = TRUE;
    }

    free(rowBuf);
    free(colorBits);
    free(maskBits);

    return result;
}

static HICON getCurrentIcon(void) {
    if (currentGroupIndex < 0 || currentGroupIndex >= iconGroupCount) return NULL;
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    if (currentIconIndex < 0 || currentIconIndex >= grp->iconCount) return NULL;
    return grp->icons[currentIconIndex];
}

static void updateViewerTitle(void) {
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    int size = grp->sizes[currentIconIndex];
    swprintf_s(iconViewerTitle, MAX_PATH, L"%ls - [%d/%d] %dx%d (%d/%d) - %ls",
        iconViewerFileName,
        currentGroupIndex + 1, iconGroupCount,
        size, size,
        currentIconIndex + 1, grp->iconCount,
        lc_str.show_icon);
    SetWindowTextW(hwndIconViewer, iconViewerTitle);
}

static void destroySizeButtons(HWND hwnd) {
    for (int i = 0; i < MAX_ICONS_PER_GROUP; i++) {
        HWND hBtn = GetDlgItem(hwnd, IDC_SIZE_BASE + i);
        if (hBtn) DestroyWindow(hBtn);
    }
}

static void createSizeButtons(HWND hwnd) {
    destroySizeButtons(hwnd);

    if (currentGroupIndex < 0 || currentGroupIndex >= iconGroupCount) return;
    struct IconGroup* grp = &iconGroups[currentGroupIndex];
    int n = grp->iconCount;

    int btnW = 70, btnH = 28, gap = 6;
    int btnsPerRow = 4;
    int rowW = btnsPerRow * btnW + (btnsPerRow - 1) * gap;
    int startX = (380 - rowW) / 2;
    int btnY = 285;  // Adjusted for larger icon area

    // Track unique sizes to avoid duplicate buttons
    int uniqueSizes[MAX_ICONS_PER_GROUP];
    int uniqueCount = 0;
    
    for (int i = 0; i < n; i++) {
        int s = grp->sizes[i];
        BOOL isDuplicate = FALSE;
        
        // Check if we already have this size
        for (int j = 0; j < uniqueCount; j++) {
            if (uniqueSizes[j] == s) {
                isDuplicate = TRUE;
                break;
            }
        }
        
        if (!isDuplicate) {
            uniqueSizes[uniqueCount] = s;
            uniqueCount++;
            
            int row = (uniqueCount - 1) / btnsPerRow;
            int col = (uniqueCount - 1) % btnsPerRow;
            wchar_t label[16];
            swprintf_s(label, 16, L"%dx%d", s, s);

            HWND hBtn = CreateWindowW(L"BUTTON", label,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                startX + col * (btnW + gap), btnY + row * (btnH + gap), btnW, btnH,
                hwnd, (HMENU)(INT_PTR)(IDC_SIZE_BASE + i), globalHInstance, NULL);
            if (hBtn && hGuiFont) {
                SendMessageW(hBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
            }
        }
    }

    // Reposition the save button below the last row of size buttons
    int nRows = (uniqueCount + btnsPerRow - 1) / btnsPerRow;
    int saveBtnY = btnY + nRows * (btnH + gap) + 5;
    HWND hSaveBtn = GetDlgItem(hwnd, IDC_SAVE_ICON);
    if (hSaveBtn) {
        SetWindowPos(hSaveBtn, NULL, 90, saveBtnY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

// Static variable to store filename clickable area
static RECT g_filenameRect = {0};

// Window procedure for filename popup window
static LRESULT CALLBACK FilenamePopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK IconViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Group navigation arrows
            int navY = 285;
            int arrowW = 28, arrowH = 28;

            HWND hPrev = CreateWindowW(L"BUTTON", L"\u25C0",
                WS_CHILD | WS_VISIBLE | (iconGroupCount > 1 ? 0 : WS_DISABLED) | BS_PUSHBUTTON,
                10, navY, arrowW, arrowH,
                hwnd, (HMENU)IDC_PREV_GROUP, globalHInstance, NULL);
            if (hPrev && hGuiFont) SendMessageW(hPrev, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            HWND hNext = CreateWindowW(L"BUTTON", L"\u25B6",
                WS_CHILD | WS_VISIBLE | (iconGroupCount > 1 ? 0 : WS_DISABLED) | BS_PUSHBUTTON,
                380 - 10 - arrowW, navY, arrowW, arrowH,
                hwnd, (HMENU)IDC_NEXT_GROUP, globalHInstance, NULL);
            if (hNext && hGuiFont) SendMessageW(hNext, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            // Save button - create first so createSizeButtons can reposition it
            HWND hSaveBtn = CreateWindowW(L"BUTTON", lc_str.save_icon,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                90, 400, 200, 28,
                hwnd, (HMENU)IDC_SAVE_ICON, globalHInstance, NULL);
            if (hSaveBtn && hGuiFont) SendMessageW(hSaveBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            // Dynamic size buttons + reposition save button
            createSizeButtons(hwnd);

            updateViewerTitle();
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HICON hCurrent = getCurrentIcon();
            if (hCurrent) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int cx = rc.right - rc.left;
                int iconAreaCy = 280;

                int curSize = iconGroups[currentGroupIndex].sizes[currentIconIndex];
                int iconCx = min(cx - 20, curSize);
                int iconCy = min(iconAreaCy - 10, curSize);
                int drawSize = min(iconCx, iconCy);
                int x = (cx - drawSize) / 2;
                int y = (iconAreaCy - drawSize) / 2;
                if (y < 5) y = 5;

                DrawIconEx(hdc, x, y, hCurrent, drawSize, drawSize, 0, NULL, DI_NORMAL);
            }

            // Draw separator line below Save Icon button
            RECT clientRc;
            GetClientRect(hwnd, &clientRc);
            int separatorY = clientRc.bottom - 35;  // 35 pixels from bottom for filename
            
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
            HPEN hOldPen = SelectObject(hdc, hPen);
            MoveToEx(hdc, 10, separatorY, NULL);
            LineTo(hdc, clientRc.right - 10, separatorY);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            // Draw filename below separator (clickable)
            if (iconViewerFileName[0] != L'\0') {
                RECT textRc = {10, separatorY + 5, clientRc.right - 10, clientRc.bottom - 5};
                g_filenameRect = textRc;  // Store for click detection
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(0, 102, 204));  // Blue color for clickable text
                
                // Create underlined font for clickable effect
                HFONT hUnderlineFont = CreateFontW(
                    -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72),  // Height
                    0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,  // Underline=TRUE
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI");
                
                HFONT hOldFont = SelectObject(hdc, hUnderlineFont);
                DrawTextW(hdc, iconViewerFileName, -1, &textRc, 
                    DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(hdc, hOldFont);
                DeleteObject(hUnderlineFont);
            }

            EndPaint(hwnd, &ps);
            break;
        }
        case WM_COMMAND: {
            int cmdId = LOWORD(wParam);
            if (HIWORD(wParam) == BN_CLICKED) {
                if (cmdId == IDC_PREV_GROUP) {
                    if (iconGroupCount > 1) {
                        currentGroupIndex = (currentGroupIndex - 1 + iconGroupCount) % iconGroupCount;
                        currentIconIndex = 0;
                        createSizeButtons(hwnd);
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId == IDC_NEXT_GROUP) {
                    if (iconGroupCount > 1) {
                        currentGroupIndex = (currentGroupIndex + 1) % iconGroupCount;
                        currentIconIndex = 0;
                        createSizeButtons(hwnd);
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId >= IDC_SIZE_BASE && cmdId < IDC_SIZE_BASE + MAX_ICONS_PER_GROUP) {
                    int idx = cmdId - IDC_SIZE_BASE;
                    if (idx != currentIconIndex && idx < iconGroups[currentGroupIndex].iconCount) {
                        currentIconIndex = idx;
                        updateViewerTitle();
                        InvalidateRect(hwnd, NULL, TRUE);
                    }
                } else if (cmdId == IDC_SAVE_ICON) {
                    HICON hCurrent = getCurrentIcon();
                    if (!hCurrent) break;

                    wchar_t filePath[MAX_PATH + 8] = {0};
                    swprintfTrunc(filePath, MAX_PATH + 8, L"%ls.ico", iconViewerFileName);
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH + 8;
                    ofn.lpstrFilter = L"Icon Files (*.ico)\0*.ico\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrDefExt = L"ico";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                    if (GetSaveFileNameW(&ofn)) {
                        if (!saveIconToFile(hCurrent, filePath)) {
                            MessageBoxW(hwnd, L"保存图标失败", lc_str.alert, MB_OK | MB_ICONERROR);
                        }
                    }
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int xPos = LOWORD(lParam);
            int yPos = HIWORD(lParam);
            POINT pt = {xPos, yPos};
            
            // Check if click is within filename area
            if (PtInRect(&g_filenameRect, pt) && iconViewerFileName[0] != L'\0') {
                // Register popup window class if not already registered
                WNDCLASSEX wcPopup = {0};
                wcPopup.cbSize = sizeof(WNDCLASSEX);
                wcPopup.lpfnWndProc = FilenamePopupWndProc;
                wcPopup.hInstance = globalHInstance;
                wcPopup.hCursor = LoadCursor(NULL, IDC_ARROW);
                wcPopup.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
                wcPopup.lpszClassName = L"FilenamePopupClass";
                RegisterClassEx(&wcPopup);
                
                // Create popup window to show full filename
                HWND hPopup = CreateWindowEx(
                    WS_EX_TOPMOST,
                    L"FilenamePopupClass",
                    L"File Name",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, 420, 180,
                    hwnd, NULL, globalHInstance, NULL);
                
                if (hPopup) {
                    // Calculate position to center popup on screen
                    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                    int popupW = 420, popupH = 180;
                    int popupX = (screenWidth - popupW) / 2;
                    int popupY = (screenHeight - popupH) / 2;
                    
                    SetWindowPos(hPopup, NULL, popupX, popupY, popupW, popupH, SWP_SHOWWINDOW);
                    
                    // Create edit control to display full filename with scrolling
                    HWND hEdit = CreateWindowEx(
                        WS_EX_CLIENTEDGE, L"EDIT", iconViewerFileName,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | 
                        ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        10, 10, popupW - 36, popupH - 95,
                        hPopup, NULL, globalHInstance, NULL);
                    
                    if (hEdit && hGuiFont) {
                        SendMessageW(hEdit, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
                    }
                    
                    // Create close button
                    HWND hCloseBtn = CreateWindowW(L"BUTTON", L"Close",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        (popupW - 100) / 2, popupH - 55, 100, 28,
                        hPopup, (HMENU)IDCANCEL, globalHInstance, NULL);
                    
                    if (hCloseBtn && hGuiFont) {
                        SendMessageW(hCloseBtn, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
                    }
                    
                    // Set focus to edit control
                    SetFocus(hEdit);
                    
                    // Show the window
                    ShowWindow(hPopup, SW_SHOW);
                    UpdateWindow(hPopup);
                }
            }
            break;
        }
        case WM_SETCURSOR: {
            // Change cursor to hand when hovering over filename
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g_filenameRect, pt)) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
            FillRect(hdc, &rc, hBrush);
            return 1;
        }
        case WM_CLOSE: {
            cleanupIconGroups();
            hwndIconViewer = NULL;
            DestroyWindow(hwnd);
            break;
        }
        case WM_DESTROY: {
            break;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void registerIconViewerClass() {
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = IconViewerWndProc;
    wc.hInstance = globalHInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"IconViewerClass";
    RegisterClassEx(&wc);
}

static void showIconInNewWindow(wchar_t* filePath, wchar_t* fileName) {
    cleanupIconGroups();

    iconGroupCount = extractAllIconGroupsFromPE(filePath);
    currentGroupIndex = 0;
    currentIconIndex = 0;

    if (iconGroupCount == 0) {
        MessageBox(hwndMain, L"无法提取文件图标", lc_str.alert, MB_OK);
        return;
    }

    // Pick the best (largest) icon in the first group as default
    struct IconGroup* grp = &iconGroups[0];
    int bestIdx = 0;
    for (int i = 1; i < grp->iconCount; i++) {
        if (grp->sizes[i] > grp->sizes[bestIdx]) bestIdx = i;
    }
    currentIconIndex = bestIdx;

    wcscpy_s(iconViewerFileName, MAX_PATH, fileName);

    WNDCLASSEX wcCheck = {0};
    if (!GetClassInfoEx(globalHInstance, L"IconViewerClass", &wcCheck)) {
        registerIconViewerClass();
    }

    // Compute client area height based on the group with the most unique icon sizes
    int maxUniqueSizes = 0;
    for (int g = 0; g < iconGroupCount; g++) {
        int uniqueSizes[MAX_ICONS_PER_GROUP];
        int uniqueCount = 0;
        
        for (int i = 0; i < iconGroups[g].iconCount; i++) {
            int s = iconGroups[g].sizes[i];
            BOOL isDuplicate = FALSE;
            
            // Check if we already have this size
            for (int j = 0; j < uniqueCount; j++) {
                if (uniqueSizes[j] == s) {
                    isDuplicate = TRUE;
                    break;
                }
            }
            
            if (!isDuplicate) {
                uniqueSizes[uniqueCount] = s;
                uniqueCount++;
            }
        }
        
        if (uniqueCount > maxUniqueSizes)
            maxUniqueSizes = uniqueCount;
    }
    
    int btnsPerRow = 4;
    int maxRows = (maxUniqueSizes + btnsPerRow - 1) / btnsPerRow;
    // Client layout: icon(280) + navRow(35) + sizeBtnRows(34 each) + saveBtn(28) + separator(10) + filename(20) + padding(15)
    int clientH = 280 + 35 + maxRows * 34 + 28 + 10 + 20 + 15;
    if (clientH < 420) clientH = 420;
    int clientW = 380;

    // Convert client size to window size (includes title bar, borders)
    DWORD dwStyle = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
    RECT rc = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - winWidth) / 2;
    int y = (screenHeight - winHeight) / 2;

    hwndIconViewer = CreateWindowEx(
        0,
        L"IconViewerClass",
        L"",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        x, y, winWidth, winHeight,
        hwndMain,
        NULL,
        globalHInstance,
        NULL
    );

    if (!hwndIconViewer) {
        cleanupIconGroups();
        MessageBox(hwndMain, L"无法创建图标查看窗口", lc_str.alert, MB_OK);
        return;
    }

    ShowWindow(hwndIconViewer, SW_SHOW);
    UpdateWindow(hwndIconViewer);
}

static void onMenuItemShowIconClick() {
    if (numSelectedItems != 1 || !selectedItems[0]) return;
    
    struct FileNode* node = selectedItems[0];
    if (!node || !node->name) return;
    
    wchar_t path[MAX_PATH] = {0};
    getFileNodePath(node, path);
    
    if (!isPathExists(path)) {
        MessageBox(hwndMain, L"文件不存在", lc_str.alert, MB_OK);
        return;
    }
    
    showIconInNewWindow(path, node->name);
}


// ===== 排序键 =====
// 排序键严格无副作用：不查图标缓存、不访问文件系统。
// 旧实现的比较器会调用 ensureItemTypeLoaded（内部走 SHGetFileInfo / 路径拼接），
// 而 qsort 必然触达每个元素 —— 连「按名称排序」都会把整个目录的图标物化一遍，
// LVS_OWNERDATA 的懒加载设计被完全抵消（含 exe 的目录会白屏数百毫秒）。

// 文件夹组相对文件组的次序（文件夹组 = 非 TYPE_FILE 的虚拟节点一起算）。
// 返回值符号已按模式含方向语义，调用方必须**直接 return**，不能再过
// finalizeCompare：
//   经典   → 随升降序翻转：升序文件夹在前、降序在后。原版 WFM 即此行为
//            （TYPE_DIR=0 < TYPE_FILE=1，type 差值放进统一的升降序翻转里）
//   置顶   → 恒在前（Windows 资源管理器语义）
//   沉底   → 恒在后
//   不区分 → 返回 0，组间次序交还给本列主键
static int compareFolderGroup(const struct ListItem* ia, const struct ListItem* ib) {
    if (folderSortMode == FOLDER_SORT_PLAIN) return 0;

    bool fa = (ia->node && ia->node->type != TYPE_FILE);
    bool fb = (ib->node && ib->node->type != TYPE_FILE);
    if (fa == fb) return 0;

    int res = fa ? -1 : 1;    // 文件夹组在前
    if (folderSortMode == FOLDER_SORT_BOTTOM) return -res;
    if (folderSortMode == FOLDER_SORT_CLASSIC) return sortAscending ? res : -res;
    return res;               // 置顶
}

static int compareNameOnly(const struct ListItem* ia, const struct ListItem* ib) {
    const wchar_t* na = (ia->node && ia->node->name) ? ia->node->name : L"";
    const wchar_t* nb = (ib->node && ib->node->name) ? ib->node->name : L"";
    // 原版用 wcscoll（项目从未 setlocale，实际即 C locale 的二进制序）。
    // 个别 locale 下 wcscoll 可能对同形异码串返回 0，补 wcscmp 收敛成
    // 确定的全序，qsort 不会因“相等”漂移。
    int res = wcscoll(na, nb);
    if (res == 0) res = wcscmp(na, nb);
    return res;
}

static int finalizeCompare(int res) {
    return sortAscending ? res : -res;
}

// 四个比较器统一结构：文件夹组（模式相关、已含方向语义）→ 本列主键 → 名称。
// compareFolderGroup 的结果必须直接 return，不能进 finalizeCompare。
static int compareName(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareFolderGroup(ia, ib);
    if (res != 0) return res;
    res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

// 「类型」列主键：原版按 FileType 枚举值排，显示的类型名字符串
// （文本文档/应用程序…）与排序解耦。
static int compareType(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareFolderGroup(ia, ib);
    if (res != 0) return res;
    res = (int)(ia->node ? ia->node->type : TYPE_FILE)
        - (int)(ib->node ? ib->node->type : TYPE_FILE);
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

static int compareSize(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareFolderGroup(ia, ib);
    if (res != 0) return res;
    // 主键就是大小。逐字段比较而非 uint64 相减：差值超过 INT_MAX 时
    // 截断成错误符号（原版的 int 相减 bug）。
    res = 0;
    if (ia->size < ib->size) res = -1;
    else if (ia->size > ib->size) res = 1;
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

static int compareDate(const void* a, const void* b) {
    const struct ListItem* ia = (const struct ListItem*)a;
    const struct ListItem* ib = (const struct ListItem*)b;
    int res = compareFolderGroup(ia, ib);
    if (res != 0) return res;
    res = CompareFileTime(&ia->modifiedTime, &ib->modifiedTime);
    if (res == 0) res = compareNameOnly(ia, ib);
    return finalizeCompare(res);
}

void clearIconCaches() {
    // 「清除图标缓存」的唯一职责：把自建图标库（来源表 + 所有显示列表池）整个丢掉，
    // 并让所有条目下次重绘时重新解析。换 exe、改文件关联之后必须走这里才能看到新
    // 图标 —— navigateRefresh()/refreshContentView() 按设计不重查图标。
    //
    // 销毁顺序：先把系统列表挂回控件的两个槽位，再释放自建列表。反过来就是控件
    // 短暂持有已销毁的句柄（Wine 下会画到野指针）。调用方随后的 refreshContentView
    // 会按当前设置重建池并挂上。
    HIMAGELIST himlBig = NULL, himlSmall = NULL;
    Shell_GetImageLists(&himlBig, &himlSmall);
    if (hwndContentView) {
        if (himlBig) ListView_SetImageList(hwndContentView, himlBig, LVSIL_NORMAL);
        if (himlSmall) ListView_SetImageList(hwndContentView, himlSmall, LVSIL_SMALL);
    }
    currentImageList = himlBig;

    // 在途任务与已完成结果一律作废：来源表马上要被整个释放，旧结果的 id 会指向新来源。
    iconRenderInvalidate();
    resetIconStore();
    for (int i = 0; i < numItems; i++) {
        items[i].icon = 0;
        items[i].iconTries = 0;
    }
}

// 更新表头排序指示箭头。
//
// 关键：Win32 / Wine 的 ListView **不会**自动绘制排序三角，必须由应用自己
// 通过 HDI_FORMAT + HDF_SORTUP / HDF_SORTDOWN 设置（Wine 的 comctl32 header.c
// 同样按这个标志绘制），否则用户完全看不出当前按哪列、哪个方向排序。
static void updateSortIndicator() {
    HWND hHeader = ListView_GetHeader(hwndContentView);
    if (!hHeader) return;

    int numCols = Header_GetItemCount(hHeader);
    for (int i = 0; i < numCols; i++) {
        HDITEM hd = {0};
        hd.mask = HDI_FORMAT;
        if (!Header_GetItem(hHeader, i, &hd)) continue;

        UINT fmt = hd.fmt & ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortColumnIdx) {
            fmt |= sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        }
        if (fmt == (UINT)hd.fmt) continue;      // 无变化就不发包，避免无谓重绘

        hd.fmt = fmt;
        Header_SetItem(hHeader, i, &hd);
    }
}

void sortItems() {
    if (!items || numItems <= 0) return;
    switch (sortColumnIdx) {
        case COLUMN_NAME_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareName);
            break;
        case COLUMN_TYPE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareType);
            break;
        case COLUMN_SIZE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareSize);
            break;
        case COLUMN_DATE_IDX:
            qsort(items, numItems, sizeof(struct ListItem), compareDate);
            break;
    }
}

// ===== 图标补齐 / 预取（把图标渲染搬出首帧绘制）=====
//
// 绘制路径（LVN_GETDISPINFO / 大图标自绘）只查已渲染好的槽，绝不现场渲染图标
// （见 getIconSlotForPaint）。缺的图标由这里补。
//
// 为什么值得这么绕：渲染一个图标要展开 PE / 解资源 / 缩放，是毫秒级的活。只要它在
// WM_PAINT 里，首帧就得等整屏图标提完；而 Wine 是先 WM_ERASEBKGND 把客户区擦成窗口
// 底色再发 WM_PAINT，用户看到的就是「点进去闪一下白/空」。所以先画（布局 + 文字 +
// 已有图标的格子），后补。
//
// **补到就画**：绘制一律「有就画」（池里有槽就画出来），缺的先空着；补齐每有进展
// （本屏就位的项数变多）就把绘制报告过缺图标的区间重画一次，重画限流。
//
// 绝不等「可见项全部就位」才画。曾经的「一次成型」是为了消除「一部分有图标、一部分空白」
// 的参差观感，但它的代价是一屏项数越多、空得越久：详细视图一屏五六十到上百项，而补齐每轮
// 只有 8ms 预算、滚动一到就被打断 —— 结果是「只要还剩几项没补上，整屏图标（含文件夹）
// 全都不显示」。渐进填充远好于整屏空白。
//
// 这套循环是**无状态**的：每一趟都重新按当前可见区算范围、重新找还没补的项，因此
// 导航/换尺寸后残留的旧消息只会白跑一趟，不需要代次号。

// 一屏可见区的项数（多算一列两行：贴边被切掉半格的那些，Wine 也会画）。
// 注意**不能**用 LVM_GETCOUNTPERPAGE：Wine 对图标视图直接返回 nItemCount
// （listview.c:6601），在这里等于「把整个目录都算成一屏」。
static int visibleIconSpan(void) {
    RECT rc;
    if (!GetClientRect(hwndContentView, &rc)) return 0;
    int clientW = rc.right - rc.left, clientH = rc.bottom - rc.top;
    if (clientW <= 0 || clientH <= 0) return 0;

    DWORD spacing = (DWORD)ListView_GetItemSpacing(hwndContentView, viewStyle != STYLE_LARGE_ICON);
    int cellW = (int)LOWORD(spacing), cellH = (int)HIWORD(spacing);
    if (cellW <= 0 || cellH <= 0) return 0;

    int cols = clientW / cellW; if (cols < 1) cols = 1;
    int rows = clientH / cellH; if (rows < 1) rows = 1;

    // 详细视图一行一项（列不重复）。这里必须单独算：Wine 的 LVM_GETITEMSPACING 在 report
    // 视图下返回的是 (表头总宽, 字体行高)（comctl32/listview.c 的 LISTVIEW_GetItemSpacing，
    // uView != LV_VIEW_ICON 走 nItemWidth/nItemHeight 分支），照网格乘出来会得到
    // 「2×(行数+2)」这种两倍于真实的跨度，而跨度直接决定每轮补齐要扫多少项 —— 一屏几十行
    // 就够，多算的那部分纯属白扫（滚动时尤其明显，每趟都在白扫上百项）。
    if (viewStyle == STYLE_DETAILS) return rows + 4;
    return (cols + 1) * (rows + 2);
}

// 当前可见区对应的项范围 [first, last)。「本屏要补哪些项」只允许用这一个函数算：
// 补齐循环拿它定范围，绘制入口（requestIconFillForVisible）拿它判「这一屏还有没有缺的」。
// 两边若各算各的，就会出现「补齐说齐了、绘制说还缺」的往复，重画一轮接一轮。
static void visibleItemRange(int* outFirst, int* outLast) {
    int first = (int)ListView_GetTopIndex(hwndContentView);
    if (first < 0) first = 0;
    if (first > numItems) first = numItems;

    int span = visibleIconSpan();
    if (span <= 0) span = 64;      // 客户区还没成形 / 取不到格子尺寸：保守按一屏算
    int last = first + span;
    if (last > numItems) last = numItems;

    *outFirst = first;
    *outLast = last;
}

// 纯查询：这一项**这一刻**在池里有没有可直接绘制的槽？（**不做任何渲染**）
// 用来判断「这一段还有没有缺图标」（见 requestIconFillForVisible / iconFillRange）。
// 判据只有一条：池里有它的槽 = 就位。退避计数**不算**就位 —— 否则补齐会误判「本屏齐了」
// 而停止投递，退避计数也就再没人递减，那几项会一直空着。
static bool itemIconPainted(struct IconPool* p, struct ListItem* item) {
    if (!item->node) return true;      // 空占位项：本来就不画图标
    return iconIdTypeNameValid(item->icon) && poolSlotLookup(p, item->icon) >= 0;
}

// 让第 i 项「该有图标就有图标」。返回 false = 还没就位（这一轮挑不出槽 / 解析还没成功）。
// 失败只做**退避**（iconTries），不写任何「永久失败」标记：哪几项恰好没就位取决于渲染顺序
// 与池的状态，写死一次就是那个文件在本目录内永远没有图标。
static bool itemIconReady(struct IconPool* p, int i, bool allowSync) {
    struct ListItem* item = &items[i];
    if (!item->node) return true;      // 空占位项：本来就不画图标
    if (!item->loaded) loadItemData(item, i);

    // 退避中：这一轮不试。否则来源表暂时满、或这一刻挑不出可淘汰的槽的那几项，会把每轮
    // 8ms 预算全吃掉 —— 而同屏其它「本来能补上」的项就永远轮不到。
    if (item->iconTries > 0) { item->iconTries--; return true; }
    if (itemIconPainted(p, item)) return true;

    // 判据是「池里有没有槽」，**不是**「有没有解析出 id」。
    // loadItemData() 早就把 id 解析好了，但槽位只有下面的 getIconSlot() → poolSlotFor()
    // 才会真正去建；拿 id 判断等于把全部待渲染项都跳过 → 池永远是空的，图标一个都不显示。
    if (getIconSlot(item, allowSync) < 0) {
        item->iconTries = ICON_RETRY_BACKOFF_ROUNDS;
        return false;
    }
    return true;
}

// 这个名字的图标是否必须「按文件」单独取？纯名字判断，不碰文件系统、不碰 shell。
// 判据与 resolveIconId 里的 exclusive 分支一致：exe 可能自带图案（要解 PE）、
// lnk 要读自己的 ICONLOCATION、ico 要解自己的图案。
static bool nodeIconIsPerFile(const struct FileNode* node) {
    if (!node || node->type != TYPE_FILE) return false;
    const wchar_t* ext = wcsrchr(node->name, L'.');
    if (!ext) return false;
    return wcsicmp(ext, L".exe") == 0 || wcsicmp(ext, L".lnk") == 0 || wcsicmp(ext, L".ico") == 0;
}

// ================= 滚动前「预渲染 + 就位再滚」 =================
//
// 为什么需要它：Wine 的 comctl32/listview.c 里 scroll_list() 是
//     ScrollWindowEx(infoPtr->hwndSelf, ..., SW_ERASE | SW_INVALIDATE);
//     UpdateWindow(infoPtr->hwndSelf);        ← 同步 WM_PAINT
// —— 滚动消息**处理的过程中**，新露出的一屏就被画掉了。任何「先滚、再补图标」的做法都只能
// 事后重画那一帧，用户看到的就是「图标先消失、紧接着又出现」。唯一能根治的顺序是反过来：
// **先把目标窗口的图标渲进池，再让控件滚**。
//
// 三种滚动分开对待：
//   · 拖滑块（SB_THUMBTRACK）**快拖**（相邻两条消息间隔 < ICON_SCROLL_FAST_DRAG_MS）：跟手优先。
//     押后会让内容比滑块滞后，连续拖动时那就是「滚动条卡一下」—— 快拖一律不押后，预渲染一次
//     就立刻把消息交回控件（稳态下池已命中，几乎不花钱）。
//   · 拖滑块（SB_THUMBTRACK）**慢拖**：连续几十上百条消息。渲完了就滚；没渲完就先押着不滚 ——
//     押着同时也省掉了这一次整屏重画。押后上限 ICON_SCROLL_HOLD_MAX_MS，超了就照滚：
//     宁可闪一下，也不能让内容粘在滑块后面不动。押后期间滑块本身照旧跟手 ——
//     它的位置由 win32u/scroll.c 的 g_tracking_info 直接画，与我们滚没滚无关。
//   · 松手（SB_THUMBPOSITION）：最后一次一定要画全，可以多花点时间（ICON_SCROLL_FINAL_MS）。
//   · 翻页 / 行滚 / 顶底（离散一次）：用户没在连着拖，多等几十毫秒看不出来，一次渲完再滚。
//
// 目标窗口的顶项怎么来：**详细视图里滚动条位置就是项号**（listview.c 的 LISTVIEW_GetTopIndex
// 在 LV_VIEW_DETAILS 下直接返回 scrollInfo.nPos），映射精确 —— 只有这种情形才押后。
// 图标 / 列表视图的位置单位是像素（nItem = GetCountPerRow × (nPos / nItemHeight)），只能估算，
// 那里只做「尽力预渲染」不押后：估错了押后就永远等不到「就位」，内容会一直滞后。
// 无论哪种视图，「滚」这个动作都是把原消息交回控件、由 Wine 自己算增量，所以窗口估错最多是
// 白渲染几次，不会滚错位置。
static bool preScrollVertInfo(SCROLLINFO* si) {
    si->cbSize = sizeof(*si);
    si->fMask = SIF_ALL;
    return GetScrollInfo(hwndContentView, SB_VERT, si) != FALSE;
}

// 这条滚动码的「目标顶项」。返回 -1 = 不认识，别拦。*exact = 映射是否精确。
static int preScrollTargetTop(UINT code, const SCROLLINFO* si, bool* exact) {
    *exact = (viewStyle == STYLE_DETAILS);

    if (*exact) {
        long top;
        switch (code) {
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: top = (long)si->nTrackPos; break;
        case SB_LINEUP:        top = (long)si->nPos - 1; break;
        case SB_LINEDOWN:      top = (long)si->nPos + 1; break;
        case SB_PAGEUP:        top = (long)si->nPos - (long)si->nPage; break;
        case SB_PAGEDOWN:      top = (long)si->nPos + (long)si->nPage; break;
        case SB_TOP:           top = 0; break;
        case SB_BOTTOM:        top = (long)si->nMax; break;
        default: return -1;
        }
        if (top < 0) top = 0;
        if (top > numItems - 1) top = numItems - 1;
        return (int)top;
    }

    // 图标 / 列表视图：位置单位是像素（列表视图是「列」），按「格高 × 每行项数」换算。
    if (code != SB_THUMBTRACK && code != SB_THUMBPOSITION) return -1;

    RECT rc;
    if (!GetClientRect(hwndContentView, &rc)) return -1;
    int clientW = rc.right - rc.left, clientH = rc.bottom - rc.top;
    if (clientW <= 0 || clientH <= 0) return -1;
    DWORD spacing = (DWORD)ListView_GetItemSpacing(hwndContentView, viewStyle != STYLE_LARGE_ICON);
    int cellW = (int)LOWORD(spacing), cellH = (int)HIWORD(spacing);
    if (cellW <= 0 || cellH <= 0) return -1;
    int cols = clientW / cellW; if (cols < 1) cols = 1;
    int rows = clientH / cellH; if (rows < 1) rows = 1;

    long d = (long)si->nTrackPos - (long)si->nPos;      // 位置单位的增量
    long top = (long)ListView_GetTopIndex(hwndContentView);
    if (viewStyle == STYLE_LIST) top += d * rows;       // 列表视图：1 个单位 = 一列
    else                         top += (d / cellH) * cols;
    if (top < 0) top = 0;
    if (top > numItems - 1) top = numItems - 1;
    return (int)top;
}

// 把 [top, top + 一屏) 这一段的来源渲进池，最多花 budgetMs。返回 true = 这一段已经全就位
// （「渲染失败、已记退避」的项也算就位 —— 它们永远不会就位，当阻塞项会让押后永远等不到头）。
static bool iconPrepareWindow(int top, int budgetMs) {
    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return true;   // 池没挂上控件：这里管不了，交给原路
    if (!items || numItems <= 0) return true;

    int span = visibleIconSpan();
    if (span <= 0) span = 64;
    if (top < 0) top = 0;
    int last = top + span;
    if (last > numItems) last = numItems;

    DWORD deadline = GetTickCount() + (DWORD)(budgetMs > 0 ? budgetMs : 0);
    bool ready = true;
    for (int i = top; i < last; i++) {
        struct ListItem* it = &items[i];
        if (!it->node) continue;          // 空占位项：本来就不画图标
        if (it->iconTries > 0) continue;  // 退避中：当它已定，别阻塞
        if (itemIconPainted(p, it)) continue;
        if ((int)(GetTickCount() - deadline) >= 0) { ready = false; break; }
        itemIconReady(p, i, true);        // 必须同步：这就是「先渲好再滚」的全部意义
        if (!itemIconPainted(p, it) && it->iconTries == 0) ready = false;
    }
    return ready;
}

// 只查池、不渲染：目标窗口里还有几项没就位。退避中的项不算 —— 它们这几轮永远不会就位，
// 计入缺口只会让「押后也没用」被误判（见 preScrollHandleVScroll）。
static int iconWindowMissing(int top) {
    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return 0;
    if (!items || numItems <= 0) return 0;

    int span = visibleIconSpan();
    if (span <= 0) span = 64;
    if (top < 0) top = 0;
    int last = top + span;
    if (last > numItems) last = numItems;

    int miss = 0;
    for (int i = top; i < last; i++) {
        struct ListItem* it = &items[i];
        if (!it->node) continue;
        if (it->iconTries > 0) continue;
        if (!itemIconPainted(p, it)) miss++;
    }
    return miss;
}

static void preScrollCancelRetry(void) {
    if (!iconScrollTimerPending) return;
    if (hwndContentView) KillTimer(hwndContentView, TIMER_ICON_SCROLL);
    iconScrollTimerPending = false;
}

// 把滚动交给控件。只允许在「目标窗口已经渲好」之后调 —— 它内部会同步重画这一屏。
static void preScrollApply(UINT code) {
    pendingScrollTop = -1;
    preScrollCancelRetry();
    inPreScroll = true;
    inScrollMessage = true;      // 这次绘制算「滚动驱动」：准许绘制路径就地渲染兜底
    OrigWndProc(hwndContentView, WM_VSCROLL, MAKEWPARAM(code, 0), 0);
    inScrollMessage = false;
    inPreScroll = false;
}

// 返回 true = 这条 WM_VSCROLL 已经处理完（已经滚过，或者正押着）。false = 照常交给控件。
static bool preScrollHandleVScroll(UINT code) {
    if (code == SB_ENDSCROLL) {
        // 拖动结束：最后一步已经在 SB_THUMBPOSITION 里画全并滚过了，押后状态就此作废。
        pendingScrollTop = -1;
        preScrollCancelRetry();
        return false;      // 这条对滚动条来说只是「结束了」的通知，照样交给控件
    }

    bool isThumb    = (code == SB_THUMBTRACK);
    bool isFinal    = (code == SB_THUMBPOSITION);
    bool isDiscrete = (code == SB_PAGEUP || code == SB_PAGEDOWN ||
                       code == SB_LINEUP || code == SB_LINEDOWN ||
                       code == SB_TOP    || code == SB_BOTTOM);
    if (!isThumb && !isFinal && !isDiscrete) return false;

    SCROLLINFO si;
    if (!preScrollVertInfo(&si)) return false;

    bool exact = false;
    int top = preScrollTargetTop(code, &si, &exact);
    if (top < 0) return false;

    // 拖动速率：相邻两条 WM_VSCROLL 的间隔。lastScrollTick 此刻还是**上一条**滚动消息的
    // 时间戳（本条的更新在窗口过程尾部），正好用来量间隔。
    //
    // 快拖与慢拖分派不同。**押后**（渲好目标窗口才让控件滚）能让那一帧不留空格子，代价是内容
    // 比滑块滞后一小段时间 —— 连续快拖时这个滞后正是用户看到的「滚动条卡一下」。所以快拖一律
    // 不押后：预渲染照做（一次小时间片就回到控件；稳态下这些来源早在池里，等于不花钱，只有
    // 首次经过新区域才真付），宁可丢一两个图标，也不能粘。
    DWORD now = GetTickCount();
    bool fastDrag = isThumb && !preScrollFromTimer &&
                    (now - lastScrollTick) < ICON_SCROLL_FAST_DRAG_MS;

    if (fastDrag) {
        pendingScrollTop = -1;
        preScrollCancelRetry();
        iconPrepareWindow(top, ICON_SCROLL_PREPARE_MS);
        return false;
    }

    // 图标 / 列表视图：目标窗口只能估算，不做押后。尽力渲一遍再照滚 —— 蒙对了这一帧就是
    // 完整的，蒙错了退化成原来的行为（绘制路径的就地渲染兜底）。
    if (isThumb && !exact) {
        iconPrepareWindow(top, ICON_SCROLL_PREPARE_MS);
        return false;
    }

    // 缺口太大（目标窗口整片都是没见过的来源）：渲不完，押着也白押 —— 上限一到照样滚，
    // 还平白让内容滞后一段时间。直接放行，让滑块跟手（缺的图标等停手后的补齐补上）。
    if (isThumb && iconWindowMissing(top) > ICON_SCROLL_HOLD_MAX_MISS) {
        pendingScrollTop = -1;
        preScrollCancelRetry();
        return false;
    }

    bool ready = iconPrepareWindow(top, isThumb ? ICON_SCROLL_PREPARE_MS : ICON_SCROLL_FINAL_MS);

    if (isFinal || isDiscrete) {
        // 一次性滚动：多补一次大预算（用户没在连着拖，等一下看不出来），然后照滚。
        if (!ready) iconPrepareWindow(top, ICON_SCROLL_FINAL_MS);
        preScrollApply(code);
        return true;
    }

    // 拖滑块：就位就滚；没就位先押着不滚（顺带省掉这次整屏重画）。
    if (pendingScrollTop < 0) pendingScrollSince = now;   // 这一轮押后的起点
    pendingScrollTop = top;

    if (ready || now - pendingScrollSince >= ICON_SCROLL_HOLD_MAX_MS) {
        preScrollApply((UINT)SB_THUMBTRACK);
        return true;
    }

    // 押后期间必须自己找机会重试（见 WM_TIMER 的 TIMER_ICON_SCROLL 分支）。
    if (!iconScrollTimerPending) {
        iconScrollTimerPending = true;
        if (!SetTimer(hwndContentView, TIMER_ICON_SCROLL, ICON_SCROLL_RETRY_MS, NULL)) {
            // SetTimer 失败就别押了：宁可闪一下，也不能把内容押死在这里。
            iconScrollTimerPending = false;
            preScrollApply((UINT)SB_THUMBTRACK);
        }
    }
    return true;
}

// 目录加载时把「共享来源」一次性渲染进池 —— 在首帧之前，且排在 iconFillVisibleSync 前面。
//
// 为什么非它不可：共享来源（目录 / 驱动器 / 按扩展名登记的非 exe、lnk、ico 文件）的图案与
// **具体哪个文件**无关 —— system32 里几百个 .dll 全部共用同一个来源 id，渲染一次就能点亮
// 整屏。可池是按尺寸惰性填充的：**首次**进那个目录时池里一个 .dll 槽都没有，首帧那一片
// dll 就是空格子，要等补齐渲染完再补画上去，看上去正是「图标消失一下又立刻显示」。
//
// 为什么必须排在 iconFillVisibleSync **之前**：那 150ms 首屏预算按项顺序花，同屏的 exe 会
// 先把它吃干（每个 exe 一次 ICONLOCATION 加一次 PE 解码，几十毫秒级），排在后面的共享来源
// 就整个轮不到 —— 这正是「首次进 system32，闪的是 dll 而不是 exe」的成因。共享来源单独
// 先跑，数量只有「本目录的扩展名种类」（通常个位数到几十），于是无论首屏预算够不够，
// dll / 文件夹 / 常见扩展名这些一定是第一帧就在的。
//
// 独占项（exe / lnk / ico）刻意**不**预热：它们每个文件都要单独解码，代价随目录里的 exe
// 数量线性增长，那正是首屏同步预算与邻域预取该花的地方。
static void prewarmSharedIcons(void) {
    if (!hwndContentView || !items || numItems <= 0) return;

    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return;

    // 共享槽**不参与 LRU 淘汰**（见 poolSlotFor），所以预热必须自带有界：最多占半个池，
    // 剩下一半留给独占项。否则整池被共享项占满，独占项进来只能反过来把共享项顶掉，
    // 一个进一个出，来回翻烧饼 —— 那又是另一种「图标闪」。
    int sharedInPool = 0;
    for (int i = 0; i < p->slotCount; i++) {
        int owner = p->slotId[i];
        if (owner > 0 && owner <= iconSourceCount && (iconSources[owner - 1].flags & ICONSRC_SHARED))
            sharedInPool++;
    }
    int budgetShared = p->slotCount / 2 - sharedInPool;
    if (budgetShared > ICON_PREWARM_MAX) budgetShared = ICON_PREWARM_MAX;
    if (budgetShared <= 0) return;

    DWORD deadline = GetTickCount() + ICON_PREWARM_BUDGET_MS;
    int added = 0;

    // 按显示顺序扫（文件夹在前、同类按名）：先在屏上的项优先，预算不够时先保它们。
    for (int i = 0; i < numItems; i++) {
        // 扫描本身也要受预算约束：几万项的目录里「所有共享来源都已在池中」时一次都不会命中
        // 下面的 break，逐项扫完虽然只要几毫秒，但没必要为它多等。每 64 项查一次时钟。
        if ((i & 63) == 0 && i > 0 && GetTickCount() >= deadline) break;

        struct ListItem* item = &items[i];
        if (!item->node) continue;
        if (nodeIconIsPerFile(item->node)) continue;   // exe/lnk/ico：交给首屏同步补齐

        // 只解析 id，**不**走 loadItemData：后者会顺带格式化大小与日期，而这一趟只要图标
        // （虚拟列表「按需物化」的约定也不该被这里打破，提前解析的只有图标这一项）。
        // 同一扩展名的第二个文件起都是去重表的命中，连 shell 都不碰，所以整趟扫描的代价是
        // 「本目录的扩展名种类 × 一次 shell 查询」，与文件总数无关。
        int id = resolveIconId(item);
        if (id <= 0) continue;
        if (poolSlotLookup(p, id) >= 0) continue;      // 这条来源已经在池里了

        if (poolSlotFor(p, id, true) >= 0) added++;
        if (added >= budgetShared) break;
        if (GetTickCount() >= deadline) break;
    }
}

// 补齐第一步用来收集「本屏用到的来源 id」的去重缓冲（见 iconFillRange）。
// 静态复用，避免每轮分配；容量不够才翻倍。nIds 通常个位数到十几。
// 进程级一次性缓冲，不随目录切换释放（最多几百个 int）。
static int* iconFillIds = NULL;
static int iconFillIdsCap = 0;

// 补一轮可见区（含绘制上报的项），deadline = 时间预算到点时刻。
// 返回 true = 范围内还有没就位的（本屏这一帧还画不出来）。
static bool iconFillRangeInner(DWORD deadline, bool allowSync) {
    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return false;

    DWORD scrollTickAtEntry = lastScrollTick;

    int first, last;
    visibleItemRange(&first, &last);
    // 可见区范围单独留一份：第一步的「按 id 去重渲染」只扫可见区（它才是用户正看着的地方，
    // 而且项数有界、去重不会变慢）；绘制上报的 miss 区间可能横跨上千项，交给第二步逐项兜底。
    int visFirst = first, visLast = last;

    // 把绘制路径上报的「缺图标」区间并进来（取走后立刻复位）。绘制看到的东西必然在屏上；
    // 只要漏掉一个，就会出现「补齐判定完成 → 重画 → 绘制还是缺 → 又安排补齐」的往复。
    int missFirst = iconMissFirst, missLast = iconMissLast;
    if (missLast >= missFirst) {
        if (missFirst < first) first = missFirst;
        if (missLast + 1 > last) last = missLast + 1;
    }
    iconMissFirst = numItems;
    iconMissLast = -1;
    if (first < 0) first = 0;
    if (last > numItems) last = numItems;
    if (first >= last) return false;

    // 本轮开始时本屏已就位的项数。收工时再数一次，用来判断「这一轮有没有真的推进」。
    int paintedAtEntry = 0;
    for (int i = first; i < last; i++) if (itemIconPainted(p, &items[i])) paintedAtEntry++;

    // ---- 第一步：按「去重后的来源 id」渲染 ----
    //
    // 为什么按 id 而不是按项：一屏几十项往往只对应**个位数**的来源 —— 文件夹、.dll、常见
    // 扩展名这些共享项一个 id 就能点亮几十个格子，只有 exe/lnk/ico 才各占一个。按项顺序补时，
    // 一旦预算用尽、或被滚动打断，靠后的项就整片空着；而它们多半和前面共用同一个来源，
    // 本来一次渲染就能点亮。先渲染 id，等于用最少的毫秒换最多的格子。
    //
    // 这正是「首次进入 windows/system32 这类目录才看到图标闪」的解法：那里的可见区里绝大多数
    // 是共用同一个 .dll 来源的格子，去重后往往只剩几次渲染 —— 首屏那 150ms 同步预算就够补完。
    int nIds = 0;
    for (int i = visFirst; i < visLast; i++) {
        struct ListItem* it = &items[i];
        if (!it->node) continue;
        if (!it->loaded) loadItemData(it, i);          // 便宜：来源命中表时连 shell 都不碰
        if (!iconIdTypeNameValid(it->icon)) continue;
        bool dup = false;
        for (int k = 0; k < nIds; k++) if (iconFillIds[k] == it->icon) { dup = true; break; }
        if (dup) continue;
        if (nIds >= iconFillIdsCap) {
            int newCap = iconFillIdsCap ? iconFillIdsCap * 2 : 64;
            int* tmp = realloc(iconFillIds, (size_t)newCap * sizeof(int));
            if (!tmp) break;                        // 扩容失败就退化成逐项补（第二步仍会走）
            iconFillIds = tmp;
            iconFillIdsCap = newCap;
        }
        iconFillIds[nIds++] = it->icon;
        // 预算用尽就停止收集：先把已经收集到的渲染掉（它们覆盖的格子最多），
        // 没收集到的项交给第二步。没有这条检查，收集阶段的 shell 查询会把首屏同步补齐
        // 的预算整个吃掉，进目录反而更慢。
        if (GetTickCount() >= deadline && nIds > 0) break;
    }

    for (int k = 0; k < nIds; k++) {
        if (lastScrollTick != scrollTickAtEntry) break;       // 用户又在滚了：让位
        if (GetTickCount() >= deadline) break;
        if (poolSlotLookup(p, iconFillIds[k]) >= 0) continue; // 池里已有槽：不必重渲染
        poolSlotFor(p, iconFillIds[k], allowSync);            // 渲染（或排给后台）+ 占一个槽
    }

    // ---- 第二步：逐项兜底 ----
    // 第一步点亮的是「已经解析出 id 的来源」；这一步负责剩下的：还没解析出 id 的项、以及渲染
    // 失败需要记退避的项。已经点亮的项在这里只是几次数组读取，开销可忽略。
    // 时间预算按「处理完一项再判」的方式用：即使预算早已过期也要保证每趟至少推进一项，
    // 否则就成了「0 进展却不停投递」的空转。
    bool ready = true;
    for (int i = first; i < last; i++) {
        if (!itemIconReady(p, i, allowSync)) ready = false;
        // 用户又在滚了：让位。滚动期间的画面应当是「池里已有的照常画、少数格子先空着」，
        // 停手后（滚动静默）的绘制会重新安排补齐。
        if (lastScrollTick != scrollTickAtEntry) return true;
        if (GetTickCount() >= deadline) { if (i + 1 < last) ready = false; break; }
    }

    int paintedAtExit = 0;
    for (int i = first; i < last; i++) if (itemIconPainted(p, &items[i])) paintedAtExit++;

    // 本轮**补进了新图标** → 把绘制报告过缺图标的区间重画一次（**不擦背景**，不闪）。
    // 判据是「本屏就位的项数变多了」，不是「全屏就位」：等全屏就位才画，正是「滚动/翻页后
    // 整屏图标长时间空白」的成因。重画限流，免得把补齐那点预算全花在重绘上。
    if (!ready && paintedAtExit > paintedAtEntry && missLast >= missFirst) {
        DWORD now = GetTickCount();
        if (now - lastIconRepaintTick >= ICON_REPAINT_MIN_MS) {
            lastIconRepaintTick = now;
            iconPaintMissed = false;              // 收口那一次不必再来一遍
            if (missFirst < 0) missFirst = 0;
            if (missLast >= numItems) missLast = numItems - 1;
            ListView_RedrawItems(hwndContentView, missFirst, missLast);
        }
    }

    if (ready) { iconFillStall = 0; return false; }

    // 连续几轮「本屏就位的项数一个都没变多」＝ 剩下的项这几轮渲染不出来（注册表坐标指向
    // 本 prefix 里不存在的文件、池这一刻挑不出可淘汰的槽……）。**停止自续投递**，别在后台
    // 空转：下一次绘制/滚动会重新安排补齐，那时它们会被再试一次。
    // 这里绝不写任何「永久失败」标记（退避计数由 itemIconReady 管）：哪几项恰好没就位取决于
    // 渲染顺序与滚动时机，把随机几项永久标成失败，正是「随机文件在本目录内永远缺图标」的根因。
    if (paintedAtExit <= paintedAtEntry) {
        if (++iconFillStall >= ICON_FILL_STALL_ROUNDS) { iconFillStall = 0; return false; }
    }
    else iconFillStall = 0;

    return true;   // 下一趟继续（scheduleIconFill 按滚动静默决定立刻投递还是挂定时器）
}

// 补齐循环的重入闸：iconFillIds 是静态复用缓冲，重入会让内层覆盖外层正在用的那份
// （外层那轮就会漏渲几条来源 —— 表现正是「随机几项缺图标」）。重入时让内层直接退出，
// 外层那轮结束后按正常节奏还会再投递。
static bool iconFillRange(DWORD deadline, bool allowSync) {
    if (inIconFillRange) return false;
    inIconFillRange = true;
    bool more = iconFillRangeInner(deadline, allowSync);
    inIconFillRange = false;
    return more;
}

// 补一轮可见区图标（异步路径）。返回 true = 还没补完，需要继续投递。
static bool iconFillStep(void) {
    if (!hwndContentView || !items || numItems <= 0) return false;

    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return false;   // 池没挂上：交给系统列表兜底

    // 这里**不再**每轮 UpdateWindow 强刷。曾经的做法是把「当前状态」立刻推到屏幕上，
    // 但补齐每轮都刷一次，等于把「一半项还没有图标」的中间状态反复亮出来 —— 滚动时看到的
    // 图标闪，有一半来自这里。画面交给控件自己调度：补进来的图标由 iconFillRange 的
    // 「有进展就重画缺过的区间」（不擦背景）负责呈现。
    if (iconFillRange(GetTickCount() + ICON_FILL_BUDGET_MS, false)) return true;

    // 本屏补完（或这几轮实在渲染不出来）：把绘制报告过「缺图标」的区间再重画一次
    // （**不擦背景**，不闪）。补齐过程中每有进展已经重画过一次，这里只是收口，
    // 覆盖「补齐先于首次绘制完成」这种一次都没重画过的情形。
    if (iconPaintMissed) {
        iconPaintMissed = false;
        InvalidateRect(hwndContentView, NULL, FALSE);
    }
    scheduleIconPrefetch();    // 可见区搞定了，再用空档预取邻域
    return false;
}

// 在**下一次绘制之前**同步把本屏补齐。绘制路径只画已经就位的图标，所以补齐跑在绘制
// 前面时，那一帧就已经是完整的 —— 首屏因此不会「先空白再一格一格冒」。
// 预算内补不完就交给异步循环收尾：剩下的格子先是空的，补齐每有进展就把缺过的区间重画
// 一次（逐步填充），绝不会出现「等整屏齐了才画」那种长时间整屏空白。
static void iconFillVisibleSync(int budgetMs) {
    if (!hwndContentView || !items || numItems <= 0) return;

    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return;

    if (iconFillRange(GetTickCount() + (DWORD)budgetMs, true)) {
        scheduleIconFill();     // 没补完：交给异步循环接着补
        return;
    }
    scheduleIconPrefetch();
}

// 绘制入口：可见区里还有缺图标的项就安排一轮补齐。
// 只做**纯池查询**（一屏最多几十次数组读取，绝不渲染）—— 这里不能渲染，否则首帧就要等
// 整屏图标提完（Wine 先 WM_ERASEBKGND 擦成窗口底色再 WM_PAINT，用户看到的是「点进去
// 闪一下白」）。
//
// 它不再决定「这一帧画不画图标」：绘制一律「有就画」。
//
// 这里**不做**「滚动静默」判断，交给 scheduleIconFill —— 它在静默期内挂一次性定时器、
// 静默过后直接投消息。必须这样：滚动静默之后很可能一次绘制都不再发生（画面已经静止），
// 若这里直接返回就再没人叫补齐，那一屏图标会永远空着。定时器则一定会到点。
static void requestIconFillForVisible(void) {
    if (!hwndContentView || !items || numItems <= 0) return;

    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return;

    int first, last;
    visibleItemRange(&first, &last);
    for (int i = first; i < last; i++) {
        if (!itemIconPainted(p, &items[i])) { scheduleIconFill(); return; }
    }
}

// ===== 邻域预取 =====
//
// 拖滚动条时新滚进来的区域之所以「有的有图标、有的空白」，是因为那些项从来没被渲染过，
// 而同屏的目录 / 常见扩展名图标早在池里 —— 于是哪几个空白取决于池里恰好缓存了什么。
// 这里在可见区就位之后，把两侧邻域也提前渲染好。
//
// 严格只用**空槽**（p->filled < p->slotCount）：槽用完就收工。这样既天然有界（一个尺寸
// 的池最多预取 slotCount 个），也绝不会为了还没看到的东西去淘汰屏上正在显示的图标 ——
// 一旦允许淘汰，刚预取的那批就会成为 LRU 受害者、被随后的渲染顶掉，来回翻烧饼。
//
// 走 WM_TIMER 而不是 PostMessage：定时器是消息队列里优先级最低的，输入永远排在它前面。
// 补齐消息当初就因为优先级高于输入，把拖动「粘住」过（见 scheduleIconFill）；预取没有
// 「必须立刻做」的理由，不该冒这个险。
static bool iconPrefetchStep(void) {
    if (!hwndContentView || !items || numItems <= 0) return false;

    struct IconPool* p = currentIconPool();
    if (!p || p->himl != currentImageList) return false;
    if (iconPrefetchDoneSize == p->size) return false;   // 这个尺寸已经收工
    if (p->filled >= p->slotCount) { iconPrefetchDoneSize = p->size; return false; }  // 没有空槽
    if (iconFillPosted) return true;                     // 可见区还有活没干完：先紧着它

    // 这里**不再**要求「滚动静默」。滚动恰恰是预取最有用的时候：滚动带进视口的项如果没被提前
    // 渲染，那一格就会先空、随后补齐才画上 —— 用户看到的就是「滚动时随机几个图标闪一下」。
    // 预取走 WM_TIMER（消息队列里优先级最低，输入永远排在它前面），每趟也只占 8ms，
    // 不会跟拖动抢消息。

    int span = visibleIconSpan();
    if (span <= 0) return false;
    int visFirst = (int)ListView_GetTopIndex(hwndContentView);
    if (visFirst < 0) visFirst = 0;
    int visLast = visFirst + span;
    if (visLast > numItems) visLast = numItems;

    int filledAtEntry = p->filled;
    DWORD deadline = GetTickCount() + ICON_FILL_BUDGET_MS;

    // 向下、再向上：拖动多数是往下走，先补下面。两个方向都走到头才算收工。
    for (int dir = 0; dir < 2; dir++) {
        int i    = (dir == 0) ? visLast : visFirst - 1;
        int end  = (dir == 0) ? numItems : -1;
        int step = (dir == 0) ? 1 : -1;
        for (; i != end; i += step) {
            if (p->filled >= p->slotCount) { iconPrefetchDoneSize = p->size; return false; }
            if (GetTickCount() >= deadline) {
                // 一趟下来一个图标都没进池（例如整段目录全是共享图标，光扫前缀就吃满预算）：
                // 再继续也只是反复扫同一段，收工。宁可不预取，也不要后台空转。
                if (p->filled == filledAtEntry) iconPrefetchDoneSize = p->size;
                return p->filled != filledAtEntry;
            }
            itemIconReady(p, i, false);   // 失败也不管：预取不需要「全部就位」的语义
        }
    }
    iconPrefetchDoneSize = p->size;   // 目录两端都走到了：彻底收工
    return false;
}

static void scheduleIconPrefetch(void) {
    if (iconPrefetchTimerPending || !hwndContentView) return;
    iconPrefetchTimerPending = true;
    if (!SetTimer(hwndContentView, TIMER_ICON_PREFETCH, ICON_PREFETCH_INTERVAL_MS, NULL))
        iconPrefetchTimerPending = false;
}

// 同一时刻只挂一条补齐消息：绘制每帧都会来安排一次，不设这个闸就会一次排上一屏
// 消息，把 WM_PAINT 挤到最后。（变量本体声明在文件开头，供窗口过程引用。）
static void scheduleIconFill(void) {
    if (!hwndContentView) return;

    DWORD now = GetTickCount();

    // 看门狗：两个调度闸（iconFillPosted / iconFillTimerPending）只要卡住超过 1 秒就强制
    // 复位。它们是「同一时刻只挂一条」的唯一凭据 —— 一旦卡在 true，之后**所有**补齐调度
    // 都被挡掉，整个图标子系统就再也不动了，表现正是「滚过一次之后图标再也不补回来」。
    // 卡住的可能来源：SetTimer 失败、消息被丢弃、定时器被外部 KillTimer。
    if (now - iconFillDispatchTick > 1000 && (iconFillPosted || iconFillTimerPending)) {
        iconFillPosted = false;
        iconFillTimerPending = false;
        KillTimer(hwndContentView, TIMER_ICON_FILL);
    }

    if (iconFillPosted) return;

    // 滚动刚发生过就先让位。这一步不是优化而是必须的：
    // 拖动滚动条时列表在连续重绘，每一帧绘制都会走到这里；而 PostMessage 投出去的消息
    // 在队列里的优先级**高于输入消息**，于是「补齐 → 每轮开头的 UpdateWindow 又触发绘制
    // → 绘制又安排补齐」这个自续环会把鼠标消息一直压在后面 —— 拖动就感觉「粘住了」，
    // 文件越多（滚动范围越大、越容易滚到还没补图标的区域）越明显。
    // 这里改成挂一个一次性定时器等滚动静默；WM_TIMER 的优先级最低，不会跟拖动抢。
    if (now - lastScrollTick < ICON_FILL_SCROLL_QUIET_MS) {
        if (!iconFillTimerPending) {
            iconFillTimerPending = true;
            iconFillDispatchTick = now;
            // 返回值必须检查：SetTimer 失败却把闸留在 true，补齐就永久停摆了。
            if (!SetTimer(hwndContentView, TIMER_ICON_FILL, ICON_FILL_SCROLL_QUIET_MS, NULL))
                iconFillTimerPending = false;
        }
        return;
    }

    iconFillPosted = true;
    iconFillDispatchTick = now;
    // 投递失败必须把闸复位：这个闸是「同一时刻只挂一条」的唯一凭据，一旦卡在 true，
    // 之后所有补齐调度都会被它挡掉，整个图标子系统就再也不动了。
    if (!PostMessage(hwndContentView, MSG_ICON_FILL, 0, 0)) iconFillPosted = false;
}

void refreshContentView() {
    if (searchData != NULL) {
        if (searchData->active) {
            searchData->active = false;
            searchData->canceled = true;
        }
        return;
    }
    
    // 换目录：押后滚动的目标（项号）已经没有任何意义，连同重试定时器一起作废。
    pendingScrollTop = -1;
    preScrollCancelRetry();

    // 清空项目数据（保留列，避免在详细信息视图中删除/重建列导致的闪烁）
    ListView_SetItemCountEx(hwndContentView, 0, 0);
    
    if (items) {
        for (int i = 0; i < numItems; i++) {
            if (items[i].path) {
                free(items[i].path);
                items[i].path = NULL;
            }
        }
        free(items);
        items = NULL;
    }
    numItems = 0;
    freeMenuItems();

    // 清理图标查看器资源
    cleanupIconGroups();
    
    // 仅在非详细信息视图中删除列（避免列闪烁）
    if (viewStyle != STYLE_DETAILS) {
        HWND hHeader = ListView_GetHeader(hwndContentView);
        if (hHeader) {
            int numCols = Header_GetItemCount(hHeader);
            for (int i = numCols - 1; i >= 0; i--) {
                ListView_DeleteColumn(hwndContentView, i);
            }
        } else {
            ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
        }
    }
    
    // 详细信息视图：保留列（避免删除/重建导致的闪烁），仅在首次进入时创建
    if (viewStyle == STYLE_DETAILS) {
        HWND hHeader = ListView_GetHeader(hwndContentView);
        if (!hHeader || Header_GetItemCount(hHeader) == 0) {
            createLVColumns();
        } else {
            // 搜索模式遗留的PATH列需要移除（非搜索模式下不需要）
            int numCols = Header_GetItemCount(hHeader);
            if (numCols > COLUMN_PATH_IDX) {
                ListView_DeleteColumn(hwndContentView, COLUMN_PATH_IDX);
            }
        }
    }

    // 先数一遍子节点再一次性分配。这一步是纯指针遍历（不碰文件系统），
    // 而旧实现从 64 起翻倍 realloc，10000 项时要累计 memcpy 约 7.7 MB；
    // sibling 链刚由 buildChildNodes 建好，数一遍几乎免费。
    // childCount == 0 时保持 items == NULL（上面刚 free 过），语义不变。
    int childCount = 0;
    for (struct FileNode* node = currPathFileNode->children; node; node = node->sibling)
        childCount++;

    numItems = 0;
    itemsCapacity = 0;
    if (childCount > 0) {
        items = malloc((size_t)childCount * sizeof(struct ListItem));
        if (!items) return;

        itemsCapacity = childCount;
        numItems = childCount;

        int idx = 0;
        for (struct FileNode* node = currPathFileNode->children; node; node = node->sibling) {
            struct ListItem* item = &items[idx++];
            memset(item, 0, sizeof(struct ListItem));
            item->node = node;
            item->loaded = false;

            fillFileInfo(node, item);
        }
    }

    // 图标缓存保留（不清空），以加速相邻导航
    // 更新图像列表：挂上当前显示尺寸对应的自建列表池。
    // 两个槽位都挂同一份 —— LVS_SMALL 供详细/列表/小图标视图用，LVS_NORMAL 供大图标
    // 视图用；两个都挂上，就不存在「换尺寸后旧列表仍被控件引着」的窗口。
    {
        struct IconPool* pool = currentIconPool();
        if (pool) {
            currentImageList = pool->himl;
            ListView_SetImageList(hwndContentView, pool->himl, LVSIL_NORMAL);
            ListView_SetImageList(hwndContentView, pool->himl, LVSIL_SMALL);
        }
        else {
            // 自建列表建不起来：退回系统列表，保证「有图可看」而不是一片空白
            HIMAGELIST himlBig = NULL, himlSmall = NULL;
            Shell_GetImageLists(&himlBig, &himlSmall);
            currentImageList = (viewStyle == STYLE_LARGE_ICON) ? himlBig : himlSmall;
            ListView_SetImageList(hwndContentView, currentImageList, LVSIL_NORMAL);
            ListView_SetImageList(hwndContentView, currentImageList, LVSIL_SMALL);
        }
        // 此刻控件已经不再引用旧尺寸的池，可以安全释放（位图预算按「同时只有一个
        // 池在用」算，最多 8 MB，不会因为来回切尺寸累积）
        pruneIconPools(pool);
    }

    if (sortColumnIdx != -1) sortItems();
    ListView_SetItemCountEx(hwndContentView, numItems, 0);

    // 大图标/小图标视图：强制重排所有项目，覆盖 SetWindowLongPtr 切换样式时
    // LISTVIEW_StyleChanged → Arrange 在旧 ItemCount 下写入的错误位置。
    // 同时重设 ItemCount 触发 LISTVIEW_UpdateScroll，修复滚动范围。
    // 大图标视图下先把格子尺寸算好（内部含 Arrange），否则 Arrange 用旧格子排布。
    // 由 setViewStyle 触发时跳过布局：它自己在 refreshContentView 之后会做一次
    // 等价的调用（见 skipLayoutInRefresh），这里再做就是纯重复。
    if (viewStyle == STYLE_LARGE_ICON || viewStyle == STYLE_SMALL_ICON) {
        if (viewStyle == STYLE_LARGE_ICON) {
            if (!skipLayoutInRefresh) updateIconViewLayout();
        }
        else ListView_Arrange(hwndContentView, LVA_DEFAULT);
        ListView_SetItemCountEx(hwndContentView, numItems, 0);
    }

    // 图标：**先补、后画**。把首屏图标在这一次重绘之前补出来，新目录画出来的第一帧
    // 就是完整的 —— 图标一起出现，而不是「一半有图标、一半空白，剩下的过一会儿才冒」。
    // （旧路径参差的根源：池是按目录保留的，上一个目录的共享图标和一些独占图标都还在，
    // 首帧只画得出来这些，其余靠异步补齐 —— 同屏两批人，看上去就是不同步。）
    // 预算（ICON_SYNC_BUDGET_MS）内补不完就走异步：首帧画的是「池里已经有槽的那些」，
    // 其余先空着，补齐每有进展就把缺过的区间重画一次 —— 逐步填充，不会整屏空白。
    iconPaintMissed = false;      // 上一屏的「缺图标」见证属于旧内容，作废
    iconMissFirst = numItems;
    iconMissLast = -1;
    iconPrefetchDoneSize = 0;     // 换了内容：邻域预取重新开始
    iconFillStall = 0;
    // 共享来源先单独渲染掉（见 prewarmSharedIcons）：本目录几十个 .dll 共用同一个来源 id，
    // 预热一次就点亮整屏，且不受后面 150ms 首屏预算被 exe 吃干的影响。
    prewarmSharedIcons();
    iconFillVisibleSync(ICON_SYNC_BUDGET_MS);

    // 整表失效重绘，恢复原版绘制路径。不用 LVSICF_NOINVALIDATEALL 做增量刷新：
    // Wine/Winlator 上增量路径会让旧行不重画（图标、文字残缺或滞留旧内容），
    // 渲染正确性优先于这点重绘开销。
    InvalidateRect(hwndContentView, NULL, TRUE);
    scheduleIconFill();

    // 排序指示箭头：列结构或排序状态一变就重设（建列、点列头、切视图都汇到这里）
    updateSortIndicator();

    updateStatusbar();
}