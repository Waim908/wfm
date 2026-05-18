#ifndef LIBCDIO_LOADER_H
#define LIBCDIO_LOADER_H

#include <windows.h>
#include <cdio/cdio.h>
#include <cdio/iso9660.h>

// 函数指针类型定义
typedef CdIo_t* (*cdio_open_fn)(const char*, driver_id_t);
typedef void (*cdio_destroy_fn)(CdIo_t*);
typedef int (*cdio_set_arg_fn)(CdIo_t*, const char*, const char*);
typedef int (*cdio_get_joliet_level_fn)(CdIo_t*);
typedef int (*cdio_read_data_sectors_fn)(CdIo_t*, void*, lsn_t, int, int);

typedef iso9660_t* (*iso9660_open_ext_fn)(const char*, iso_extension_mask_t);
typedef void (*iso9660_close_fn)(iso9660_t*);
typedef int (*iso9660_ifs_get_joliet_level_fn)(iso9660_t*);
typedef CdioISO9660FileList_t* (*iso9660_fs_readdir_fn)(CdIo_t*, const char*);
typedef CdioISO9660FileList_t* (*iso9660_ifs_readdir_fn)(iso9660_t*, const char*);
typedef int (*iso9660_iso_seek_read_fn)(iso9660_t*, void*, lsn_t, int);
typedef void (*iso9660_name_translate_ext_fn)(const char*, char*, int);
typedef void (*iso9660_filelist_free_fn)(CdioISO9660FileList_t*);

// 内部列表函数
typedef CdioListNode_t* (*_cdio_list_begin_fn)(CdioISO9660FileList_t*);
typedef void* (*_cdio_list_node_data_fn)(CdioListNode_t*);
typedef void (*_cdio_list_node_next_fn)(CdioListNode_t**);

// 全局函数指针
extern cdio_open_fn ptr_cdio_open;
extern cdio_destroy_fn ptr_cdio_destroy;
extern cdio_set_arg_fn ptr_cdio_set_arg;
extern cdio_get_joliet_level_fn ptr_cdio_get_joliet_level;
extern cdio_read_data_sectors_fn ptr_cdio_read_data_sectors;

extern iso9660_open_ext_fn ptr_iso9660_open_ext;
extern iso9660_close_fn ptr_iso9660_close;
extern iso9660_ifs_get_joliet_level_fn ptr_iso9660_ifs_get_joliet_level;
extern iso9660_fs_readdir_fn ptr_iso9660_fs_readdir;
extern iso9660_ifs_readdir_fn ptr_iso9660_ifs_readdir;
extern iso9660_iso_seek_read_fn ptr_iso9660_iso_seek_read;
extern iso9660_name_translate_ext_fn ptr_iso9660_name_translate_ext;
extern iso9660_filelist_free_fn ptr_iso9660_filelist_free;

extern _cdio_list_begin_fn ptr__cdio_list_begin;
extern _cdio_list_node_data_fn ptr__cdio_list_node_data;
extern _cdio_list_node_next_fn ptr__cdio_list_node_next;

// 加载/卸载 libcdio.dll
BOOL libcdio_load(void);
void libcdio_free(void);
BOOL libcdio_is_loaded(void);

#endif // LIBCDIO_LOADER_H
