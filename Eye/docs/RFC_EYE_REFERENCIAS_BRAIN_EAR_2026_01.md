# RFC-EYE-REF-2026-01 — Validación futura de referencias Brain y categoría acústica Ear

**Estado: solicitud de revisión, SIN implementación ni cambios de ABI.** Posterior a la estabilización del playtest Eye/Skin/Bones y a la aprobación del catálogo gráfico. No forma parte del PR #12.

## Referencia real de los contratos

- Brain: `Brain/spec/CONTRATO_BRAIN_EYE_REFERENCIAS_v1.txt` propone `BrainVisualSurfaceKey(sector_x,sector_y,object_id,surface_id)`, `BrainVisualContext(world_id,world_seed,world_generator_version,eye_generator_version)`, cambios semánticos (SCORCHED/BROKEN/DESTROYED/PAINTED) y `BrainVisualRef(material_id,variant_id,eye_generator_version,logical_state)`. BVR1 es un **códec de ensayo**: 48 B de cabecera + 56 B por override, **NO save oficial**.
- Ear: `Ear/contract/CONTRATO_EYE_EAR_BONES_v1.txt` y `Ear/contract/p144_acoustic_ids.h` identifican categorías acústicas **independientes**: 0 MINERAL, 1 WOOD, 2 METAL, 3 EARTH, 4 VEGETATION, 5 FABRIC (`P144_ACOUSTIC_VERSION=1`). `P144SurfaceBinding` propone `uint16_t visual_material_id; uint8_t acoustic_id; uint8_t flags`. Esa tabla corresponde a autoría/estado de mundo en Bones; los materiales visuales son opacos para Ear.
- Eye: `eye_bank_open/eye_bank_render` soportan bancos EYE1–EYE3; `eye4_generate` y `eye5_generate` generan a través de recetas tipadas. La versión de FORMATO NO identifica automáticamente catálogo mundial o versión de algoritmo persistida.

## A. Política de visuales reconstruibles (Brain → Eye)

1. Brain envía **clave de superficie y variante lógica**; ningún array de pixels, textura en caché, puntero a `P144_Renderer` ni índices de carga. `object_id` y `surface_id` los asigna Bones.
2. Eye resuelve explícitamente `(eye_generator_version, material_id, variant_id, seed, key)` contra una tabla versionada. Si generador o receta no existe, devolver error tipado; jamás reinterpretar una referencia EYE1 como EYE2 por conveniencia.
3. Override `SCORCHED/BROKEN/DESTROYED/PAINTED` representa el **estado autorizado por juego/Bones**. Eye solo elige su aspecto. Destrucción no elimina colliders ni modifica física desde Eye.
4. `BVR1` no se adopta como guardado oficial; conservar claves canónicas y `world_id` distinto a `world_seed`. Los campos `eye_generator_version` son opacos: **NO asumir que los valores de prueba 1001/1002 sean identificadores oficiales**.
5. No asignar `material_id` persistido a base de un índice de textura, orden de generación o `Eye5Recipe.family`. El catálogo visual tendrá tabla explícita propuesta en RFC-EYE-GFX-2026-01.

**Validación a solicitar al propietario de Brain + Soul, con generadores reales:**
- Compilar y enlazar la implementación Brain BVR1 con `eye_generate` / `eye2_generate` reales; generar golden hash 64×64 por clave determinista, no solo mock.
- Orden de carga A→B vs B→A, evicción y recarga tienen mismo hash/píxeles; semilla distinta da variante distinta sin alterar la clave de objeto.
- Abrir guardado EYE1 en EYE2: rechazo explícito salvo migración aprobada. Probar errores de versión y claves mal formadas, corrupción checksum y duplicados sin mutar destino.
- Documentar la ruta de regeneración para `EYE3`, `EYE4`, `EYE5` sin imponer campos BVR1 nuevos por cuenta de Eye.

## B. Binding visual/acústico (Eye ↔ Bones ↔ Ear)

**La tabla de autoría debe tener IDs separados.** Un valor visual 2 WOOD de Bones NO es el ID acústico 2 METAL de Ear: son universos de nombres diferentes y **no intercambiables**.

Defaults orientativos de FAMILIA (no identificadores reales de catálogo):
| Familia de Eye | Categoría acústica sugerida |
|---|---|
| STONE, BRICK, PLASTER, COBBLE, CARVING | MINERAL (0) |
| WOOD, wood shingles | WOOD (1) |
| METAL | METAL (2) |
| DIRT | EARTH (3) |
| FOLIAGE, THATCH | VEGETATION (4) |
| CLOTH | FABRIC (5) |
| SLATE, CLAY roof | MINERAL (0) |

El binding real puede ser anulado por una capa (ej.: estructura visual metálica forrada con tela, acústica FABRIC). **La autoridad del contacto pertenece a Bones**: detectar paso/impacto, determinar collider/superficie, intensidad y momento. Ear recibe solo evento acústico válido y sintetiza audio. Eye NO infiere golpes, no escribe `BonesModEvent`, no genera PCM ni mezcla sonidos.

**Limitación ABI confirmada:** `BonesModEvent` ModKit ABI1 no contiene `acoustic_id`, intensidad ni ID fiable de contacto. No adaptar forzando bits en campos existentes ni inventar equivalencias. Ear puede reproducir JUMP/LAND genéricos; pasos por superficie exigen nueva propuesta host/capacidades y revisión de Bones+Soul.

**QA compartida futura:**
- Verificar correspondencias visual→acústica por familia y overrides, y rechazo de category 6..255.
- Misma superficie con cambios meramente cromáticos mantiene acústica; cambio de gameplay puede actualizarla por autorización de Bones.
- Colisión y sonido no dependen del nivel de detalle visual ni la expulsión de texturas.
- Tests de integración en Windows y medición PE después de aprobación; no compartir punteros a texturas entre módulos.

## Solicitud / estado

**Brain:** confirme catálogo `eye_generator_version` opaco, claves persistentes de Bones y suite BVR1 con Eye real. **Ear:** confirme categorías neutral v1, overrides y uso exclusivamente desde Bones; no generar audio desde Eye. **Bones:** confirme dónde se almacena binding y cómo se notifican contactos sin mezclar ABI. **Soul:** revise la matriz de responsabilidades, pruebas y versiones antes de adoptar estos contratos. **Dirección:** decidir titularidad de SAVE/journal PBW pendiente, sin atribuirla a Eye, Brain ni Ear.

**No hay cambios de C ni de ABI en esta propuesta.**
