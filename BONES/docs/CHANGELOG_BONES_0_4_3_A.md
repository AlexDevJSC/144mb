# Changelog Bones 0.4.3-A
- `src/app/win32_main.c`: enlaza y utiliza compositor real Skin 0.3 y base Eye 0.5.2 en framebuffer propietario del host; modo menú por defecto, lifecycle world nuevo/menú, pausa real y despacho de teclado sin autorepeat.
- `tests/test_host_ui.c`: prueba conjunta de los proveedores reales Eye y Skin.
- `Makefile`: objetivo `host_ui_test`.
- `scripts/test_host_ui_msvc.bat`: compila y ejecuta prueba de integración Windows.
- `.github/workflows/bones-host-ui-ci.yml`: CI Linux+Windows y PE.
- Contratos públicos sin cambios. Ear, Brain y Mouth no alterados.
