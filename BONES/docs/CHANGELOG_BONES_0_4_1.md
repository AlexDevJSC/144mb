# Changelog Bones 0.4.1 — Fase 1
- `src/app/win32_main.c`: Raw Input relativo, control de foco/captura, trazas opcionales y presentación GDI en memoria.
- `src/demo/test_house.[ch]`: colisiones reales de la casa, hueco en la puerta, apertura física demostrativa y escena consistente.
- `tests/test_geometry.c`: prueba del esquema visual de 12 quads.
- `tests/test_playtest.c`: regresión WASD por cuatro rumbos, diagonal normalizada, salto estacionario y en movimiento, paredes y puerta.
- `Makefile`: ejecuta la nueva suite en el conjunto de pruebas.
- `docs/HANDOFF_PLAYTEST_FASE1_2026-10-21.md`: informe de implementación y verificación.
**API pública:** Bones API 2; ModKit ABI 1. Sin cambios.
**Responsabilidad:** solo Bones y host Windows. Sin integración de menús, redes o audio en esta fase.
**Validación:** pruebas nativas Linux pasadas; Windows MSVC/PE y playtest visual pendientes hasta CI/ejecución real.
