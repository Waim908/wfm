#ifndef STRINGS_ZH_H
#define STRINGS_ZH_H

#include "resource.h"

static inline void loadStrings_zh() {
    lc_str.app_name = APP_NAME;
    lc_str.app_version = L"版本 " APP_VERSION;
    lc_str.app_dev_name = L"作者：" APP_DEV_NAME;
    lc_str.app_mod_name = L"修改者：Waim908";
    lc_str.app_url = L"https://github.com/Waim908/wfm";
    lc_str.application = L"应用程序";
    lc_str.shortcut = L"快捷方式";
    lc_str.file = L"文件";
    lc_str.folder = L"文件夹";
    lc_str.local_drive = L"本地磁盘";
    lc_str.cd_drive = L"CD 驱动器";
    lc_str.computer = L"计算机";
    lc_str.desktop = L"桌面";
    lc_str.documents = L"文档";
    lc_str.exit = L"退出";
    lc_str.edit = L"编辑";
    lc_str.cut = L"剪切";
    lc_str.copy = L"复制";
    lc_str.paste = L"粘贴";
    lc_str.paste_shortcut = L"粘贴快捷方式";
    lc_str.select_all = L"全选";
    lc_str.view = L"查看";
    lc_str.large_icons = L"大图标";
    lc_str.small_icons = L"小图标";
    lc_str.list = L"列表";
    lc_str.details = L"详细信息";
    lc_str.help = L"帮助";
    lc_str.about = L"关于";
    lc_str.ok = L"确定";
    lc_str.cancel = L"取消";
    lc_str.loading = L"加载中...";
    lc_str.open = L"打开";
    lc_str.create_shortcut = L"创建快捷方式";
    lc_str.delete = L"删除";
    lc_str.rename = L"重命名";
    lc_str.new_folder = L"新建文件夹";
    lc_str.new_file = L"新建文件";
    lc_str.items = L"项";
    lc_str.load_iso_image = L"加载 ISO 映像";
    lc_str.unload_iso_image = L"卸载 ISO 映像";
    lc_str.no_media = L"无介质";
    lc_str.alert = L"警告";
    lc_str.enter_folder_name = L"请输入文件夹名称：";
    lc_str.enter_file_name = L"请输入文件名称：";
    lc_str.enter_new_name = L"请输入新名称：";
    lc_str.name = L"名称";
    lc_str.type = L"类型";
    lc_str.size = L"大小";
    lc_str.date = L"日期";
    lc_str.path = L"路径";
    lc_str.deleting_files = L"正在删除文件";
    lc_str.copying_files = L"正在复制文件";
    lc_str.moving_files = L"正在移动文件";
    lc_str.extracting_files = L"正在解压文件";
    lc_str.confirm_delete = L"确认删除";
    lc_str.confirm_exit = L"确认退出";
    lc_str.search = L"搜索";
    lc_str.up = L"向上";
    lc_str.show_icon = L"显示图标";
    lc_str.bookmarks = L"收藏";
    lc_str.bookmark = L"收藏";
    lc_str.add_bookmark = L"添加到收藏";
    lc_str.remove_bookmark = L"移除收藏";
    lc_str.bookmark_exists = L"该路径已在收藏列表中";
    lc_str.bookmark_path_not_found = L"收藏的路径不存在：%ls";
    lc_str.msg_cannot_launch_system_app = L"无法启动系统程序：%ls";
    lc_str.auto_open_on_start = L"启动时打开";
    lc_str.cancel_auto_open = L"取消启动";
    lc_str.auto_open_path_not_found = L"启动时无法打开路径（不存在）：%ls";

    lc_str.clear_icon_cache = L"清除图标缓存";
    lc_str.mount = L"挂载";
    lc_str.locate_iso = L"定位到映像文件";
    lc_str.open_file_location = L"打开文件所在路径";
    lc_str.unmount_iso = L"取消挂载";
    lc_str.msg_no_mounted_image = L"没有挂载的映像文件";
    lc_str.msg_x_drive_not_found = L"未找到 X: 盘，请先在 winecfg 中添加驱动器 X: 并设置为光驱类型";
    lc_str.msg_image_dir_not_found = L"映像文件所在目录不存在";
    lc_str.msg_no_libcdio = L"此版本不支持 libcdio 挂载功能";
    lc_str.msg_confirm_unmount_iso = L"取消挂载会清空 X: 盘的全部内容。\n\nX: 通常软链到真实目录，此操作不可撤销。确定继续吗？";
    lc_str.save_icon = L"保存图标";
    lc_str.fmt_drive_space = L"%ls / %ls";

    lc_str.fmt_file = L"%ls 文件";

    lc_str.msg_invalid_iso_image_file = L"无效的 ISO 映像文件！";

    lc_str.msg_deleting_files = L"正在删除文件，请稍候...";
    lc_str.msg_copying_files = L"正在复制文件，请稍候...";
    lc_str.msg_moving_files = L"正在移动文件，请稍候...";
    lc_str.msg_cancel_file_operation = L"是否要取消此操作？";
    lc_str.msg_confirm_delete_item = L"确定要删除“%ls”吗？";
    lc_str.msg_confirm_delete_multiple_items = L"确定要删除 %d 个项目吗？";
    lc_str.msg_confirm_exit_app = L"确定要退出吗？";

    // Toolbar short strings
    lc_str.tb_up = L"向上";
    lc_str.tb_copy = L"复制";
    lc_str.tb_cut = L"剪切";
    lc_str.tb_paste = L"粘贴";
    lc_str.tb_delete = L"删除";
    lc_str.tb_new_folder = L"新建文件夹";
    lc_str.tb_new_file = L"新建文件";
    lc_str.tb_bookmark = L"收藏";
    lc_str.import_reg = L"导入到注册表";
    lc_str.show_hidden_files = L"显示隐藏文件";
    lc_str.open_with = L"打开方式";
    lc_str.open_with_label = L"选择要用来打开此文件的程序：";
    lc_str.browse = L"浏览...";
    lc_str.always_use = L"始终使用此程序打开此类文件";
    lc_str.open_with_menu = L"打开方式...";

    // 文件夹位置（排序策略）
    lc_str.folder_position = L"文件夹位置";
    lc_str.folder_pos_classic = L"跟随排序方向（经典）";
    lc_str.folder_pos_top = L"置顶（资源管理器）";
    lc_str.folder_pos_bottom = L"沉底（固定）";
    lc_str.folder_pos_plain = L"不区分（纯按本列）";
}

#endif