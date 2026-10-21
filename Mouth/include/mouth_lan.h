#ifndef MOUTH_LAN_H
#define MOUTH_LAN_H
/* Mouth LAN v0.4.0. Experimental direct UDP (IPv4) for two peers.
   Does not change Bones ModKit ABI1. No external image or asset transport.
   This is not cryptographic authentication or a production netcode. */
#include <stdint.h>
#include "bones/modkit.h"

#define MOUTH_LAN_VERSION 1u
#define MOUTH_LAN_PORT 14444u
#define MOUTH_LAN_MAX_ACTORS 2u
#define MOUTH_LAN_PACKET_MAX 128u
#define MOUTH_LAN_INVALID_SOCKET ((uintptr_t)~(uintptr_t)0)
#define MOUTH_LAN_TIMEOUT_MS 5000u
#define MOUTH_LAN_HELLO_MS 500u

typedef enum {
    MOUTH_LAN_OFF = 0, MOUTH_LAN_CONNECTING = 1,
    MOUTH_LAN_CONNECTED = 2, MOUTH_LAN_REJECTED = 3,
    MOUTH_LAN_DISCONNECTED = 4
} MouthLanStatus;

typedef struct {
    uint64_t actor_id;
    uint32_t owner;
    float x, y, z, yaw;
} MouthLanActor;

typedef struct {
    uint64_t world_id, seed;
    uint32_t generator_version;
    uint16_t port;
    uint64_t remote_actor_id; /* Host only: allocated through Bones Foundation. */
    uint32_t remote_owner;    /* Host only: must match Bones ownership. */
} MouthLanConfig;

/* Public integration contract for Skin/Bones. Never call these on an
   uninitialized context: use MouthLan lan={0} (or memset to zero) once.
   Bones must create the remote actor before calling session_create, and must
   register/start ModKit only AFTER session_create. Host networking opens
   during ModKit start; calling create alone does not bind a socket. */
#define MOUTH_SESSION_API_VERSION 1u

typedef enum {
    MOUTH_SESSION_OK = 0,
    MOUTH_SESSION_INVALID_ARGUMENT = 1,
    MOUTH_SESSION_INVALID_ADDRESS = 2,
    MOUTH_SESSION_SOCKET_ERROR = 3,
    MOUTH_SESSION_BUSY = 4,
    MOUTH_SESSION_NOT_READY = 5,
    MOUTH_SESSION_WORLD_INCOMPATIBLE = 6,
    MOUTH_SESSION_PROTOCOL_INCOMPATIBLE = 7,
    MOUTH_SESSION_TIMEOUT = 8,
    MOUTH_SESSION_CONNECTION_LOST = 9
} MouthSessionResult;

typedef enum {
    MOUTH_SESSION_OFFLINE = 0,
    MOUTH_SESSION_HOST_PREPARED = 1,
    MOUTH_SESSION_WAITING_FOR_CLIENT = 2,
    MOUTH_SESSION_JOINING = 3,
    MOUTH_SESSION_CONNECTED = 4,
    MOUTH_SESSION_RECONNECTING = 5,
    MOUTH_SESSION_INCOMPATIBLE = 6,
    MOUTH_SESSION_TIMED_OUT = 7,
    MOUTH_SESSION_LOST = 8
} MouthSessionPhase;


typedef struct {
    MouthLanConfig config;
    MouthLanStatus status;
    uintptr_t socket_handle;
    uint32_t peer_addr;           /* IPv4 network-byte-order, opaque. */
    uint16_t peer_port;           /* UDP port network-byte-order, opaque. */
    uint32_t client_nonce;
    uint32_t session_id;          /* Session marker, NOT a security token. */
    uint32_t last_input_seq;
    uint32_t next_input_seq;
    uint64_t last_rx_ms, last_hello_ms, last_input_ms;
    uint64_t connect_begin_ms; /* initial or reconnection attempt deadline */
    MouthSessionResult last_error; /* from latest socket/protocol event */
    uint8_t ever_connected;
    uint64_t last_snapshot_tick, next_command_seq;
    int8_t forward, strafe;
    int16_t yaw_milliradians;
    uint8_t sprint, jump;
    MouthLanActor actors[MOUTH_LAN_MAX_ACTORS];
    uint32_t actor_count;
    uint64_t packets_sent, packets_received, bytes_sent, bytes_received;
    uint64_t rejected_packets;
    uint8_t host_mode, socket_active, winsock_started;
    const BonesModServices *services; /* Provided at host ModKit startup. */
} MouthLan;

typedef struct {
    uint32_t size;                  /* caller sets sizeof(MouthSessionInfo) */
    uint32_t api_version;
    MouthSessionPhase phase;
    MouthSessionResult error;
    uint8_t is_host;
    uint8_t socket_open;
    uint16_t port;
    uint64_t remote_actor_id;
    uint32_t remote_owner;
    uint64_t confirmed_tick;
    uint64_t bytes_sent, bytes_received, rejected_packets;
} MouthSessionInfo;

/* Result codes are stable and presentation-free; Skin maps them to text.
   No UI, physics or game-engine work occurs inside these functions. */
MouthSessionResult mouth_session_create(MouthLan *lan,
                                         const MouthLanConfig *config);
MouthSessionResult mouth_session_join(MouthLan *lan,
                                       const MouthLanConfig *config,
                                       const char *host_ipv4);
MouthSessionResult mouth_session_disconnect(MouthLan *lan);
MouthSessionResult mouth_session_get_status(const MouthLan *lan,
                                             MouthSessionInfo *out);
/* Client only; call ~60 Hz. Host polls in ModKit before_tick. */
MouthSessionResult mouth_session_poll(MouthLan *lan);
/* Copy only complete, monotonically confirmed Bones-authoritative snapshots.
   Client renderer consumes this; never overwrite host physics with it. */
MouthSessionResult mouth_session_copy_snapshot(const MouthLan *lan,
                                                 MouthLanActor out[MOUTH_LAN_MAX_ACTORS],
                                                 uint64_t *out_tick);

/* Use one host module per process. Call configure_host before registering
   mouth_lan_query. The context must outlive bones_mod_host_destroy. */
int mouth_lan_configure_host(MouthLan *lan, const MouthLanConfig *config);
int32_t mouth_lan_query(uint32_t abi, BonesModDescriptor *out);
/* Client is a separate executable instance, no Bones simulation on client. */
int mouth_lan_client_open(MouthLan *lan, const MouthLanConfig *config,
                          const char *host_ipv4);
void mouth_lan_client_controls(MouthLan *lan, int forward, int strafe,
                               int yaw_milliradians, int sprint, int jump);
void mouth_lan_client_poll(MouthLan *lan);
void mouth_lan_close(MouthLan *lan);
uint64_t mouth_lan_clock_ms(void);

/* Pure codec used by unit tests, strict fixed width / little endian. */
int mouth_lan_encode_hello(uint8_t *dst, uint32_t cap,
                           const MouthLanConfig *config, uint32_t nonce);
int mouth_lan_decode_hello(const uint8_t *src, uint32_t len,
                           MouthLanConfig *config, uint32_t *nonce);
int mouth_lan_encode_input(uint8_t *dst, uint32_t cap, uint32_t session,
                           uint32_t sequence, int forward, int strafe,
                           int yaw_milliradians, int sprint, int jump);
int mouth_lan_decode_input(const uint8_t *src, uint32_t len, uint32_t session,
                           uint32_t *sequence, int *forward, int *strafe,
                           int *yaw_milliradians, int *sprint, int *jump);
#endif
