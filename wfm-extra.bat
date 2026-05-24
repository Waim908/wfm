reg add "HKCU\Software\Winlator\WFM\ContextMenu\7-Zip" /v "Extract Here" /t REG_SZ /d "Z:\\opt\\apps\\7-Zip\\7zG.exe x \"%%FILE%%\" -r -o\"%%DIR%%\" -y" /f
reg add "HKCU\Software\Winlator\WFM\ContextMenu\7-Zip" /v "Extract to Folder" /t REG_SZ /d "Z:\\opt\\apps\\7-Zip\\7zG.exe x \"%%FILE%%\" -r -o\"%%DIR%%\\%%BASENAME%%\" -y" /f
reg add "HKCU\Software\Winlator\WFM\ContextMenu\7-Zip" /v "Open Archive" /t REG_SZ /d "Z:\\opt\\apps\\7-Zip\\7zFM.exe \"%%FILE%%\"" /f
