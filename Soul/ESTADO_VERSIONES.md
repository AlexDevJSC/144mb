# Estado de versiones — Proyecto 1.44

**Auditoría Soul:** 24/10/2026 (Europe/Madrid). **Rama examinada:** `main`. **HEAD verificado:** `63e294b403f41cc43f667059f1fa417741ff100c` (último commit: 22/10/2026 05:51:49 UTC). No hay nuevos commits en `main` ni nuevas actualizaciones de las PR #28–#31 respecto a la revisión del 23/10.

## Versiones: declaración, código y publicación

| Responsable | Código publicado en `main` | Versión interna o evidencia leída | Límite / diferencia |
|---|---|---|---|
| BONES | Base ModKit 0.4.0 con correcciones 0.4.1 de PR #11 | `BONES/docs/CHANGELOG_BONES_0_4_1.md`, `BONES/src/app/win32_main.c`; API Bones 2, ModKit ABI 1 | `BONES/README.md` aún titula 0.4.0. El selector 0.4.2 de PR #21 se fusionó en rama distinta de `main`; 0.4.3 A–D está en PR abiertas, NO en `main`. |
| EYE | Eye 0.5.2 de PR #12 | `BONES/modules/eye/modkit.c`: `implementation_version=502u` | `Eye/README.md` todavía declara 0.5.1; la escena ModKit continúa limitada a materiales 0–2. |
| BRAIN | Brain 0.3.1, PR #9 | `BONES/modules/brain/modkit.c`: `implementation_version=4` | `Brain/README.md` todavía declara 0.2.0. Prueba NPC contra mundo corregido aún pendiente de aceptación humana. |
| EAR | Ear 0.4.0, PR #13 | `BONES/modules/ear/modkit.c`: `EAR_MODKIT_VERSION 4u` | `Ear/README.md` comienza con 0.2.0 histórico. CI no demuestra audibilidad en hardware real. |
| SKIN | Skin fase 1 + UI LAN, PR #6 | `BONES/modules/skin/skin_modkit_impl.c`: `implementation_version=5u`; `modkit.c` incluye este fichero | `Skin/v0_3/README.md` todavía declara implementación 4. En `main` no hay integración de menús en el host Win32. |
| MOUTH | Mouth 0.4.0 con protocolo M144/1 | `BONES/modules/mouth/modkit.c`: `implementation_version=4u`; `Mouth/README.md` describe Session API v1 | Código real publicado; loopback/CI no equivalen a LAN en dos equipos físicos ni a juego gráfico validado. |

Los valores `implementation_version` son identificadores del descriptor ModKit y **no** números de versión semántica. La presencia de fuentes y el registro de un módulo no certifican que el ejecutable exponga todas sus funciones.

## Estado de integración

`main` todavía usa el host `BONES/src/app/win32_main.c` sin llamadas a `bones_skin_ui_input` ni a `bones_skin_ui_compose`; Esc libera la captura del ratón y no abre un menú. La integración funcional propuesta se encuentra en cuatro PR **abiertas y encadenadas**, no fusionadas:

| Fase | PR | HEAD inspeccionado | CI Windows (según registros revisados 23/10) | Tamaño EXE registrado |
|---|---|---|---|---|
| A — Eye + Skin | [#28](https://github.com/AlexDevJSC/144mb/pull/28) | `0a5e5d3a1018` | [39782471673](https://github.com/AlexDevJSC/144mb/actions/runs/39782471673) | 261.120 B |
| B — Brain + Ear | [#29](https://github.com/AlexDevJSC/144mb/pull/29) | `51862a63d6ad` | [39783333785](https://github.com/AlexDevJSC/144mb/actions/runs/39783333785) | 261.632 B |
| C — Mouth LAN | [#30](https://github.com/AlexDevJSC/144mb/pull/30) | `82bdc512b495` | [39784847994](https://github.com/AlexDevJSC/144mb/actions/runs/39784847994) | 270.336 B |
| D — interacción E | [#31](https://github.com/AlexDevJSC/144mb/pull/31) | `82c8a3a0659c` | [39786058595](https://github.com/AlexDevJSC/144mb/actions/runs/39786058595) | 273.920 B |

Las cuatro PR tienen `state=open` y `merged_at=null` a 24/10. El árbol `main` no contiene `BONES/src/bones/interaction_core.c`; el archivo **sí** se ha leído en el HEAD de #31. Los registros de CI muestran compilación satisfactoria, pero aquí no se han descargado ni ejecutado sus ejecutables Windows. Los cuatro workflows reutilizan el nombre de artefacto `bones-integration-a-windows`: nombre de ZIP no equivale a versión de fuente ni prueba ejecutada.

**Nuevo hallazgo de inspección 24/10:** en la rama #31, `BONES/modules/skin/skin_ui2.c` aún responde `AUDIO: PENDIENTE EAR` en Opciones y `BONES/src/app/win32_main.c` no despacha controles de volumen/música. Ear sí publica `ear_audio_enable_music`, `ear_audio_set_music_volume` y `ear_audio_set_sfx_volume`. Por tanto, la fase B integra el motor de audio, **no las opciones de audio operativas desde Skin**. Véase S-014.

## Compilación y límites de prueba

[GitHub Actions — instalador de main, run 39730456641](https://github.com/AlexDevJSC/144mb/actions/runs/39730456641): estado `success` en SHA `63e294b4`; la auditoría del 23/10 registró EXE de **247.296 B** y prueba automatizada de instalación/desinstalación. Esto no acredita HUD visible, audio audible, interacción física en `main`, LAN en dos PCs ni RPG terminado. Máximo de referencia: 1.474.560 B. No se ha repetido la compilación local en esta auditoría.

## Procedencia de entregas externas

Se buscó en los archivos accesibles de ChatGPT y en el contexto recuperable de otras conversaciones. Se localizó la petición de integración a Bones y documentos históricos de los especialistas, pero **no** una nueva entrega binaria reciente con bytes inspeccionables. Los nombres de ZIP históricos, resúmenes de otros chats y documentación de handoff no son prueba de que el ZIP haya sido abierto, de que los fuentes coincidan byte a byte ni de que se haya ejecutado una compilación. No existe acceso íntegro garantizado a las conversaciones especializadas.

El archivo de discrepancias conserva identificadores S-001 a S-013 y añade S-014 por la nueva verificación de código: [DISCREPANCIAS.md](DISCREPANCIAS.md). No se ha modificado código de los especialistas.
