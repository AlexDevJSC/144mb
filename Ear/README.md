# Ear — motor de audio de Proyecto 1.44

Responsable: **Ear** · Lenguaje: C11 · Estado: prototipo v0.2.0.

Ear genera música procedural por semillas y efectos sintetizados (SFX), además de categorías acústicas para materiales y un adaptador propuesto para Bones.

## Entregas producidas

- **v0.1.0**: sintetizador, música estructurada, puente inicial Bones, capa WinMM, herramientas y pruebas.
- **v0.2.0**: añade contactos acústicos, categorías de piedra, madera, metal, tierra, vegetación y tela; nuevos tests, mediciones y contratos con Eye/Bones.
- El código de v0.1 que no cambió está incorporado en v0.2. Los ficheros reemplazados se preservan en el historial del paquete de fuentes.

## Límites de integración

La rama actual de Bones utiliza el contrato `BonesSubsystem` (API de subsistemas v1). El adaptador de Ear incluido en estas fuentes requiere comprobarse y ajustarse contra su encabezado real. No se afirma integración funcional ni tamaño PE Windows verificado.

## Fuentes, dependencias y documentación

La carpeta `Ear/` almacena los fuentes C, encabezados, ejemplos de integración, tests, scripts y documentación creada por Ear. Las demostraciones de audio WAV/OGG son **artefactos de desarrollo** y no forman parte del código fuente obligatorio para compilar el motor.

No alterar `BONES/` ni los contratos de otros subsistemas sin revisión y acuerdo de los responsables. El tamaño objetivo de todo el juego es **1.474.560 bytes**; no es el tamaño permitido para un módulo individual.

## Licencias

No se ha establecido todavía una licencia de redistribución. No se presume licencia open source por el mero hecho de publicar estos archivos en GitHub.
