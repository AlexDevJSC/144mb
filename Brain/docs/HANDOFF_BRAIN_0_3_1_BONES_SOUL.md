# Brain → Bones / Soul — Traspaso playtest v0.3.1

**Fecha:** 21/10/2026. **Responsable:** Brain. **Estado:** PR de preparación, **NO integración validada**.  
**Dependencia:** corrección funcional de movimientos y colisiones de Bones (BUG-001–004 de Soul). **No se da por satisfecha** en la `main` consultada, commit `f9fcb3550a9cd8d2dac2374af5cf9d4448f0a5fa`.

## Cambios exclusivos de Brain

- `BONES/modules/brain/modkit.c`: `BonesModServices` ABI1, decisiones `brain_npc_decide`, órdenes `issuer=0`, `tick+1`, `sequence=tick+1`, filtrado `kind=NPC/owner=0`, conservación de compatibilidad sin cambiar interfaces Bones.
- Detección del NPC parado durante **8 ticks con órdenes de avance aceptadas**. Activa 14 ticks de giro in situ (máximo 100 mrad/tick). No crea físicas propias ni garantiza que exista salida. Si la orden fue **rechazada**, reinicia detección y no presupone pared.
- `brain_mod_diagnostics.h`: instantánea de contadores de órdenes aceptadas, rechazadas, actor_at fallido, cantidad de actores inválida, propietarios ignorados, y recuperaciones. Solo para diagnóstico en el mismo hilo; no es la ABI de Bones.
- `Brain/src/brain_interaction.c`: intención semántica de saludo para un NPC suministrado por la capa de selección; **no** vincula la tecla E, selecciona rayos, ni genera UI.
- Adaptador `modkit.c` incluye una única vez `brain_npc.c` para respetar el script de Bones. No se añade este auxiliar a la lista de compilación.
- `implementation_version=4`, `state_schema_version=0`, `abi=1` y permisos sin cambios.

## Pruebas realizadas, diferenciadas por nivel de evidencia

| Escenario | Resultado | Evidencia |
|---|---|---|
| Regresión Brain v0.1/v0.2, BVR1 | PASA | `make test`, gcc C11 local |
| Contrato de Brain con host ModKit simulado | PASA | `test_modkit_bridge` |
| NPC en zona libre simulada (100 ticks) | PASA | +8.00 en Z, 100 órdenes aceptadas |
| Pared rectangular simulada | PASA en *mock*, no Bones | 100 aceptadas, 2 recuperaciones, no cruza pared simulada |
| 12 órdenes denegadas | PASA en *mock* | 12 rechazadas, 88 aceptadas, sin falsa recuperación |
| Independencia de orientación del jugador | PASA en *mock* | host varía yaw del jugador sin cambiar órdenes NPC |
| `actor_at` fallido / owner incorrecto | PASA en *mock* | error contado, jugador y actor remoto ignorados |
| Código de prueba con ASan/UBSan | PASA | test mock, gcc en Linux |
| Interacción mínima semántica | PASA | test pure intent; sin tecla E ni interfaz |
| Movimiento real NPC mediante BonesFoundation | **NO EJECUTADO** | requiere publicar código nuevo Bones |
| Colisiones reales de casa/puertas | **NO EJECUTADO** | falta fixture físico alineado a geometría de casa |
| Ejecución Windows / tamaño PE 1.44MB | **NO EJECUTADO** | no compilador Windows ni nueva entrega Bones |

*IMPORTANTE:* el bloqueo de la pared en el mock se debe a la **física simulada del test**. No demuestra que las colisiones de la casa estén corregidas en Bones.

## Petición mínima a Bones (sin editar núcleo)

1. Publicar commit/PR con física, orientación y colisiones corregidas; indicar SHA exacta y prueba reproducible; ejecutar `make test` y `make modkit_test` en esa revisión.
2. Proporcionar fixture de escena en la que una pared **visible** de la casa posea collider sólido y una abertura transitable de puerta; coordenadas y radio/altura de NPC definidos para pruebas. Debe permitir verificar la coincidencia entre pared visual y física. El simple test de collider creado por Brain no verifica esta correspondencia.
3. Confirmar cómo observar por `BonesModServices` si un NPC está *bloqueado* (si se quisiera sustituir heurística de velocidad por flag real) y cómo acceder a destino/navegabilidad. No pedir pathfinding completo ni alterar ABI1 sin Soul.
4. Si se añade E: Bones resuelve **objetivo NPC + distancia + oclusión**, envía una interacción validada una única vez; Brain responde con **identificador semántico** como `BRAIN_NPC_REPLY_GREETING=1`; Skin presenta texto/UI. En ModKit ABI1 actual no hay servicio `on_interaction`: diseñar extensión versionada previa aprobación de Soul, no reutilizar `on_event` como confirmación de transacción.
5. Comprobar que las órdenes NPC siempre proceden de `before_tick`, con `issuer=0` y sin afectar a propietarios de Mouth. Si cambia validación de movimiento, explicar códigos de error `submit_move`.

## Pasos de revalidación tras commit de Bones

En el checkout con carpetas hermanas `BONES/` y `Brain/`:

```sh
cd Brain && make test
cd ../BONES && make modkit_test
../Brain/bridge/bones_0_4/tests/run_real_bones_tests.sh
```

Luego playtest Windows: NPC pegado a muro exterior/interior de casa, pillar, portal libre; video/captura con tick, actor_id, feet.x/y/z, yaw y contadores; comparar estado tras girar jugador y saltar en reposo. Reportar el tamaño de `Proyecto144_Bones_ModKit.exe` medido en bytes reales.

## Revisión Soul

Revisar ausencia de cambios en ABI1, autoridad de jugadores, `brain_npc.c` enlazado solo una vez, contadores sin acceso a hilos paralelos, carácter temporal del heurístico de recuperación y ausencia de persistencia de estado privado (`state_schema_version=0`). Dejar la PR sin fusionar hasta confirmación del nuevo Bones y aprobación.
