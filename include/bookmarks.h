#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include "main.h"

#define MAX_BOOKMARKS 50
#define BOOKMARK_REGISTRY_PATH L"SOFTWARE\\Winlator\\WFM\\Bookmarks"
#define AUTOOPEN_REGISTRY_PATH L"SOFTWARE\\Winlator\\WFM\\AutoOpenBookmark"

struct Bookmark {
    wchar_t path[MAX_PATH];
    wchar_t name[MAX_PATH];
};

extern struct Bookmark g_bookmarks[MAX_BOOKMARKS];
extern int g_bookmarkCount;
extern int g_autoOpenBookmarkIndex;  // -1 means no auto-open bookmark

void loadBookmarks();
void saveBookmarks();
void loadAutoOpenBookmark();
void saveAutoOpenBookmark();
void addBookmark(const wchar_t* path);
void removeBookmark(int index);
int findBookmark(const wchar_t* path);
void buildBookmarkTree();
void addCurrentPathToBookmark();
void setAutoOpenBookmark(int index);
void openAutoOpenBookmark();

#endif
