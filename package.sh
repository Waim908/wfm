
targetFiles=(wfm.exe del-reg.bat wfm-extra.bat install-winlator.bat readme.txt)

scriptDir="$(dirname $(readlink -f "$0"))"

targetVerFile="${scriptDir}/include/resource.h"

getVersion=$(sed -n 's/.*APP_VERSION[[:space:]]*L"\([^"]*\)".*/\1/p' "${targetVerFile}" | sed 's/mod\.//')


# 生成随包 readme.txt。按当前变体写入架构与是否带挂载功能，避免把只适用于
# libcdio 版的说明发给 nolibcdio 用户。
write_readme () {
    local arch="$1" use_libcdio="$2"
    local libcdioDesc archNote mountZh mountEn

    if [ "$arch" = "aarch64" ]; then
        archNote="（ARM64 原生，无需 box64）"
    else
        archNote=""
    fi

    if [ "$use_libcdio" = 1 ]; then
        libcdioDesc="Yes（支持挂载镜像）"
        mountZh="挂载镜像（本包支持）
  1. 用 winecfg 新建盘符 X:，路径指向一个非根目录，例如 ~/.wine/drive_x。
  2. 建议把 X: 的类型设为「光驱」；只解压镜像内容时用默认类型也可以。
  3. 在工具栏点挂载按钮选择 ISO，即可浏览或解压镜像内的文件。
  Winlator bionic 分支只要建好 X: 盘，挂载与解压同样可用。"
        mountEn="Mounting images (supported by this build)
  1. Create an X: drive in winecfg pointing to a non-root directory, e.g. ~/.wine/drive_x.
  2. Setting its type to CD-ROM is recommended; the default type also works for extraction.
  3. Use the mount button on the toolbar to pick an ISO and browse or extract it.
  This also works on the Winlator bionic branch once the X: drive exists."
    else
        libcdioDesc="No（不含挂载功能）"
        mountZh="挂载镜像（本包不支持）
  本包为 nolibcdio 版本，不含镜像挂载功能。需要挂载请改用文件名不带
  -nolibcdio 的包。"
        mountEn="Mounting images (not supported by this build)
  This is the nolibcdio build and cannot mount images. Use the package whose
  name does not end with -nolibcdio if you need this feature."
    fi

cat > "${scriptDir}/readme.txt" << EOF
[Info]
version: ${getVersion}
arch: ${arch}${archNote}
libcdio: ${libcdioDesc}
home: https://github.com/Waim908/wfm/

Winlator File Manager：运行在 Wine / Winlator 上的文件管理器，解压即用。

快速开始
  1. 直接运行 wfm.exe，无需安装，也不需要 libcdio.dll。
  2. 界面语言在菜单栏 Language 里切换（中文 / English / Português / Русский），
     保存后需重启 wfm 生效，不受系统 locale 与编码设置影响。
  3. 配置保存在 HKEY_CURRENT_USER\Software\Winlator\WFM，只写当前用户，不需要管理员权限。
  4. 恢复默认设置：运行 del-reg.bat，或删除上述注册表项后重启 wfm。

${mountZh}

附带脚本
  del-reg.bat           删除 WFM 的全部注册表配置，恢复默认设置。
  wfm-extra.bat         给右键菜单添加 7-Zip 条目；其中的 7-Zip 路径请按自己的环境修改。
  install-winlator.bat  把 wfm.exe 复制到 C:\windows 并结束正在运行的 wfm，用于覆盖升级。

卸载
  1. 先取消已挂载的镜像。
  2. 删除 wfm.exe。
  3. 运行 del-reg.bat，或手动删除 HKEY_CURRENT_USER\Software\Winlator\WFM。


Quick start
  1. Extract and run wfm.exe. No installer and no libcdio.dll required.
  2. Switch the UI language from the Language menu (Chinese / English / Português / Русский).
     It takes effect after restarting wfm and is independent of the system locale.
  3. Settings live under HKEY_CURRENT_USER\Software\Winlator\WFM (current user only).
  4. To reset everything, run del-reg.bat or delete that registry key.

${mountEn}

Bundled scripts
  del-reg.bat           Remove all WFM registry settings.
  wfm-extra.bat         Add 7-Zip entries to WFM's context menu; edit the 7-Zip path first.
  install-winlator.bat  Copy wfm.exe to C:\windows for an in-place upgrade.

Uninstall
  1. Unmount any mounted image first.
  2. Delete wfm.exe.
  3. Run del-reg.bat or delete HKEY_CURRENT_USER\Software\Winlator\WFM.
EOF
}


# 构建一个变体并打包。参数: 架构(x86_64/aarch64) 是否带libcdio(0/1) zip文件名
# 每次构建后立即打包，wfm.exe 属于当前变体；Makefile 会自动处理变体/架构
# 切换时的隔离（目标文件按 工具链-架构-变体 分目录存放）。
build_zip () {
    local arch="$1" use_libcdio="$2" zipname="$3"

    make USE_LIBCDIO=$use_libcdio ARCH=$arch -j$(nproc)
    if [ ! -f wfm.exe ]; then
        echo "build failed: $zipname" >&2
        exit 1
    fi
    write_readme "$arch" "$use_libcdio"
    7z a -tzip "$zipname" "${targetFiles[@]}"
}


echo "Building version ${getVersion}..."

make clean

case $1 in
	nolibcdio)
		build_zip x86_64 0 wfm-${getVersion}-nolibcdio.zip
	;;
	aarch64)
		build_zip aarch64 1 wfm-${getVersion}-aarch64.zip
	;;
	aarch64-nolibcdio)
		build_zip aarch64 0 wfm-${getVersion}-aarch64-nolibcdio.zip
	;;
	all)
		# x86_64 与 aarch64 各打 nolibcdio 与 libcdio 两个包，共四个
		build_zip x86_64 0 wfm-${getVersion}-nolibcdio.zip
		build_zip x86_64 1 wfm-${getVersion}.zip
		build_zip aarch64 0 wfm-${getVersion}-aarch64-nolibcdio.zip
		build_zip aarch64 1 wfm-${getVersion}-aarch64.zip
	;;
	*)
		build_zip x86_64 1 wfm-${getVersion}.zip
	;;
esac
