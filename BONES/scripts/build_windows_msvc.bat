@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if not exist build mkdir build
if not exist build\obj_modkit mkdir build\obj_modkit
where py >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python [py -3] requerido solo para generar registro.
  exit /b 1
)
py -3 scripts\gen_modkit_registry.py
if errorlevel 1 exit /b 1
where cl >nul 2>nul
if errorlevel 1 (
  echo ERROR: abre x64 Native Tools Command Prompt.
  exit /b 1
)
rem IMPORTANT: many modules are called modkit.c; each needs a DISTINCT /Fo.
set "OBJ_FILES="
set /a NUM=0
for %%F in (
  src\app\win32_main.c
  src\bones\core.c
  src\bones\world.c
  src\bones\physics.c
  src\bones\scene.c
  src\bones\subsystems.c
  src\foundation\foundation.c
  src\modkit\modkit.c
  src\generated\modkit_registry.c
  src\demo\test_house.c
  src\adapters\eye_adapter.c
  third_party\eye\sw_renderer.c
) do (
  set /a NUM+=1
  cl /nologo /std:c11 /TC /O2 /GL /MT /W4 /DBONES_MODKIT_NO_DYNLIB /D_CRT_SECURE_NO_WARNINGS ^
    /c "%%F" /Fo"build\obj_modkit\unit!NUM!.obj"
  if errorlevel 1 goto :build_error
  set OBJ_FILES=!OBJ_FILES! "build\obj_modkit\unit!NUM!.obj"
)
for /R modules %%F in (modkit.c) do (
  set /a NUM+=1
  cl /nologo /std:c11 /TC /O2 /GL /MT /W4 /DBONES_MODKIT_NO_DYNLIB /D_CRT_SECURE_NO_WARNINGS ^
    /c "%%F" /Fo"build\obj_modkit\unit!NUM!.obj"
  if errorlevel 1 goto :build_error
  set OBJ_FILES=!OBJ_FILES! "build\obj_modkit\unit!NUM!.obj"
)
link /nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG ^
  !OBJ_FILES! user32.lib gdi32.lib kernel32.lib ws2_32.lib ^
  /OUT:build\Proyecto144_Bones_ModKit.exe
if errorlevel 1 goto :build_error
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verificar_tamano.ps1 build\Proyecto144_Bones_ModKit.exe
exit /b %errorlevel%
:build_error
echo ERROR: no se pudo compilar Bones ModKit.
exit /b 1
