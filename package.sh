case $1 in
	*)
		7z a -tzip wfm.zip wfm.exe libcdio.dll del-reg.bat wfm-extra.bat install-winlator.bat
	;;
	nolibcdio)
		7z a -tzip wfm.zip wfm.exe del-reg.bat wfm-extra.bat install-winlator.bat
	;;
esac

