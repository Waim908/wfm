case $1 in
	nolibdio)
		7z a -tzip wfm-nolibcdio.zip wfm.exe del-reg.bat wfm-extra.bat install-winlator.bat
	;;
	*)
		7z a -tzip wfm.zip wfm.exe libcdio.dll del-reg.bat wfm-extra.bat install-winlator.bat
	;;
esac

