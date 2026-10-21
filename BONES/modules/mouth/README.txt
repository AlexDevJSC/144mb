BONES 0.4.0 / MOUTH 0.4.0 LAN — fuente ModKit ABI 1
Code SHA probado: 7c0506eacc969692eb76ab34893a5aa3fe177e32
Contrato Skin/Bones: Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md

Archivos para instalar: modkit.c, mouth_lan.c, mouth_lan.h.
Exporta bones_mod_mouth_query y usa BONES_ROLE_MOUTH.
Implementation_version=4, state_schema_version=0.
El registro Bones compila sólo modules/*/modkit.c: ese archivo
incluye mouth_lan.c intencionalmente. No compilar la implementación
por segunda vez. El ejecutable enlaza Winsock2 en Windows.
No se requieren DLL ni proceso Python en tiempo de ejecución.

Modo individual: sin llamada mouth_session_create, el descriptor
queda inerte y no abre sockets. Antes de registrar ModKit para LAN,
Bones debe crear el actor remoto owner=2 (ID real asignado por Bones)
y llamar mouth_session_create con world_id,seed,generator_version,
port,remote_actor_id y remote_owner. Al arrancar ModKit se abre UDP.
El cliente usa mouth_session_join con IPv4/port y MISMA identidad
de mundo, poll, get_status, client_controls y copy_snapshot.
Los snapshots se entregan a la capa de presentación, no sustituyen
física ni decisiones de Bones.
Tras destruir BonesModHost, llamar mouth_session_disconnect
si se desea. La operación es idempotente.

El programa gráfico actual no muestra menú multijugador; Skin/Bones
deben integrarlo. Cliente de demo aún consola. Dos procesos UDP
loopback CI PASS Windows y Linux. No se han probado 2 PCs físicos.
No usar en Internet público: sin autenticación criptográfica/NAT.
El juego completo sigue sujeto a presupuesto total de 1.44 MB.
