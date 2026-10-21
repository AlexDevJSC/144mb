PROYECTO 1.44 - SKIN v0.1.0 - MODULO DE ESTADO, GUARDADO Y UX
================================================================
20/10/2026

Skin NO sustituye a Bones (simulacion/fisicas) ni a Eye (render/arte).
Es un addon de C11 que observa las estructuras publicas de Bones v0.1,
registra cambios acotados y dibuja overlays en un framebuffer entregado
por el motor visual. Mantiene fuera del core el I/O de guardado.

LO QUE INCLUYE
  include/skin/skin.h    API publica Skin/Bones, observador, guardado, UI.
  src/skin.c            seguimiento de cambios y BonesModule on_tick.
  src/save.c            save v1 LE + CRC32 + doble slot A/B + carga validada.
  src/ui.c              fuente 5x7, layout, controles, tema reconfigurable.
  tests/fixtures/bones/bones.h  CONTRATO DE COMPILACION reconstruido.
  tests/mock_bones.c     DOBLE DE PRUEBA, NO motor fisico real.
  tests/test_skin.c      tests eventos, integridad, truncamiento y UX.
  examples/win32_skin_demo.c    UI independiente, con mock de Bones.
  scripts/build_windows_demo_*.bat     recetas Windows SIN verificar.
  docs/                  Especificacion, contratos, estado y cambios.
  build/skin_*.png       CAPTURAS AUTENTICAS producidas por Skin.

TESTS PORTABLES (LINUX)
  make test
  make sanitize
  make build/libskin.a
  make preview       (ejecuta tests y crea PPM para revision grafica)

EJECUTABLE DE PRUEBA WINDOWS (NO JUEGO, NO BONES REAL)
  scripts\build_windows_demo_mingw.bat
  o scripts\build_windows_demo_msvc.bat
  build\Skin144_TestUI.exe

CONTROLES DE LA DEMO
  Flechas / Enter: seleccionar elementos y ejecutar accion.
  Esc: desde juego, pausa; desde pausa, volver al juego.
  WASD / Espacio: mueven solo el mock de la demo, sin fisicas reales.
  Nueva partida: reinicia el mock; Continuar: busca un guardado valido.
  Guardar: escribe skin_demo.a/b; Cargar: ultimo valido de esas ranuras.
  Alt+F4: salir.

USAR CON EL BONES REAL
  No compilar tests/mock_bones.c en el juego. Usar -I al include REAL de
  Bones, incluir src/skin.c, src/save.c y src/ui.c; ver
  docs/CONTRATO_BONES_EYE.txt. Mantener el usuario de Skin estable en RAM
  durante los callbacks. Llamar skin_attach antes del primer tick. El core
  Bones es autoritativo. Eye aporta framebuffer y tematizacion visual.

REQUISITOS Y LIMITES
  - No existe EXE Windows compilado ni bytes reales certificados en esta sesion.
  - NO se ha integrado con codigo fuente original de Bones/Eye.
  - No se han proporcionado todavia assets finales de Eye: tema PROVISIONAL.
  - Solo se serializan player, collider estático activo, tick, semilla, ids,
    generacion, rama e historial limitado; no inventario, quests ni PBW/merge.
  - Los eventos registrados NO estan clasificados como fusionables. NO se
    implementa fusión de mundos ni prevención de objetos únicos duplicados.
  - Se requiere fallo explícito de historia truncada; no se oculta esa pérdida.
  - El backup de dos ranuras no equivale a guardado distribuido/seguro contra
    fallos de almacenamiento simultáneos, ataques o cambios de versión.

FUENTE DE VERDAD: docs/PLAN_MAESTRO_ORIGINAL_v1.md
