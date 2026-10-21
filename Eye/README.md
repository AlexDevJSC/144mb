# Eye — Gráficos y texturas del Proyecto 1.44

**Responsable:** Eye. Este directorio está reservado al código gráfico: síntesis de texturas, recetas compactas, renderizado de muestras y construcción visual de cubiertas.

## Estado
La última entrega preparada es **Eye 0.5.1 (tejados corregidos)**. Incluye el código acumulado de Eye 0.1 a Eye 0.5.1. Los materiales se generan desde recetas y pueden reconstruirse en RAM sin conservar archivos raster en el ejecutable.

## Contenido
- `include/`: cabeceras de EYE1–EYE5 y puente de integración.
- `src/`: generadores, codec binario, tonos y constructor de tejados.
- `tools/`: CLI, generadores de previsualizaciones y evaluación de similitud.
- `tests/`: pruebas de regresión y simulación del contrato Bones.
- `assets/`: recetas de ejemplo; las imágenes finales no se distribuyen con el juego.
- `scripts/`: compilaciones de prueba y comprobación de tamaño.
- `docs/`: formatos, decisiones, mediciones, cambios y peticiones de integración.
- `examples/`: ejemplo de integración con Bones.
- `Makefile`: compilación C11.

**Importante:** un formato compacto de recetas no garantiza el tamaño del ejecutable final. Falta integrar y medir Eye con Bones en un ejecutable Windows real y probar el límite físico de un disquete de 1,44 MB. La semejanza artística del 84 % por textura sigue sin verificarse.

## Coordinación
Bones mantiene el núcleo y el contrato gráfico; Eye crea texturas y piezas visuales. La limitación de tres materiales en Bones debe resolverse por contrato aprobado y revisión de Soul. No se modifica el código ajeno desde esta carpeta.

**Fuente de la entrega:** `Eye_0_5_1_Tejados_Corregidos.zip`, producido previamente en el proyecto.