# MOUTH 0.4.0 — Network / Session API (Bones ModKit ABI 1)

**Code tested (SHA):** 7c0506eacc969692eb76ab34893a5aa3fe177e32
**CI:** https://github.com/AlexDevJSC/144mb/actions/runs/39606820015
**Full API handoff:** [CONTRATO_SESIONES_BONES_SKIN_v1.md](docs/CONTRATO_SESIONES_BONES_SKIN_v1.md).

Source resides in Mouth/include/mouth_lan.h and Mouth/src/mouth_lan.c.
The installed, statically linked Bones module resides in
BONES/modules/mouth/{modkit.c,mouth_lan.h,mouth_lan.c}.
ModKit ABI remains 1, implementation_version=4, wire protocol M144/1.
Run python Mouth/scripts/sync_bones_module.py --check for source sync.

Public C API: mouth_session_create, mouth_session_join,
mouth_session_disconnect, mouth_session_get_status, mouth_session_poll,
mouth_session_copy_snapshot and mouth_lan_client_controls.
MouthSessionInfo provides UI-agnostic statuses for Skin:
offline, prepared, waiting, joining, connected, reconnecting,
incompatible, timeout and lost, plus typed failure causes.

Singleplayer does not open a socket. Bones owns actor IDs, owners,
movement and physical authority. Host must create remote player owner=2,
then call mouth_session_create BEFORE registering/starting ModKit.
Join takes IPv4/port and Bones-supplied world_id/seed/generator_version.
A client cannot discover an unknown procedural world by IP yet.
Client snapshots are for representation (Bones/Eye), not for restoring
host-authoritative physics.

Compile from repo root with Visual Studio x64 developer console:
    Mouth\scripts\build_windows_msvc.bat
    BONES\scripts\build_windows_msvc.bat
Test in LAN: run Mouth\build\mouth_lan_demo.exe host 14444
and on a second process:
    Mouth\build\mouth_lan_demo.exe join 192.168.1.10 14444
Use the real host LAN IPv4; permit UDP 14444 only in trusted private LAN.
The current client demonstration is CONSOLE, not 3D.

Linux tests:
    make -C Mouth test
    make -C Mouth loopback
    make -C BONES modkit_test
Negative tests include incompatible version/world, no host timeout,
invalid IP, graceful exit and forged control packet without session.

Limitations: one host and one remote client; UDP IPv4, no encryption,
NAT traversal, internet auth, full persistence or multiplayer save merge.
No physical two-PC network validation or complete game-graphics playtest.
The 1.44 MB constraint is for the entire final game, not this demo.
Skin/Bones integration and Soul verification are still outstanding.
