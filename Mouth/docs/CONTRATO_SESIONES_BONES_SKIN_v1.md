# MOUTH 0.4.0 — Contrato para Skin y Bones (API de sesión v1)

Fecha: 21-10-2026. Compatible con Bones ModKit ABI 1; M144 wire v1.
Implementación y pruebas: commit 7c0506eacc969692eb76ab34893a5aa3fe177e32.
CI verificado Linux/Windows: https://github.com/AlexDevJSC/144mb/actions/runs/39606820015

## Contrato público C11
Cabeceras: Mouth/include/mouth_lan.h (fuente), BONES/modules/mouth/mouth_lan.h (instalado).
Las funciones son independientes de GUI; Skin consume estados, Bones configura el host.

    MouthSessionResult mouth_session_create(MouthLan *lan,
                                             const MouthLanConfig *config);
    MouthSessionResult mouth_session_join(MouthLan *lan,
                                           const MouthLanConfig *config,
                                           const char *host_ipv4);
    MouthSessionResult mouth_session_disconnect(MouthLan *lan);
    MouthSessionResult mouth_session_get_status(const MouthLan *lan,
                                                 MouthSessionInfo *out);
    MouthSessionResult mouth_session_poll(MouthLan *lan);
    MouthSessionResult mouth_session_copy_snapshot(
        const MouthLan *lan, MouthLanActor out[MOUTH_LAN_MAX_ACTORS],
        uint64_t *out_tick);
    void mouth_lan_client_controls(MouthLan *lan,
                    int forward, int strafe, int yaw_milliradians,
                    int sprint, int jump);

MouthLan requiere inicialización a cero ANTES de la primera llamada y vida
estable hasta bones_mod_host_destroy. API monohilo, no es reentrante.
MouthSessionInfo info={0}; info.size=sizeof(info) ANTES de get_status.

## Secuencia exacta: jugar en solitario
No llamar a session_create ni session_join. El descriptor Mouth registrado
queda inerte: sin socket, sin privilegios de red ni callbacks en ModKit.
Bones puede registrar los cinco proveedores sin activar el multijugador.

## Secuencia exacta: crear partida (Bones, autoridad anfitrión)
1. bones_foundation_init(&foundation, world_id, seed, branch_id).
2. bones_foundation_spawn(&foundation, BONES_KIND_PLAYER, 2,
                           spawn_position, &remote_actor_id).
   El actor local preexistente usa ID=1. El remoto pertenece a owner=2.
3. Crear contexto MouthLan persistente, inicializado con = {0}.
4. Rellenar MouthLanConfig: world_id, seed, generator_version
   (= BONES_GENERATOR_VERSION), port, remote_actor_id, remote_owner=2.
5. Llamar mouth_session_create(&mouth, &cfg). Si OK, fase HOST_PREPARED,
   todavía SIN socket abierto.
6. Crear BonesModHost; llamar bones_mod_register_builtins(host)
   (o bones_mod_host_add(host,bones_mod_mouth_query) en host de prueba
   aislado, NO ambas rutas) y bones_mod_host_start(host). El start
   comprueba remote_actor_id/owner con actor_get de Bones y abre UDP.
   Si puerto ocupado, arranque falla y info.error=SOCKET_ERROR.
7. Cada tick: bones_mod_host_step(host, &local_input). El before_tick
   somete controles remotos por BonesModServices.submit_move con
   issuer=2 y actor_id realmente autorizado; Bones aplica físicas.
8. Cada frame Skin puede llamar mouth_session_get_status para pintar
   WAITING_FOR_CLIENT o CONNECTED.
9. Para finalizar el modo host: bones_mod_host_destroy(host), después
   mouth_session_disconnect(&mouth) si se desea (operación idempotente).
   Para recrear una partida se debe construir un nuevo BonesModHost.

Bones debe conservar world_id y seed de la configuración del mundo.
NO inventar campos de BonesEngine ni modificar BonesModServices.

## Secuencia exacta: unirse a partida (cliente)
Skin solicita sólo cadena IPv4 y puerto. Bones debe aportar world_id,
seed y generator_version del mundo ya conocido por el cliente.
ATENCIÓN: M144/1 NO DESCUBRE mundos desconocidos por IP. Se puede unir
por IP/puerto si previamente conoce el mismo mundo; la experiencia
desde menú sin partida cargada necesita futuro descubrimiento versionado.

    MouthLan client={0};
    MouthLanConfig cfg={0};
    cfg.world_id = world_id;
    cfg.seed = seed;
    cfg.generator_version = BONES_GENERATOR_VERSION;
    cfg.port = port;
    MouthSessionResult result=mouth_session_join(&client,&cfg,ipv4);

Cada tick ~60Hz del proceso cliente:
    mouth_lan_client_controls(&client,fwd,strafe,yaw_mrad,sprint,jump);
    (void)mouth_session_poll(&client);
    MouthSessionInfo info={0}; info.size=sizeof(info);
    (void)mouth_session_get_status(&client,&info);

Si info.phase == MOUTH_SESSION_CONNECTED:
    MouthLanActor actors[MOUTH_LAN_MAX_ACTORS];
    uint64_t tick=0;
    if(mouth_session_copy_snapshot(&client,actors,&tick)
           == MOUTH_SESSION_OK) {
        /* Entregar COPIA a representación visual, no a física */
    }

Al salir: mouth_session_disconnect(&client).
Mouth no crea motor físico cliente, no guarda escenas y no rasteriza.
La demo del cliente todavía muestra las transformaciones en CONSOLA;
Skin/Bones/Eye deberán integrar la segunda ventana y su representación.

## Estados para Skin (MouthSessionPhase)
OFFLINE=0: sin sesión.
HOST_PREPARED=1: host configurado, todavía no arrancó el módulo.
WAITING_FOR_CLIENT=2: UDP abierto escuchando.
JOINING=3: enviando HELLO al anfitrión.
CONNECTED=4: sesión confirmada.
RECONNECTING=5: intentando recuperar sesión interrumpida.
INCOMPATIBLE=6: rechazo de mundo o versión.
TIMED_OUT=7: sin respuesta del anfitrión al cabo de 5 segundos.
LOST=8: desconectado, no recuperado o LEAVE remoto.

Causas para Skin (MouthSessionResult):
OK=0; INVALID_ARGUMENT=1; INVALID_ADDRESS=2; SOCKET_ERROR=3;
BUSY=4; NOT_READY=5; WORLD_INCOMPATIBLE=6;
PROTOCOL_INCOMPATIBLE=7; TIMEOUT=8; CONNECTION_LOST=9.

get_status devuelve OK al obtener el estado; el error de sesión se
lee de info.error. El consumidor debe comprobar AMBOS info.phase y
info.error. copy_snapshot devuelve NOT_READY hasta el primer estado
confirmado y completo de dos actores.

## Protocolo, seguridad y límites
HELLO valida world_id, seed y generator_version. Un HELLO wire
incompatible recibe REJECT code 2 (PROTOCOL_INCOMPATIBLE); un
world_id/seed/generator incompatible recibe REJECT code 1.
No es una comprobación criptográfica de assets Eye ni hashes de reglas.
El cliente jamás envía actor_id ni issuer en INPUT: host fija actor
y owner antes del arranque, y filtra endpoint, sesión y secuencia.
Sin autenticación criptográfica: restringido a LAN de confianza.
UDP LEAVE (tipo 6, 12 bytes) es de mejor esfuerzo, sin ACK.
Host y cliente detectan pérdidas tras timeout 5000ms.
Sólo 2 jugadores (1 host, 1 cliente), sin NAT, cifrado, 4 jugadores,
descubrimiento de servidores ni guardado/merge concurrentes.

## Pruebas
Reproducción Linux:
    make -C Mouth test
    make -C Mouth loopback
    make -C BONES modkit_test
Windows MSVC:
    Mouth\scripts\build_windows_msvc.bat
    BONES\scripts\build_windows_msvc.bat

Verificado en CI: dos procesos sobre loopback, acciones del cliente
aplicadas exclusivamente al remoto, actor local separado, mismatch
world/wire, IP inválida, timeout, cierre ordenado y entradas falsas
sin sesión. NO verificado en dos PCs físicos o UI gráfica completa.

## Entrega y dependencias
Skin: implementar botones Individual/Crear/Unirse/Salir y texto de
errores tomando fases y causas anteriores. No editar sockets ni Bones.
Bones: respetar orden de spawn, configurar Mouth ANTES del registro
ModKit y gestionar su ciclo de vida; entregar mundo/semilla al cliente.
Eye: consumir snapshots confirmados, sin texturas transmitidas.
Soul: revisión e integración definitiva sólo DESPUÉS de unificar
Bones+Skin+Eye y repetir la batería de pruebas con ese código.
