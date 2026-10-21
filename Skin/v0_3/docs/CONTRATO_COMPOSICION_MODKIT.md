# Contrato Skin ↔ Bones ModKit 0.4 ↔ Eye — v0.3

## Registro

`BONES/modules/skin/modkit.c` define `bones_mod_skin_query(uint32_t abi, BonesModDescriptor *out)`.

- `abi=1`, `role=BONES_ROLE_SKIN`, `id="skin"`, `implementation_version=3`, `state_schema_version=0`.
- `required_roles=0`: un host sin Eye puede arrancar Skin para pruebas, sin forzar renderización.
- Permisos estrictos: `BONES_PERMISSION_READ_TIME | BONES_PERMISSION_READ_ACTORS`.
- Callbacks `start`, `stop`, `after_tick`, `on_event`. **No** `present`, ni `build_scene`, ni comandos, ni restauración.
- `after_tick` observa el tick confirmado y el número de actores, sin inferir HP, dinero ni datos de inventario.
- `on_event` presenta avisos transitorios de salto/aterrizaje. No convierte notificaciones efímeras en transacciones.

## Límite UI / escena

Skin **no dibuja dentro de** `BonesModFrame` ni de la API de quads; Eye posee la escena tridimensional y el Z-buffer. La ABI1 **no dispone de callbacks `render_ui` ni de servicio de framebuffer 2D**. La integración inicial es explícita a nivel de aplicación y no requiere tocar la ABI compartida.

API pública en `BONES/modules/skin/skin_modkit.h`:

```c
Skin2Action bones_skin_ui_input(Skin2Key key);
int32_t bones_skin_ui_update(const Skin2View *view);
int32_t bones_skin_ui_compose(uint32_t *pixels,int width,int height,int stride);
uint32_t bones_skin_ui_actor_count(void);
uint64_t bones_skin_ui_last_tick(void);
const Skin2Ui *bones_skin_ui_state(void);
```

Secuencia **propuesta al anfitrión** (no integrada aún en `BONES/src/app/win32_main.c`):

```c
/* Una sola vez: bones_mod_host_start(host). */
/* Evento de teclado: interpretar la tecla según la pantalla activa. */
Skin2Action action = bones_skin_ui_input(SKIN2_BACK);
/* El anfitrión resuelve explícitamente action: START, SAVE, LOAD, etc.
   Skin NO ejecuta transacciones ni sobrescribe el estado del mundo. */

/* Por tick, la aplicación ejecuta bones_mod_host_step(host, input).
   El callback after_tick de Skin se ejecuta automáticamente. */
/* Puntos de vida/inventario: solo un proveedor real de lógica RPG. */
(void)bones_skin_ui_update(&view_read_only);

/* Por fotograma: Eye renderiza mediante bones_mod_host_frame(host). */
const uint32_t *eye_pixels = bones_eye_mod_pixels();
/* Requerido: copiar a buffer 0xAARRGGBB PROPIO de la aplicación. */
memcpy(frame_ui,eye_pixels,320u*200u*sizeof(uint32_t));
(void)bones_skin_ui_compose(frame_ui,320,200,320);
/* Presentar frame_ui mediante GDI/otra API del anfitrión. */
```

En Windows el ancho/alto pueden escalarse por la aplicación al presentar. La UI utiliza coordenadas virtuales 320×200 en una región centrada 16:10. El compositor aplica alpha sobre un buffer `0xAARRGGBB`, sin modificar ningún búfer de profundidad. Se exige `stride >= width`. No se permite escribir el `const uint32_t*` de Eye.

## Instalación, compilación y límites

`modkit.c` es *unity build*: incluye los `.c` locales. No compilar sus auxiliares por separado al mismo tiempo. El script actual de Bones detecta automáticamente su nombre en `modules/skin/modkit.c`, por lo que el registro no cambia. La distribución final es un único EXE, sin DLL. La API `SKIN_MODKIT_DYNAMIC` exporta `bones_mod_query_v1` solo para compilación aislada opcional; no se activa para los proveedores estáticos.

La capa UI2 utiliza marcos/áreas y fuente bitmap reutilizables, sin imágenes completas de cada botón. La RAM de estado es fija; un búfer completo extra de 320×200×4 = 256.000 bytes si la aplicación necesita duplicar los píxeles de Eye. Este coste es RAM (no tamaño del EXE) y puede reconsiderarse con un API de composición aprobada por Soul y Eye.

## Pendiente de aprobación Soul/Bones/Eye

- Punto de enganche de composición 2D posterior a Eye en el bucle Win32, **sin acoplamiento directo al renderer**.
- Enrutamiento de teclado de forma que menú/pausa no muevan el personaje y que el salto siga funcionando en juego.
- Paleta final de Eye, tamaño de viewport, reglas para UI sobre resoluciones modernas y estrategia de evitar framebuffer duplicado.
- Decisión explícita sobre el futuro propietario de guardados y transacciones; no introducirla en este módulo por defecto.
