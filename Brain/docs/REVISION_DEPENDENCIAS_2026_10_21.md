# Brain — Dependencias físicas, interacción E y referencias Eye

Fecha: 21/10/2026. PR Brain #9 (draft), issue #10.

## 1. Estado comprobado en GitHub

- PR Bones #11 `bones/fix-playtest-phase1-20261021`, SHA `7444855afbd6274c4eaaf5e0661a2314e68d2182`: código funcional publicado, `test_playtest.c` y `bones_house_register_colliders` disponibles. Informe de CI Windows PASS, PE 243.200 bytes en comentario de Bones; esto **NO** prueba Brain sobre la revisión corregida.
- PR #11 sigue abierta y no fusionada en la inspección; sin revisión formal registrada de Soul. **No ejecutar ni certificar la batería Brain real sin permiso de Soul.**
- PR #21 `bones/interaction-phase2-experimental`, SHA `2d4b42e52b977ca3df919fd32619dafa6f406298`: selector físico *interno*, sin `BonesModServices::on_interaction`, ni teclado E ni protocolo de respuesta semántica. No introducir API de interacción en ModKit ABI1 por nuestra cuenta.
- PR Brain #9: las pruebas preexistentes son host ficticio, mock de pared, biblioteca independiente y un test Foundation con collider sintético no ejecutado. **Ninguno demuestra colisiones en casa real**.

## 2. Nuevo escenario REAL preparado (aún NO ejecutado)

`Brain/bridge/bones_0_4/tests/test_brain_house_fixture.c` usa `BonesFoundation`, `BonesModHost`, el módulo Brain real y `BONES/src/demo/test_house.c::bones_house_register_colliders / bones_house_open_door` de PR #11. Tras precalentar tick para orientación recta, crea NPC owner=0 frente a entrada Z=2 y actor remoto owner=2. Verifica:

- Casa real de 7 colisionadores; pared y puerta impiden interpenetración de cápsula en cada tick.
- Tras abrir la puerta con helper de Bones, desaparece su collider y se atraviesa el portal (sin simular el raycast de E).
- Órdenes Brain ABI1 se aceptan con `issuer=0`, `tick+1`, secuencia válida; inyección host de orden previa provoca rechazo contabilizado por Brain, sin atribuirlo erróneamente a choque.
- El jugador que camina, gira y salta no altera los comandos del NPC; actor controlado por otro owner tampoco recibe comandos Brain.
- Recuperación local sigue siendo solo heurística, sin pathfinding.

El script `run_house_after_soul_approval.sh` exige `BRAIN_SOUL_PHASE1_APPROVED=yes` y que el archivo de demo contenga `BONES_HOUSE_COLLIDERS`, de lo contrario sale con estado de bloqueo. No se ha ejecutado la prueba real y sus aserciones se ajustarán **solo según resultados reales**; no se certificarán de antemano.

La prueba Foundation anterior `test_brain_real_bones.c` usa collider creado desde la propia prueba y sigue siendo distinta del fixture de casa de Bones. Ambas deberán ejecutarse para diferenciar física básica y escena real.

## 3. Regresión local que sí se ha realizado

`cd Brain && make test && make modkit-test`: PASS en GCC Linux con fuente Brain 0.3.1; `test_brain`, `test_visual_refs`, `test_modkit_bridge`, `test_brain_playtest_mock`, `test_brain_interaction`. Resultado del MOCK: 100 aceptadas en abierto; 100 y 2 recuperaciones con pared simulada; 12 rechazadas/88 aceptadas en prueba de rechazos sin falsa recuperación. Es exactamente **un mock** de la pared y no reemplaza `BonesFoundation`.

No he ejecutado una compilación Windows MSVC conjunta Brain + Bones #11 ni he medido su nuevo PE.

## 4. Interacción con E — contrato mínimo propuesto, NO aprobado

Bones: valida `actor_id` estable de NPC, distancia/oclusión desde cámara y autoridad del host una sola vez por flanco de E; no basta un ID de sprite o collider. El selector interno de PR21 necesita un adaptador aprobado hacia el dueño semántico del NPC. Brain: `brain_npc_interaction_offer` produce intención semántica `BRAIN_NPC_REPLY_GREETING` para un NPC ya validado; no comprueba raycast, no emite eventos de juego persistentes ni confirma transacción. Skin: recibe ID de respuesta semántica y representa el texto o ventana. Un `on_event` volátil ModKit ABI1 no equivale a un contrato de interacción confirmada. La nueva interfaz necesita revisión de Bones/Soul; Brain no modifica `modkit.h`.

## 5. Revisión técnica Eye1 / Eye2 — realidad vs propuesta Brain

**Eye implementado y verificable en fuentes**:
- `Eye/include/eye.h`, `Eye/src/eye.c`: `eye_pack_write`, `eye_pack_read`, `eye_generate`, píxeles RGBA, formato EYE1, recetas de 12 bytes; los IDs usados son **índices locales de banco**, no identidades persistentes de entidad.
- `Eye/include/eye2.h`, `Eye/src/eye2.c`: `eye2_pack_write/read`, `eye2_generate` y `EyeTextureBank` con `eye_bank_open` / `eye_bank_render`. `bank.version` selecciona EYE1 o EYE2 explícitamente. EYE2 no debe reinterpretar silenciosamente recetas EYE1.
- Eye5 trae recetas de tejado y Eye v0.5.1 alimenta materiales 0..2 del Bones ModKit real; el límite de material de Bones no ha sido ampliado para usar IDs arbitrarios.

**Brain implementado independientemente**:
- `brain_visual_init/upsert/resolve/encode/decode` (BVR1): referencias versionadas (material, variante, versión) y overrides escasos semánticos; no guarda imágenes.
- Test de ida/vuelta BVR1 y rechazo de versión, pero sin integrar `BrainVisualRef` en `BonesModFrame`.

**Propuesto, NO implementado**:
- Identidad visual persistente oficial de superficie: `{sector, object_id, surface_id}` con clave de mundo/generación canónica en Bones. No existe en `BonesModQuad` (solo `material_id`), de modo que una superficie de casa todavía no puede enlazarse automáticamente con BVR1.
- Mapeo formal `eye_generator_version` ↔ formatos magic EYE1/EYE2/EYE4/EYE5, más `variant_id` persistente. No inferir que los valores de test 1001/1002 sean versiones oficiales. Eye1/2 no exponen una clave variante semántica de mundo como BVR1 propone.
- Protocolo de estados SCORCHED/BROKEN/PAINTED con causa canónica y ownership asignado. Ejemplo: puerta destruida tiene que ser un hecho físico de Bones; la apariencia de Eye debe derivarse de ese hecho, sin duplicar un estado autoritativo en Brain.
- Persistencia final de mundo/partidas o fusión PBW: no confundir códec BVR1 con formato SAVE aprobado.

**Test cruzado preparado**: `Brain/tests/test_eye1_eye2_boundary.c` y script que enlaza fuentes EYE1/EYE2/EYE3 reales (sin mocks Eye). Comprueba píxeles deterministas por versión, diferencia de algoritmos, ida/vuelta BVR1 y rechazo de versión incorrecta. No se ejecutó todavía con los archivos reales de Eye en este checkout local; por tanto sigue PENDIENTE.

## 6. Decisiones solicitadas a Bones/Eye/Soul

1. Soul: autorizar SHA exacto de Bones #11 a usar y si se puede probar la PR antes de merge; validar el criterio Windows posterior.
2. Bones: confirmar colisionadores de escena de PR #11 y contrato de datos de interacción E tras PR #21 (o devolver bloqueo explícito). Exponer target ID autoritativo, distancia y oclusión validados para Brain y Skin solo mediante contrato aprobado.
3. Eye: asignar identificadores oficiales de versión del generador, interpretación persistente de `material_id`/`variant_id`, y test golden real por versión. Especificar cómo reconstruir descriptor para una superficie estable, sin almacenar texturas en guardado.
4. Soul: revisar BVR1 como prototipo desacoplado; no elevarlo a formato SAVE ni alterar ABI sin aprobación.

No se han modificado Bone core, Eye, Skin, ni Mouth en esta entrega. La integración final de Brain queda BLOQUEADA por autorización Soul y prueba Windows combinada.
