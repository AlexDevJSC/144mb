@echo off
setlocal EnableDelayedExpansion
rem Run from any folder; MSVC available in a VS Developer Command Prompt.
pushd "%~dp0\..\.."
if not exist Mouth\build mkdir Mouth\build
if not exist Mouth\build\obj mkdir Mouth\build\obj
where cl >nul 2>nul
if errorlevel 1 (
  echo ERROR: MSVC cl.exe no encontrado.
  popd
  exit /b 1
)
set "OBJECTS="
set /a N=0
for %%F in (
  Mouth\examples\lan_demo.c
  BONES\modules\mouth\modkit.c
  BONES\src\modkit\modkit.c
  BONES\src\foundation\foundation.c
  BONES\src\bones\core.c
  BONES\src\bones\world.c
  BONES\src\bones\physics.c
  BONES\src\bones\scene.c
  BONES\src\bones\subsystems.c
) do (
  set /a N+=1
  cl /nologo /std:c11 /TC /O2 /W4 /MT /DBONES_MODKIT_NO_DYNLIB /D_CRT_SECURE_NO_WARNINGS ^
      /IBONES\include /IMouth\include /c "%%F" /Fo"Mouth\build\obj\unit!N!.obj"
  if errorlevel 1 goto :build_error
  set OBJECTS=!OBJECTS! "Mouth\build\obj\unit!N!.obj"
)
link /nologo /SUBSYSTEM:CONSOLE !OBJECTS! ws2_32.lib user32.lib ^
  /OUT:Mouth\build\mouth_lan_demo.exe
if errorlevel 1 goto :build_error
cl /nologo /std:c11 /TC /O2 /W4 /MT /D_CRT_SECURE_NO_WARNINGS /IBONES\include ^
  Mouth\tests\test_codec.c Mouth\src\mouth_lan.c ^
  /link /OUT:Mouth\build\mouth_test_codec.exe ws2_32.lib
if errorlevel 1 goto :build_error
Mouth\build\mouth_test_codec.exe
if errorlevel 1 goto :build_error
for %%F in (Mouth\build\mouth_lan_demo.exe) do echo Mouth demo PE bytes: %%~zF
echo No se ha validado el limite de 1.44 MB del juego completo.
popd
exit /b 0
:build_error
popd
exit /b 1
