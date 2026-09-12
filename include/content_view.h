#ifndef CONTENT_VIEW_H
#define CONTENT_VIEW_H

enum ViewStyle {
    STYLE_LARGE_ICON,
    STYLE_SMALL_ICON,
    STYLE_LIST,
    STYLE_DETAILS
};

// 文件夹在列表中的位置策略。
// 注意「沉底」不是「类型名当主键」的旧实现——旧实现是把本地化类型名
// （文件夹 / Folder / Папка …）当排序键，结果随界面语言变化：中文/葡文下
// 文件夹恰好沉底，英文/俄文下却夹在中间。这里改成显式策略，排序结果与
// 语言无关，观感（文件夹沉底）则与旧版一致。
enum FolderSortMode {
    FOLDER_SORT_TOP = 0,     // 置顶：Windows 资源管理器语义
    FOLDER_SORT_BOTTOM = 1,  // 沉底：WFM 经典观感
    FOLDER_SORT_PLAIN = 2    // 不区分：纯按本列排，文件夹与文件混排
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
enum ViewStyle loadViewStyle(void);
void updateViewMenuCheckmarks(void);

void setFolderSortMode(enum FolderSortMode newMode);
void loadFolderSortMode(void);
void updateFolderSortMenuCheckmarks(void);

#endif