@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
where gcc >nul 2>nul
if errorlevel 1 (echo ERROR: MinGW-w64 GCC no encontrado & exit /b 1)
gcc -std=c11 -Os -flto -s -mwindows -Wall -Wextra -Iinclude ^
 examples\win32_ui2_demo.c src\ui2.c src\ui_font.c ^
 -o build\Skin144_UI2.exe -lgdi32 -luser32
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_bytes.ps1 build\Skin144_UI2.exe
exit /b %errorlevel%
