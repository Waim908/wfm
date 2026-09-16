#include "main.h"

static WNDPROC OrigWndProc;
static bool isTracking = false;
static bool isSizing = false;
static POINTS currPos;
static int startX;
static HCURSOR cursorArrow;
static HCURSOR cursorSizeLR;

extern HINSTANCE globalHInstance;
extern HWND hwndMain;
extern HWND hwndTreeview;

HWND hwndSizebar = NULL;

static void setMouseTracking() {
    TRACKMOUSEEVENT tme = {0};
    tme.cbSize = sizeof(TRACKMOUSEEVENT);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwndSizebar;
    TrackMouseEvent(&tme);
    isTracking = true;
}

LRESULT CALLBACK SizebarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOVE: {
            currPos = MAKEPOINTS(lParam);
            break;
        }
        case WM_SETCURSOR: {
            return 1;
        }       
        case WM_LBUTTONDOWN: {
            startX = MAKEPOINTS(lParam).x;
            // 自己拿捕获即可。**不要**把 SetCapture 的返回值（本线程先前的捕获窗口）
            // 记下来、松手时再 SetCapture 还回去 —— 那不是「还给上一个操作者」，
            // 而是把本线程的捕获长期交给那个窗口：wine/win32u/message.c 的鼠标派发开头就是
            //     if (info.hwndCapture) { hittest = HTCLIENT; msg->hwnd = info.hwndCapture; }
            // 一旦捕获落在别的窗口上，之后**整个线程**的鼠标消息（包括模态对话框里按钮的
            // 点击）都会送进那个窗口，而键盘不受影响 —— 表现正是「对话框按钮点不动、回车却
            // 能用」。而且「还回去」没有任何收益：SetCapture/ReleaseCapture 走的是
            // gui_flags = 0，wine/dlls/winex11.drv/window.c 的 X11DRV_SetCapture 开头
            // `if (!(flags & (GUI_INMOVESIZE | GUI_INMENUMODE))) return;` 直接返回，
            // 连 X 指针 grab 都不会碰。
            SetCapture(hwnd);
            isSizing = true;
            break;
        }
        case WM_LBUTTONUP: {
            if (isSizing) ReleaseCapture();
            isSizing = false;
            break;
        }
        case WM_CAPTURECHANGED: {
            // 拖到一半丢了捕获（被别人抢走、或弹了模态框时系统发 WM_CANCELMODE）：
            // 必须收手。否则下一次**不含按键**的鼠标移动还会继续拖分隔条。
            isSizing = false;
            break;
        }
        case WM_MOUSELEAVE: {
            SetCursor(cursorArrow);
            isTracking = false;
            break;
        }
        case WM_MOUSEMOVE: {
            POINTS point = MAKEPOINTS(lParam);
            if (isSizing) {
                int dx = point.x - startX;
                RECT rect;
                GetWindowRectInParent(hwndTreeview, &rect);
                int width = (rect.right - rect.left) + dx;
                if (width > 50) {
                    SetWindowPos(hwndTreeview, NULL, 0, 0, width, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOMOVE);
                    resizeControls();
                }
            } 
            else if (!isTracking) {
                SetCursor(cursorSizeLR);
                setMouseTracking();
            }
            break;
        }       
    }
    return OrigWndProc(hwnd, msg, wParam, lParam);
}

void createSizebar() {
    hwndSizebar = CreateWindowEx(0, WC_STATIC, NULL, WS_VISIBLE | WS_CHILD | WS_CLIPSIBLINGS, 
                                 0, 0, 0, 0, hwndMain, (HMENU)NULL, globalHInstance, NULL);
                                 
    cursorArrow = LoadCursor(NULL, IDC_ARROW);
    cursorSizeLR = LoadCursor(NULL, IDC_SIZEWE);                                 
                                 
    OrigWndProc = (WNDPROC)SetWindowLongPtr(hwndSizebar, GWLP_WNDPROC, (LONG_PTR)SizebarWndProc);
    UpdateWindow(hwndSizebar);
}