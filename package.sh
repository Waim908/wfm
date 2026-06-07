targetFiles=(wfm.exe del-reg.bat wfm-extra.bat install-winlator.bat readme.txt)

scriptDir="$(dirname $(readlink -f "$0"))"

targetVerFile="${scriptDir}/include/resource.h"

getVersion=$(sed -n 's/.*APP_VERSION[[:space:]]*L"\([^"]*\)".*/\1/p' "${targetVerFile}" | sed 's/mod\.//')

echo "Building version ${getVersion}..."

case $1 in
	nolibcdio)
		make USE_LIBCDIO=0 -j$(nproc)
		7z a -tzip wfm-${getVersion}-nolibcdio.zip "${targetFiles[@]}"
	;;
	all)
		make USE_LIBCDIO=0 -j$(nproc)
		7z a -tzip wfm-${getVersion}-nolibcdio.zip "${targetFiles[@]}"
		make USE_LIBCDIO=1 -j$(nproc)
		7z a -tzip wfm-${getVersion}.zip "${targetFiles[@]}"
	;;
	*)
		make USE_LIBCDIO=1 -j$(nproc)
		7z a -tzip wfm-${getVersion}.zip "${targetFiles[@]}"
	;;
esac

