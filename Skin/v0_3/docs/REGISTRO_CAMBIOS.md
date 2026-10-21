# Registro de cambios — Skin

## 0.3.0 (21-10-2026)

- Nuevo adaptador para **Bones ModKit 0.4.0 ABI1**: `bones_mod_skin_query`, rol `BONES_ROLE_SKIN`, versión interna 3 y privilegios mínimos.
- Integración de los renderizadores UI2 existentes de v0.2 mediante módulo *unity build* autocontenido, sin alterar el núcleo de Bones.
- Nuevo puente explícito de entrada (`bones_skin_ui_input`), datos de solo presentación (`bones_skin_ui_update`) y composición a buffer ajeno (`bones_skin_ui_compose`).
- Datos observables: tick / número de actores / avisos JUMP/LAND. Sin manipular físicas, transacciones ni P2P.
- Nuevas pruebas ABI/ciclo/permisos/UI/alpha/resoluciones. GCC, Clang y sanitizadores verificados sobre interfaz simulada.
- Documentada la dependencia pendiente del anfitrión para presentar UI en Windows y la necesidad de revisión por Soul/Eye.
- Retenidas v0.1 y v0.2 sin reescritura ni cambio retrospectivo; v0.3 no implementa guardados.

**Reversión:** restaurar el antiguo `BONES/modules/skin/modkit.c` placeholder desde historial Git y borrar los auxiliares de ese subdirectorio; los módulos restantes y la ABI no cambian.
