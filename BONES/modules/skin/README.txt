SKIN 0.4.0-fase1 — Bones 0.4 ModKit ABI1
=======================================
Proveedor de menús/HUD/inventario/interacción de interfaz, exclusivamente.
modkit.c incluye la unidad C11 completa para enlazarla con el mecanismo de registro actual.
Autonomía verificada: make -C Skin/v0_3 test preview sanitize (desde la raíz del repo).
El módulo arranca en menú principal y bloquea movimiento local hasta que el host confirme la sesión.
API: skin_modkit.h (bones_skin_ui_input, blocks_gameplay, enter_game, enter_main, target,
ui_update, ui_compose). Skin solo recibe datos canónicos y compone en memoria del HOST.
No manipula framebuffer const de Eye, render 3D, física, transacciones, audio, LAN ni guardado.
OJO: Bones aún debe conectar teclado, entrada neutral y composición DESPUES de Eye.
NO afirmar integración Windows ni cumplimiento del tamaño 1.44 MB sin pruebas.
Contrato completo y changelog: Skin/v0_3/docs/TRASPASO_A_BONES_SOUL_FASE1.md.
Dependencia de Eye: GitHub issue #3. Ear: #4. Mouth: #5.
