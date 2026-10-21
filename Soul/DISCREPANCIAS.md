# Discrepancias — Soul

**Última revisión:** 21/10/2026 · `main` árbol `8a3216adb0e6239130c6b88c47c70e0512b60659`.

## S-001 — BONES 0.4.0: estados de sincronización contradictorios
- **Gravedad:** MEDIA · **Estado:** EN VERIFICACIÓN (sustituye incidencia de subida parcial 0.3).
- **Evidencia:** `BONES/README.md` indica sincronización v0.4; `BONES/ESTADO_SINCRONIZACION_0_4.md` aún afirma que faltan fuentes. El árbol incluye el código y scripts de ModKit.
- **Acción:** verificar todos los hashes/entradas del ZIP 0.4.0 contra `BONES/`, ejecutar tests desde GitHub y actualizar/quitar la advertencia documental obsoleta si procede.

## S-002 — EYE 0.5.1 publicada; integración pendiente
- **Gravedad:** ALTA · **Estado:** PUBLICACIÓN RESUELTA, INTEGRACIÓN ABIERTA.
- **Evidencia:** `Eye/` contiene fuentes y tests EYE1–EYE5; `Eye/ESTADO_SUBIDA.md` declara integridad 102/102. Bones ModKit sigue usando renderizador heredado.
- **Acción:** verificar con fuentes reales Bones/Eye y aprobar o modificar contrato de materiales.

## S-003 — BRAIN, EAR, SKIN publicados pero aún aislados
- **Gravedad:** ALTA · **Estado:** INTEGRACIÓN ABIERTA.
- **Evidencia:** carpetas `Brain/`, `Ear/`, `Skin/v0_2/` con código. `BONES/modules/brain/modkit.c` es placeholder, y las guías de los tres especialistas indican falta de enlace con Bones real.
- **Acción:** implementar adaptadores por dueño, registrar pruebas conjuntas y compatibilidad de versiones.

## S-004 — Propiedad de guardados
- **Gravedad:** MEDIA · **Estado:** PENDIENTE DE ACLARACIÓN DIRECTOR.
- **Evidencia:** `Skin/README.md` niega que mantener `save.c` implique atribución de guardados; ciertas guías Bones proponen que Skin gestione almacenamiento. `Usos de los chats.txt` había dejado responsabilidad sin adjudicar.
- **Acción:** resolver responsabilidad en protocolo y contratos; no asumir propiedad por presencia de archivos.

## S-005 — ModKit ABI 1 experimental sin aceptación arquitectónica
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** `BONES/docs/MODKIT_V0_4_0_REVISION_SOUL.txt` requiere examinar layout ABI Windows, permisos, ciclo, persistencia de estado privado y carga DLL.
- **Acción:** no estabilizar contrato antes de pruebas C11/MSVC/MinGW e interoperabilidad.

## S-006 — Renderizador de Bones no soporta biblioteca Eye 0.5.1
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** `BONES/modules/eye/modkit.c` ignora `material_id>2`; contrato de Bones limita la escena a 3 materiales y 96 quads. Eye 0.5.1 proporciona múltiples materiales y tejados más complejos.
- **Acción:** definir IDs versionados, banco de texturas, UV/triángulos y pruebas que detecten descarte involuntario.

## S-007 — No existe ensamblado integral del RPG
- **Gravedad:** CRÍTICA PARA PRUEBA COMPLETA · **Estado:** ABIERTA.
- **Evidencia:** `BONES/scripts/build_windows_msvc.bat` enlaza núcleo, ModKit, cinco conectores y renderizador heredado, pero **no enlaza** `Eye/src/eye5.c`, `Brain/src`, `Ear/src` ni `Skin/v0_2/src`; no hay compilación principal del juego en raíz.
- **Acción:** definir y aprobar una build de integración gradual y pruebas de funcionalidades reales. No llamar al binario actual «juego completo».

## S-008 — MOUTH 0.2 sin código publicado
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** `Mouth/README.md` afirma que el código v0.1/v0.2 no está transferido; carpeta contiene README y script de importación.
- **Acción:** publicar fuentes, compilar módulo y verificar conexión P2P LAN con Bones autoritativo y pruebas de pérdida/repetición.

## Política
Las declaraciones de los propios desarrolladores no sustituyen las pruebas independientes. No se modifica código ajeno por decisión de Soul. Cerrar incidencias exige evidencia reproducible, versión/commit y criterio satisfecho.
