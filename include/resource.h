#define IDI_MAIN 101
#define IDD_ABOUT 102
#define IDI_GO 103
#define IDI_REFRESH 104
#define IDI_NAV_ARROW 105
#define IDD_INPUT 106
#define IDC_LABEL 107
#define IDC_EDIT 108
#define IDC_APP_NAME 109
#define IDC_APP_VERSION 110
#define IDC_APP_DEV_NAME 111
#define IDC_APP_MOD_NAME 123
#define IDC_APP_URL 124
#define IDD_FILE_ACTION 112
#define IDC_PRELOADER 113
#define IDI_SEARCH 114
#define IDI_CANCEL 115
#define IDI_UP 116
#define IDI_COPY 117
#define IDI_CUT 118
#define IDI_PASTE 119
#define IDI_DELETE 120
#define IDI_NEW_FOLDER 121
#define IDI_NEW_FILE 122

#define IDI_PRELOADER_1 201
#define IDI_PRELOADER_2 202
#define IDI_PRELOADER_3 203
#define IDI_PRELOADER_4 204
#define IDI_PRELOADER_5 205
#define IDI_PRELOADER_6 206
#define IDI_PRELOADER_7 207
#define IDI_PRELOADER_8 208
#define IDI_BOOKMARK 209

#define IDD_ICON_VIEWER 212
#define IDC_ICON_IMAGE 213
#define IDC_SAVE_ICON 214
#define IDC_ICON_SIZE_16  215
#define IDC_ICON_SIZE_32  216
#define IDC_ICON_SIZE_48  217
#define IDC_ICON_SIZE_256 218
#define IDC_PREV_GROUP    219
#define IDC_NEXT_GROUP    220
#define IDC_SIZE_BASE     230  /* 230..259 reserved for dynamic size buttons */

#define ID_EDIT_CUT 301
#define ID_EDIT_COPY 302
#define ID_EDIT_PASTE 303
#define ID_VIEW_LARGEICONS 304
#define ID_VIEW_SMALLICONS 305
#define ID_VIEW_LIST 306
#define ID_VIEW_DETAILS 307
#define ID_HELP_ABOUT 308
#define ID_FILE_EXIT 309
#define ID_FILE_NEW_WINDOW 325
#define ID_EDIT_PASTE_SHORTCUT 310
#define ID_EDIT_SELECT_ALL 311
// 「清空剪贴板」：放弃待粘贴的内容（编辑菜单与右键菜单共用这个命令 ID）
#define ID_EDIT_CLEAR_CLIPBOARD 326
#define ID_SHOW_ICON 312
#define ID_VIEW_CLEAR_ICON_CACHE 313
#define ID_MOUNT_LOCATE_ISO  314
#define ID_MOUNT_UNMOUNT_ISO  315
#define ID_LANG_EN  316
#define ID_LANG_ZH  317
#define ID_LANG_PT  318
#define ID_LANG_RU  319
#define ID_VIEW_SHOW_HIDDEN 320
#define ID_VIEW_FOLDER_CLASSIC 321
#define ID_VIEW_FOLDER_TOP     322
#define ID_VIEW_FOLDER_BOTTOM  323
#define ID_VIEW_FOLDER_PLAIN   324

// 大图标视图设置。两组内部各段连续，供 CheckMenuRadioItem 使用
#define ID_VIEW_ICONSIZE_32    330
#define ID_VIEW_ICONSIZE_48    331
#define ID_VIEW_ICONSIZE_64    332
#define ID_VIEW_ICONSIZE_96    333
#define ID_VIEW_ICONSIZE_128   334
#define ID_VIEW_LINES_AUTO     340
#define ID_VIEW_LINES_1        341
#define ID_VIEW_LINES_2        342
#define ID_VIEW_LINES_3        343
#define ID_VIEW_LINES_4        344
#define ID_VIEW_LINES_5        345
#define ID_VIEW_DRIVE_BAR      350
#define ID_VIEW_DRIVE_BAR_TOTAL 351
#define ID_VIEW_DRIVE_BAR_NONE  352
#define IDD_OPEN_WITH 260
#define IDC_EDIT_PROGRAM 261
#define IDC_BTN_BROWSE 262
#define IDC_CHECK_ALWAYS 263
#define IDC_BTN_REMOVE_ASSOC 264

// 粘贴同名冲突对话框（替换 / 跳过 / 保留两者）
#define IDD_CONFLICT 270
#define IDC_CONFLICT_TEXT 271
#define IDC_BTN_REPLACE 272
#define IDC_BTN_SKIP 273
#define IDC_BTN_KEEP_BOTH 274
#define IDC_CHECK_APPLY_ALL 275

#define ICON_UP          0
#define ICON_COPY        1
#define ICON_CUT         2
#define ICON_PASTE       3
#define ICON_DELETE      4
#define ICON_NEW_FOLDER  5
#define ICON_NEW_FILE    6
#define ICON_GO          7
#define ICON_REFRESH     8
#define ICON_SEARCH      9
#define ICON_NAV_ARROW   10
#define ICON_BOOKMARK    11
#define ICON_CMD         12
#define ICON_EXPLORER    13
#define NUM_UI_ICONS     14

extern HICON uiIcons[NUM_UI_ICONS];

void preloadIcons();
void freeUIcons();

#ifndef IDC_STATIC
#define IDC_STATIC -1
#endif

#define APP_NAME L"Winlator File Manager"
#define APP_VERSION L"1.5-mod.2.7"
#define APP_DEV_NAME L"BrunoSX"

#ifndef RESOURCE_H
#define RESOURCE_H

struct LC_STR {
    wchar_t* app_name;
    wchar_t* app_version;
    wchar_t* app_dev_name;
    wchar_t* app_mod_name;
    wchar_t* app_url;
    wchar_t* application;
    wchar_t* shortcut;
    wchar_t* file;
    wchar_t* folder;
    wchar_t* local_drive;
    wchar_t* cd_drive;
    wchar_t* computer;
    wchar_t* desktop;
    wchar_t* documents;
    wchar_t* exit;
    wchar_t* new_window;
    wchar_t* edit;
    wchar_t* cut;
    wchar_t* copy;
    wchar_t* paste;
    wchar_t* paste_shortcut;
    // 「清空剪贴板」：只丢弃待粘贴的内容，不删除文件本身
    wchar_t* clear_clipboard;
    // 剪贴板状态指示：参数依次为「复制/剪切」词、条目数、首个文件名、源目录
    wchar_t* clipboard_info;
    wchar_t* select_all;
    wchar_t* view;
    wchar_t* large_icons;
    wchar_t* small_icons;
    wchar_t* list;
    wchar_t* details;
    wchar_t* help;
    wchar_t* about;
    wchar_t* ok;
    wchar_t* cancel;
    wchar_t* loading;
    wchar_t* open;
    wchar_t* create_shortcut;
    wchar_t* delete;
    wchar_t* rename;
    wchar_t* new_folder;
    wchar_t* new_file;
    wchar_t* items;
    wchar_t* load_iso_image;
    wchar_t* unload_iso_image;
    wchar_t* no_media;
    wchar_t* alert;
    wchar_t* enter_folder_name;
    wchar_t* enter_file_name;
    wchar_t* enter_new_name;
    wchar_t* name;
    wchar_t* type;
    wchar_t* size;
    wchar_t* date;
    wchar_t* path;
    wchar_t* deleting_files;
    wchar_t* copying_files;
    wchar_t* moving_files;
    wchar_t* extracting_files;
    wchar_t* confirm_delete;
    wchar_t* confirm_exit;
    wchar_t* search;
    wchar_t* up;
    wchar_t* show_icon;
    wchar_t* bookmarks;
    wchar_t* bookmark;
    wchar_t* add_bookmark;
    wchar_t* remove_bookmark;
    wchar_t* bookmark_exists;
    wchar_t* bookmark_path_not_found;
    wchar_t* msg_cannot_launch_system_app;
    wchar_t* msg_cannot_open_new_window;
    wchar_t* auto_open_on_start;
    wchar_t* cancel_auto_open;
    wchar_t* auto_open_path_not_found;
    
    wchar_t* fmt_file;
    
    wchar_t* msg_invalid_iso_image_file;
    wchar_t* msg_deleting_files;
    wchar_t* msg_copying_files;
    wchar_t* msg_moving_files;
    wchar_t* msg_extracting_files;
    wchar_t* msg_cancel_file_operation;
    // 同名冲突：对话框标题、正文（%ls 为条目名，文件/文件夹两版）、三个按钮、
    // 「对全部冲突项使用相同操作」复选框，以及收尾汇总（%d 成功/跳过/失败）
    wchar_t* conflict_title;
    wchar_t* conflict_existing_file;
    wchar_t* conflict_existing_folder;
    wchar_t* conflict_replace;
    wchar_t* conflict_skip;
    wchar_t* conflict_keep_both;
    wchar_t* copy_suffix;
    wchar_t* conflict_apply_all;
    wchar_t* msg_file_op_summary;
    wchar_t* msg_confirm_delete_item;
    wchar_t* msg_confirm_delete_multiple_items;
    wchar_t* msg_confirm_exit_app;
    wchar_t* clear_icon_cache;
    wchar_t* mount;
    wchar_t* locate_iso;
    wchar_t* open_file_location;
    wchar_t* open_link_target;
    wchar_t* unmount_iso;
    wchar_t* msg_no_mounted_image;
    wchar_t* msg_image_dir_not_found;
    wchar_t* msg_x_drive_not_found;
    wchar_t* msg_no_libcdio;
    wchar_t* msg_confirm_unmount_iso;
    wchar_t* msg_link_target_not_found;
    wchar_t* save_icon;
    wchar_t* fmt_drive_space;

    // Toolbar-specific short strings (for narrow buttons)
    wchar_t* tb_up;
    wchar_t* tb_copy;
    wchar_t* tb_cut;
    wchar_t* tb_paste;
    wchar_t* tb_delete;
    wchar_t* tb_new_folder;
    wchar_t* tb_new_file;
    wchar_t* tb_bookmark;
    wchar_t* import_reg;
    wchar_t* show_hidden_files;
    wchar_t* open_with;
    wchar_t* open_with_label;
    wchar_t* browse;
    wchar_t* always_use;
    wchar_t* open_with_menu;
    wchar_t* folder_position;
    wchar_t* folder_pos_classic;
    wchar_t* folder_pos_top;
    wchar_t* folder_pos_bottom;
    wchar_t* folder_pos_plain;
    wchar_t* icon_size;
    wchar_t* label_lines;
    wchar_t* label_lines_auto;
    wchar_t* drive_usage_bar;
    wchar_t* drive_usage_bar_graph;
    wchar_t* drive_usage_bar_total;
    wchar_t* drive_usage_bar_none;
};

extern struct LC_STR lc_str;

#include "locale/strings_en.h"
#include "locale/strings_pt.h"
#include "locale/strings_ru.h"
#include "locale/strings_zh.h"

#define STARTS_WITH(a, b) (a[0] == b[0] && a[1] == b[1])

static inline void loadLCStrings(wchar_t* localeName) {
    if (STARTS_WITH(localeName, L"pt")) {
        loadStrings_pt();
    }
    else if (STARTS_WITH(localeName, L"ru")) {
        loadStrings_ru();
    }
    else if (STARTS_WITH(localeName, L"zh")) {
        loadStrings_zh();
    }
    else loadStrings_en();
}

#undef STARTS_WITH

#endif