# BONES 0.4.3-D — E, objetivos físicos y puerta autoritativa

Base: PR C commit `82bdc512b4954111284d4aa27ce307e9d0901931`, apilada sobre B y A; pendiente de revisión de Soul.

## Contrato real
- Al comprobar `main`, los archivos `src/bones/interaction_core.[ch]` no estaban disponibles aunque existía trabajo anterior en PR #21. Esta entrega los incorpora sin modificar Bones API 2 / ModKit ABI 1.
- `BonesInteractionRegistry` contiene solo candidatos interactuables explícitos; en la demo se registra la puerta con ID estable calculado a partir de la semilla y caja física vinculada a su collider.
- `bones_interaction_select` realiza raycast desde el centro de cámara, alcance máximo 3.0 metros y oclusión por colisionadores sólidos.
- El host llama a `bones_skin_ui_target` para mostrar el aviso **E — INTERACTUAR** en Skin si hay objetivo real; al pulsar E, Skin retorna `SKIN2_INTERACT_REQUEST`.
- Bones **revalida** cámara/distancia/obstáculos e ID de la puerta al recibir la intención, luego retira mediante `bones_house_open_door` su collider. El módulo de escena de la casa presenta la puerta solo si su collider continúa presente, de modo que hay paso real y desaparece la hoja. Bones devuelve control a Skin sin que Skin ejecute gameplay.
- En cliente LAN de Mouth no se permite abrir la puerta, porque M144/1 carece de un mensaje autenticado de interacción y no replica cambios de puertas. Solo el host o modo individual pueden actuar.
- No se inventan respuestas Brain/diálogos, comercio, inventario ni animación de puerta. No hay puertas de mundo procedural ni guardado persistente.

## Pruebas
- `make -C BONES test` incluye `test_interaction_core`: distancia, yaw, pitch, ID, oclusión, puerta abierta.
- `make -C BONES host_interaction_test` enlaza Skin y Eye reales: comunica objetivo, recibe E, abre collider tras revalidación, quita prompt y reconstruye escena Eye.
- `BONES/scripts/test_host_interaction_msvc.bat`: repite integración real bajo MSVC. Scripts de distribución MSVC, MinGW y ModKit dev compilan nueva fuente interna.
- Prueba visual en Windows sigue pendiente: acercarse a la puerta por el frente con la cámara centrada, ver E, pulsar E, cruzar la entrada y comprobar que ya no se dibuja la puerta.

## Riesgos / seguimiento
- Las puertas no se replican a clientes de LAN, y el estado no tiene todavía guardado/fusión de Skin. No atribuir estas funciones a D.
- Si Soul aprueba ampliar ModKit, la exposición del target deberá mantenerse limitada a servicios versionados, sin entregar privilegios indiscriminados.
- Ninguna fuente de otros especialistas modificada. Revisión de Soul, Skin, Eye y Mouth antes de merge.
