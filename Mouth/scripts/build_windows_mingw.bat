@echo off
setlocal
rem Requiere GCC MinGW-w64 disponible en PATH.
pushd "%~dp0\..\.."
if not exist Mouth\build mkdir Mouth\build
where gcc >nul 2>nul
if errorlevel 1 (
  echo ERROR: MinGW-w64 GCC no encontrado.
  popd
  exit /b 1
)
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -DBONES_MODKIT_NO_DYNLIB ^
  -IBONES\include -IMouth\include ^
  Mouth\examples\lan_demo.c BONES\modules\mouth\modkit.c ^
  BONES\src\modkit\modkit.c BONES\src\foundation\foundation.c ^
  BONES\src\bones\core.c BONES\src\bones\world.c ^
  BONES\src\bones\physics.c BONES\src\bones\scene.c ^
  BONES\src\bones\subsystems.c -o Mouth\build\mouth_lan_demo.exe ^
  -lws2_32 -luser32 -lm
if errorlevel 1 (
  popd
  exit /b 1
)
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic ^
  -IBONES\include -IMouth\include ^
  Mouth\tests\test_codec.c Mouth\src\mouth_lan.c ^
  -o Mouth\build\mouth_test_codec.exe -lws2_32 -lm
if errorlevel 1 (
  popd
  exit /b 1
)
Mouth\build\mouth_test_codec.exe
if errorlevel 1 (
  popd
  exit /b 1
)
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic ^
  -IBONES\include -IMouth\include ^
  Mouth\tests\test_modkit_provider.c BONES\modules\mouth\modkit.c ^
  -o Mouth\build\mouth_test_provider.exe -lws2_32 -lm
if errorlevel 1 (popd & exit /b 1)
Mouth\build\mouth_test_provider.exe
if errorlevel 1 (popd & exit /b 1)
for %%F in (Mouth\build\mouth_lan_demo.exe) do echo Mouth demo PE bytes: %%~zF
echo NOT a full-game 1.44 MB certification; test with two Windows computers.
popd
exit /b 0
