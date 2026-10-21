@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
where gcc >nul 2>nul
if errorlevel 1 (echo ERROR: GCC MinGW-w64 no detectado. & exit /b 1)
REM A test harness uses mock_bones.c. Do not ship it as the gameplay engine!
gcc -std=c11 -Os -flto -s -mwindows -Wall -Wextra -Iinclude -Itests\fixtures ^
 examples\win32_skin_demo.c tests\mock_bones.c src\skin.c src\save.c src\ui.c src\ui_font.c src\ui2.c ^
 -o build\Skin144_TestUI.exe -lgdi32 -luser32 -lm
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_bytes.ps1 build\Skin144_TestUI.exe
exit /b %errorlevel%
