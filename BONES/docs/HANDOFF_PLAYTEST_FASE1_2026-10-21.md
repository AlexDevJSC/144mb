# Bones 0.4.1 — Traspaso playtest fase 1
Responsable: Bones. Revisor: Soul. Origen: `Soul/PLAYTEST_BUGS_2026-10-21.md` y `Soul/COORDINACION_CORRECCIONES_PLAYTEST_2026-10-21.md`.

## Cambios
- BUG-001: la casa de prueba tiene siete colisionadores físicos (pared frontal dividida en dos jambas y dintel, paredes laterales y posterior, puerta cerrada). IDs por semilla y alta transaccional. Un helper de la demo retira la puerta física y su dibujo; **no hay interacción de gameplay ni puerta animada**.
- BUG-002: entrada relativa mediante WM_INPUT (Raw Input) en vez de GetCursorPos/SetCursorPos por tick. Deltas acumulados y consumidos una vez. Se limpian al perder foco o captura. Flechas conservadas.
- BUG-003: WASD probado en cuatro yaw y diagonal normalizada. No se invierten ejes sin pruebas.
- BUG-004: desde reposo, saltar no cambia X/Z ni yaw/pitch en pruebas nativas; con W sigue el desplazamiento en el aire. La desviación observada por el jugador sigue pendiente de reproducción Windows.
- BUG-005: composición en DIB de memoria + un BitBlt al DC visible, y WM_PAINT repinta el último frame completo; ya no se borra de negro el DC visible cada frame. Diagnóstico opcional del framebuffer Eye para aislar la causa si sigue ocurriendo.
  
## Reproducción Windows
En una consola de desarrollador MSVC x64, desde BONES:

```bat
scripts\build_windows_msvc.bat
set BONES_INPUT_TRACE=1
set BONES_FRAME_TRACE=1
build\Proyecto144_Bones_ModKit.exe
```

`BONES_playtest_input.csv` contiene 600 ticks como máximo: foco/captura, mouse_dx/dy, WASD, Space, XYZ, velocidades y yaw/pitch pre y post tick. `BONES_playtest_frames.csv` contiene hashes FNV64 y el número de píxeles negros de hasta 600 frames tal como Eye los proporciona antes de GDI. No se escriben diagnósticos salvo que se activen las variables.

## Pruebas y límites
PASS en Linux: GCC y Clang `make test`, GCC `make modkit_test` (incluida prueba .so), sanitizadores AddressSanitizer + UndefinedBehaviorSanitizer en `test_playtest`. **No se ha ejecutado todavía el ejecutable en Windows ni se ha medido el PE real en el entorno local Linux**. Se añade trabajo CI Windows a la PR, cuyo resultado debe verificarse antes del merge. El objetivo de 1.474.560 bytes NO está certificado aquí.

## Traspaso
**Eye:** comprobar yaw/pitch perceptivo y si los hashes del framebuffer tienen frames oscuros, además de la presentación tras redimensionar/Alt+Tab. **Brain:** validar colisiones de NPC en la casa con la rama integrada. **Soul:** revisar compatibilidad y cerrar o reabrir BUG-001..005 por separado; no declarar aceptación sin prueba Windows.

No se modificaron los headers públicos `bones.h` / `modkit.h` ni el código fuente de otros especialistas. Fases 2 (interacción) y 3 (Skin/Ear/Mouth) pendientes.
