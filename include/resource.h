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

#define IDI_CMD 210
#define IDI_EXPLORER 211
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
#define ID_EDIT_PASTE_SHORTCUT 310
#define ID_EDIT_SELECT_ALL 311
#define ID_SHOW_ICON 312
#define ID_VIEW_CLEAR_ICON_CACHE 313
#define ID_MOUNT_LOCATE_ISO  314
#define ID_MOUNT_UNMOUNT_ISO  315
#define ID_LANG_EN  316
#define ID_LANG_ZH  317
#define ID_LANG_PT  318
#define ID_LANG_RU  319

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
#define APP_VERSION L"1.5-mod.2.5"
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
    wchar_t* edit;
    wchar_t* cut;
    wchar_t* copy;
    wchar_t* paste;
    wchar_t* paste_shortcut;
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
    wchar_t* msg_confirm_delete_item;
    wchar_t* msg_confirm_delete_multiple_items;
    wchar_t* msg_confirm_exit_app;
    wchar_t* clear_icon_cache;
    wchar_t* mount;
    wchar_t* locate_iso;
    wchar_t* open_file_location;
    wchar_t* unmount_iso;
    wchar_t* msg_no_mounted_image;
    wchar_t* msg_image_dir_not_found;
    wchar_t* msg_x_drive_not_found;
    wchar_t* msg_no_libcdio;
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