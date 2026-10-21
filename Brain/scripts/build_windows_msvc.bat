@echo off
setlocal
REM Launch from x64 Native Tools Command Prompt for Visual Studio.
if not exist build mkdir build
where cl >nul 2>nul
if errorlevel 1 (
    echo Error: cl.exe no encontrado. Abre Developer Command Prompt.
    exit /b 1
)
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Fe:build\brain_demo.exe ^
 demo\brain_demo.c src\brain_clock.c src\brain_random.c ^
 src\brain_economy.c src\brain_npc.c src\brain_world.c src\brain_bones.c src\brain_visual.c
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Fe:build\test_brain.exe ^
 tests\test_brain.c src\brain_clock.c src\brain_random.c ^
 src\brain_economy.c src\brain_npc.c src\brain_world.c src\brain_bones.c src\brain_visual.c
if errorlevel 1 exit /b 1
build\test_brain.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /TC /O2 /W4 /Iinclude /Fe:build\test_visual_refs.exe ^
 tests\test_visual_refs.c src\brain_clock.c src\brain_random.c ^
 src\brain_economy.c src\brain_npc.c src\brain_world.c ^
 src\brain_bones.c src\brain_visual.c
if errorlevel 1 exit /b 1
build\test_visual_refs.exe
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$f = Get-Item 'build\brain_demo.exe'; Write-Host ('Demo Brain EXE: ' + $f.Length + ' bytes'); Write-Host 'NO representa tamano de Bones + Brain + juego completo.'"
exit /b %errorlevel%
