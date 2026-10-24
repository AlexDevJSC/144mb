# Discrepancias — Soul

**Última revisión:** 24/10/2026 · `main` SHA `1e794185ec9d5bf77c760adc662c4aa0a08a1d8f` (solo documentación Soul; base funcional auditada `63e294b403f41cc43f667059f1fa417741ff100c`). Se conservan identificadores S-001–S-013 y se añade S-014. Las PR #28–#31 continúan abiertas.

## S-001 — BONES 0.4.0: estados de sincronización contradictorios
- **Gravedad:** MEDIA · **Estado:** EN VERIFICACIÓN (sustituye incidencia de subida parcial 0.3).
- **Evidencia:** `BONES/README.md` indica sincronización v0.4; `BONES/ESTADO_SINCRONIZACION_0_4.md` aún afirma que faltan fuentes. El árbol incluye el código y scripts de ModKit.
- **Actualización 24/10:** `BONES/README.md` sigue encabezado 0.4.0, mientras `BONES/docs/CHANGELOG_BONES_0_4_1.md` registra correcciones de 0.4.1 ya incorporadas en `main`. El ZIP original no se ha contrastado byte a byte. **Estado:** ABIERTA, DOCUMENTACIÓN OBSOLETA.
- **Acción:** verificar todos los hashes/entradas del ZIP 0.4.0 contra `BONES/` y corregir estados por Bones.

## S-002 — EYE 0.5.2 publicada; integración de catálogo pendiente
- **Gravedad:** ALTA · **Estado:** PUBLICACIÓN 0.5.2 RESUELTA, INTEGRACIÓN DE MATERIALES ABIERTA.
- **Evidencia actualizada:** PR #12 fusionada; `BONES/modules/eye/modkit.c` contiene `implementation_version=502u` y base gráfica para Skin. `Eye/README.md` aún dice 0.5.1. El host sigue restringido a materiales 0–2; RFC #25/#26 no implementan el catálogo.
- **Acción:** verificar con fuentes reales Bones/Eye y aprobar o modificar contrato de materiales.

## S-003 — BRAIN, EAR, SKIN publicados pero aún aislados
- **Gravedad:** ALTA · **Estado:** PUBLICACIÓN DE FUENTES RESUELTA, INTEGRACIÓN FUNCIONAL ABIERTA.
- **Evidencia actualizada:** `main` incluye Brain 0.3.1 (`implementation_version=4`), Ear 0.4.0 (`EAR_MODKIT_VERSION 4u`) y Skin fase 1/UI LAN (`implementation_version=5u`). Ya no corresponde llamar placeholder a Brain. Falta validación de NPC físico, audio audible y menús/HUD en host Win32 de `main`.
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
- **Evidencia actualizada:** GitHub Actions run 39730456641 compiló en Windows el EXE de `main` (247.296 bytes registrados) y probó instalación/desinstalación; los módulos reales están en `BONES/modules`. No obstante, `main` no conecta Skin a teclado/render, y las PR #28–#31 con integración funcional siguen abiertas. No hay aceptación humana de juego completo.
- **Acción:** definir y aprobar una build de integración gradual y pruebas de funcionalidades reales. No llamar al binario actual «juego completo».

## S-008 — MOUTH 0.2 sin código publicado
- **Gravedad:** ALTA histórica · **Estado:** PUBLICACIÓN RESUELTA, QA LAN REAL PENDIENTE.
- **Evidencia actualizada:** `Mouth/` y `BONES/modules/mouth/` contienen Mouth 0.4.0 con Session API 1, `implementation_version=4`, pruebas Windows/Linux y loopback. La ausencia de código de S-008 está resuelta para 0.4.0; no se ha probado LAN entre dos PCs físicos.
- **Acción:** publicar fuentes, compilar módulo y verificar conexión P2P LAN con Bones autoritativo y pruebas de pérdida/repetición.

## S-009 — Documentación de versiones de especialistas desfasada
- **Gravedad:** MEDIA · **Estado:** ABIERTA (detectada 22/10; persiste 24/10).
- **Evidencia:** `Eye/README.md` dice 0.5.1 vs descriptor 502; `Brain/README.md` 0.2.0 vs Brain 0.3.1/descriptor 4; `Ear/README.md` abre con 0.2.0 vs módulo 0.4.0/descriptor 4; `Skin/v0_3/README.md` dice implementación 4 vs `skin_modkit_impl.c` versión 5. Son documentos históricos o encabezados sin actualizar, no prueba de ausencia de código.
- **Acción:** propietarios deben actualizar estado vigente sin borrar historia y referenciar el SHA efectivo.

## S-010 — PR BONES 0.4.2 fusionada fuera de main
- **Gravedad:** ALTA PARA ETIQUETADO · **Estado:** ABIERTA EN MAIN.
- **Evidencia:** PR [#21](https://github.com/AlexDevJSC/144mb/pull/21) se fusionó en `bones/fix-playtest-phase1-20261021`, no en `main`. `BONES/src/bones/interaction_core.c` no existe en el árbol funcional de `main`; sí está presente en la rama de PR #31.
- **Acción:** no anunciar 0.4.2 como publicada en `main`; revisar y fusionar en orden cuando corresponda.

## S-011 — Instalador PowerShell externo rechazaba Skin válido
- **Gravedad:** MEDIA · **Estado:** CORRECCIÓN ESTÁTICA, WINDOWS PENDIENTE.
- **Evidencia:** script `Instalar_Proyecto144_Actual_2026-10-22.ps1` de conversación buscaba `implementation_version=5` en `BONES/modules/skin/modkit.c`, que solo incluye `skin_modkit_impl.c`; este último declara `5u`. Existe script `_CORREGIDO.ps1` inspeccionado previamente, sin prueba de ejecución Windows. No confundir con instalador oficial Actions.
- **Acción:** ejecutar corregido en Windows y verificar manifiesto y ejecutable instalado.

## S-012 — BONES 0.4.3 A–D solo en PR encadenadas
- **Gravedad:** ALTA PARA PUBLICACIÓN · **Estado:** ABIERTA (detectada 23/10; sin cambio 24/10).
- **Evidencia:** [#28](https://github.com/AlexDevJSC/144mb/pull/28) Eye+Skin, [#29](https://github.com/AlexDevJSC/144mb/pull/29) Ear+Brain, [#30](https://github.com/AlexDevJSC/144mb/pull/30) Mouth LAN, [#31](https://github.com/AlexDevJSC/144mb/pull/31) interacción E. HEAD respectivos `0a5e5d3a`, `51862a63`, `82bdc512`, `82c8a3a0`; `state=open` y `merged_at=null` en todas al 24/10. Compilaciones Windows de las fases A–D constan como `success` en runs 39782471673, 39783333785, 39784847994, 39786058595; no se ejecutaron EXE gráficos ni se probó LAN físico/audio audible.
- **Acción:** revisión y merge A→B→C→D con pruebas de usuario, sin etiquetar `main` como 0.4.3 antes de fusionar.

## S-013 — Artefactos Windows de fases A–D con nombre ambiguo
- **Gravedad:** MEDIA DE TRAZABILIDAD · **Estado:** ABIERTA (detectada 23/10).
- **Evidencia:** workflow de las PR A–D reutiliza `bones-integration-a-windows` para binarios diferentes; logs anteriores registraron tamaños de 261.120, 261.632, 270.336 y 273.920 bytes. El nombre del ZIP no prueba qué ejecutable contiene ni cuál se ha probado.
- **Acción:** nombrar artefactos por fase y SHA, incluir manifiesto de commit y SHA-256 real. No confundir artefacto CI con instalador oficial de `main`.

## S-014 — NUEVA: opciones de audio Skin siguen sin conexión en la integración D
- **Gravedad:** MEDIA/ALTA DE ACEPTACIÓN FUNCIONAL · **Estado:** ABIERTA (detectada por lectura de fuente 24/10).
- **Evidencia:** en el HEAD `82c8a3a0659c7a1ffd51bdd91ed239186b462811` de [PR #31](https://github.com/AlexDevJSC/144mb/pull/31), `BONES/modules/skin/skin_ui2.c`, función `skin2_input`, responde literalmente `AUDIO: PENDIENTE EAR` al seleccionar Audio en Opciones. `BONES/src/app/win32_main.c` maneja las acciones de menú pero no conecta los controles `ear_audio_enable_music`, `ear_audio_set_music_volume` o `ear_audio_set_sfx_volume` de `BONES/modules/ear/ear_audio_control.h`. En esa misma rama `ear_audio_pump()` sí se invoca cada frame. **La integración del motor Ear no equivale a opciones de audio utilizables.**
- **Acción:** solicitar a Skin la UI/acciones de control y a Bones el despacho a Ear, sin duplicar código del especialista. Verificar funcionamiento con dispositivo real y sin él; registrar test y SHA antes de cerrar.

## Política
Las declaraciones de los propios desarrolladores no sustituyen las pruebas independientes. No se modifica código ajeno por decisión de Soul. Cerrar incidencias exige evidencia reproducible, versión/commit y criterio satisfecho.
