# BONES 0.4.3-C — Integración Mouth LAN experimental

Base: Entrega B commit `51862a63d6ad67e464a4dae244b4b77cdfd9f134`. Fuente Mouth 0.4, ModKit ABI 1 / sesión API 1, contrato `Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md`.

## Integrado en anfitrión Win32
- La pantalla Skin main/LAN utiliza `SKIN2_LAN_CREATE_REQUEST`, `SKIN2_LAN_JOIN_REQUEST`, `SKIN2_LAN_DISCONNECT_REQUEST`. Se leen IP/puerto desde `bones_skin_ui_lan_request()`; no se fabrican valores. `WM_CHAR` entrega dígitos y '.' al formulario.
- Solitario nunca llama create/join ni abre UDP. La sesión Mouth queda desconectada.
- Crear LAN: Bones crea Foundation, NPC Brain y jugador remoto (`owner=2`) con ID real, configura `MouthLanConfig` con identidad/semilla/generator/puerto, invoca `mouth_session_create` **antes** del registro ModKit, y solo confirma el menú tras arrancar con éxito el host. Mouth posee socket, HELLO, orden por tick y snapshots.
- Unirse: `mouth_session_join` abre cliente UDP validando IPv4 y conocimiento previo del mismo mundo fijo (no existe descubrimiento remoto). El cliente envía controles con `mouth_lan_client_controls` y `mouth_session_poll` aun con UI bloqueando input. El host mantiene la simulación y red activas durante pausa LAN (input del jugador local neutro): no equivale a pausa global de un mundo en red.
- Render cliente: se consume `mouth_session_copy_snapshot` (2 jugadores) y se actualiza **solo una Foundation local de presentación**, que no avanza física autoritativa. La cámara sigue al actor remoto (`owner=2`); el anfitrión se presenta como sprite mediante un `BonesModule.on_scene` local. Brain/NPC se oculta en vista cliente porque M144/1 no transmite NPC. Este modo es un preview sin interpolación ni reconciliación.
- El título de ventana muestra `MouthSessionInfo` (phase, bytes RX). Skin recibe `bones_skin_ui_lan_status` con estados confirmados; los errores de creación/unión se indican al usuario y se vuelve al menú local.
- Salir / volver al menú destruye primero host ModKit, luego desconecta Mouth; no deja socket host abierto.
- Ni la API pública Bones2 ni ModKit ABI1 cambian. No se modificó código de Mouth, Eye ni Skin.

## Pruebas automatizadas
`make -C BONES host_lan_test` y `BONES/scripts/test_host_lan_msvc.bat` hacen un **loopback real de Mouth** entre el servidor ModKit y cliente UDP en el mismo proceso: 300 ticks, HELLO, INPUT, snapshots con IDs/owners, actor remoto en movimiento, actor local inmóvil, estado Skin confirmado y desconexión. El ejecutable gráfico Windows también compila con MSVC en CI.

## Límites y riesgos
- **Sin prueba manual de dos procesos gráficos Windows todavía**. El test usa sockets loopback reales, pero no automatiza clics/teclado en dos ventanas. El proyecto Mouth dispone de sus propios tests de dos procesos; revisarlos no equivale a jugar con dos ventanas.
- Cliente solo recibe **dos actores**, no NPC, proyectiles, cambios de puertas ni hechos persistentes. El mundo procedural fijo es local. Snapshot 20 Hz aprox., sin interpolación; pitch de cámara no viaja por protocolo.
- La identidad del mundo/semilla se configura con constantes conocidas en ambos ejecutables. Unirse a mundos ajenos solo por IP es IMPOSIBLE con M144/1.
- Sin Internet P2P, NAT, descubrimiento de servidores, cifrado ni autenticación criptográfica. Usar solamente en LAN de confianza.
- Aún hace falta playtest Soul/Eye/Mouth para representación estable, resize/Alt+Tab y reconexión, antes de considerar C aceptada.

## Guía manual
1. Desde el EXE en dos máquinas LAN o dos procesos Windows, seleccionar MENÚ → MULTIJUGADOR → CREAR LAN en el anfitrión.
2. Segundo EXE → MULTIJUGADOR → UNIRSE, escribir `127.0.0.1` (solo localhost) o IPv4 local del host y puerto `14444`.
3. Ambos deben usar el mismo build/semilla/mundo. Si no hay cliente, el host muestra fase esperando. Tras conexión, el cliente muestra el mundo de la sesión con transformaciones confirmadas.
4. WASD/mouse/Space envían INPUT en cliente; el host aplica físicas a owner=2. El modo cliente muestra snapshots, no física local.
5. Usar Esc → Volver al menú, o cerrar ventana, para desconectar.

**Handoff Soul:** no mezclar la aceptación con el juego online definitivo; comprobar ID del actor remoto y cierre de sockets. C depende de la aprobación A y B.
