@echo off
setlocal
rem Run from the project root in Visual Studio x64 Native Tools Command Prompt.
if not exist build mkdir build
where cl >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe no detectado. Abrir Developer Command Prompt.
    exit /b 1
)
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\eye.exe tools\eye_cli.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye.exe tests\test_eye.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye2.exe tests\test_eye2.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c
if errorlevel 1 exit /b 1
build\test_eye.exe
if errorlevel 1 exit /b 1
build\test_eye2.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Itests\mock_bones /Febuild\test_bones_bridge.exe tests\test_bones_bridge.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c
if errorlevel 1 exit /b 1
build\test_bones_bridge.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye_tone.exe tests\test_eye_tone.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c
if errorlevel 1 exit /b 1
build\test_eye_tone.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye3.exe tests\test_eye3.c src\eye.c src\eye2.c src\eye3.c src\eye_bones_bridge.c src\eye_tone.c
if errorlevel 1 exit /b 1
build\test_eye3.exe
if errorlevel 1 exit /b 1
rem EYE4 standalone decoder and new tests (Windows validation required on target).
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\eye4.exe tools\eye4_cli.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye4.exe tests\test_eye4.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c
if errorlevel 1 exit /b 1
build\test_eye4.exe
if errorlevel 1 exit /b 1
rem Eye 0.5: compilacion y test local Windows; NO prueba el juego completo.
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Febuild\test_eye5.exe tests\test_eye5.c src\eye.c src\eye2.c src\eye3.c src\eye_tone.c src\eye4.c src\eye5.c
if errorlevel 1 exit /b 1
build\test_eye5.exe
if errorlevel 1 exit /b 1
rem For the FINAL PE, run: python scripts\verificar_floppy.py path\to\game.exe
exit /b 0
