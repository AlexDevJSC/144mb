# Bones 0.4.2 — Fase 2: selector de objetivos (experimental)

Responsable Bones; propuesta pendiente de Soul y coordinación con Skin. Sin cambiar BONES_API_VERSION=2 ni ModKit ABI 1.

## Mecanismo
- `src/bones/interaction_core.[ch]`: registry de candidatos interactuables con IDs lógicos estables y volúmenes AABB explícitos. No convierte toda geometría visual o física en interactuable.
- `bones_interaction_select(world, registry, camera, max_distance, out)` lanza un rayo desde el centro de cámara con yaw y pitch, comprueba distancia, dirección y oclusión con colisionadores Bones. Devuelve el más cercano, desempatando por ID.
- Con `BONES_INTERACTION_REQUIRE_COLLIDER` la selección de la puerta solo existe mientras está su collider.
- **No ejecuta acciones de juego.** El anfitrión debe revalidar objetivos al confirmar cada solicitud E; Mouth no puede confiar en un target sugerido por el cliente.
- Sin HUD, dibujo, sonidos, menú, economía, estado de puertas en red, guardados nuevos ni modificación a la ABI pública.

## Pruebas
`make test CC=gcc` y `make test CC=clang`: PASS, incluye suite `test_interaction_core` con distancia, pitch/yaw, oclusión, IDs, duplicados y puerta abierta/cerrada. ASan+UBSan nueva suite: PASS. `make core`: PASS. Scripts Windows MSVC/MinGW y ModKit dev actualizados para compilar el fichero. Validación Windows CI de esta fase pendiente.

## Siguiente revisión
Skin no recibe todavía un servicio ModKit 1 para interactuar. Tras visto bueno de Soul, el anfitrión podrá pasar un estado inmutable `{target_id, kind, distance}` a Skin para mostrar el prompt `E — Interactuar`, con revalidación autoritativa en el servidor. No ampliar `BonesModServices` sin versionar y coordinar el impacto.
