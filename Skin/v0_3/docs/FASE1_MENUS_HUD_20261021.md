# SKIN fase 1 — Menús y HUD del primer playtest

Fecha: 2026-10-21. Implementación Skin 0.4 (ModKit Bones 0.4 ABI1). Estado: **fase 1 autónoma comprobada**, presentación gráfica en Bones Windows **pendiente de contrato y entrega Eye**.

## Alcance probado

- Menú principal: **Nueva partida**, **Un jugador**, **LAN experimental**, **Opciones**, **Salir**. Nueva partida y un jugador emiten intenciones, sin fingir iniciar mundo: el host confirmará el inicio.
- Pausa: **Continuar**, **Inventario**, **Opciones**, **Menú principal**. Guardar/cargar no se muestran porque no existe integración de persistencia aprobada para el playtest.
- HUD: muestra **tick** y **número de actores** de ModKit (`after_tick`), sin inventar estadísticas RPG. PV/PM/oro/inventario solo si un módulo autoritativo proporciona los datos con bits `SKIN2_AVAIL_*`.
- Entrada Esc, I, flechas/Enter y E (`Skin2Key`). Interfaz proporciona `bones_skin_ui_blocks_gameplay()`, que el host DEBE respetar para no enviar movimiento a Bones mientras haya menú. Skin no puede impedir el movimiento si el host ignora esta función.
- Contextual: `bones_skin_ui_target(id,host_valid,label)`. Bones establece un objetivo válido y cercano. Skin no calcula distancia, raycast, hitbox ni transacciones; E devuelve `SKIN2_INTERACT_REQUEST` y abre un panel **esperando a Bones**; el host debe validar de nuevo el objetivo antes de hacer nada.
- LAN experimental y audio muestran **pendiente**, sin networking/audio propio. Primer paso: integración UI con datos reales; segunda entrega solo tras API de Ear/Mouth.
- UI2 composición alfa sin framebuffer interno propio ni Z-buffer; el host renderiza Eye primero, copia al framebuffer de destino y después llama a Skin, y realiza una sola presentación.

## Verificación local

`cd Skin/v0_3 && make test && make preview && make sanitize`

Compila usando un encabezado ModKit de prueba equivalente a Bones 0.4.0 para la suite aislada; **no es una prueba contra todo el núcleo real**.

Prueba manual adicional: a 320×200, la pantalla inicial muestra cinco opciones, LAN advierte que falta API, inventario no muestra valores no confirmados, el HUD permite un objetivo validado. Para versiones Windows, probar letra pequeña y contraste con usuarios a escala real; las pruebas locales no sustituyen esa validación.

GCC -Os objeto independiente (`size build/skin_modkit_phase1.o`): text 8195 B, data 72 B, bss 792 B; no sumarlo sin más al tamaño final PE Windows.

## Dependencias externas abiertas

- [Eye, issue #3](https://github.com/AlexDevJSC/144mb/issues/3): contrato de framebuffer final 2D, formato, escala, fondos, transparencia y ausencia de parpadeos; Eye debe entregar código y prueba real.
- [Ear, issue #4](https://github.com/AlexDevJSC/144mb/issues/4): API runtime pública de activar música y volumen.
- [Mouth, issue #5](https://github.com/AlexDevJSC/144mb/issues/5): contrato de host/unión IPv4/puerto, estado y errores; prototipo actual anunciado para 2 participantes.

## No implementado

Renderizador 3D alternativo, lógica de inicio/creación de sesión, datos de inventario canónico, combate, acciones económicas, guardados, integración activa del host Win32, conexión LAN desde interfaz, audio real, Eye UI surface oficialmente publicada, prueba PE Windows. Soul revisará antes de integración final.
