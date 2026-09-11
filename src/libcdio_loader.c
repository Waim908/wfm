#include "libcdio_loader.h"

#ifndef USE_LIBCDIO_STATIC
    static HMODULE hLibcdio = NULL;
    
    // 全局函数指针
    cdio_open_fn ptr_cdio_open = NULL;
    cdio_destroy_fn ptr_cdio_destroy = NULL;
    cdio_set_arg_fn ptr_cdio_set_arg = NULL;
    cdio_get_joliet_level_fn ptr_cdio_get_joliet_level = NULL;
    cdio_read_data_sectors_fn ptr_cdio_read_data_sectors = NULL;
    
    iso9660_open_ext_fn ptr_iso9660_open_ext = NULL;
    iso9660_close_fn ptr_iso9660_close = NULL;
    iso9660_ifs_get_joliet_level_fn ptr_iso9660_ifs_get_joliet_level = NULL;
    iso9660_fs_readdir_fn ptr_iso9660_fs_readdir = NULL;
    iso9660_ifs_readdir_fn ptr_iso9660_ifs_readdir = NULL;
    iso9660_iso_seek_read_fn ptr_iso9660_iso_seek_read = NULL;
    iso9660_name_translate_ext_fn ptr_iso9660_name_translate_ext = NULL;
    iso9660_filelist_free_fn ptr_iso9660_filelist_free = NULL;
    
    _cdio_list_begin_fn ptr__cdio_list_begin = NULL;
    _cdio_list_node_data_fn ptr__cdio_list_node_data = NULL;
    _cdio_list_node_next_fn ptr__cdio_list_node_next = NULL;
    
    #define LOAD_FUNC(handle, name, type) \
        ptr_##name = (type)GetProcAddress(handle, #name); \
        if (!ptr_##name) goto fail;
    
    BOOL libcdio_load(void) {
        if (hLibcdio) return TRUE;
        
        // S13：不要用相对文件名 LoadLibraryA("libcdio.dll")——它走默认搜索顺序，
        // 会优先命中「当前工作目录」，攻击者只要诱使 WFM 在含恶意 libcdio.dll 的
        // 目录下启动即可实现 DLL 劫持。这里改成从 wfm.exe 自身所在目录取绝对路径。
        wchar_t self[MAX_PATH] = {0};
        wchar_t dllPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0) return FALSE;
        
        // 截断到最后一个路径分隔符（保留分隔符本身）
        int cut = -1;
        for (int i = 0; self[i] != L'\0'; i++) {
            if (self[i] == L'\\' || self[i] == L'/') cut = i;
        }
        if (cut < 0 || cut + 1 + (int)wcslen(L"libcdio.dll") >= MAX_PATH) return FALSE;
        
        wcsncpy_s(dllPath, MAX_PATH, self, (size_t)(cut + 1));
        if (wcscat_s(dllPath, MAX_PATH, L"libcdio.dll") != 0) return FALSE;
        
        hLibcdio = LoadLibraryW(dllPath);
        if (!hLibcdio) return FALSE;
        
        LOAD_FUNC(hLibcdio, cdio_open, cdio_open_fn);
        LOAD_FUNC(hLibcdio, cdio_destroy, cdio_destroy_fn);
        LOAD_FUNC(hLibcdio, cdio_set_arg, cdio_set_arg_fn);
        LOAD_FUNC(hLibcdio, cdio_get_joliet_level, cdio_get_joliet_level_fn);
        LOAD_FUNC(hLibcdio, cdio_read_data_sectors, cdio_read_data_sectors_fn);
        
        LOAD_FUNC(hLibcdio, iso9660_open_ext, iso9660_open_ext_fn);
        LOAD_FUNC(hLibcdio, iso9660_close, iso9660_close_fn);
        LOAD_FUNC(hLibcdio, iso9660_ifs_get_joliet_level, iso9660_ifs_get_joliet_level_fn);
        LOAD_FUNC(hLibcdio, iso9660_fs_readdir, iso9660_fs_readdir_fn);
        LOAD_FUNC(hLibcdio, iso9660_ifs_readdir, iso9660_ifs_readdir_fn);
        LOAD_FUNC(hLibcdio, iso9660_iso_seek_read, iso9660_iso_seek_read_fn);
        LOAD_FUNC(hLibcdio, iso9660_name_translate_ext, iso9660_name_translate_ext_fn);
        LOAD_FUNC(hLibcdio, iso9660_filelist_free, iso9660_filelist_free_fn);
        
        LOAD_FUNC(hLibcdio, _cdio_list_begin, _cdio_list_begin_fn);
        LOAD_FUNC(hLibcdio, _cdio_list_node_data, _cdio_list_node_data_fn);
        LOAD_FUNC(hLibcdio, _cdio_list_node_next, _cdio_list_node_next_fn);
        
        return TRUE;
        
    fail:
        FreeLibrary(hLibcdio);
        hLibcdio = NULL;
        return FALSE;
    }
    
    void libcdio_free(void) {
        if (hLibcdio) {
            FreeLibrary(hLibcdio);
            hLibcdio = NULL;
            
            ptr_cdio_open = NULL;
            ptr_cdio_destroy = NULL;
            ptr_cdio_set_arg = NULL;
            ptr_cdio_get_joliet_level = NULL;
            ptr_cdio_read_data_sectors = NULL;
            
            ptr_iso9660_open_ext = NULL;
            ptr_iso9660_close = NULL;
            ptr_iso9660_ifs_get_joliet_level = NULL;
            ptr_iso9660_fs_readdir = NULL;
            ptr_iso9660_ifs_readdir = NULL;
            ptr_iso9660_iso_seek_read = NULL;
            ptr_iso9660_name_translate_ext = NULL;
            ptr_iso9660_filelist_free = NULL;
            
            ptr__cdio_list_begin = NULL;
            ptr__cdio_list_node_data = NULL;
            ptr__cdio_list_node_next = NULL;
        }
    }
    
    BOOL libcdio_is_loaded(void) {
        return hLibcdio != NULL;
    }
#else
    cdio_open_fn ptr_cdio_open = cdio_open;
    cdio_destroy_fn ptr_cdio_destroy = cdio_destroy;
    cdio_set_arg_fn ptr_cdio_set_arg = cdio_set_arg;
    cdio_get_joliet_level_fn ptr_cdio_get_joliet_level = (cdio_get_joliet_level_fn)cdio_get_joliet_level;
    cdio_read_data_sectors_fn ptr_cdio_read_data_sectors = (cdio_read_data_sectors_fn)cdio_read_data_sectors;
    
    iso9660_open_ext_fn ptr_iso9660_open_ext = iso9660_open_ext;
    iso9660_close_fn ptr_iso9660_close = (iso9660_close_fn)iso9660_close;
    iso9660_ifs_get_joliet_level_fn ptr_iso9660_ifs_get_joliet_level = (iso9660_ifs_get_joliet_level_fn)iso9660_ifs_get_joliet_level;
    iso9660_fs_readdir_fn ptr_iso9660_fs_readdir = (iso9660_fs_readdir_fn)iso9660_fs_readdir;
    iso9660_ifs_readdir_fn ptr_iso9660_ifs_readdir = (iso9660_ifs_readdir_fn)iso9660_ifs_readdir;
    iso9660_iso_seek_read_fn ptr_iso9660_iso_seek_read = (iso9660_iso_seek_read_fn)iso9660_iso_seek_read;
    iso9660_name_translate_ext_fn ptr_iso9660_name_translate_ext = (iso9660_name_translate_ext_fn)iso9660_name_translate_ext;
    iso9660_filelist_free_fn ptr_iso9660_filelist_free = iso9660_filelist_free;
    
    _cdio_list_begin_fn ptr__cdio_list_begin = (_cdio_list_begin_fn)_cdio_list_begin;
    _cdio_list_node_data_fn ptr__cdio_list_node_data = (_cdio_list_node_data_fn)_cdio_list_node_data;
    _cdio_list_node_next_fn ptr__cdio_list_node_next = (_cdio_list_node_next_fn)_cdio_list_node_next;
#endif
