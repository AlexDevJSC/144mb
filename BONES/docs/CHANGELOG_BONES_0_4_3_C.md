# Changelog 0.4.3-C
- `src/app/win32_main.c`: host/client LAN a través de Mouth Session API 1; UI Skin LAN activa, control de sesión, polling, INPUT y render cliente desde snapshot confirmado en copia local.
- `tests/test_host_lan.c`: prueba real loopback con host Bones ModKit y cliente Mouth, salida Skin.
- `Makefile`, `scripts/test_host_lan_msvc.bat` y CI: pruebas Windows y Linux.
- Sin cambios en fuentes especializadas ni ABI pública.
