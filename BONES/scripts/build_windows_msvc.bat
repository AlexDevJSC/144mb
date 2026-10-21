@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if not exist build mkdir build
where py >nul 2>nul
if errorlevel 1 (echo ERROR: Python (py -3) necesario SOLO para generar el registro de modulos. & exit /b 1)
py -3 scripts\gen_modkit_registry.py
if errorlevel 1 exit /b 1
set MODULE_FILES=
for /R modules %%F in (modkit.c) do set MODULE_FILES=!MODULE_FILES! "%%F"
where cl >nul 2>nul
if errorlevel 1 (echo ERROR: Abre x64 Native Tools Command Prompt. & exit /b 1)
cl /nologo /std:c11 /TC /O2 /GL /MT /W4 /DBONES_MODKIT_NO_DYNLIB /D_CRT_SECURE_NO_WARNINGS ^
 src\app\win32_main.c src\bones\core.c src\bones\world.c src\bones\physics.c src\bones\scene.c src\bones\subsystems.c src\foundation\foundation.c src\modkit\modkit.c src\generated\modkit_registry.c !MODULE_FILES! ^
 src\demo\test_house.c src\adapters\eye_adapter.c third_party\eye\sw_renderer.c ^
 /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG ^
 user32.lib gdi32.lib kernel32.lib /OUT:build\Proyecto144_Bones_ModKit.exe
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verificar_tamano.ps1 build\Proyecto144_Bones_ModKit.exe
exit /b %errorlevel%
