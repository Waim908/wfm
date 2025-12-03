if not defined NUMBER_OF_PROCESSORS set NUMBER_OF_PROCESSORS=8
if exist wfm.exe del wfm.exe
if exist obj del /s /q obj
set PATH=C:\w64devkit\bin
make -j4