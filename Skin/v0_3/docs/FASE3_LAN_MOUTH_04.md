# Skin fase 3 — presentación LAN mediante Mouth Session API v1

Se basa únicamente en el contrato oficial `Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md`, disponible en `main`, y en `BONES/modules/mouth/mouth_lan.h`. **No** se abren sockets desde Skin.

## Pantallas
- Menú LAN: crear, unirse, estado, volver.
- Unirse: editar IPv4 con dígitos y puntos, puerto decimal, conectar, volver. Se rechazan IP vacía, octetos >255, puerto 0/>65535.
- Estado: `MouthSessionPhase` del 0 al 8; `MouthSessionResult` del 0 al 9 y puerto informado por Mouth. Botón desconectar sólo solicita, no cierra.
- Aviso obligatorio: **requiere world_id, seed y generator_version previamente conocidos**; con una IP sola no se puede entrar a mundos desconocidos en M144/1.

## Contrato de anfitrión

`bones_skin_ui_lan_status(info.phase,info.error,info.port,world_identity_known)` recibe los datos de `mouth_session_get_status`, sin permitir que Skin los adivine. El adaptador opcional `skin_mouth_bridge.h` ofrece `bones_skin_ui_refresh_mouth(mouth,known)` cuando Mouth está instalado; se incluye solamente desde código del anfitrión.

`SKIN2_LAN_CREATE_REQUEST`: Bones debe configurar Foundation/world e incorporar al actor remoto antes de `mouth_session_create` y antes del arranque del ModKit. **El host actual no se reinicializa por Skin.**

`SKIN2_LAN_JOIN_REQUEST`: `char ipv4[16]; uint16_t port; if (bones_skin_ui_lan_request(ipv4,&port)==BONES_MOD_OK) { /* host consulta identidad mundial y entrega a Mouth */ }`

`SKIN2_LAN_DISCONNECT_REQUEST`: Bones delega a `mouth_session_disconnect` y retorna el resultado confirmado a Skin a través de `bones_skin_ui_lan_status`.

Para teclado: `WM_CHAR` ASCII '0'–'9' o '.', llamar `bones_skin_ui_lan_text(codepoint)`; `VK_BACK` = 8. Enter/flechas/Escape por la API de entrada existente.

**No verificable solo con Skin:** socket real, error de Winsock, mundo compatible entre procesos, cliente 3D, LAN en dos PCs. Mouth ya documenta CI de sus protocolos; falta prueba de integración gráfica de Bones/Soul.
