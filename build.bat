@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W4 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS /LD /Fobuild\ /Febuild\libScePad_x64.dll src\pad.c src\device.c src\haptics.c src\audio.cpp /link /DEF:src\pad.def hid.lib setupapi.lib ole32.lib mmdevapi.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\padtest.exe test\padtest.c
cl /nologo /O2 /W4 /MT /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\check.exe test\check.c
