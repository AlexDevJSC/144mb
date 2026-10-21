@echo off
setlocal
rem MinGW-w64 GCC must be available in PATH.
if not exist build mkdir build
where gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: MinGW-w64 GCC no detectado.
    exit /b 1
)
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tools\eye_cli.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c -o build\eye.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c -o build\test_eye.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye2.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c -o build\test_eye2.exe
if errorlevel 1 exit /b 1
build\test_eye.exe
if errorlevel 1 exit /b 1
build\test_eye2.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude -Itests\mock_bones tests\test_bones_bridge.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c -o build\test_bones_bridge.exe
if errorlevel 1 exit /b 1
build\test_bones_bridge.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye_tone.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c -o build\test_eye_tone.exe
if errorlevel 1 exit /b 1
build\test_eye_tone.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye3.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c -o build\test_eye3.exe
if errorlevel 1 exit /b 1
build\test_eye3.exe
if errorlevel 1 exit /b 1
rem EYE4 standalone decoder and new tests (Windows validation required on target).
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tools\eye4_cli.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c -o build\eye4.exe
if errorlevel 1 exit /b 1
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye4.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c -o build\test_eye4.exe
if errorlevel 1 exit /b 1
build\test_eye4.exe
if errorlevel 1 exit /b 1
rem Eye 0.5: compilacion y test local Windows; NO prueba el juego completo.
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude tests\test_eye5.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c src\eye5.c -o build\test_eye5.exe
if errorlevel 1 exit /b 1
build\test_eye5.exe
if errorlevel 1 exit /b 1
rem For the FINAL PE, run: python scripts\verificar_floppy.py path\to\game.exe
exit /b 0
