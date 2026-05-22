@echo off
REG DELETE "HKEY_CURRENT_USER\Software\Winlator\WFM" /f
if %errorlevel% equ 0 (
    echo Done!
) else (
    echo Failed!
)
pause