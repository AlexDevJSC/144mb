# Proyecto 1.44 — Anexo técnico Skin v0.1

**Fecha:** 20 de octubre de 2026. **Especificación principal:** `PLAN_MAESTRO_ORIGINAL_v1.md`, conservada íntegra. Este anexo no altera las decisiones no cerradas del documento.

Skin implementa el inicio de las secciones **12 y 13** del plan maestro: observación de cambios confirmados, snapshot de prueba versionado, interfaz HUD y menús. Implementa una superficie para conectar a Bones v0.1 sin alterar su core, y para dibujar sobre el framebuffer que Eye suministre. Se verificó el funcionamiento con un **mock** que replica el contrato público de Bones v0.1; no se ha enlazado con las fuentes originales de Bones/Eye porque no se encuentran disponibles en los archivos de esta sesión.

El binario de guardado v1 contiene el estado mínimo de jugador, colisionadores, mundo y rama con CRC, en dos ranuras alternas. Puede recuperarse una ranura anterior íntegra. El registro de eventos está limitado a 128 entradas y no dispone de la semántica suficiente para ejecutar fusiones PBW, verificar inventarios ni conciliar eventos comerciales. Una marca explícita indica si se ha perdido parte del historial. Se mantiene pendiente todo lo referido a ancestro común, merge tres vías, protección de objetos únicos y conflictos de misión. Los eventos de movimiento son informativos, no operacionales/reproducibles.

La UI tiene menú principal, pausa, HUD de estado y avisos. Los colores, los marcos y el fondo constituyen elementos **provisionales** hasta recibir el arte final de Eye. La propuesta D3D11 sigue abierta; Skin dibuja un overlay que en la prueba se aplica a un framebuffer BGRA de 320 × 200 píxeles.

**Puerta de validación pendiente:** compilar el juego real en Windows, acoplar fuentes Bones y Eye verdaderas, comprobar coste de bytes y rendimiento, e introducir eventos semánticos del juego en la persistencia antes de reclamar interoperabilidad PBW.


## Actualización Skin UI2 (20/10/2026)

Respondiendo a la solicitud de Eye, Skin entrega un compositor **de interfaz**
C11 independiente del renderizador que mantiene la profundidad y la lógica del
mundo fuera de Skin. La API de `include/skin/ui2.h` funciona con una vista de
datos de presentación y un destino de color provisto por la aplicación.
Soporta marco reutilizable, menús, HUD y 20 casillas de inventario de prueba;
color alfa ARGB32, paleta provisional, coordenadas virtuales 320×200 y escalado
16:10. Compila y pasa tests en Linux, NO integra todavía los motores actuales
reales de Bones/Eye ni se ha medido como PE Windows.

La responsabilidad de Skin queda limitada a UI/UX según
`00_LEER_PRIMERO_Responsabilidades_Proyecto_144_v1.txt` (externo, director).
El código de guardado legacy v0.1 permanece archivado sin ampliar su
funcionalidad hasta que el director designe expresamente a su responsable.
El plan maestro original no se ha alterado.
