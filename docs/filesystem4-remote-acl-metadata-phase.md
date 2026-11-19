# FileSystem4 — siguiente fase: ACL de solo lectura, metadatos y RPC (19-11-2026)

Estado: propuesta en revisión. **No equivale a implementar NTFS ACL
completas** ni a certificar Fedora físico. Base de implementación:
`main` `94655cdcc711bab2277e047f43c9bcd16711124c`.
La PR independiente de hardlinks #204 continúa separada y no está
incluida en este cambio.

## Frontera de seguridad existente

Se utiliza el IPC binario **v3** sin cambiar tamaños de sus estructuras
(56 bytes solicitud, 32 bytes respuesta). Cada sesión valida
`SO_PEERCRED`, nonce, UID del servidor, longitud, cuota y handles
aleatorios; el broker conserva los FDs físicos, sin SCM_RIGHTS ni
fallback al backing directo. El adaptador PE sigue opt-in mediante
`NT_STORAGE_EXPERIMENTAL_HANDLES=1` y su supervisor.

Los nuevos opcodes, intencionalmente separados del 24 reservado a
`CreateHardLinkW` de la PR #204, son:

| Opcode | Operación | Contrato |
| --- | --- | --- |
| 25 | `FS4_SECURITY_PATH` | Ruta UTF-8 DOS C:/ sin NUL, 1–1024 bytes, `access=SECURITY_INFORMATION`, `disposition=capacidad <= 2048`, `size=bytes de ruta`. Respuesta: error Win32, `value=needed`, descriptor relativo de máximo 128 bytes solo en éxito. |
| 26 | `FS4_NATIVE_SECURITY` | Capacidad HANDLE, `access=SECURITY_INFORMATION`, `size=capacidad <= 2048`, sin payload. `value=NTSTATUS`, `handle=needed` (no es una capacidad), bytes solo en éxito. |
| 27 | `FS4_TIME_SET` | HANDLE válido, 24 bytes de tres `FILETIME` nativos (creación/acceso/escritura), máscara de presencia `flags & 7`. Sin FDs ni punteros en tránsito. |
| 28 | `FS4_INFO_GET` | HANDLE válido, clases Win32 0/1/9 y tamaños exactos 40/24/8; resultado binario solo en éxito. |
| 29 | `FS4_INFO_SET` | HANDLE válido, clase Win32 0 (`FILE_BASIC_INFO`, 40 bytes). Otras clases explícitamente rechazadas. |

Las respuestas que reclamen más bytes de los permitidos o un tamaño
incompatible fallan cerradas; el cliente copia al búfer invitado solo
después de autenticación y validación. Las consultas NTSTATUS no alteran
`GetLastError` en el runtime PE.

## ACL: extensión conservadora de lectura

`GetFileSecurityW/A` y `NtQuerySecurityObject` remotos utilizan la
representación de **SECURITY_DESCRIPTOR_RELATIVE** existente del
runtime local para `OWNER_SECURITY_INFORMATION=1`,
`GROUP_SECURITY_INFORMATION=2` y `DACL_SECURITY_INFORMATION=4`.

- La proyección de hasta 128 bytes expresa UID/GID y bits POSIX
  `st_mode` como SID owner/group y tres ACE permitidas. No equivale a
  herencia, grupo suplementario, propietario NT real ni semántica general
  NTFS. **No se crean ACL de escritura.**
- Se rechaza SACL (`ACCESS_DENIED`, falta modelo de privilegios),
  valores desconocidos y POSIX ACL extendidas. La proyección mantiene
  el *fail-closed* del runtime original.
- Una capacidad HANDLE sin derechos suficientes no permite saltarse
  la comprobación del worker. Para NTSTATUS se mantienen los tamaños
  requeridos y errores separados de LastError.
- Las ACL NTFS reales, `SetFileSecurityW`, `NtSetSecurityObject`,
  herencia y SID/ACE arbitrarias siguen **no implementadas**.

## Metadata por HANDLE

`GetFileTime` remoto usa `BY_HANDLE_FILE_INFORMATION` (ya mediado)
para leer tres FILETIME del mismo inode. `SetFileTime` remoto usa el
opcode 27 y no da acceso directo al fichero de Linux.

Local y broker comparten ahora un camino de cambios de fecha para
handles con acceso `FILE_WRITE`: `file_lock`, WAL exclusivo del C:
privado, FD duplicado del inode ya abierto, `futimens` y xattr de fecha
de creación; `fsync` del inode antes de confirmar. Una fecha cero
significa dejar el campo sin modificar.

`GetFileInformationByHandleEx` remoto se amplía para
`FileBasicInfo=0`, `FileStandardInfo=1`,
`FileAttributeTagInfo=9`. `SetFileInformationByHandle` clase 0
reutiliza la ruta de fechas; la fecha de cambio arbitraria, atributos
en esta clase y escritura ACL siguen rechazándose explícitamente.

**Limitación transaccional:** una solicitud multi-campo que falle entre
`futimens` y xattr puede producir un cambio parcial observable. El
objetivo de esta fase es autorización, serialización y persistencia en
éxito, no atomicidad NTFS universal de múltiples atributos. En caso de
fallo de barrera, el WAL permanece protegido/poisoned según el runtime.

## Verificación prevista

```sh
make -j2 check-fs4-security-metadata
make -j2 check-fs4-handle-broker check-fs4-ntfs-diff
make -j2 check
```

El test nuevo comprueba por IPC real: tamaño insuficiente de security
descriptor, DACL/owner/group, denegación de SACL, rutas hostiles,
NTSTATUS y bytes requeridos, permisos de HANDLE de solo lectura,
timestamp fsync, clases 0/1/9, fallo por metadatos no modelados y
lectura desde broker nuevo tras cerrar sesión. Ejecuta
`-W error::ResourceWarning`. Registrar los resultados de GitHub
Actions para el SHA exacto, sin presentar SKIP montado/FUSE como PASS.

## Fuera de alcance

No se modifica FUSE `utimens/setxattr` en UID de servicio: siguen
`EOPNOTSUPP` hasta disponer de una autorización de metadatos común
suficientemente granular. No se amplía la matriz D:/UNC, se evita
modificar el núcleo NT, APC, gráficos o APIs ajenas. El usuario no
prioriza la aceptación física Fedora; no atribuir a CI sus garantías.
