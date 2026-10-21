# BONES 0.4.0 — Proyecto 1.44

Código y documentación originales de la entrega `PROYECTO_144_Bones_ModKit_v0_4_0.zip`, sincronizados en `BONES/`.

## Contenido
- `include/bones/`: API central y ModKit experimental.
- `src/`: motor, fundación, integración de Eye y aplicaciones de prueba.
- `modules/`: puntos de inserción de Eye, Ear, Brain, Skin y Mouth.
- `tests/`, `scripts/`, `tools/`, `examples/`, `samples/`: pruebas, compilación y ejemplos.
- `docs/`: contratos, plan maestro y documentación histórica.

## Compilación
En Linux, `make test` y `make modkit_test` desde `BONES/`. La compilación Windows debe verificarse en una máquina compatible mediante los scripts incluidos.

## Compatibilidad
API de Bones 2; ModKit ABI 1 **experimental, pendiente de revisión de Soul**. La publicación final persigue un EXE Windows ≤1.474.560 bytes, **aún no certificado**. Los módulos de especialidad no están todos implementados: algunos son conectores de integración.

El archivo `docs/MODKIT_v0_4_SHA256.txt` documenta hashes de la distribución de referencia. Los archivos de código y documentación publicados aquí mantienen el contenido del ZIP original.
