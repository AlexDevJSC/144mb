# Skin 0.3 — compatibilidad Bones ModKit 0.4.0

**Autor:** Skin · **ámbito actual:** menús, HUD, inventario visual, UX y puentes de interfaz con el motor. La política de responsabilidades `00_LEER_PRIMERO` tiene prioridad; no se reivindica la lógica de guardados, economía o gameplay.

## Entrega

`modules/skin/` contiene **siete archivos de código**: `modkit.c`, `skin_modkit_impl.c`, `skin_modkit.h`, `skin_ui2.c/.h` y `skin_font.c/.h`. También están instalados en `BONES/modules/skin/` del repositorio para la compilación estática del ModKit.

No se ha modificado el núcleo, la ABI ni el adaptador de Eye. El `modkit.c` incorpora las unidades auxiliares al mismo fichero de compilación (*unity build*) porque los scripts actuales de Bones solo incluyen los `modules/*/modkit.c` automáticamente.

## Instalación en otro proyecto Bones 0.4.0

1. Copia **todos** los archivos `modules/skin/` de este paquete a `BONES/modules/skin/`, reemplazando el `modkit.c` provisional.
2. Desde la raíz de Bones, ejecuta `python3 scripts/gen_modkit_registry.py` y `make modkit_test` (Linux); en Windows, utiliza `scripts\build_windows_msvc.bat` o `build_windows_mingw.bat`.
3. Bones registrará automáticamente la función `bones_mod_skin_query` con ABI 1 y rol Skin. No se necesitan cambios en `src/modkit/`, `include/bones/` ni en el registro generado por parte del especialista.
4. **Para ver realmente los menús**, la aplicación debe redirigir teclado a `bones_skin_ui_input`, proporcionar los datos de vista a `bones_skin_ui_update` y llamar a `bones_skin_ui_compose` **después** de Eye, sobre una copia de framebuffer de propiedad de la aplicación. Véase `docs/CONTRATO_COMPOSICION_MODKIT.md`.

**Importante:** el ejecutable estándar de Bones 0.4.0 registra Skin al arrancar, pero actualmente su `win32_main.c` solo presenta los píxeles de Eye y no invoca este compositor ni envía eventos de teclado a Skin. Por tanto, este cambio **no hace visibles los menús automáticamente** en el EXE estándar; requiere que Bones/Integración/Soul aprueben y conecten esos dos puntos en el anfitrión. No se alteran módulos ajenos por cuenta propia.

## Pruebas

En la estructura del repositorio (carpetas hermanas `BONES/` y `Skin/`):

```sh
cd Skin/v0_3
make test
make sanitize
```

Valida el registro ModKit ABI1, privilegios, callbacks, interacción de menú, visualización de inventario y composición 320×200 / 800×450, además de formato de datos de vista. El test simula servicios que reflejan las firmas de `BONES/include/bones/modkit.h`; **no equivale** a la compilación/prueba de todo Bones en Windows.

## Limitaciones intencionadas

- Ni lectura/escritura de partida, ni snapshot BSF1, ni redes, ni física, ni modificación de mundo.
- `Skin2View` solo se rellena con datos enviados expresamente por la lógica del juego. La ABI actual de Bones expone actores y ticks, **no puntos de vida, oro ni inventarios**: no se inventan valores.
- La paleta es temporal hasta recibir recursos definitivos de Eye.
- Una instancia estática de Skin por proceso en esta versión. Sin hot reload.
- No se incluye DLL en el paquete de distribución compacto; la variante DLL es un experimento de desarrollo optativo.
- El límite global de 1,44 MB del ejecutable Windows continúa sin verificarse.

Ver documentos `docs/` para el contrato, pruebas, riesgos y solicitud a Soul.
