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

// 「刷新」的视觉反馈（见 main.c 的 navigateRefresh）：把内容区清空并**立刻**画一帧，
// 让用户看到「刷新过了」。目录太大时返回 false —— 那种目录重建本身就要上百毫秒，屏幕上
// 自然看得出来，不需要额外表演。返回值 true 表示调用方应当隔 REFRESH_FLASH_MS 再重建。
bool refreshFlashBegin(void);
#define TIMER_REFRESH_FLASH        0x7FFA   // 定时器号段同 content_view.c 里的 TIMER_*
#define REFRESH_FLASH_MS           70
#define REFRESH_FLASH_MAX_ITEMS    400      // 超过这么多条目就不表演了

// 显式刷新（工具栏按钮 / 右键菜单「刷新」）时调用：把当前目录各直接子目录的条目数缓存
// 标成过时。数字仍然显示（不清空），后台会重数一遍再改对 —— 见 content_view.c 的
// 「条目数缓存」一节。普通导航（进/退目录）不调用它，缓存直接命中。
void markFolderCountsStale(void);

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

// 大图标视图：图标尺寸（32/48/64/96/128）与文件名行数（0=无限自动适配, 1..5）。
// 两项都挂在「查看 → 大图标视图」子菜单下，且只对大图标视图生效 ——
// 非大图标视图时 updateIconViewMenuCheckmarks 会把父项整体置灰。
void setIconViewIconSize(int size);
void setIconViewLabelLines(int lines);
void loadIconViewSettings(void);
void updateIconViewMenuCheckmarks(void);

// 详细信息视图：驱动器"大小"列的磁盘占用显示模式
enum DriveBarMode {
    DRIVE_BAR_GRAPH = 0, // 图形化占用条 + "空闲 / 总量"文字（默认）
    DRIVE_BAR_TOTAL = 1, // 仅显示总容量（与普通文件一致）
    DRIVE_BAR_NONE  = 2  // "大小"列留空
};

void setDriveBarMode(int mode);
void loadDriveBarMode(void);
void updateDriveBarMenuCheckmarks(void);

#endif