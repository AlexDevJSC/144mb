# Brain — IA y simulación del Proyecto 1.44

Código fuente completo de **Brain v0.2** (incluye los módulos y tests de v0.1). Brain proporciona rutinas de NPC, reloj lógico, economía, decisiones de IA y referencias lógicas visuales que Eye puede regenerar sin persistir píxeles.

## Compilación y pruebas

Desde esta carpeta: `make test`. En Windows, usar `scripts/build_windows_msvc.bat` en una consola de desarrollo MSVC.

La integración con Bones real y la validación gráfica con los generadores EYE1/EYE2 siguen pendientes. El adaptador de Bones de este paquete es un *stub* de prueba; no debe considerarse una integración completa.

El documento `docs/ESTADO_BRAIN.txt` distingue lo implementado de lo pendiente. Los contratos están en `spec/`. El changelog se mantiene en `docs/REGISTRO_CAMBIOS_BRAIN.txt`.

**Responsable:** Brain. **Versión:** 0.2.0. **Idioma:** C11.
