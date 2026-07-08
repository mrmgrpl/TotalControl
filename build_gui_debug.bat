@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
cmake --build D:\Dev\TotalControl\out\build\x64-Debug --target TotalControlGUI 2>&1
echo BUILD_DONE %ERRORLEVEL%
