# Skin — interfaz/UX Proyecto 1.44

**Responsabilidad confirmada:** menús, HUD, inventarios (presentación), accesibilidad y puentes de interfaz con Eye/Bones. No implica propiedad de IA, audio, gráficos 3D, red, física, economía ni autoridad de inventarios. Ver `00_LEER_PRIMERO` del proyecto.

- `v0_1/` — prototipo histórico UI y guardado exploratorio basado en un contrato antiguo. Se conserva por trazabilidad; no se debe integrar con Bones 0.4 directamente.
- `v0_2/` — UI2 C11 independiente de renderizador, menús/inventarios y pruebas visuales para Eye.
- **`v0_3/`** — nuevo módulo Bones 0.4 **ModKit ABI1**. Su fuente desplegable reside en `v0_3/modules/skin/` y se encuentra integrada en `BONES/modules/skin/` para enlazado estático. Contrato, límites y pruebas en `v0_3/docs/`.

**Integración gráfica pendiente:** Bones ModKit 0.4 no ofrece un servicio de composición UI y el `win32_main.c` actual no llama a Skin. Se necesita el pequeño punto de integración de aplicación descrito en `v0_3/docs/CONTRATO_COMPOSICION_MODKIT.md`, con revisión Soul/Bones/Eye. No modificar los contratos de Bones ni el renderizador de Eye desde Skin.
