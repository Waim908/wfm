#include "main.h"
#include <windowsx.h>     // GET_X_LPARAM

extern HINSTANCE globalHInstance;
extern HWND hwndMain;
extern HWND hwndContentView;

HWND hwndStatusbar = NULL;

// ---------------------------------------------------------------------------
// 状态栏右端的视图切换按钮
//
// 照资源管理器的样子把「怎么显示」放成右下角一排小图标：大图标 / 小图标 / 列表 /
// 详细信息。点一下直接切，不用开菜单 —— 切视图是高频动作，菜单要多两次点击，而 wine 下
// 弹菜单还要过一次合成器。
//
// **为什么不是状态栏的 SBT_OWNERDRAW 分区**：那条路要把 SBT_OWNERDRAW 的位塞进
// `SB_SETTEXT` 的 wParam（part 与 style 挤在同一个 DWORD 里），`SB_SETTEXT` 宏用的是
// `(style) << 8`，而 Wine 的 `STATUSBAR_SetTextT`（dlls/comctl32/status.c:1190）取的是
// `wParam & 0xff00` —— 位约定稍有出入就静默变成「把 itemData 当字符串读」。
// 自绘子窗口没有这层约定：跨 Wine / Windows 行为一致，命中判断只按 x 坐标算槽位，
// 也省掉一次 WM_DRAWITEM 往返回父窗口。
//
// 盖在状态栏上的前提：状态栏创建时带 WS_CLIPSIBLINGS（见 createStatusbar），它绘制时
// 会裁掉重叠的、z 序更高的兄弟窗口，所以按钮区域不会被状态栏刷掉。
// ---------------------------------------------------------------------------

#define VIEWBTN_CLASS       L"WFMViewButtons"
// 槽宽 26：状态栏高度约 24px，垂直方向本来就被限住了，横向给足一点，
// 触屏（Winlator 上常见）也还点得中。四个槽共 104px。
#define VIEWBTN_SLOT_W      26
#define VIEWBTN_COUNT       4
#define VIEWBTN_INSET       2       // 距状态栏上下边框的留白

static HWND hwndViewButtons = NULL;
static HBRUSH viewBtnBrushBg = NULL;     // 普通槽底（状态栏本色）
static HBRUSH viewBtnBrushSel = NULL;    // 当前视图的高亮底
static HBRUSH viewBtnBrushDown = NULL;   // 按下中的槽
static HBRUSH viewBtnBrushFg = NULL;     // 图标色（普通）
static HBRUSH viewBtnBrushFgSel = NULL;  // 图标色（高亮底上）
static int viewBtnPressed = -1;          // 按下但还没抬起的槽序号，-1 表示没有
static bool viewBtnShown = true;         // 创建时带 WS_VISIBLE，见 layoutStatusbarViewButtons

// 槽位顺序固定为「图标从大到小、再列表、再详细信息」，与资源管理器一致。
static enum ViewStyle viewBtnStyleAt(int i) {
    switch (i) {
        case 0:  return STYLE_LARGE_ICON;
        case 1:  return STYLE_SMALL_ICON;
        case 2:  return STYLE_LIST;
        default: return STYLE_DETAILS;
    }
}

static void viewBtnBox(HDC hdc, HBRUSH br, int x, int y, int w, int h) {
    RECT r = { x, y, x + w, y + h };
    FillRect(hdc, &r, br);
}

// 每个槽画一个 12px 见方的示意图标。纯几何线条，不依赖图标资源与字体。
static void viewBtnGlyph(HDC hdc, const RECT* slot, int which, HBRUSH fg) {
    int cx = (slot->left + slot->right) / 2;
    int cy = (slot->top + slot->bottom) / 2;

    switch (which) {
        case 0: {   // 大图标：2x2 大方块
            int x0 = cx - 6, y0 = cy - 6;
            viewBtnBox(hdc, fg, x0,     y0,     5, 5);
            viewBtnBox(hdc, fg, x0 + 7, y0,     5, 5);
            viewBtnBox(hdc, fg, x0,     y0 + 7, 5, 5);
            viewBtnBox(hdc, fg, x0 + 7, y0 + 7, 5, 5);
            break;
        }
        case 1: {   // 小图标：3x3 小方块
            int x0 = cx - 5, y0 = cy - 5;
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    viewBtnBox(hdc, fg, x0 + i * 4, y0 + j * 4, 3, 3);
            break;
        }
        case 2: {   // 列表：每行一个小方块 + 一条短线
            int x0 = cx - 6, y0 = cy - 5;
            for (int j = 0; j < 3; j++) {
                viewBtnBox(hdc, fg, x0,     y0 + j * 4,     3, 3);
                viewBtnBox(hdc, fg, x0 + 5, y0 + j * 4 + 1, 7, 1);
            }
            break;
        }
        default: {  // 详细信息：四条横线
            static const int lens[4] = { 12, 12, 9, 9 };
            int x0 = cx - 6, y0 = cy - 5;
            for (int j = 0; j < 4; j++)
                viewBtnBox(hdc, fg, x0, y0 + j * 3, lens[j], 1);
            break;
        }
    }
}

static void viewBtnRebuildBrushes(void) {
    if (viewBtnBrushBg)    { DeleteObject(viewBtnBrushBg);    viewBtnBrushBg = NULL; }
    if (viewBtnBrushSel)   { DeleteObject(viewBtnBrushSel);   viewBtnBrushSel = NULL; }
    if (viewBtnBrushDown)  { DeleteObject(viewBtnBrushDown);  viewBtnBrushDown = NULL; }
    if (viewBtnBrushFg)    { DeleteObject(viewBtnBrushFg);    viewBtnBrushFg = NULL; }
    if (viewBtnBrushFgSel) { DeleteObject(viewBtnBrushFgSel); viewBtnBrushFgSel = NULL; }

    // 系统色随主题变，所以刷子建一次常驻、WM_SYSCOLORCHANGE 时重建，而不是每次绘制
    // 现建现删（状态栏重画虽然低频，但那是白花的 GDI 对象创建）。
    viewBtnBrushBg    = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
    viewBtnBrushSel   = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
    viewBtnBrushDown  = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
    viewBtnBrushFg    = CreateSolidBrush(GetSysColor(COLOR_BTNTEXT));
    viewBtnBrushFgSel = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHTTEXT));
}

static LRESULT CALLBACK viewButtonsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            viewBtnRebuildBrushes();
            return 0;

        case WM_ERASEBKGND:
            return 1;      // 背景在 WM_PAINT 里一次填完，避免闪

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            if (viewBtnBrushBg) FillRect(hdc, &rc, viewBtnBrushBg);

            enum ViewStyle current = getViewStyle();
            for (int i = 0; i < VIEWBTN_COUNT; i++) {
                RECT slot = { i * VIEWBTN_SLOT_W, 0, (i + 1) * VIEWBTN_SLOT_W, rc.bottom };
                bool selected = (viewBtnStyleAt(i) == current);

                // 高亮块比槽小一圈，几个按钮之间才有分隔感，不至于连成一条色带
                RECT box = slot;
                InflateRect(&box, -1, -1);
                HBRUSH bg = selected ? viewBtnBrushSel
                                     : (viewBtnPressed == i ? viewBtnBrushDown : viewBtnBrushBg);
                if (bg) FillRect(hdc, &box, bg);

                HBRUSH fg = selected ? viewBtnBrushFgSel : viewBtnBrushFg;
                if (fg) viewBtnGlyph(hdc, &slot, i, fg);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int idx = GET_X_LPARAM(lParam) / VIEWBTN_SLOT_W;
            if (idx >= 0 && idx < VIEWBTN_COUNT) {
                viewBtnPressed = idx;
                SetCapture(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (viewBtnPressed < 0) return 0;
            // 顺序很要紧：**先把状态取出来并清掉，再 ReleaseCapture**。
            // NtUserReleaseCapture 会同步给失去捕获的窗口发 WM_CAPTURECHANGED
            // （`dlls/win32u/input.c:1900` 的 send_message），而下面的 WM_CAPTURECHANGED
            // 分支会把 viewBtnPressed 清成 -1 —— 若先 ReleaseCapture，取到的 was 永远是 -1，
            // `idx == was` 就永远不成立，按钮只闪一下、视图不切。
            int was = viewBtnPressed;
            viewBtnPressed = -1;
            int idx = GET_X_LPARAM(lParam) / VIEWBTN_SLOT_W;

            if (GetCapture() == hwnd) ReleaseCapture();   // 此时 WM_CAPTURECHANGED 已是空操作
            InvalidateRect(hwnd, NULL, FALSE);
            // 同步刷掉「按下」那一块：setViewStyle 会重建列表（几到几十毫秒），不先画完的话
            // 那个深色块会一直留到切换结束。
            UpdateWindow(hwnd);

            // 只有在同一个槽上按下又抬起才算点击（按住拖出去再松手是取消，和普通按钮一致）
            if (idx == was && idx >= 0 && idx < VIEWBTN_COUNT) {
                setViewStyle(viewBtnStyleAt(idx));
                // 别让焦点停在这个自绘窗口上：方向键、打字都还得给内容区
                if (hwndContentView) SetFocus(hwndContentView);
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // 捕获被别的东西拿走（弹菜单、窗口失活、程序别处 SetCapture）：按取消处理，
            // 别留一个按下的高亮。正常点击路径上这里已经是空操作（见 WM_LBUTTONUP 的顺序）。
            if (viewBtnPressed >= 0) {
                viewBtnPressed = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_SYSCOLORCHANGE:
            viewBtnRebuildBrushes();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_DESTROY:
            if (viewBtnBrushBg)    { DeleteObject(viewBtnBrushBg);    viewBtnBrushBg = NULL; }
            if (viewBtnBrushSel)   { DeleteObject(viewBtnBrushSel);   viewBtnBrushSel = NULL; }
            if (viewBtnBrushDown)  { DeleteObject(viewBtnBrushDown);  viewBtnBrushDown = NULL; }
            if (viewBtnBrushFg)    { DeleteObject(viewBtnBrushFg);    viewBtnBrushFg = NULL; }
            if (viewBtnBrushFgSel) { DeleteObject(viewBtnBrushFgSel); viewBtnBrushFgSel = NULL; }
            hwndViewButtons = NULL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 摆到状态栏右端。父窗口尺寸一变就要调一次（状态栏自己会跟着父窗口改大小，
// 但这些按钮是它的兄弟窗口，位置得自己算）。
void layoutStatusbarViewButtons(void) {
    if (!hwndViewButtons || !hwndStatusbar) return;

    RECT sb;
    GetWindowRectInParent(hwndStatusbar, &sb);
    int sbW = sb.right - sb.left;
    int sbH = sb.bottom - sb.top;

    // 窗口窄到放不下（手机竖屏被拖到很窄，或者刚创建还没布局）：直接藏起来。
    // 不能只是 return —— 那样它会留在上一次的位置上，压在内容区上面。
    if (sbW < VIEWBTN_SLOT_W * VIEWBTN_COUNT + 40 || sbH < 12) {
        if (viewBtnShown) {
            ShowWindow(hwndViewButtons, SW_HIDE);
            viewBtnShown = false;
        }
        return;
    }
    if (!viewBtnShown) {
        ShowWindow(hwndViewButtons, SW_SHOWNA);
        viewBtnShown = true;
    }

    SetWindowPos(hwndViewButtons, HWND_TOP,
                 sb.right - VIEWBTN_SLOT_W * VIEWBTN_COUNT - VIEWBTN_INSET,
                 sb.top + VIEWBTN_INSET,
                 VIEWBTN_SLOT_W * VIEWBTN_COUNT,
                 sbH - VIEWBTN_INSET * 2,
                 SWP_NOACTIVATE);
}

// 视图变了就重画那 4 个槽。只失效自己那块，不惊动状态栏的文本。
void refreshStatusbarViewButtons(void) {
    if (hwndViewButtons) InvalidateRect(hwndViewButtons, NULL, FALSE);
}

static void createStatusbarViewButtons(void) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = viewButtonsWndProc;
        wc.hInstance = globalHInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = VIEWBTN_CLASS;
        if (!RegisterClassExW(&wc)) return;
        registered = true;
    }

    hwndViewButtons = CreateWindowExW(0, VIEWBTN_CLASS, L"",
                                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                      0, 0, 0, 0, hwndMain, NULL, globalHInstance, NULL);
    layoutStatusbarViewButtons();
}

void setStatusbarText(wchar_t* text) {
    SetWindowText(hwndStatusbar, text); 
}

void createStatusbar() {
    hwndStatusbar = CreateWindowEx(0, STATUSCLASSNAME, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS, 
                                   0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
    UpdateWindow(hwndStatusbar);
    createStatusbarViewButtons();
}
