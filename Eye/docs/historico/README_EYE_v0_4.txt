PROYECTO 1.44 — EYE 0.4.0 — Generador de fachadas compactas
===========================================================
RESPONSABLE: Eye, programación visual. Construido sobre EYE3 real.
OBJETIVO: ejecutable final en disquete FAT12 1.44MB, texturas EN RAM.

REPRODUCIR (Linux / macOS con GCC/Clang y make)
  make test                  Pruebas antiguas y nuevas EYE4
  make eye4integration       3 previews + packs + test sin filesystem runtime
  make eye4materials        PPM de cada motivo en sus 3 variantes

VISTAS PREVIAS GENERADAS POR EL CODIGO C
  build/previews/Eye4_tres_sistemas.png
  build/previews/Eye4_materiales_individuales.png
  build/previews/eye4_procedural.png
  build/previews/eye4_estatico.png
  build/previews/eye4_hibrido.png

RECETAS COMPACTAS
  build/materiales_hibrido.eyep            8 recetas + cabecera = 140 bytes
  build/materiales_eye4_embebidos.h      array C que VA dentro del EXE
  build/eye4_cli build 2 pack.eyep        genera banco hibrido
  build/eye4_cli inspect pack.eyep        inspeccion de banco
  build/eye4_cli embed pack.eyep out.h    serializa como array C
  El fichero .eyep NO debe distribuirse separado en el objetivo A.

TESTS DE REGENERACION EN MEMORIA
  eye4_read(array_embebido,bytes,&bank)
  eye4_cache_init(&cache,&bank,arena,texels,slots)
  eye4_cache_get(&cache,id) --> const uint32_t * de 4096 píxeles RGBA.
  No guardar punteros si la caché puede expulsar el material; no thread-safe.
  No se realizan asignaciones heap ni E/S de archivos por el sintetizador.

REGLAS DE ALCANCE
  - Bones 0.2 API2/subsystem1, contrato conocido. Integracion REAL pendiente.
  - Una casa de prueba con triángulos NO equivale a geometría caminable.
  - Tres propuestas presentadas, híbrida preferida pero no validada como 84%.
  - Sin texturas extraídas de Daggerfall. Enlace a REFERENCIA_ARTISTICA.txt.
  - Tools de desarrollo y PNG no se meten en el binario del juego.
  - El juego COMPLETO debe caber en un fichero de FAT12 vacio, no solo
    limitar su EXE a 1.474.560 bytes brutos de disquete.
  - Scripts Windows son recetas; no se han verificado en Windows en este turno.

DATOS / DOCS: docs/ESTADO_EYE.txt, FORMATO_EYE4.txt,
 docs/MEDICION_EYE4.txt, docs/PETICION_BONES_EYE_0_4.txt,
 docs/REGISTRO_CAMBIOS_EYE.txt.

VALIDACION POR MATERIAL
  build/materiales_hibridos/*.png son solo CANDIDATOS exportados desde la
  textura reconstruida por C11 (no se distribuyen en el exe).
  Cuando existan referencias originales alineadas, utilizar:
  python tools/medir_similitud.py REFERENCIAS_ORIGINALES build/materiales_hibridos
  No puede darse por alcanzado el 84% hasta superar cada prueba.
