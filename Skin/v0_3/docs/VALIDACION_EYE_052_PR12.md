# Skin fase 1b — composición UI2 sobre Eye 0.5.2 (PR #12)
Fecha: 21/10/2026. Revisión pendiente de Soul. Esta fase NO cambia el renderizador, el Z-buffer ni el código de Eye.

## Fuente real y límite de la prueba
Se ha consultado el contrato `Eye/docs/CONTRATO_GRAFICO_SKIN_0_5_2.md` de la rama de Eye PR #12. El test `tests/test_eye_skin_integration.c` enlaza el código real de Skin en el ModKit ABI1 contra una **reproducción funcional test-only** de `eye_display_prepare`/`eye_display_scan` tomada de PR12. El fondo no es un fotograma 3D del renderizador Eye completo; la escena de referencia se construye como patrón controlado.

## Pruebas ejecutables

`cd Skin/v0_3 && make test && make sanitize && make preview`

Comprobar: salida y capturas `build/eye_skin_{main,hud,hud_large,pause,inventory}.ppm`.

- Fondo arquitectónico procedural de menú (solo Eye); composición Skin encima.
- Tras entrada y ACK de juego, HUD con tick y número de actores.
- Esc -> pausa, Esc -> reanudar, I -> inventario; menús bloquean movimiento.
- Panel de pausa sin cruce del último botón con el pie de página.
- HUD ampliado 2x con fondo y glifos dentro del área 320x200.
- Menú oscuro de Eye atenuando sin mutar píxeles originales.
- Sentinel de Z-buffer: bytes idénticos antes y después.
- Escalado UI independiente 1280x720 con bandas laterales sin modificar.

**No comprobado aquí:** `bones_eye_mod_prepare_base` real del proveedor completo, `win32_main.c`, GDI WM_PAINT/WM_ERASEBKGND, rendimiento PE x64, ausencia de parpadeos en Windows o dispositivo real. Bones debe conectar y Soul debe verificar tras merge de Eye.

## Orden preciso para Bones

```c
/* ModKit ABI1, propietario del framebuffer: app Bones */
static uint32_t output[320u*200u];
/* Entrada: bones_skin_ui_input(SKIN2_*) solo en flanco de tecla. */
/* Si bones_skin_ui_blocks_gameplay() => BonesInput input = {0}, soltar ratón. */
(void)bones_mod_host_frame(host); /* Eye rasteriza la escena */
EyeDisplayMode mode = bones_skin_ui_state()->screen==SKIN2_MAIN
    ? EYE_DISPLAY_MENU_GENERATED
    : bones_skin_ui_blocks_gameplay()?EYE_DISPLAY_MENU_DIM_SCENE:EYE_DISPLAY_GAME;
int ok=bones_eye_mod_prepare_base(output,320u*200u,mode,128u);
if(ok) (void)bones_skin_ui_compose(output,320,200,320);
/* La aplicación presenta output una sola vez, sin borrar el fondo en cada WM_PAINT. */
```

No dibujar Skin desde `Eye.present`: en ModKit solo Eye tiene ese callback. No escribir sobre `bones_eye_mod_pixels()` (`const`). Si falla la composición, conservar un frame de salida válido y notificar, no renderizar basura.
