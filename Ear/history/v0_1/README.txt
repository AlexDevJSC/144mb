EAR — MOTOR DE AUDIO PROCEDURAL — v0.1.0
Responsable: Ear. Fecha: 20/10/2026.
Referencia: docs/PLAN_MAESTRO_REFERENCIA_v1.md, §7.5, §13 y §14.

MISION
  Sonidos de estilo fantasía medieval retro generados con C11, sin WAV/MP3
  obligatorios en el ejecutable. Música de fondo por semilla + estilo,
  estructura de 16 compases A-A'-B-A, bajo, armonía, melodía y ritmo opcional.
  SFX por preset o receta codificada (forma de onda, frecuencias, duración).
  Mix mono 16-bit PCM a 22.050 Hz. Salida Windows mediante waveOut/winmm.

COMO PROBAR EN LINUX
  make all && make test
  ./build/ear_tool music tavern 144 examples/tavern.wav
  ./build/ear_tool music dungeon 144 examples/dungeon.wav 12
  ./build/ear_tool sfx sword examples/sword.wav 34
  ./build/ear_tool recipe 'noise:1800:100:300:95:42' examples/recipe.wav
  La herramienta offline genera WAV para escuchar/comprobar, NO va en el EXE.

COMO COMPILAR EN WINDOWS
  Ejecutar scripts_build_windows.bat desde x64 Native Tools Command Prompt.
  Si se añade a Bones, compilar src/ear_winmm.c + núcleo y enlazar winmm.lib.
  La integración de módulo opcional está en integration/ y requiere las
  cabeceras REALES de Bones para compilar: no están en este paquete.

API PRINCIPAL
  ear_compose(EarScore*, EarMusicConfig) -> forma y notas deterministas
  ear_init(EarEngine*, const EarScore*) -> inicializa mezclador sin heap
  ear_render(EarEngine*, int16_t* pcm, size_t frames) -> produce PCM
  ear_trigger(EarEngine*, EarSfx, uint32_t seed) -> efecto de catálogo
  ear_trigger_recipe(EarEngine*, EarSfxRecipe) -> efecto desde código
  ear_set_volumes(EarEngine*, float music, float sfx)
  ear_score_hash(const EarScore*) -> hash canónico de notas, no audio universal

TIPOS MUSICALES
  dungeon:      menor, drones de horn, melodía de flauta, bajo lento
  tavern:       mayor, pulso de laúd, línea melódica, percusión leve
  wilderness:   dórico, flauta, acordes suaves y bajo
  combat:       menor, horn, bajo pulsante, percusión

SFX DE SERIE
  step, sword, hit, pickup, magic, ui, door
  No se usan muestras externas. Son efectos estilizados, no Foley realista.

LIMITACIONES REALES
  - Algoritmo elemental; no equivale a una sinfonía humana orquestal.
  - Sin envolventes largas por capas, mezcla estéreo, reverb, espacialización,
    mezcla por buses ni sistema interactivo de transiciones todavía.
  - WinMM escrito pero NO probado en Windows con hardware.
  - Bones: revisión de sus contratos v0.1 realizada, su código fuente NO está
    disponible; adaptar con sus cabeceras para afirmar compatibilidad real.
  - Tests Linux C11 y pruebas de PCM WAV, todavía no mediciones de EXE Windows.

ARCHIVOS DE DOCUMENTACION
  docs/OBJETIVOS_INSTRUCCIONES.txt
  docs/ESTADO_REAL.txt
  docs/REGISTRO_CAMBIOS.txt
  docs/DECISIONES_TECNICAS.txt
  integration/INTEGRACION_BONES_EYE.txt
