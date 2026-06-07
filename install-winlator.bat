taskkill /f /im wfm.exe
copy /y .\wfm.exe C:\windows
@REM if exist ".\libcdio.dll" (
@REM     echo "Copying libcdio.dll"
@REM     copy /y ".\libcdio.dll" C:\windows
@REM )

echo "Reboot To Apply Update!"

pause
