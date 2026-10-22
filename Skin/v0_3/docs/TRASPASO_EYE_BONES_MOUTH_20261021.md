# Skin — fases Eye (PR12) y Mouth 0.4: traspaso a Bones y Soul

Fecha: 2026-10-21. PR de integración principal: **#6**. ABI ModKit de Bones: **1**, intacta.

## Reparto y dependencias

- Skin posee UI2, entrada de interfaz, navegación, avisos, composición alpha y formularios IP/puerto.
- Eye posee la base gráfica y renderizador 3D. Su **PR #12 sigue en borrador**, no puede darse por incorporado al ejecutable normal de Bones.
- Bones posee ventana, bucle de juego, modo activo, teclado/GDI, validación de interacción y la activación/autorización de Mouth.
- Soul revisa contratos, traspaso, regresiones y binarios PE Windows.
- Ear PR #13 sigue sin aprobación registrada de Soul a la fecha: **no activamos controles musicales** ni compilamos Ear en Skin. Issue #14 abierto.
- Bones PR #21 selector de interactuables sigue experimental y sin contrato de exposición a Skin. E solo consume objetivos validados por Bones: **no inventa uno**.

## Presentación exacta en el host (Eye 0.5.2 PR12)

```c
#include "../modules/eye/eye_modkit_ext.h" /* tras integrar Eye PR12 */
#include "../modules/skin/skin_modkit.h"
static uint32_t frame_ui[EYE_DISPLAY_PIXELS]; /* 256 000 B de RAM del host */

/* Tras tratar entrada y actualizar datos: */
if (world_running) (void)bones_mod_host_frame(mod_host);
const Skin2Ui *ui=bones_skin_ui_state();
EyeDisplayMode mode = !ui || ui->screen==SKIN2_MAIN
    ? EYE_DISPLAY_MENU_GENERATED
    : ui->screen==SKIN2_GAME ? EYE_DISPLAY_GAME
                            : EYE_DISPLAY_MENU_DIM_SCENE;
if (bones_eye_mod_prepare_base(frame_ui,EYE_DISPLAY_PIXELS,mode,128u)) {
    (void)bones_skin_ui_compose(frame_ui,320,200,320);
    /* Bones presenta frame_ui una sola vez, sin borrar con PatBlt antes. */
}
```

El acceso a `frame_ui` es del host, no de Skin ni de Eye. Es opaco ARGB `0xAARRGGBB` (32-bit, W320/H200). No pasar el `const uint32_t*` de Eye a una API que modifica píxeles. Skin no accede al Z-buffer. Para escalado en pantalla moderna, Bones implementa letterbox/pixel-nearest antes del último GDI/Present; `WM_PAINT` repinta el último frame. No se crea otra capa rasterizadora 3D.

**Prueba ejecutada:** `make test` en `Skin/v0_3/` compila `test_eye_skin_integration.c` con la lógica portable `eye_display_prepare` funcionalmente reproducida desde PR12 y el auténtico módulo Skin ModKit. Verifica menu principal, HUD, pausa/reanudación, inventario, alpha opaco final, inmutabilidad de escena y Z-buffer sentinel del test, y coordenadas 1280×720. No equivale a haber compilado Eye 0.5.2 completo contra Bones ni a una prueba GDI Windows. Soul debe repetir tras integrar PR12.

## Teclado y neutralización en Bones (responsabilidad host)

- Para Esc/I/E/flechas/Enter, invocar `bones_skin_ui_input(SKIN2_*)` **solo en el flanco key-down** (ignorar auto-repeat sostenido). No procesar I/E para movimiento.
- `WM_CHAR` envía únicamente ASCII dígitos y punto a `bones_skin_ui_lan_text((uint32_t)wparam)` cuando `screen==SKIN2_LAN_JOIN` y `selection` corresponde a IP o puerto. `VK_BACK` equivale a ASCII 8, y `Escape` vuelve; no permitir caracteres arbitrarios.
- Antes de cada `bones_mod_host_step`: si `bones_skin_ui_blocks_gameplay()` devuelve no cero, enviar `BonesInput in={0}` y soltar la captura de ratón. Limpiar estado de teclas retenidas para evitar movimiento involuntario al reanudar.
- Acciones `SKIN2_START` / `SKIN2_SINGLE`: solicitud. Crear la sesión en Bones y confirmar con `bones_skin_ui_enter_game()` solo si se ha creado correctamente.
- `SKIN2_MAIN_REQUEST`: Bones resuelve/cierra sesión y confirma mediante `bones_skin_ui_enter_main()`.
- `SKIN2_INTERACT_REQUEST`: **revalidar** target, alcance y oclusión en Bones ANTES de ejecutar la acción. Sin contrato PR21 aprobado, permanecer en diálogo no funcional y no mostrar objetivos inventados.

## Módulo Mouth LAN v0.4 — contrato Session API 1 publicado en main

- Las pantallas LAN nuevas ofrecen Crear partida, Unirse por IPv4+puerto, Estado y Desconectar, con estados y errores `MouthSessionPhase` / `MouthSessionResult` descritos en `Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md`.
- Skin no llama `mouth_session_create`, `join`, `poll` o `disconnect`; emite `SKIN2_LAN_CREATE_REQUEST`, `SKIN2_LAN_JOIN_REQUEST`, `SKIN2_LAN_DISCONNECT_REQUEST`.
- Bones debe disponer de `world_id`, `seed` y `generator_version` y de un `MouthLan` válido. Configuración del anfitrión requiere *spawn de actor remoto y `session_create` antes de registrar/start del ModKit*. No intentar configurar sobre host ya arrancado.
- `bones_skin_ui_lan_status(phase,error,port,world_identity_known)` solo recibe **estado confirmado**; la alternativa opcional `skin_mouth_bridge.h` recibe `MouthSessionInfo` obtenida de `mouth_session_get_status()` y rellena el estado UI.
- El formulario de unión guarda solo una IPv4 y un puerto validado. Al recibir `SKIN2_LAN_JOIN_REQUEST`, Bones llama `bones_skin_ui_lan_request(ipv4,&port)` y decide si se puede unir según identidad conocida. La UI **no** promete entrar a un mundo desconocido introduciendo solo IP: ese descubrimiento no existe en protocolo M144/1.
- El ciclo de conexión y render del cliente continúa bajo Bones/Mouth; Skin no hace sockets ni spawn.
- Modo solo: NO se llama a Mouth create/join y no se abre UDP.

## Aprobaciones pendientes

1. **Eye #12**: source merge, frame real, alpha, base y contrato confirmados por Soul.
2. **Bones**: entrada y modo menú, GDI sin parpadeos, host-owned buffer, no input fantasma, y flujo de creación/reinicio LAN.
3. **Bones PR21 / Soul**: exposición del objetivo autoritativo a `bones_skin_ui_target` y revalidación.
4. **Ear PR13 / Soul**: aprobar API audio antes de activar sus controles.
5. **Soul**: `make`/MSVC Windows, memoria y bytes PE reales, playtest de cliente LAN en dos procesos/PCs.
