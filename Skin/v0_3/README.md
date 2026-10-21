# Skin — interfaz C11, Bones 0.4 ModKit ABI1

**Estado actual: 0.4.0-fase1, autónoma; integración visual Windows bloqueada por entrega Eye #3.**

Skin diseña menús, HUD, inventario visual y ventanas contextuales. No implementa Eye (render 3D), Bones (física y mundo), Ear (audio) ni Mouth (red). Skin NO genera estadísticas RPG, guardados ni transacciones económicas.

Código instalable en `BONES/modules/skin/`; el punto de entrada `modkit.c` es unity build (así Bones detecta automáticamente el módulo). Esta carpeta `Skin/v0_3/modules/skin/` contiene una copia idéntica para distribuir al especialista sin modificar el núcleo. `implementation_version=4`, `state_schema_version=0` y ModKit ABI=1.

`make test` ejecuta tests aislados; `make preview` produce PPM reales 320x200 de menú/HUD/objetivo/inventario/LAN; `make sanitize` ejecuta ASan + UBSan. Usa la cabecera local de prueba ModKit equivalente para tests, **no** la integración total del repositorio Bones.

Documentación: `docs/FASE1_MENUS_HUD_20261021.md`, `docs/TRASPASO_A_BONES_SOUL_FASE1.md` y `docs/REGISTRO_CAMBIOS.md`.

Acciones: SKIN2_START/SINGLE son solicitudes que el host confirma mediante `bones_skin_ui_enter_game()`. Durante los menús `bones_skin_ui_blocks_gameplay()` indica que Bones debe mandar input neutro. Skin recibe objetivo/rango validados de Bones con `bones_skin_ui_target` y devuelve `SKIN2_INTERACT_REQUEST` sin ejecutar acciones. Presentación pendiente de Eye con un destino host-owned, nunca tocando el Z-buffer. Audio y LAN tienen panel pendiente a la espera de Ear y Mouth.
