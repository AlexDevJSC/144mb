PROYECTO 1.44 — EYE 0.5.1 — CUBIERTAS POR RECETAS
=================================================
Responsable: Eye (programación del sistema gráfico).
El motor Bones es OTRO trabajo de desarrollo. Eye5 no altera Bones.

QUÉ HAY
- Fuentes EYE1/2/3/4 mantenidas sin modificar sus algoritmos.
- Eye5: 4 materiales de cubierta x 12 bytes cada uno; pack de 60 bytes.
- 2 generadores de tejado: a dos aguas (9 quads) y a cuatro aguas (13
  superficies, 2 triangulares, remates y aleros completos). Son mallas VISUALES, sin colisiones.
- Texturas 64x64 generadas en RAM del usuario a partir de recetas C.
- LRU con memoria proporcionada por el llamante, descarte y regeneración.
- CLI que transforma pack binario en header C para incrustarlo en EXE.
- Vistas previas C sobre un visualizador de desarrollo con Z-buffer.

COMANDOS LINUX
  make test                Todas las pruebas originales + EYE5
  make eye5                Genera previews de tejados
  make eye5integration     Prueba receta embebida y regeneración RAM
  make eye5size            Sondas ELF Linux de tamaño relativo

ARCHIVOS ESENCIALES
  include/eye5.h, src/eye5.c
  tools/eye5_preview.c, tools/eye5_cli.c
  tests/test_eye5.c, tests/test_eye5_embed.c
  assets/recetas_cubiertas.eye5
  docs/FORMATO_EYE5.txt
  docs/RFC_BONES_EYE5_INTEGRACION.txt
  docs/ESTADO_EYE.txt
  docs/MEDICION_EYE5.txt
  docs/REGISTRO_CAMBIOS_EYE.txt

LIMITACIONES
- La maqueta renderizada NO es un juego ni una vivienda transitable.
- Bones 0.2.1 mantiene 3 materiales efectivos; >3 no integrado todavía.
- No se ha compilado contra las fuentes auténticas de Bones 0.2.1.
- La orientación UV principal de las cuatro aguas está corregida, pero
  faltan evaluación artística y pruebas de integración real en Bones.
- No se ha medido similitud individual >=84% con referencias originales.
- Tamaño PE completo + copia y ejecución FAT12 en Windows sin validar.

Criterios de versiones/fuentes y alcance en docs/ESTADO_EYE.txt.

NOTA 0.5.1: comparar build/previews/Eye5_1_correcion_antes_despues.png.
