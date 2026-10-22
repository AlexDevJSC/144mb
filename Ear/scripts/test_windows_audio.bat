@echo off
setlocal
REM Hardware acceptance on real Windows: output required by default.
REM --allow-no-device: CI-only recoverable error smoke check.
REM --retry-device: unplug output before startup, reconnect when prompted.
cd /d "%~dp0..\.."
if not exist Ear\build mkdir Ear\build
where cl >nul 2>nul
if errorlevel 1 (
 echo ERROR: cl.exe no encontrado. Abre x64 Native Tools Command Prompt.
 exit /b 1
)
cl /nologo /std:c11 /TC /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS ^
 Ear\tools\ear_windows_playtest.c BONES\modules\ear\modkit.c ^
 /link /SUBSYSTEM:CONSOLE kernel32.lib /OUT:Ear\build\EarWinMMPlaytest.exe
if errorlevel 1 exit /b 1
if /I "%~1"=="--allow-no-device" (
 Ear\build\EarWinMMPlaytest.exe --allow-no-device
) else if /I "%~1"=="--retry-device" (
 Ear\build\EarWinMMPlaytest.exe --require-device --retry-device --listen-confirm
) else (
 Ear\build\EarWinMMPlaytest.exe --require-device --listen-confirm
)
exit /b %errorlevel%
