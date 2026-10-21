# Estado de subida — Eye 0.5.1

**Estado: COMPLETA** · 21 de octubre de 2026.

Origen: paquete `Eye_0_5_1_Tejados_Corregidos.zip` del proyecto.

## Resultado en GitHub

- **102/102 archivos de la entrega original** incorporados a `Eye/`.
- 85 archivos de fuentes, recetas, herramientas, pruebas y documentación (incluye la copia secundaria de recetas).
- 17 imágenes PNG de ejemplo incluidas tal como se generaron.
- Las **102 huellas Git SHA-1 (blob)** coinciden con los archivos del ZIP, sin faltantes ni diferencias.
- Los archivos de `BONES/` y los de otros desarrolladores no han sido modificados.
- `Eye/README.md` y este estado son documentación adicional propia de GitHub.

## Verificación funcional realizada

`make test` ejecutado en Linux/GCC C11 sobre la fuente extraída: **9 pruebas superadas** (EYE1–EYE5, tono, adaptador simulado, incrustación de recetas).

## Limitaciones conocidas

La integridad de la subida **no equivale** a la certificación de la integración: no hay compilación conjunta con el código completo Bones real, ni certificación del .exe Windows/FAT12 ni de la semejanza artística del 84 %.

No se publican los ejecutables de las herramientas ni la caché runtime. Se conserva el código fuente para reproducirlos mediante `make test`.
