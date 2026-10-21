@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build
where cl >nul 2>nul
if errorlevel 1 (echo ERROR: Usa Developer Command Prompt de Visual Studio. & exit /b 1)
cl /nologo /std:c11 /TC /O2 /GL /MT /W4 /D_CRT_SECURE_NO_WARNINGS ^
 /Iinclude /Itests\fixtures ^
 examples\win32_skin_demo.c tests\mock_bones.c src\skin.c src\save.c src\ui.c src\ui_font.c src\ui2.c ^
 /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG ^
 user32.lib gdi32.lib kernel32.lib /OUT:build\Skin144_TestUI.exe
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify_bytes.ps1 build\Skin144_TestUI.exe
exit /b %errorlevel%
