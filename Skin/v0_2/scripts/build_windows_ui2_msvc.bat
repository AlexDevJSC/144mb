@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
where cl >nul 2>nul
if errorlevel 1 (echo ERROR: Developer Command Prompt no detectado & exit /b 1)
cl /nologo /std:c11 /TC /O2 /GL /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Iinclude ^
 examples\win32_ui2_demo.c src\ui2.c src\ui_font.c ^
 /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG user32.lib gdi32.lib kernel32.lib ^
 /OUT:build\Skin144_UI2.exe
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_bytes.ps1 build\Skin144_UI2.exe
exit /b %errorlevel%
