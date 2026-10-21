EYE 0.5.1 — SISTEMA DE TEJADOS COMPACTOS (CORRECCION UV Y REMATES)
========================================
Código C11. No es motor Bones completo ni juego navegable.

OBJETIVO
- Arquitectura de tejados y cuatro materiales para vistas 3D inspiradas
  en RPG de los 90, sin texturas PNG de runtime, generadas en RAM.
- Un fichero .eye5 de 60 bytes contiene 4 recetas distintas.
- Se construye geometría visual de cubierta de 2/4 aguas por parámetros.
- Se conserva código y documentación histórica de EYE1-EYE4.

PRUEBAS REPRODUCIBLES EN LINUX
  make test
  make eye5
  make eye5integration
  make eye5size

VISTAS PREVIAS
  build/previews/Eye5_antes_despues.png
  build/previews/Eye5_cuatro_cubiertas.png
  build/previews/cubiertas_4_tipos.png
  build/previews/casa_cuatro_aguas.png  (mejorado; sigue sin validacion en Bones)
  build/previews/Eye5_1_correcion_antes_despues.png
Las vistas previas se generan a partir de C, pero su composicion PNG
es EXCLUSIVAMENTE herramienta de desarrollo; no se distribuyen con EXE.

CODIGO
  include/eye5.h     API para recetas y mallado visual
  src/eye5.c         generador, serialización, caché, constructor
  tools/eye5_preview.c  rasterizador ortográfico SOLO de desarrollo
  tools/eye5_cli.c      embeber archivo .eye5 como cabecera C
  tests/test_eye5.c       codec/semilla/mallado/caché
  tests/test_eye5_embed.c prueba de header embebido

LIMITACIONES
- Bones 0.2.1 soporta solo 3 materiales efectivos; la ampliación
  a catálogo de hasta 256 IDs requiere APROBACIÓN y cambios de Bones.
- No se ha enlazado con Bones real, solo con contratos documentales.
- Orientacion UV principal del tejado a cuatro aguas corregida y probada.
- Su aspecto aun necesita revision artistica y validacion en Bones real.
- Objetivo >=84%% de similitud a originales no comprobado aún.
- PE completo del juego 1.44MB y lectura de floppy sin verificar.

Ver docs/ESTADO_EYE.txt y docs/RFC_BONES_EYE5_INTEGRACION.txt.

REVISION 0.5.1
- Las filas de cubiertas siguen pendiente/cumbrera en lugar de longitudinal.
- Remates diagonales sobre cuatro limatesas y aleros frontal/posterior.
- Cumbrera comparte material de cubierta, no depende de madera.
- 13 superficies a cuatro aguas, 9 a dos aguas.
- Archivo EYE5 y recetas anteriores sin cambios: 60 bytes.
- Contrato Bones API2/subsystem1 NO modificado ni probado en enlace real.

Revisar docs/CORRECCION_EYE5_1.txt y docs/MEDICION_EYE5_1.txt.
