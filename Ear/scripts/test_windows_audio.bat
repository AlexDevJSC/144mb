@echo off
setlocal
REM Run from x64 Native Tools Command Prompt for VS. Compiles Ear alone,
REM but uses REAL Bones 0.4 ModKit header from the checkout.
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
if "%~1"=="--allow-no-device" (
  Ear\build\EarWinMMPlaytest.exe
) else (
  Ear\build\EarWinMMPlaytest.exe --require-device
)
exit /b %errorlevel%
