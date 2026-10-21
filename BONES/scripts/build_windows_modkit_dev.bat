@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if not exist build mkdir build
where cl >nul 2>nul
if errorlevel 1 (echo ERROR: ejecutar desde x64 Native Tools Command Prompt & exit /b 1)
cl /nologo /TC /std:c11 /W4 /LD samples\plugin\clock_mod.c /link /OUT:build\clock_mod.dll
if errorlevel 1 exit /b 1
set MODULE_FILES=
for /R modules %%F in (modkit.c) do set MODULE_FILES=!MODULE_FILES! "%%F"
py -3 scripts\gen_modkit_registry.py
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /W4 tests\test_modkit_dll.c src\bones\core.c src\bones\world.c src\bones\physics.c src\bones\scene.c src\bones\subsystems.c src\foundation\foundation.c src\modkit\modkit.c src\generated\modkit_registry.c !MODULE_FILES! src\demo\test_house.c src\adapters\eye_adapter.c third_party\eye\sw_renderer.c /link /OUT:build\test_modkit_dll.exe
if errorlevel 1 exit /b 1
build\test_modkit_dll.exe build\clock_mod.dll
exit /b %errorlevel%
