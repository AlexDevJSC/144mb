# FileSystem4 — política NT ACL unificada y su frontera de seguridad

**Alcance:** integración del motor de seguridad NT en el almacenamiento
privado C: mediado por servicio UID. No sustituye a NTFS ni a Active
Directory. Esta especificación corresponde a la
[PR #219](https://github.com/AlexDevJSC/144mb/pull/219);
no se considera aceptada hasta revisar CI del SHA final.

## 1. Motor, identidad y formato ÚNICOS

El evaluador canónico es `src/runtime/nt_acl_engine.c/.h`. No se
mantiene en producción el motor anterior `fs4_acl.c`. La persistencia
canónica es `src/runtime/nt_acl_storage.c/.h` mediante el atributo
`user.ntnative.security.sd` con cabecera NTSD0001, tamaño, identidad
(st_dev,st_ino), checksum, descriptor self-relative y fsync/readback.

El xattr anterior `user.ntnative.ntacl-v1` **no se migra de forma
silenciosa**: si está presente, con o sin la versión canónica, la lectura
falla con EUCLEAN y deniega el acceso administrado. Una migración
administrativa posterior debe autenticar el inode, recuperar cada ACE y
confirmar la transición. El checksum detecta corrupción accidental,
no es una MAC criptográfica frente al administrador físico.

Se rechazan DACL nulas o ausentes en el registro canónico: en NT pueden
representar concesión indiscriminada y no son una configuración segura
para el servicio. La ausencia total de SD administrado mantiene la
política anterior de DAC del backing/lease; no significa explícitamente
que la DACL haya autorizado el acceso.

## 2. Tokens de sesión

El broker obtiene `SO_PEERCRED` (UID/GID) y `SO_PEERGROUPS` del
kernel, y rechaza listas de grupos demasiado grandes o inconsistentes.
La vista FUSE usa `fuse_get_context` y `fuse_getgroups`. Esas
identidades se convierten en SID sintéticos S-1-22-1-UID,
S-1-22-2-GID, los grupos suplementarios y S-1-1-0 (Everyone).
`DENY_ONLY` no satisface ACE ALLOW. No existe entrada de IPC para
declarar identidades arbitrarias, grupos de dominio o privilegios.

**Límite:** un UID no es una sesión de inicio de Windows ni un
token de dominio. Impersonación, SACL, derechos de sistema y
privilegios de token permanecen no soportados; el código no los
sustituye por el UID del servicio.

## 3. Win32/NT e IPC

El broker contiene un token por sesión y capacidades por HANDLE.
`SetFileSecurityW/A` y `NtSetSecurityObject` aceptan únicamente
`DACL_SECURITY_INFORMATION` y un
`SECURITY_DESCRIPTOR_RELATIVE` validado de tamaño acotado.
La memoria PE se copia de forma protegida mediante
`process_vm_readv`, sin dar punteros guest al broker. Los opcodes
IPC v3 **30** (ruta+SD) y **31** (HANDLE+SD) transportan bytes
acotados y conservan el ABI de 56/32 bytes. El broker verifica
`WRITE_DAC` del peer; un HANDLE no puede elevar permisos al
duplicarse. La seguridad solicitada no altera propietario/grupo
autenticados ni acepta una DACL nula. Solicitar
`WRITE_OWNER`, SACL u otros SECURITY_INFORMATION sin modelo
de privilegios sigue rechazándose.

Antes de modificar un inode, el servicio obtiene el WAL de namespace,
un FD confinado `openat2` o la identidad exacta de un HANDLE ya abierto,
la comprobación de permiso y el lease OFD de compartición.
`fsetxattr` + `fsync` + verificación de relectura ocurren antes del
éxito. Un error de fsync puede dejar un cambio observable; no se
garantiza rollback de pérdida eléctrica.

El broker aplica DACL a apertura, atributos, búsquedas, borrado,
renombrado, hardlink, consultas de seguridad y modificaciones de
metadatos dentro de su contrato actual. Los permisos de DATA de los
HANDLE se verifican al abrir, y los duplicados conservan SAME_ACCESS.
Un usuario ajeno al servicio no recibe FDs Linux ni acceso al backing.

## 4. Herencia antes de publicación

- FUSE regular: `O_TMPFILE` privado, DACL del padre, ACE heredadas,
  lease, fsync, `linkat(AT_EMPTY_PATH)`, directorio fsync y alias DOS.
- FUSE directorio: staging en
  `:ntnative-acl-stage`, DACL heredada antes de renombrado
  `RENAME_NOREPLACE`, fsync del padre y limpieza controlada de huérfanos.
- Broker PE CreateFile en padre con DACL administrada:
  staging O_TMPFILE, comprobación sobre padre e hijo, lease y adopción de
  HANDLE **después** de la publicación durable. No se utiliza un
  O_CREAT visible como fallback cuando hay DACL de padre.

ACE `OI`, `CI`, `NO_PROPAGATE`, `INHERIT_ONLY` y
`CREATOR_OWNER/GROUP` se serializan mediante el motor canónico.
Los tipos de ACE no comprendidos se rechazan, no se pierden.

## 5. FUSE, chmod virtual y límites

El servicio aplica el mismo evaluador a las rutas FUSE relevantes,
incluidos fechas, xattrs user.* ordinarios y acceso a archivos/directorios.
`user.ntnative.*` sigue invisible y no modificable por FUSE.
`chmod` virtual solo transforma una DACL canónica de tres ACE ALLOW
(propietario/grupo/Everyone), bajo WAL, autorización `WRITE_DAC`,
lease y fsync; la vista POSIX proyecta su modo. **Jamás se ejecuta
fchmod del backing del UID servicio.** DACL con ACE DENY o SID nombrado
no puede simplificarse: devuelve ENOTSUP.

Las rutas de FUSE/PE y la CLI que no trabajan sobre el backing
confinado y la identidad autenticada siguen fuera de esta garantía.
El broker PE y los test FUSE offline no certifican un montaje físico.

## Pruebas

- `check-nt-acl-engine`, `check-nt-acl-storage`,
  `check-nt-acl-policy`: offsets, SID largos, DENY/ALLOW, herencia,
  CHECKSUM/inode, identidad de sesión y fail-closed.
- `check-fs4-acl-enforcement`, `check-fs4-acl-setter`:
  IPC real, hardlinks, DACL read/write, owner no suplantable,
  HANDLE SAME_ACCESS, SIGKILL offline tras fsync, cold restart y
  creación PE con herencia.
- `check-fs4-fuse-acl`: operaciones FUSE en modo offline sin montaje,
  herencia de archivos/directorios y chmod virtual sin alterar DAC.
- `check-pe-fs4-security-metadata`: PE MinGW real con importaciones
  ADVAPI32 y rechazo de setter cuando no hay broker autenticado.

**Pruebas que todavía no acreditan paridad NTFS total:** montajes FUSE
físicos, Active Directory, SID de dominio autenticados, token impersonation,
SACL/auditoría, ACE de objeto/callback/condicional, comparación en
Windows con casos concurrentes equivalentes, recuperación ante
pérdida real de alimentación y protección frente a clientes Linux que
eluden deliberadamente el servicio. La CI registra esos límites y no debe
interpretarse como certificación física.
