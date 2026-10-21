# Eye — Gráficos y materiales del Proyecto 1.44

**Programador responsable:** Eye. Este directorio contiene el proyecto fuente **Eye 0.5.1** completo, extraído de `Eye_0_5_1_Tejados_Corregidos.zip`, junto con sus demostraciones visuales.

## Contenido

- `include/`: cabeceras para los generadores EYE1, EYE2, EYE3, EYE4 y EYE5, tonos y adaptador.
- `src/`: código C11 original de los generadores, texturas, recetas, caché y constructor gráfico de cubiertas.
- `assets/`: recetas compactas y ejemplos de materiales (incluida la biblioteca binaria EYE5).
- `tools/`: programas de línea de comandos, generadores de previsualizaciones y comparación de similitud.
- `tests/`: pruebas deterministas y simulaciones del contrato gráfico de Bones; **no** es una compilación conjunta con el Bones real.
- `examples/`: ejemplo de integración heredada con Bones.
- `scripts/`: comprobaciones de tamaño y scripts de compilación.
- `docs/`: formatos versionados, decisiones, resultados, cambios y peticiones a Bones/Soul.
- `build/previews/`: las 17 imágenes de demostración originales y copia de la receta EYE5.

## Compilación y pruebas

El proyecto se compila desde esta misma carpeta:

```sh
make test
```

Verificado en Linux/GCC C11 al publicar: **9 pruebas correctas**.

Las imágenes en `build/previews/` son recursos de demostración y no se deben incrustar en el ejecutable final de 1,44 MB. Las recetas y el constructor están pensados para reconstruir las texturas en RAM, descartarlas y regenerarlas bajo demanda.

## Limitaciones pendientes

1. Aún no está compilado con las fuentes reales y completas de Bones 0.2.1: el puente incluido usa un mock en ciertas pruebas.
2. Bones limita su escena actual a tres materiales efectivos; su ampliación requiere acuerdo técnico y revisión de Soul.
3. Falta comprobar la similitud artística de al menos el 84 % por textura con referencias homologables de Daggerfall.
4. No está verificado que el ejecutable final Windows completo quepa y arranque desde un disquete FAT12 real.
5. La maqueta de tejados no constituye por sí misma un edificio colisionable y caminable.

No se han modificado otros directorios del repositorio.

Ver `ESTADO_SUBIDA.md` para el resultado de la subida y comprobación de integridad.
