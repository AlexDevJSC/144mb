# Eye 0.5.2 ↔ Skin 0.3 ↔ Bones ModKit ABI 1 — Contrato de presentación (solicitud de integración)

## Autoridad y formato

- Bones conserva `BONES_MOD_ABI=1`, físicas, cámara y presentación GDI Win32. Eye no cambia esas APIs.
- Eye posee `P144_Renderer` y los materiales EYE1–EYE5. **Skin posee y llama `bones_skin_ui_input`, `bones_skin_ui_update`, `bones_skin_ui_compose`** y diseña UI/UX. Eye no implementa duplicados de `Skin2Canvas`, botones, fuentes, acciones, estado o compositor alpha.
- El juego crea un **framebuffer de salida propio** de `320*200` elementos `uint32_t` (`0xAARRGGBB`, opaco). Sus 256.000 bytes son RAM, no recursos de EXE. Las coordenadas Skin son virtuales 320x200.
- Eye conserva el framebuffer 3D original y el búfer de profundidad, sin que Skin los modifique.

## API nueva de Eye (fuera de la ABI ModKit 1)

`#include "modules/eye/eye_modkit_ext.h"` (ruta de inclusión desde el host a convenir).

```c
int bones_eye_mod_prepare_base(uint32_t *dst, size_t capacity,
                               EyeDisplayMode mode, unsigned dim);
int bones_eye_mod_frame_stats(EyeFrameStats *out, uint64_t *frame_serial);
```

`EyeDisplayMode`:

- `EYE_DISPLAY_GAME` — Copia escena 3D actual, sin oscurecimiento.
- `EYE_DISPLAY_MENU_DIM_SCENE` — Copia la última escena 3D y aplica oscurecimiento RGB (`dim=0..255`); escena original intacta.
- `EYE_DISPLAY_MENU_GENERATED` — Crea fondo decorativo de piedra procedural, determinista; **sin texto ni widgets de UI**. Funciona incluso antes del primer `bones_mod_host_frame`.

`bones_eye_mod_frame_stats()` mide el *último frame 3D* antes de Skin/GDI; expone `all_black`, `clear_only`, cantidad RGB negra/de fondo/restante, píxeles sin opacidad y hash FNV64. Devuelve 0 si Eye no está arrancado o aún no hay fotograma.

## Ejemplo exacto de Bones, en el orden necesario

```c
#include "../../modules/eye/eye_modkit_ext.h"
#include "../../modules/skin/skin_modkit.h"
static uint32_t frame_ui[EYE_DISPLAY_PIXELS];

/* Bones decide la pantalla actual de Skin, no Eye. */
/* 1. INPUT: bones_skin_ui_input(key); el host resuelve la acción. */
/* 2. Simulación: bones_mod_host_step(host,&input) cuando juego corresponda. */
/* 3. Datos de juego: bones_skin_ui_update(&read_only_view); */
/* 4. Render 3D (o conservar último si se congela el mundo): */
(void)bones_mod_host_frame(mod_host);
/* 5. Eye prepara el fondo en el buffer DEL HOST: */
EyeDisplayMode mode = EYE_DISPLAY_MENU_DIM_SCENE; /* host/Skin decide */
if (!bones_eye_mod_prepare_base(frame_ui,EYE_DISPLAY_PIXELS,mode,128)) {
    /* error controlado: conservar último frame válido o usar fondo generado */
}
/* 6. Skin compone alpha SOBRE ese mismo buffer (NO sobre Eye->pixels): */
(void)bones_skin_ui_compose(frame_ui,320,200,320);
/* 7. Bones presenta frame_ui mediante GDI atómico/backbuffer en WM_PAINT. */
```

NO llamar a Skin desde `eye_present` del ModKit: el único callback `present` en ABI1 es el del rol EYE. Skin se compone desde el anfitrión después de Eye, como ya documenta `Skin/v0_3/docs/CONTRATO_COMPOSICION_MODKIT.md`.

## Flujo juego ↔ menú

Cambiar `mode` y llamar a `prepare_base()` **no reinicializa** el renderer, no genera texturas EYE1–EYE5 y no reinicia la escena. Para congelar la última imagen 3D, Bones puede detener las llamadas a `bones_mod_host_frame` mientras está en menú si su ciclo de vida lo permite; o seguir renderizando la cámara actual. Eye usa los últimos píxeles disponibles. El compositor UI se ejecuta siempre sobre el búfer final de la aplicación.

Si Skin ofrece una interfaz pausada, el host debe además decidir si procesa o bloquea comandos de movimiento. Esta capa gráfica no controla movimiento ni guardados.

## GDI / resolución

`320x200` es **resolución lógica**, sin escalado dentro de Eye/Skin. El host Bones calcula factor entero cuando quepa, en otro caso letterboxing proporcional; usa GDI sobre la imagen completa en memoria, no borra la ventana antes de presentar y redibuja el último frame en `WM_PAINT`. Este mecanismo corresponde a Bones.

## Tests, limitaciones y revisión

- `tests/test_eye_display.c` comprueba scan negro/despejado/crosshair, fondo con determinismo, fuente 3D inmutable y oscurecimiento, límites de búfer y ejes de cámara por fórmula; GCC y Clang sanitizados.
- Falta demostrar `bones_skin_ui_compose` **real integrado** en el anfitrión/Windows; Skin y Soul deben probarlo y revisarlo.
- Eye no se autoconcede recursos de UI dentro de la ABI 1, ni modifica `bones_skin_ui_compose`. La integración definitiva queda abierta hasta demostración de Skin y revisión Soul.
- `modkit.c` **incluye** todos los auxiliares Eye, incluido `eye_display.c`; NO añadirlos individualmente a la compilación, pues produciría símbolos duplicados.
