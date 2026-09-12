
targetFiles=(wfm.exe del-reg.bat wfm-extra.bat install-winlator.bat readme.txt)

scriptDir="$(dirname $(readlink -f "$0"))"

targetVerFile="${scriptDir}/include/resource.h"

getVersion=$(sed -n 's/.*APP_VERSION[[:space:]]*L"\([^"]*\)".*/\1/p' "${targetVerFile}" | sed 's/mod\.//')


vanilla_reademe_txt () {
cat > "${scriptDir}/readme.txt"	<< EOF
[Info]
version: ${getVersion}
libcdio: ${libcdioSupport}

Modify By Waim908
https://github.com/Waim908/wfm/

从1.5-mod.2.5版本开始，彻底脱离了libcdio.dll的动态依赖，无需libcdio.dll动态库
挂载功能必须使用非nolibcdio的版本
如果需要挂载镜像文件，请通过winecfg 新建X盘符到有效路径，类型设置为光驱
如果需要切换为其他语言在菜单栏'language'完成设置此设置不受系统环境语言编码设定的影响
挂载功能在winlator bionic分支也能正常使用只要你创建了X盘

Starting from version 1.5-mod.2.5, the dynamic dependency on libcdio.dll has been completely removed, so libcdio.dll is no longer required.
The mounting feature must use a non-nolibcdio version.
If you need to mount an image file, please use winecfg to create a new X: drive pointing to a valid path, and set its type to CD-ROM.
If you need to switch to another language, you can do so from the 'Language' menu in the menu bar. This setting is not affected by the system's locale or language encoding settings.
The mounting feature also works properly on the Winlator bionic branch, as long as you have created the X: drive.
EOF
}


echo "Building version ${getVersion}..."

make clean

case $1 in
	nolibcdio)
		make USE_LIBCDIO=0 -j$(nproc)
		libcdioSupport="No"
		vanilla_reademe_txt
		7z a -tzip wfm-${getVersion}-nolibcdio.zip "${targetFiles[@]}"
	;;
	all)
		make USE_LIBCDIO=0 -j$(nproc)
		libcdioSupport="No"
		vanilla_reademe_txt
		7z a -tzip wfm-${getVersion}-nolibcdio.zip "${targetFiles[@]}"
		make USE_LIBCDIO=1 -j$(nproc)
		libcdioSupport="Yes"
		vanilla_reademe_txt
		7z a -tzip wfm-${getVersion}.zip "${targetFiles[@]}"
	;;
	*)
		make USE_LIBCDIO=1 -j$(nproc)
		libcdioSupport="Yes"
		vanilla_reademe_txt
		7z a -tzip wfm-${getVersion}.zip "${targetFiles[@]}"
	;;
esac

