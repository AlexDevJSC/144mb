BONES 0.4.0 / MODKIT ABI1 — MOUTH LAN SOURCE PROVIDER
Version Mouth LAN 0.3.1 | 21/10/2026

PROPIETARIO
  Mouth: comunicación y red, no física ni persistencia.

ARCHIVOS QUE CONSTITUYEN EL MÓDULO INSTALABLE
  modkit.c       — punto de entrada bones_mod_mouth_query
  mouth_lan.h    — API interna / tipos / protocolo
  mouth_lan.c    — transporte UDP y comandos/snapshots ModKit
  README.txt     — esta guía

MODO DE INSTALACION ACTUAL (COMPACTO)
  Copiar esta carpeta completa a <Bones>/modules/mouth y compilar BONES.
  scripts/gen_modkit_registry.py detecta el punto de entrada modkit.c.
  El build oficial solo compila el punto de entrada, por eso modkit.c
  incluye deliberadamente mouth_lan.c como una única unidad de traducción.
  NO agregar mouth_lan.c como fuente independiente a ese build: duplicaría
  los símbolos del transporte. El ejecutable se enlaza con ws2_32 en Windows.
  No hay DLL obligatoria ni Python durante la ejecución del juego.
  El directorio Mouth/ del repositorio contiene los originales de desarrollo.
  Regenerar/copias: python Mouth/scripts/sync_bones_module.py --check
  Empaquetar ZIP: python Mouth/scripts/sync_bones_module.py --pack

MODO INDIVIDUAL / MULTIJUGADOR
  Por defecto bones_mod_mouth_query devuelve descriptor Mouth inerte,
  por lo que el juego base puede iniciarse sin configurar red ni sockets.
  Para LAN anfitrión, inicializar un actor Bones propiedad del cliente,
  construir MouthLanConfig con world_id, seed, generator_version, port,
  remote_actor_id y remote_owner y llamar a mouth_lan_configure_host ANTES
  de bones_mod_register_builtins/bones_mod_host_start. En ese caso el
  descriptor adquiere callbacks y envía comandos exclusivamente a Bones.
  La prueba de referencia completa está en Mouth/examples/lan_demo.c.
  No se añade botón "Unirse"/"Crear partida" al ejecutable Win32 normal.
  La demo cliente es de consola: no dibuja una segunda escena de Eye.

API
  BONES_MOD_ABI=1, BONES_ROLE_MOUTH=4, implementation_version=3,
  state_schema_version=0. No se modifican modkit.h, bones.h ni foundation.h.
  Permisos en host LAN: READ_ACTORS, READ_TIME, SEND_COMMANDS.
  Mantener un único proveedor Mouth. Fuentes complementarias se versionan
  en Mouth/ y se sincronizan con este directorio de forma explícita.

LIMITACIONES
  Solo host + 1 cliente UDP LAN de confianza; sin NAT traversal/cifrado,
  sin comprobación completa de guardados/materiales, sin persistencia
  online ni reconciliación, sin carga automática de DLL. Las conexiones
  no tienen autenticación criptográfica. No exponer puerto a Internet.
  Bones conserva autoridad sobre física y eventos, Skin sobre guardados.

ESTADO DE PRUEBAS
  Pruebas Linux y Windows automatizadas desde el GitHub workflow Mouth LAN.
  La prueba Windows real entre dos máquinas no se ha realizado.
  El tamaño total del juego aún debe medirse en el PE final.
