#include "main.h"
#include <windowsx.h>     // GET_X_LPARAM / GET_WHEEL_DELTA_WPARAM

// 本项目的约定：这两个句柄不进头文件，各文件自己 extern（content_view.c / navbar.c 同）
extern HINSTANCE globalHInstance;
extern HWND hwndMain;

// ---------------------------------------------------------------------------
// 内置图片查看器（最小可用版）
//
// 只做「看图」：适应窗口、滚轮缩放、拖动平移、左右键翻页、Esc 关闭。
// 不做 EXIF 方向（wine 的 gdiplus 不解析 Orientation，见 dlls/gdiplus/*.c），
// 不做动画 GIF（只显示首帧）。这两条是已知限制，写在这里免得以后当成 bug 追。
//
// 渲染走 GDI+ 直接画到窗口 DC：不用内存 DC / 双缓冲 —— 项目里已有结论，Wine 的
// GL 合成下那条路会不显示（见 content_view.c 里 LVS_EX_DOUBLEBUFFER 那段注释）。
//
// 缩放的关键是**别每帧重采样**：解码一次原图，按当前显示尺寸预先做一张降采样缓
// 存（imgScaled），绘制时只是把它 1:1 贴到窗口上。只有换图 / 改窗口大小 / 缩放时
// 才重做缓存。
// ---------------------------------------------------------------------------

// GDI+ flat API（只声明本文件用到的部分）。这些是 gdiplus.dll 的导出名，写错只会在
// 链接期报 undefined symbol，改这里时按 AGENTS.md 的办法回导入库核一遍。
typedef int GpStatus;
typedef void GpBitmap;
typedef void GpImage;
typedef void GpGraphics;
#define WINGDIPAPI __stdcall
#define GDIPCONST const
enum { GpOk = 0 };

GpStatus WINGDIPAPI GdipCreateBitmapFromFile(GDIPCONST WCHAR*, GpBitmap**);
GpStatus WINGDIPAPI GdipGetImageWidth(GpImage*, UINT*);
GpStatus WINGDIPAPI GdipGetImageHeight(GpImage*, UINT*);
GpStatus WINGDIPAPI GdipCreateBitmapFromScan0(INT, INT, INT, INT, BYTE*, GpBitmap**);
GpStatus WINGDIPAPI GdipGetImageGraphicsContext(GpImage*, GpGraphics**);
GpStatus WINGDIPAPI GdipSetInterpolationMode(GpGraphics*, INT);
GpStatus WINGDIPAPI GdipDrawImageRectI(GpGraphics*, GpImage*, INT, INT, INT, INT);
GpStatus WINGDIPAPI GdipDrawImageI(GpGraphics*, GpImage*, INT, INT);
GpStatus WINGDIPAPI GdipCreateFromHDC(HDC, GpGraphics**);
GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics*);
GpStatus WINGDIPAPI GdipDisposeImage(GpImage*);

#define IMG_CLASS_NAME          L"WFMImageViewer"
#define IMG_PIXEL_FORMAT_ARGB   0x0026200A   // PixelFormat32bppARGB
#define IMG_INTERP_HQ_BICUBIC   7            // InterpolationModeHighQualityBicubic
#define IMG_INTERP_BILINEAR     3            // InterpolationModeBilinear（滚轮预览用）
#define IMG_BG_RGB              RGB(24, 24, 24)    // 图像区底色
#define IMG_BAR_RGB             RGB(16, 16, 16)    // 底部按钮条，比图像区更深一档
#define IMG_BAR_LINE_RGB        RGB(52, 52, 52)    // 按钮条顶边那条分隔线
// 降采样缓存的边长上限。一张 32bpp 位图的大小是 w*h*4，2560 就是 26 MB；
// 再大对「在窗口里看图」没有意义，只会白吃内存。它同时也是**源图**的上限：
// 解码回来超过这个尺寸的图会先缩到以内，因为超出部分永远进不了缓存。
#define IMG_MAX_DIM             2560
#define IMG_MAX_ZOOM            8.0
#define IMG_MAX_FILES           4096

// 底部按钮条高度。图像区域是「客户区减去这一条」。
// 44 而不是 36：按钮 26 高，上下各留 9 的空白，看着才不像贴边。
#define IMG_BAR_H               44
#define IMG_BTN_W               40
#define IMG_BTN_H               26
#define IMG_BTN_GAP             6
#define IMG_BTN_MIN_W           24   // 窗口太窄时按钮能压到的最窄宽度
// 滚轮停下多久之后才做高质量重采样。滚动过程中先用旧缓存拉伸预览，手感才跟得上。
#define IMG_ZOOM_DELAY_MS       140
#define IMG_TIMER_ZOOM          1

#define IMG_ID_PREV             4001
#define IMG_ID_NEXT             4002
#define IMG_ID_ZOOM_OUT         4003
#define IMG_ID_ZOOM_IN          4004
#define IMG_ID_FIT              4005
#define IMG_ID_CLOSE            4006

static HWND hwndViewer = NULL;

static GpBitmap* imgSrc = NULL;      // 解码后的原图
static UINT imgSrcW = 0, imgSrcH = 0;
static GpBitmap* imgScaled = NULL;   // 按显示尺寸降采样好的缓存
static int imgScaledW = 0, imgScaledH = 0;
static double imgZoom = 1.0;         // 1.0 = 适应窗口
static int imgPanX = 0, imgPanY = 0; // 相对「居中显示」位置的偏移
// 上一帧图像实际占的矩形（客户区坐标）。拖动平移时用它算新旧位置的并集，只失效这两块，
// 不必整屏重绘 —— 放大到 2560 宽之后再拖动，这一下省的是整屏的填色和重画。
static RECT imgLastPaint = {0};

static wchar_t imgPath[MAX_PATH] = {0};
static wchar_t imgDir[MAX_PATH] = {0};

// 底部按钮条
static HWND imgBtnPrev = NULL, imgBtnNext = NULL;
static HWND imgBtnZoomOut = NULL, imgBtnZoomIn = NULL;
static HWND imgBtnFit = NULL, imgBtnClose = NULL;

// 三种底色刷子。以前每次 WM_PAINT 都 CreateSolidBrush 一次再删掉，拖动平移时就是每帧
// 三次 GDI 对象创建/销毁；现在建窗口时建一次，关窗口时删。
static HBRUSH imgBrushBg = NULL;
static HBRUSH imgBrushBar = NULL;
static HBRUSH imgBrushBarLine = NULL;

// 定义在后面（挨着窗口过程），viewerLoad 会用到
static void viewerUpdateButtons(void);

// 同目录里的图片名，供左右键翻页
static wchar_t** siblingNames = NULL;
static int siblingCount = 0;
static int siblingCap = 0;
static int siblingIndex = -1;

static LRESULT CALLBACK viewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool isImageFileName(const wchar_t* name) {
    if (!name) return false;
    const wchar_t* ext = wcsrchr(name, L'.');
    if (!ext) return false;
    // 只列 wine 的 windowscodecs 真的带解码器的格式：bmp / gif / ico / jpeg / png / tiff
    // （dlls/windowscodecs 下就 bmpdecode.c、pngformat.c、libjpeg.c、ungif.c、icoformat.c、
    // libtiff.c 这几个）。**不要加 webp / heic / avif** —— 那样只会让这些文件被当成图片、
    // 双击进来再弹「无法打开」，等于骗人。系统里装了额外解码器是另一回事，这里按默认装法。
    static const wchar_t* exts[] = {
        L".jpg", L".jpeg", L".jpe", L".png", L".bmp", L".gif", L".tif", L".tiff", L".ico"
    };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (wcsicmp(ext, exts[i]) == 0) return true;
    }
    return false;
}

static void viewerFreeScaled(void) {
    if (imgScaled) {
        GdipDisposeImage((GpImage*)imgScaled);
        imgScaled = NULL;
    }
    imgScaledW = imgScaledH = 0;
}

static void viewerFreeSource(void) {
    viewerFreeScaled();
    if (imgSrc) {
        GdipDisposeImage((GpImage*)imgSrc);
        imgSrc = NULL;
    }
    imgSrcW = imgSrcH = 0;
}

static void viewerFreeSiblings(void) {
    for (int i = 0; i < siblingCount; i++) free(siblingNames[i]);
    free(siblingNames);
    siblingNames = NULL;
    siblingCount = 0;
    siblingCap = 0;
    siblingIndex = -1;
}

// 图像区域 = 客户区减去底部按钮条
static void viewerImageArea(int* cw, int* ch) {
    // 先给出安全的默认值再谈取尺寸：窗口可能在关掉的过程中，或者句柄还没建好，
    // GetClientRect 失败时栈上的 rc 是未初始化的垃圾值，直接拿去做比例会算出负数。
    *cw = 1;
    *ch = 1;
    if (!hwndViewer) return;

    RECT rc = {0, 0, 0, 0};
    if (!GetClientRect(hwndViewer, &rc)) return;

    *cw = rc.right - rc.left;
    *ch = rc.bottom - rc.top - IMG_BAR_H;
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

// 当前该显示多大：适应窗口的比例 × imgZoom，再受 IMG_MAX_DIM 封顶。
// 绘制和重采样都用它，保证两边算出来的尺寸永远一致。
static void viewerTargetSize(int* dw, int* dh) {
    *dw = 1;
    *dh = 1;
    if (!imgSrcW || !imgSrcH) return;

    int cw = 0, ch = 0;
    viewerImageArea(&cw, &ch);

    double fitX = (double)cw / (double)imgSrcW;
    double fitY = (double)ch / (double)imgSrcH;
    double s = ((fitX < fitY) ? fitX : fitY) * imgZoom;

    int w = (int)((double)imgSrcW * s + 0.5);
    int h = (int)((double)imgSrcH * s + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // 超上限就等比缩下来：宁可少放大，也不要为了一张缓存吃掉几十 MB
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
        int biggest = (w > h) ? w : h;
        double k = (double)IMG_MAX_DIM / (double)biggest;
        w = (int)((double)w * k);
        h = (int)((double)h * k);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    *dw = w;
    *dh = h;
}

// 把平移量夹在合法范围内：图比可视区大时，边缘不能被拖进可视区里面（那样会拖出一片
// 空白，看起来像图丢了）；比可视区小时一律居中（pan 归零），因为本来就没有可拖的余地。
// 放大 / 缩小之后也要再夹一次 —— 缩放改变了可拖范围。
static void viewerClampPan(void) {
    int dw = 0, dh = 0;
    viewerTargetSize(&dw, &dh);
    int cw = 0, ch = 0;
    viewerImageArea(&cw, &ch);

    if (dw <= cw) imgPanX = 0;
    else {
        int maxPan = (dw - cw) / 2;
        if (imgPanX > maxPan) imgPanX = maxPan;
        if (imgPanX < -maxPan) imgPanX = -maxPan;
    }
    if (dh <= ch) imgPanY = 0;
    else {
        int maxPan = (dh - ch) / 2;
        if (imgPanY > maxPan) imgPanY = maxPan;
        if (imgPanY < -maxPan) imgPanY = -maxPan;
    }
}

// 按目标尺寸重做降采样缓存。高质量重采样只在这里做 —— 绘制时永远是「有缓存就贴」，
// 所以滚轮连滚也不会每格都跑一次 bicubic。
static void viewerRebuildScaled(void) {
    if (!hwndViewer || !imgSrc) return;

    int dw = 0, dh = 0;
    viewerTargetSize(&dw, &dh);
    if (dw == imgScaledW && dh == imgScaledH && imgScaled) return;   // 尺寸没变，缓存还能用

    viewerFreeScaled();
    if (GdipCreateBitmapFromScan0(dw, dh, 0, IMG_PIXEL_FORMAT_ARGB, NULL, &imgScaled) != GpOk || !imgScaled) {
        imgScaled = NULL;
        return;
    }
    GpGraphics* g = NULL;
    if (GdipGetImageGraphicsContext((GpImage*)imgScaled, &g) == GpOk && g) {
        GdipSetInterpolationMode(g, IMG_INTERP_HQ_BICUBIC);
        GdipDrawImageRectI(g, (GpImage*)imgSrc, 0, 0, dw, dh);
        GdipDeleteGraphics(g);
    }
    imgScaledW = dw;
    imgScaledH = dh;
}

static void viewerUpdateTitle(void) {
    if (!hwndViewer) return;
    const wchar_t* name = wcsrchr(imgPath, L'\\');
    name = name ? name + 1 : imgPath;
    wchar_t title[MAX_PATH + 64] = {0};
    if (siblingCount > 1 && siblingIndex >= 0) {
        swprintfTrunc(title, MAX_PATH + 64, L"%ls  -  %d/%d  -  %u x %u  -  %d%%",
                      name, siblingIndex + 1, siblingCount, imgSrcW, imgSrcH, (int)(imgZoom * 100 + 0.5));
    }
    else {
        swprintfTrunc(title, MAX_PATH + 64, L"%ls  -  %u x %u  -  %d%%",
                      name, imgSrcW, imgSrcH, (int)(imgZoom * 100 + 0.5));
    }
    SetWindowTextW(hwndViewer, title);
}

static void viewerBuildSiblings(void) {
    viewerFreeSiblings();

    wchar_t pattern[MAX_PATH] = {0};
    wcscpy_s(pattern, MAX_PATH, imgDir);
    wcscat_s(pattern, MAX_PATH, L"\\*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!isImageFileName(fd.cFileName)) continue;
        if (siblingCount >= IMG_MAX_FILES) break;

        // 容量按倍数涨，不要每收一个名字就 realloc 一次（大目录里那是 O(n^2) 拷贝）
        if (siblingCount >= siblingCap) {
            int newCap = siblingCap ? siblingCap * 2 : 64;
            if (newCap > IMG_MAX_FILES) newCap = IMG_MAX_FILES;
            wchar_t** grown = realloc(siblingNames, (size_t)newCap * sizeof(wchar_t*));
            if (!grown) break;
            siblingNames = grown;
            siblingCap = newCap;
        }
        siblingNames[siblingCount] = wcsdup(fd.cFileName);
        if (!siblingNames[siblingCount]) break;
        siblingCount++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // 当前这张在列表里的位置（按文件名比，路径的目录部分已经单独存了）
    const wchar_t* name = wcsrchr(imgPath, L'\\');
    name = name ? name + 1 : imgPath;
    for (int i = 0; i < siblingCount; i++) {
        if (wcsicmp(siblingNames[i], name) == 0) { siblingIndex = i; break; }
    }
}

// 载入一张图。失败时返回 false，并且不动当前已显示的内容。
static bool viewerLoad(const wchar_t* fullPath) {
    if (!fullPath || !fullPath[0]) return false;

    // 拆出目录（翻页要用）
    wchar_t dir[MAX_PATH] = {0};
    wcsncpy(dir, fullPath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    *slash = L'\0';

    gdiplusInit();

    GpBitmap* bmp = NULL;
    if (GdipCreateBitmapFromFile(fullPath, &bmp) != GpOk || !bmp) return false;

    UINT w = 0, h = 0;
    if (GdipGetImageWidth((GpImage*)bmp, &w) != GpOk) w = 0;
    if (GdipGetImageHeight((GpImage*)bmp, &h) != GpOk) h = 0;
    if (!w || !h) {
        GdipDisposeImage((GpImage*)bmp);
        return false;
    }

    // 超大图先缩一次：解码回来是完整的（4000x3000 的 JPEG 就是 48MB），而显示缓存的边长
    // 上限只有 IMG_MAX_DIM，比它大的像素**永远进不了屏幕**。留着一份 48MB 的原图只为在
    // 缩小时当采样源，翻几张图内存就上去了。
    // 这里缩到 IMG_MAX_DIM 以内再换掉原图，长期占用从「原图 + 缓存」降到「一张缩图 + 缓存」。
    // 峰值压不掉：GDI+ 只能整张解码，没有流式缩放的接口，那一瞬间原图和缩图会同时在。
    GpBitmap* prepared = bmp;
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
        int tw, th;
        if (w >= h) {
            tw = IMG_MAX_DIM;
            th = (int)((double)h * (double)IMG_MAX_DIM / (double)w + 0.5);
        }
        else {
            th = IMG_MAX_DIM;
            tw = (int)((double)w * (double)IMG_MAX_DIM / (double)h + 0.5);
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;

        GpBitmap* small = NULL;
        if (GdipCreateBitmapFromScan0(tw, th, 0, IMG_PIXEL_FORMAT_ARGB, NULL, &small) == GpOk && small) {
            GpGraphics* g = NULL;
            if (GdipGetImageGraphicsContext((GpImage*)small, &g) == GpOk && g) {
                GdipSetInterpolationMode(g, IMG_INTERP_HQ_BICUBIC);
                GdipDrawImageRectI(g, (GpImage*)bmp, 0, 0, tw, th);
                GdipDeleteGraphics(g);
                prepared = small;
            }
            else {
                GdipDisposeImage((GpImage*)small);   // 建不出来就还按原图用，别把这张图搞丢了
            }
        }
    }
    if (prepared != bmp) GdipDisposeImage((GpImage*)bmp);

    viewerFreeSource();
    imgSrc = prepared;
    // 记的是**原始**尺寸：标题要显示它，适应窗口的比例也按它算 —— 换成缩图之后
    // 长宽比不变，所以算出来的目标尺寸仍然对，而标题不会被改成 2560x1920 那种谎话。
    imgSrcW = w;
    imgSrcH = h;

    wcsncpy(imgPath, fullPath, MAX_PATH - 1);
    imgPath[MAX_PATH - 1] = L'\0';

    bool dirChanged = (wcsicmp(imgDir, dir) != 0);
    wcsncpy(imgDir, dir, MAX_PATH - 1);
    imgDir[MAX_PATH - 1] = L'\0';
    if (!dirChanged) {
        // 目录没变：列表还能用，只要重新定位当前图的索引。翻页时走这条，省一次目录扫描。
        const wchar_t* base = wcsrchr(imgPath, L'\\');
        base = base ? base + 1 : imgPath;
        siblingIndex = -1;
        for (int i = 0; i < siblingCount; i++) {
            if (wcsicmp(siblingNames[i], base) == 0) { siblingIndex = i; break; }
        }
    }
    if (dirChanged || siblingCount == 0) viewerBuildSiblings();

    imgZoom = 1.0;
    imgPanX = imgPanY = 0;
    viewerRebuildScaled();     // 换图时直接做一次高质量重采样
    viewerUpdateTitle();
    viewerUpdateButtons();
    if (hwndViewer) {
        KillTimer(hwndViewer, IMG_TIMER_ZOOM);
        InvalidateRect(hwndViewer, NULL, TRUE);
    }
    return true;
}

// 左右键翻页：只在同目录的图片之间循环
static void viewerStep(int delta) {
    if (siblingCount <= 0 || siblingIndex < 0) return;

    for (int tries = 0; tries < siblingCount; tries++) {
        siblingIndex = (siblingIndex + delta + siblingCount) % siblingCount;
        wchar_t next[MAX_PATH] = {0};
        swprintfTrunc(next, MAX_PATH, L"%ls\\%ls", imgDir, siblingNames[siblingIndex]);
        if (next[0] && viewerLoad(next)) return;
        // 这张打不开（格式不支持 / 文件没了）：继续找下一张
    }
}

// 高质量重采样推迟到「停止操作 140ms 之后」。期间绘制用的是旧缓存的拉伸预览，
// 所以连滚滚轮不会有每格一次的 bicubic 卡顿。
static void viewerArmZoomRebuild(void) {
    if (!hwndViewer) return;
    KillTimer(hwndViewer, IMG_TIMER_ZOOM);
    SetTimer(hwndViewer, IMG_TIMER_ZOOM, IMG_ZOOM_DELAY_MS, NULL);
}

static void viewerZoomBy(double factor) {
    double z = imgZoom * factor;
    if (z < 1.0) z = 1.0;
    if (z > IMG_MAX_ZOOM) z = IMG_MAX_ZOOM;
    if (z == imgZoom) return;
    imgZoom = z;
    viewerClampPan();      // 缩放改变了可拖范围，缩小时 pan 要归零
    // 这里**不**调 viewerUpdateTitle：标题里带着缩放百分比，滚轮连滚时每格都设一次
    // SetWindowText，而 wine 下改窗口标题要过一次合成器（几十毫秒级），滚起来就是一顿一顿。
    // 百分比在 IMG_TIMER_ZOOM 里更新 —— 那时高质量重采样也刚好做完，停下 140ms 后补上。
    InvalidateRect(hwndViewer, NULL, FALSE);   // 先用旧缓存拉伸预览
    viewerArmZoomRebuild();
}

// 底部按钮条：全部居中排在客户区最下一条里。窗口变窄时按比例压窄按钮，
// 而不是让它们溢出到窗口外面去。
static void viewerLayoutButtons(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HWND btns[6] = { imgBtnPrev, imgBtnNext, imgBtnZoomOut, imgBtnZoomIn, imgBtnFit, imgBtnClose };
    const int n = 6;
    const int sidePadding = 8;          // 左右各留一点，按钮不贴边

    int bw = IMG_BTN_W;
    int gap = IMG_BTN_GAP;
    int avail = (rc.right - rc.left) - sidePadding * 2;
    int need = n * bw + (n - 1) * gap;
    if (need > avail) {
        // 先压间隙，还不够再压按钮宽度，都不低于 IMG_BTN_MIN_W
        gap = IMG_BTN_GAP;
        bw = (avail - (n - 1) * gap) / n;
        if (bw < IMG_BTN_MIN_W) {
            bw = IMG_BTN_MIN_W;
            gap = 2;
        }
        if (bw < 1) bw = 1;
        need = n * bw + (n - 1) * gap;
    }

    int x = (rc.right - rc.left - need) / 2;
    if (x < sidePadding) x = sidePadding;
    int y = rc.bottom - IMG_BAR_H + (IMG_BAR_H - IMG_BTN_H) / 2;
    for (int i = 0; i < n; i++) {
        if (btns[i]) SetWindowPos(btns[i], NULL, x, y, bw, IMG_BTN_H, SWP_NOZORDER);
        x += bw + gap;
    }
}

static HWND viewerCreateButton(HWND parent, const wchar_t* text, int id) {
    // 自绘（BS_OWNERDRAW）：默认按钮是系统浅灰立体边，放在这条深色底栏上很扎眼。
    // 自绘之后按键的常态 / 按下 / 置灰三种样子都能和底栏统一。
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             0, 0, IMG_BTN_W, IMG_BTN_H, parent, (HMENU)(INT_PTR)id,
                             globalHInstance, NULL);
    if (b && hGuiFont) SendMessageW(b, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
    return b;
}

static void viewerPaintButton(const DRAWITEMSTRUCT* dis) {
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = disabled ? RGB(32, 32, 32) : (pressed ? RGB(96, 96, 96) : RGB(56, 56, 56));
    COLORREF fg = disabled ? RGB(104, 104, 104) : RGB(232, 232, 232);

    HBRUSH brush = CreateSolidBrush(bg);
    if (brush) {
        FillRect(dis->hDC, &dis->rcItem, brush);
        DeleteObject(brush);
    }

    wchar_t text[64] = {0};
    GetWindowTextW(dis->hwndItem, text, 64);
    RECT rc = dis->rcItem;
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    HFONT oldFont = (HFONT)SelectObject(dis->hDC, hGuiFont);
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, oldFont);
}

// 翻页按钮在没有同目录图片时置灰，避免点了没反应
static void viewerUpdateButtons(void) {
    BOOL multi = (siblingCount > 1) ? TRUE : FALSE;
    if (imgBtnPrev) EnableWindow(imgBtnPrev, multi);
    if (imgBtnNext) EnableWindow(imgBtnNext, multi);
}

static LRESULT CALLBACK viewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool dragging = false;
    static POINT dragStart = {0};
    static int panStartX = 0, panStartY = 0;

    switch (msg) {
        case WM_CREATE:
            imgBrushBg      = CreateSolidBrush(IMG_BG_RGB);
            imgBrushBar     = CreateSolidBrush(IMG_BAR_RGB);
            imgBrushBarLine = CreateSolidBrush(IMG_BAR_LINE_RGB);
            imgBtnPrev    = viewerCreateButton(hwnd, L"\u25C0", IMG_ID_PREV);
            imgBtnNext    = viewerCreateButton(hwnd, L"\u25B6", IMG_ID_NEXT);
            imgBtnZoomOut = viewerCreateButton(hwnd, L"\u2212", IMG_ID_ZOOM_OUT);
            imgBtnZoomIn  = viewerCreateButton(hwnd, L"+",      IMG_ID_ZOOM_IN);
            imgBtnFit     = viewerCreateButton(hwnd, L"100%",   IMG_ID_FIT);
            imgBtnClose   = viewerCreateButton(hwnd, L"\u00D7", IMG_ID_CLOSE);
            viewerUpdateButtons();
            viewerLayoutButtons(hwnd);
            return 0;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis && dis->CtlType == ODT_BUTTON) {
                viewerPaintButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;
            switch (LOWORD(wParam)) {
                case IMG_ID_PREV:     viewerStep(-1); break;
                case IMG_ID_NEXT:     viewerStep(1);  break;
                case IMG_ID_ZOOM_OUT: viewerZoomBy(0.8); break;
                case IMG_ID_ZOOM_IN:  viewerZoomBy(1.25); break;
                case IMG_ID_FIT:
                    imgZoom = 1.0;
                    imgPanX = imgPanY = 0;
                    viewerRebuildScaled();
                    viewerUpdateTitle();
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                case IMG_ID_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
            }
            // 点完按钮把焦点还给窗口，否则方向键翻页和 Esc 关闭就失效了（焦点停在按钮上）
            SetFocus(hwnd);
            return 0;
        }

        case WM_TIMER:
            if (wParam == IMG_TIMER_ZOOM) {
                KillTimer(hwnd, IMG_TIMER_ZOOM);
                viewerRebuildScaled();          // 这一刻才做高质量重采样
                viewerUpdateTitle();            // 缩放停下才更新标题里的百分比（见 viewerZoomBy）
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;      // 背景由 WM_PAINT 一次画完，避免闪烁

        case WM_GETMINMAXINFO: {
            // 底下那排按钮是固定 6 个，窗口窄到放不下就只能压宽度、最后挤成一团。
            // 直接卡住最小尺寸，用户拖不到那个程度。
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 420;
            mmi->ptMinTrackSize.y = 320;
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);

            // 只填本次真正脏的那块。拖动平移时脏区只有图像新旧位置的一小块，填整屏
            // 等于白刷一遍 900x700。ps.rcPaint 是空矩形时就什么都不用填。
            RECT pr = ps.rcPaint;
            if (imgBrushBg && !IsRectEmpty(&pr)) FillRect(hdc, &pr, imgBrushBg);

            // 底部按钮条：比图像区深一档，顶边一条 1px 分隔线 —— 否则整屏同色，
            // 按钮像是浮在图上，没有「底栏」这个视觉结构。
            RECT bar = rc;
            bar.top = rc.bottom - IMG_BAR_H;
            if (bar.top < 0) bar.top = 0;
            RECT barPart;
            if (imgBrushBar && bar.bottom > bar.top && IntersectRect(&barPart, &pr, &bar)) {
                FillRect(hdc, &barPart, imgBrushBar);
                RECT line = bar;
                line.bottom = line.top + 1;
                RECT linePart;
                if (imgBrushBarLine && IntersectRect(&linePart, &pr, &line)) {
                    FillRect(hdc, &linePart, imgBrushBarLine);
                }
            }

            if (imgScaled) {
                int dw = 0, dh = 0;
                viewerTargetSize(&dw, &dh);
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top - IMG_BAR_H;
                if (ch < 1) ch = 1;

                int x = (cw - dw) / 2 + imgPanX;
                int y = (ch - dh) / 2 + imgPanY;
                // 记下来：拖动平移时用它算「旧位置」，只失效新旧两个矩形
                imgLastPaint.left = x;
                imgLastPaint.top = y;
                imgLastPaint.right = x + dw;
                imgLastPaint.bottom = y + dh;

                // 图像裁在「客户区减去底栏」里。放大之后图比可视区大，不裁的话它会画到
                // 底栏上面去，把按钮盖住；拖到底部时尤其明显。
                IntersectClipRect(hdc, rc.left, rc.top, rc.right, rc.top + ch);

                GpGraphics* g = NULL;
                if (GdipCreateFromHDC(hdc, &g) == GpOk && g) {
                    if (dw == imgScaledW && dh == imgScaledH) {
                        // 缓存就是按当前目标尺寸做的：直接 1:1 贴。这条是常态（适配窗口、
                        // 缩放到位之后），走 GdipDrawImageI 而不是 DrawImageRectI —— 后者
                        // 即使尺寸相同也会过一次插值管线，拖动平移时每帧都白花这笔钱。
                        GdipDrawImageI(g, (GpImage*)imgScaled, x, y);
                    }
                    else {
                        // 滚轮刚动、缓存还没重做（去抖 140ms 内）：把旧缓存拉伸成临时预览，
                        // bilinear 够快，等定时器到了再换成高质量重采样的结果。
                        GdipSetInterpolationMode(g, IMG_INTERP_BILINEAR);
                        GdipDrawImageRectI(g, (GpImage*)imgScaled, x, y, dw, dh);
                    }
                    GdipDeleteGraphics(g);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            viewerLayoutButtons(hwnd);
            if (imgSrc) {
                // 适应窗口时保持居中；放大后保留用户的平移位置，但要按新的可视区重新夹一次
                viewerClampPan();
                // 这里**不**更新标题：标题里的文件名、序号、原图尺寸、缩放百分比都跟窗口
                // 尺寸无关，而 SetWindowText 在 wine 下要过一次合成器（几十毫秒级），
                // 拖窗口边缘时每次 WM_SIZE 都调一次就是明显的卡顿。
                InvalidateRect(hwnd, NULL, FALSE);   // 先拉伸预览
                viewerArmZoomRebuild();              // 尺寸稳定后再高质量重做
            }
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) viewerZoomBy(1.25);
            else if (delta < 0) viewerZoomBy(0.8);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // 底栏上按下不算拖图：那是按钮条，拖起来会把按钮下面的图拽走，很怪。
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (GET_Y_LPARAM(lParam) >= rc.bottom - IMG_BAR_H) return 0;

            dragging = true;
            dragStart.x = GET_X_LPARAM(lParam);
            dragStart.y = GET_Y_LPARAM(lParam);
            panStartX = imgPanX;
            panStartY = imgPanY;
            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (dragging) {
                RECT oldRc = imgLastPaint;
                imgPanX = panStartX + (GET_X_LPARAM(lParam) - dragStart.x);
                imgPanY = panStartY + (GET_Y_LPARAM(lParam) - dragStart.y);
                viewerClampPan();     // 别让图被拖出可视区外找不回来
                // 只失效「旧位置 + 新位置」两块。整屏 Invalidate 也能对，但拖动是每帧都来，
                // 放大后的图整屏重绘就是白刷一遍背景和底栏。
                int dw = 0, dh = 0;
                viewerTargetSize(&dw, &dh);
                int cw = 0, ch = 0;
                viewerImageArea(&cw, &ch);
                RECT newRc;
                newRc.left = (cw - dw) / 2 + imgPanX;
                newRc.top = (ch - dh) / 2 + imgPanY;
                newRc.right = newRc.left + dw;
                newRc.bottom = newRc.top + dh;
                InvalidateRect(hwnd, &oldRc, FALSE);
                InvalidateRect(hwnd, &newRc, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
            }
            return 0;

        case WM_KEYDOWN: {
            switch (wParam) {
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    return 0;
                case VK_LEFT:
                    viewerStep(-1);
                    return 0;
                case VK_RIGHT:
                    viewerStep(1);
                    return 0;
                case VK_ADD:
                case VK_OEM_PLUS:
                    viewerZoomBy(1.25);
                    return 0;
                case VK_SUBTRACT:
                case VK_OEM_MINUS:
                    viewerZoomBy(0.8);
                    return 0;
                case '0':
                    imgZoom = 1.0;      // 回到适应窗口
                    imgPanX = imgPanY = 0;
                    viewerRebuildScaled();
                    viewerUpdateTitle();
                    InvalidateRect(hwnd, NULL, TRUE);
                    return 0;
            }
            break;
        }

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_DESTROY:
            KillTimer(hwnd, IMG_TIMER_ZOOM);
            viewerFreeSource();
            viewerFreeSiblings();
            imgBtnPrev = imgBtnNext = imgBtnZoomOut = imgBtnZoomIn = imgBtnFit = imgBtnClose = NULL;
            if (imgBrushBg)      { DeleteObject(imgBrushBg);      imgBrushBg = NULL; }
            if (imgBrushBar)     { DeleteObject(imgBrushBar);     imgBrushBar = NULL; }
            if (imgBrushBarLine) { DeleteObject(imgBrushBarLine); imgBrushBarLine = NULL; }
            hwndViewer = NULL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void openImageViewer(const wchar_t* path) {
    if (!path || !path[0]) return;

    if (!hwndViewer) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = viewerWndProc;
        wc.hInstance = globalHInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = IMG_CLASS_NAME;
        RegisterClassExW(&wc);

        RECT rc = {0, 0, 900, 700};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        // 居中到主窗口（主窗口可能被移动过，别固定死在屏幕中央）
        RECT mainRc = {0};
        int x = 0, y = 0;
        if (hwndMain && GetWindowRect(hwndMain, &mainRc)) {
            x = mainRc.left + ((mainRc.right - mainRc.left) - w) / 2;
            y = mainRc.top + ((mainRc.bottom - mainRc.top) - h) / 2;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;

        hwndViewer = CreateWindowExW(0, IMG_CLASS_NAME, L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                     x, y, w, h, hwndMain, NULL, globalHInstance, NULL);
        if (!hwndViewer) return;
        ShowWindow(hwndViewer, SW_SHOW);
        UpdateWindow(hwndViewer);
    }

    if (!viewerLoad(path)) {
        // 打不开：已经开着就保留原来那张，没开就把空窗口关掉
        if (!imgSrc) {
            DestroyWindow(hwndViewer);
            hwndViewer = NULL;
        }
        MessageBoxW(hwndMain, lc_str.msg_open_image_failed, lc_str.alert, MB_OK | MB_ICONWARNING);
        return;
    }
    // 查看器是最小化的就恢复出来：SetForegroundWindow 不会把最小化的窗口还原，
    // 那样用户双击另一张图只会看到任务栏闪一下，窗口还在最小化状态里。
    if (IsIconic(hwndViewer)) ShowWindow(hwndViewer, SW_RESTORE);
    SetForegroundWindow(hwndViewer);
    SetFocus(hwndViewer);     // 方向键翻页 / Esc 关闭都靠键盘焦点
}
