@echo off
REM Run in x64 Native Tools Command Prompt for Visual Studio.
if not exist build mkdir build
cl /nologo /std:c11 /W4 /O2 /MT /I include ^
  tools\ear_tool.c src\ear_music.c src\ear_synth.c src\ear_bones_bridge.c ^
  /Fe:build\ear_tool.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /MT /I include ^
  tests\test_ear.c src\ear_music.c src\ear_synth.c src\ear_bones_bridge.c ^
  /Fe:build\test_ear.exe
if errorlevel 1 exit /b 1
build\test_ear.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /MT /I include ^
  tools\ear_play_win.c src\ear_music.c src\ear_synth.c ^
  src\ear_bones_bridge.c src\ear_winmm.c ^
  /Fe:build\ear_play_win.exe /link winmm.lib
if errorlevel 1 exit /b 1
REM Ear acoustic-material standalone tool and test (not the game's EXE).
cl /nologo /std:c11 /W4 /O2 /MT /I include /I contract ^
  tools\ear_surface_tool.c src\ear_synth.c src\ear_music.c ^
  src\ear_bones_bridge.c src\ear_surface.c ^
  src\ear_bones_contacts.c src\surface_materials.c ^
  /Fe:build\ear_surface_tool.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /MT /I include /I contract ^
  tests\test_surface.c src\ear_synth.c src\ear_music.c ^
  src\ear_bones_bridge.c src\ear_surface.c ^
  src\ear_bones_contacts.c src\surface_materials.c ^
  /Fe:build\test_surface.exe
if errorlevel 1 exit /b 1
build\test_surface.exe
if errorlevel 1 exit /b 1
@echo on
REM Validate audio via build\ear_play_win.exe on a real Windows computer.
REM Do NOT mistake the size of test tools for the eventual full RPG EXE.
