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
GpStatus WINGDIPAPI GdipCreateFromHDC(HDC, GpGraphics**);
GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics*);
GpStatus WINGDIPAPI GdipDisposeImage(GpImage*);

#define IMG_CLASS_NAME          L"WFMImageViewer"
#define IMG_PIXEL_FORMAT_ARGB   0x0026200A   // PixelFormat32bppARGB
#define IMG_INTERP_HQ_BICUBIC   7            // InterpolationModeHighQualityBicubic
#define IMG_BG_RGB              RGB(24, 24, 24)
// 降采样缓存的边长上限。一张 32bpp 位图的大小是 w*h*4，2560 就是 26 MB；
// 再大对「在窗口里看图」没有意义，只会白吃内存。
#define IMG_MAX_DIM             2560
#define IMG_MAX_ZOOM            8.0
#define IMG_MAX_FILES           4096

static HWND hwndViewer = NULL;

static GpBitmap* imgSrc = NULL;      // 解码后的原图
static UINT imgSrcW = 0, imgSrcH = 0;
static GpBitmap* imgScaled = NULL;   // 按显示尺寸降采样好的缓存
static int imgScaledW = 0, imgScaledH = 0;
static double imgZoom = 1.0;         // 1.0 = 适应窗口
static int imgPanX = 0, imgPanY = 0; // 相对「居中显示」位置的偏移

static wchar_t imgPath[MAX_PATH] = {0};
static wchar_t imgDir[MAX_PATH] = {0};

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
    static const wchar_t* exts[] = {
        L".jpg", L".jpeg", L".jpe", L".png", L".bmp", L".gif", L".tif", L".tiff", L".webp"
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

// 按当前窗口尺寸与缩放比例重做降采样缓存
static void viewerRebuildScaled(void) {
    if (!hwndViewer || !imgSrc) return;

    RECT rc;
    GetClientRect(hwndViewer, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw < 1 || ch < 1) return;

    // 适应窗口的比例（取两个方向里较小的那个，整张图都放进窗口）
    double fitX = (double)cw / (double)imgSrcW;
    double fitY = (double)ch / (double)imgSrcH;
    double s = ((fitX < fitY) ? fitX : fitY) * imgZoom;

    int dw = (int)((double)imgSrcW * s + 0.5);
    int dh = (int)((double)imgSrcH * s + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    // 超上限就等比缩下来：宁可少放大，也不要为了一张缓存吃掉几十 MB
    if (dw > IMG_MAX_DIM || dh > IMG_MAX_DIM) {
        int biggest = (dw > dh) ? dw : dh;
        double k = (double)IMG_MAX_DIM / (double)biggest;
        dw = (int)((double)dw * k);
        dh = (int)((double)dh * k);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

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

    viewerFreeSource();
    imgSrc = bmp;
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
    viewerRebuildScaled();
    viewerUpdateTitle();
    if (hwndViewer) InvalidateRect(hwndViewer, NULL, TRUE);
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

static void viewerZoomBy(double factor) {
    double z = imgZoom * factor;
    if (z < 1.0) z = 1.0;
    if (z > IMG_MAX_ZOOM) z = IMG_MAX_ZOOM;
    if (z == imgZoom) return;
    imgZoom = z;
    viewerRebuildScaled();
    viewerUpdateTitle();
    InvalidateRect(hwndViewer, NULL, TRUE);
}

static LRESULT CALLBACK viewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool dragging = false;
    static POINT dragStart = {0};
    static int panStartX = 0, panStartY = 0;

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;      // 背景由 WM_PAINT 一次画完，避免闪烁

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(IMG_BG_RGB);
            if (bg) { FillRect(hdc, &rc, bg); DeleteObject(bg); }

            if (imgScaled && imgScaledW > 0 && imgScaledH > 0) {
                GpGraphics* g = NULL;
                if (GdipCreateFromHDC(hdc, &g) == GpOk && g) {
                    int x = (rc.right - imgScaledW) / 2 + imgPanX;
                    int y = (rc.bottom - imgScaledH) / 2 + imgPanY;
                    // 缓存就是按显示尺寸做的，这里是 1:1 贴，不再重采样
                    GdipDrawImageRectI(g, (GpImage*)imgScaled, x, y, imgScaledW, imgScaledH);
                    GdipDeleteGraphics(g);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            // 窗口尺寸变了：适应比例跟着变，缓存必须重做
            if (imgSrc) {
                bool wasFit = (imgZoom <= 1.0001);
                if (wasFit) { imgPanX = imgPanY = 0; }
                viewerRebuildScaled();
                viewerUpdateTitle();
            }
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) viewerZoomBy(1.25);
            else if (delta < 0) viewerZoomBy(0.8);
            return 0;
        }

        case WM_LBUTTONDOWN:
            dragging = true;
            dragStart.x = GET_X_LPARAM(lParam);
            dragStart.y = GET_Y_LPARAM(lParam);
            panStartX = imgPanX;
            panStartY = imgPanY;
            SetCapture(hwnd);
            return 0;

        case WM_MOUSEMOVE:
            if (dragging) {
                imgPanX = panStartX + (GET_X_LPARAM(lParam) - dragStart.x);
                imgPanY = panStartY + (GET_Y_LPARAM(lParam) - dragStart.y);
                InvalidateRect(hwnd, NULL, FALSE);
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
            viewerFreeSource();
            viewerFreeSiblings();
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
    SetForegroundWindow(hwndViewer);
    SetFocus(hwndViewer);     // 方向键翻页 / Esc 关闭都靠键盘焦点
}
