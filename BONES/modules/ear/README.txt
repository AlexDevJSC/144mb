EAR v0.3.0 / BONES ModKit v0.4.0
==================================
Instalado en BONES/modules/ear. Compilar desde BONES con:
  python3 scripts/gen_modkit_registry.py
  make modkit_demo
  make modkit_test
Windows: scripts\build_windows_msvc.bat (o mingw), sin tocar scripts/core.

ÚNICO entrypoint compilado: modkit.c. Incluye los .c auxiliares del
directorio ear_core y ear_output.c como una sola unidad por limitación de
los scripts del host; NO añadirlos por separado al enlace.

Descriptor bones_mod_ear_query, ABI1, rol EAR, versión funcional 3,
sin dependencias de Eye/Brain/Skin/Mouth y con permisos read actors/time.
start sintetiza música de mazmorra, on_event responde a JUMP/LAND,
after_tick bombea WinMM (de forma no bloqueante), stop limpia audio.
Windows usa winmm.dll del sistema cargada bajo demanda: sin archivos
WAV ni DLL de Ear obligatorios. Falla de audio no bloquea al juego.
Para crear una .so/.dll separada en modo de desarrollo compilar
modkit.c con EAR_MODKIT_DLL, que exporta bones_mod_query_v1.
El exe de distribución NO busca ni instala DLL automáticamente.

No existen categoría acústica/intensidad en BonesModEvent ABI1, por lo
que los efectos de salto/aterrizaje son genéricos y mono; el sintetizador
de materiales permanece disponible para una futura integración revisada
por Bones/Soul. El esquema privado es 0: no hay persistencia del score.
La compilación con host Bones completo y Windows está pendiente de QA.
