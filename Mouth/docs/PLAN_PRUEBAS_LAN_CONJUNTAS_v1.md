# Plan de aceptación conjunta Mouth 0.4.0 / Bones / Skin / Eye
21/10/2026 — Escenario y evidencias, NO equivalentes a un juego LAN ya integrado.
Contrato API vigente: Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md
Wire vigente: M144/1, Bones ModKit ABI 1. Sin cambios de física ni UI a cargo de Mouth.

## Alcance del recorrido
Individual → Crear LAN → Unirse desde cliente con mundo previamente conocido →
Enviar INPUT remoto → Bones valida y mueve actor owner=2 → Cliente recibe
SNAPSHOT → Eye representa ambos actores → Desconectar de forma limpia.

* Condición BLOQUEANTE actual: Bones/src/app/win32_main.c no llama a
  mouth_session_create/join ni enlaza eventos de UI Skin. La prueba
  extremo a extremo gráfica con menú NO PUEDE declararse superada.
* M144/1 exige que world_id, seed y generator_version estén presentes
  en el cliente antes de conectarse; entrar a un mundo desconocido con
  sólo IP/puerto no está implementado. Hay una propuesta aparte pendiente
  de decisión Bones/Soul.
* La prueba actual por consola aporta evidencias de red, no de render.

## Matriz de escenarios y responsable

| Caso | Responsable | Criterio observable | Situación |
|---|---|---|---|
| SP-01 Iniciar Individual | Bones/Skin | No invoca create/join, Mouth offline y sin socket, los ticks siguen | Probado por API/ModKit aislado; menú pendiente |
| HOST-01 Crear LAN | Bones | Spawn remoto owner=2 antes de registro único, start UDP no bloquea | Probado en demo, integración gráfica pendiente |
| HOST-02 Puerto ocupado | Mouth/Bones | Start falla; Skin muestra SOCKET_ERROR sin bloqueo ni destruir juego incorrectamente | Pendiente prueba gráfica |
| JOIN-01 IPv4 y puerto | Mouth/Skin | JOINING→CONNECTED; world_id/seed/gen correctos | Probado en demo de consola |
| JOIN-02 IP mal escrita | Skin/Mouth | INVALID_ADDRESS inmediatamente, no socket | Probado por tests API |
| JOIN-03 Sin anfitrión | Mouth/Skin | TIMED_OUT tras aprox 5 segundos, opción Salir | Probado por dos procesos |
| JOIN-04 Incompatibilidad | Mouth/Skin | WORLD_INCOMPATIBLE o PROTOCOL_INCOMPATIBLE | Probado en consola/tests |
| OWN-01 Control remoto | Mouth/Bones | INPUT sólo se somete al actor 2; actor 1 no cambia | Probado loopback; menú pendiente |
| OWN-02 Orden ajena | Mouth/Bones | Packet de endpoint/session ajena no puede decidir actor/issuer | Probado packet sin sesión; no hay auth criptográfica |
| SNAP-01 Estado confirmado | Mouth/Bones/Eye | tick creciente, IDs distintos, estado validado por Bones; Eye lo dibuja | Copia comprobada en tests; cliente gráfico pendiente |
| DISC-01 Cierre limpio | Mouth/Bones/Skin | LEAVE best-effort y destroy/disconnect, sin bloqueo | Probado en consola; menú pendiente |
| STAB-01 Pérdidas acotadas | Mouth | 15% pérdida simulada y corte de 1,2s, retoma snapshots | Test CI nuevo; consultar resultado por commit |
| STAB-02 Sesión prolongada | Mouth | Sesión ≥30 min con medición y corte de conexión/recuperación | No probada |
| LAN-01 2 PC Windows | Mouth/Bones/Skin | Conectividad IPv4 UDP física, comandos y snapshots, sin firewall falso positivo | No probada |
| GUI-01 Flujo del menú | Skin/Bones/Eye | UI inicial y 2 representaciones locales reales, sin leer consola | No integrado |
| SIZE-01 PE final | Bones/Soul | medir tras todos los módulos, ≤1.474.560 B | No validado para juego final |

## Prueba reproducible existente (1 equipo: loopback, consola)
Usar mismo checkout de GitHub para ambos procesos. Compilar con Windows
x64 Native Tools Command Prompt, desde raíz del repositorio:
  Mouth\scripts\build_windows_msvc.bat
  BONES\scripts\build_windows_msvc.bat

Proceso A (host):
  Mouth\build\mouth_lan_demo.exe host 14444
Proceso B (cliente):
  Mouth\build\mouth_lan_demo.exe join 127.0.0.1 14444

Controles: WASD/Shift/Espacio en la ventana de consola correcta.
Ambos imprimen actor 1 y actor 2 (tick y posiciones). En este
demo el mundo es el conocido por ambos programas; NO prueba
entrada libre en mundos desconocidos. Para terminar Ctrl+C en
cliente y host. El host solo debe mover al invitado mediante
su owner=2. No registrar proveedores Mouth dos veces.

Automatización existente:
  make -C Mouth test
  make -C Mouth loopback
  make -C BONES modkit_test
  python Mouth/tests/test_lan_loss_proxy.py Mouth/build/mouth_lan_demo.exe
El último requiere el binario compilado. El test inyecta pérdida
determinista y corte corto, pero NO es un test de múltiples horas.

## Prueba manual 2 PC físicos (PENDIENTE: necesita dos equipos)
1. Instalar el MISMO commit/EXE de prueba en PC-A y PC-B Windows.
   Ambos deben estar en la misma LAN de confianza.
2. Obtener IPv4 del anfitrión con ipconfig. Comprobar que el puerto UDP
   14444 no está ocupado. Autorizar la aplicación/puerto ÚNICAMENTE
   para el perfil de red Privada. No abrir NAT ni exponer Internet.
3. En PC-A lanzar host 14444. Consultar logs: modo esperando.
4. En PC-B lanzar join IPv4-del-PC-A 14444. Comprobar CONNECTED
   sin extrapolarlo a protocolo Internet seguro.
5. PC-B pulsa WASD. Verificar movimiento actor2 y posición confirmada
   en ambos logs; actor1 debe seguir inmóvil si nadie lo controla.
6. PC-A pulsa WASD en su propia ventana, comprobar actor1 cambia sin
   mover actor2 por error. Comprobar ID y owner reales en Bones logs.
7. Desconectar primero PC-B, comprobar que host vuelve a WAITING.
   Volver a unirse con el mismo mundo, verificar que no duplica actores
   ni reinicia las secuencias autoritativas del actor.
8. Cerrar PC-A abruptamente y observar LOST/TIMED_OUT cliente
   sin bloqueo de la aplicación. Repetir 10 veces.
9. Medir en cada PC latencia/RTT cuando exista instrumentación,
   pérdida real/efectiva, bytes/s, CPU, RAM, jitter y contador de
   datagramas rechazados. No inventar métricas ausentes.
10. Ejecutar versión GUI tras integración de Skin/Bones/Eye y repetir
    la secuencia a partir del menú. Adjuntar vídeo/capturas y SHA.

Plantilla de informe:
  SHA_COMMIT:
  Windows versión/build:
  PC_A IP (ocultable en informe público):
  PC_B IP (ocultable):
  EXE hash/tamaño:
  Sesión inicio/fin y segundos de operación:
  Repeticiones y reconexiones:
  Estado e IDs actor1/actor2:
  Errores Skin (phase/error):
  Bytes tx/rx, paquetes perdidos, RTT/jitter si medidos:
  Capturas/logs:
  Resultado PASS/FAIL y defecto asociado:

## Contrato para integración UI/host
Bones inicia mundo y crea actor remoto antes de mouth_session_create;
mouth_session_create antes de bones_mod_register_builtins;
bones_mod_register_builtins UNA VEZ y bones_mod_host_start;
bones_mod_host_step a 60 Hz. Mouth sólo somete comandos autenticados
de forma *limitada a sesión LAN*, no eventos de gameplay.
Skin traduce phase/error de MouthSessionInfo; no abre sockets desde UI.
El cliente llama client_controls/poll, y luego copy_snapshot si ready.
Eye dibuja ambos actores, no modifica estado autoritativo.
Cerrar destroy(host) y desconectar, evitando liberar contexto antes
de las callbacks del host. No hay manejo de DLL obligatorio.

## Validación Soul
No emitir informe de integración gráfica aprobado hasta que Bones y
Skin estén realmente fusionados e Eye represente snapshots. Soul debe
revisar SHA conjunto, CI Windows/Linux, fuga de autoridad, roles
duplicados, orden de arranque, presupuesto PE final y test LAN de PC
físicos pendiente. El reporte de Mouth de esta etapa será PARCIAL.
