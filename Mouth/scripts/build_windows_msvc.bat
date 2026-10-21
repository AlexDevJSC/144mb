@echo off
setlocal
rem Ejecutar desde "x64 Native Tools Command Prompt" de Visual Studio.
pushd "%~dp0\..\.."
if not exist Mouth\build mkdir Mouth\build
where cl >nul 2>nul
if errorlevel 1 (
  echo ERROR: MSVC cl.exe no encontrado.
  popd
  exit /b 1
)
cl /nologo /std:c11 /TC /O2 /W4 /MT /DBONES_MODKIT_NO_DYNLIB ^
  /IBONES\include /IMouth\include ^
  Mouth\examples\lan_demo.c Mouth\src\mouth_lan.c ^
  BONES\src\modkit\modkit.c BONES\src\foundation\foundation.c ^
  BONES\src\bones\core.c BONES\src\bones\world.c ^
  BONES\src\bones\physics.c BONES\src\bones\scene.c ^
  BONES\src\bones\subsystems.c ^
  /link /SUBSYSTEM:CONSOLE /OUT:Mouth\build\mouth_lan_demo.exe ws2_32.lib user32.lib
if errorlevel 1 (
  popd
  exit /b 1
)
cl /nologo /std:c11 /TC /O2 /W4 /MT /IBONES\include ^
  Mouth\tests\test_codec.c Mouth\src\mouth_lan.c ^
  /link /OUT:Mouth\build\mouth_test_codec.exe ws2_32.lib
if errorlevel 1 (
  popd
  exit /b 1
)
Mouth\build\mouth_test_codec.exe
if errorlevel 1 (
  popd
  exit /b 1
)
for %%F in (Mouth\build\mouth_lan_demo.exe) do echo Mouth demo PE bytes: %%~zF
echo NOT a full-game 1.44 MB certification; test with two Windows computers.
popd
exit /b 0
