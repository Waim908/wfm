#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>
#include <wingdi.h>
#include <winnls.h>
#include <wchar.h>
#include <strsafe.h>
#include <shlobj.h>
#include <process.h>
#include <time.h>
#include <math.h>
#ifdef USE_LIBCDIO
#include <cdio/cdio.h>
#include <cdio/iso9660.h>
#endif

static inline void timetToFileTime(time_t t, LPFILETIME result) {
    ULARGE_INTEGER timeValue;
    timeValue.QuadPart = (t * 10000000LL) + 116444736000000000LL;
    result->dwLowDateTime = timeValue.LowPart;
    result->dwHighDateTime = timeValue.HighPart;
}

#include "resource.h"
#include "content_view.h"
#include "toolbar.h"
#include "navbar.h"
#include "treeview.h"
#include "sizebar.h"
#include "statusbar.h"
#include "file_node.h"
#include "file_actions.h"
#include "file_utils.h"
#include "input_dialog.h"
#include "bookmarks.h"
#include "open_with_dialog.h"
#include "strings.h"

#define MEMFREE(x) \
    do { \
        if (x != NULL) { \
            free(x); \
            x = NULL; \
        } \
    } \
    while(0)

extern HFONT hGuiFont;

void updateGuiFont();
void navigateToFileNode(struct FileNode* node);
void navigateToPath(wchar_t* path);
void navigateUp();
void navigateRefresh();
void openFileNode(struct FileNode* node);
void GetWindowRectInParent(HWND hwnd, RECT* rect);
void resizeControls();

#endif