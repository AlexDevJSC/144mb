# Skin v0.3 — estado real / medidas

**Implementado:** adaptador C11 `bones_mod_skin_query` ABI1 con descriptor íntegro, permisos de consulta, ciclo de vida, lectura de tick / número de actores, recepción de eventos efímeros, navegación de UI2, menús, inventario de presentación, composición de píxeles en superficie externa sin profundidad.

**Compatible para enlace estático en Bones 0.4.0:** el `modkit.c` está ubicado en el puerto `BONES/modules/skin/` y reúne sus auxiliares en una sola unidad de compilación. El generador de registro de Bones ya sabe detectar este puerto sin modificaciones.

**Validado en el laboratorio local:** suite de test sobre los servicios ModKit ABI1 simulados, compilador GCC y Clang y ASan/UBSan en Linux. Se verificó exportación `bones_mod_query_v1` de biblioteca dinámica de desarrollo Linux usando `-DSKIN_MODKIT_DYNAMIC`.

**No validado aún:** compilación **completa** del núcleo Bones 0.4.0 desde el repositorio en este entorno, comportamiento en Windows, PE final y uso de los menús desde `win32_main.c` de Bones (aún no llama a Skin). Tampoco hay integración con una paleta definitiva de Eye.

**Medición aislada de Skin** (GCC/Linux `-Os -ffunction-sections -fdata-sections`, fuente única `BONES/modules/skin/modkit.c`):

- Sección `.text`: 6.625 bytes.
- Sección `.data`: 80 bytes.
- Sección `.bss`: 720 bytes.
- Total de secciones declaradas: 7.425 bytes (no es el tamaño del EXE).
- Archivo objeto: 19.096 bytes (incluye tablas, símbolos y metadatos de objeto; no equivale a bytes de distribución).
- Copia opcional de un framebuffer 320×200×4: **256.000 bytes en RAM**, propiedad de la aplicación.

Estas cantidades NO son el incremento final del PE de Windows ni demuestran cumplir el límite de 1,44 MB. Deben repetirse con MSVC y el enlazado real de Bones.

**Riesgo de compatibilidad:** la documentación histórica de Bones atribuye guardados a Skin; la asignación funcional confirmada posteriormente se limita a UI/UX. El código v0.3 no usa `snapshot_export` ni `snapshot_restore_at_start`. Coordinar con Soul cualquier corrección de documentación central sin reinterpretar silenciosamente la decisión del director.

## Actualización 2026-10-21 — Skin 0.4 fase 1

El estado histórico 0.3 más arriba se conserva por trazabilidad. La fase 1 añade menús y HUD sin datos RPG inventados, contratos de pausa/acciones, mensaje LAN experimental y contexto E validado por host. Pruebas `make test`, `make preview`, `make sanitize`: PASS aislado, cabecera local ModKit compatible. GCC `-Os` objeto text=8195 B, data=72 B, bss=792 B, no corresponde al incremento PE Windows. Fuente de verdad: `FASE1_MENUS_HUD_20261021.md`; faltan presentación Eye #3, sistema host de entrada y Soul. **No se afirma fase de playtest integrada**.
