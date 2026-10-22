# BONES 0.4.3-A — Integración Eye + Skin (22/10/2026)

## Procedencia
Base: `main` commit `63e294b403f41cc43f667059f1fa417741ff100c`. La aplicación enlaza fuentes ya publicadas en `BONES/modules/eye/` (Eye 0.5.2/ModKit `implementation_version=502`) y `BONES/modules/skin/` (Skin 0.3/ModKit `implementation_version=5`). No modifica sus fuentes ni la ABI ModKit 1.

## Funcionalidad de la entrega A
- Al abrir EXE, Skin arranca en `SKIN2_MAIN` y Eye genera `EYE_DISPLAY_MENU_GENERATED`. No se ejecutan ticks ni se abre captura de ratón.
- Arranque individual / nueva partida (Enter en la opción Skin): Bones recrea Foundation, registra los módulos, inicia ModKit y **solo después** confirma con `bones_skin_ui_enter_game()`.
- En la partida se mantienen WASD, ratón Raw Input, Space y Shift.
- Esc (flanco de tecla): pausa/reanuda; I: inventario; flechas/Enter: navegan y confirman las pantallas Skin sin repeticiones de `WM_KEYDOWN`.
- `bones_skin_ui_blocks_gameplay()` detiene por completo `bones_mod_host_step` mientras se está en menú, sin acumulación de ticks pendientes; se sueltan captura y teclas retenidas.
- El anfitrión llama `bones_mod_host_frame` únicamente jugando. Eye prepara en `frame_ui` el fondo generado, la escena actual o la escena oscurecida, y Skin la compone mediante `bones_skin_ui_compose`. GDI muestra la composición con DIB de memoria y un BitBlt; `WM_PAINT` repinta el último frame.
- `Skin2View.available=0`: no se inventan PV, PM, oro, nivel ni objetos. Los ticks y conteo de actores proceden del ModKit.
- Solicitud de regreso al menú: Bones recrea el estado y solo tras éxito llama a `bones_skin_ui_enter_main`.
- `WM_CHAR` pasa únicamente números, '.' y retroceso cuando Skin está en el formulario LAN (la conexión de red es entrega C).

## Límites / pendientes honestos
- Los comandos LAN, interacción E, audio de opciones y guardados siguen SIN activar desde esta primera entrega. Skin controla la presentación de esos menús, no la implementación del host.
- La entrada por ratón en los menús no se desarrolla aquí; teclado es el método implementado.
- Este PR NO cambia ModKit ABI1, API Bones2, ni reescribe Eye o Skin.
- CI MSVC prueba compilación y tamaño PE pero no garantiza una prueba humana de menús ni audio. Se requiere playtest Windows, resize y Alt+Tab con Soul.

## Tests
`make -C BONES host_ui_test` enlaza **Eye y Skin reales** y ejercita menú generado, acción solicitada, HUD, pausa, inventario, copia de framebuffer y escena original inmutable. `BONES/scripts/test_host_ui_msvc.bat` repite el smoke con MSVC. Son pruebas headless, no automatización visual de GDI.
`BONES/scripts/build_windows_msvc.bat` construye el PE real y realiza comprobación 1.474.560 B.

## Traspaso Soul
Revisar este PR y su CI, probar con teclado en Windows: menú principal, Enter, escena HUD, I, Esc, retorno al menú y salida. Confirmar que no aparece movimiento fantasma tras pausar, Alt+Tab, ni daño del z-buffer. No fusionar sin aprobación.
