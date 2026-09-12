#ifndef CONTENT_VIEW_H
#define CONTENT_VIEW_H

enum ViewStyle {
    STYLE_LARGE_ICON,
    STYLE_SMALL_ICON,
    STYLE_LIST,
    STYLE_DETAILS
};

// 文件夹在列表中的位置策略。
// 「经典」是原版 WFM 的排序语义：文件夹/文件分组是类型主键的一部分，
// 随升降序一起翻转 —— 升序文件夹在前、降序在后（TYPE_DIR=0 < TYPE_FILE=1，
// 原版把 type 差值放进统一的升降序翻转里）。
// 置顶/沉底/不区分是后加的可选项，位置不随方向变化。
enum FolderSortMode {
    FOLDER_SORT_CLASSIC = 0, // 经典：分组随排序方向翻转（原版行为，默认）
    FOLDER_SORT_TOP = 1,     // 恒置顶：Windows 资源管理器语义
    FOLDER_SORT_BOTTOM = 2,  // 恒沉底：WFM 观感的固定版
    FOLDER_SORT_PLAIN = 3    // 不分组：纯按本列排，文件夹与文件混排
};

LRESULT contentViewNotify(NMHDR* nmhdr);
void createContentView();
void clearContentView();
void refreshContentView();
void setViewStyle(enum ViewStyle newViewStyle);
void searchFor(wchar_t* keyword);

void onMenuItemUpClick();
void onMenuItemOpenClick();
void onMenuItemEditClick();
void onMenuItemCutClick();
void onMenuItemCopyClick();
void onMenuItemCreateShortcutClick();
void onMenuItemDeleteClick();
void onMenuItemRenameClick();
void onMenuItemPasteClick();
void onMenuItemPasteShortcutClick();
void onMenuItemNewFolderClick();
void onMenuItemNewFileClick();
void onMenuItemSelectAllClick();
void onBookmarkButtonClick();
void onMenuItemUnloadISOImageClick();
void onMenuItemLocateISOImageClick();
void clearIconCaches();

// 从 PE 文件（exe/dll）提取第一个图标组（程序主图标）中最接近目标尺寸的
// 图标。绕开 Wine 有颜色反转 bug 的 ExtractIconEx 系图标 API。返回的 HICON
// 由调用方 DestroyIcon()，失败返回 NULL。
HICON extractIconFromExe(const wchar_t* exePath, int cxDesired, int cyDesired);
enum ViewStyle loadViewStyle(void);
void updateViewMenuCheckmarks(void);

void setFolderSortMode(enum FolderSortMode newMode);
void loadFolderSortMode(void);
void updateFolderSortMenuCheckmarks(void);

#endif