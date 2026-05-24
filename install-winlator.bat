taskkill /f /im wfm.exe
copy /y .\wfm.exe C:\windows
if exist ".\libcdio.dll" (
    echo "Copying libcdio.dll"
    copy /y ".\libcdio.dll" C:\windows
)

echo "Reboot To Apply Update!"

pause
