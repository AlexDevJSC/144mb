# FileSystem4 — auditoría de mutaciones y durabilidad (18-11-2026)

Esta tabla refleja los caminos concretos de la rama PR #185, no un compromiso
de compatibilidad NTFS completa. C: significa backing físico servido por UID
dedicado. El guard WAL sigue siendo cooperativo; ningún flock puede imponer
bloqueos a escritores que posean acceso físico fuera de la arquitectura.

| Operación | Frontera y protección | Estado/prueba |
|---|---|---|
| CreateFile, CREATE_NEW/ALWAYS, OPEN_ALWAYS, truncado Win32 | file_lock → WAL → OFD FILE_SHARE → fsync inode/directorio | Mediado, test concurrente, SIGKILL tras apertura |
| FUSE create, mknod regular (modo servicio) | O_TMPFILE → lease OFD → linkat → fsync → alias, bajo WAL | Sin publicar sin lease, test 4 etapas SIGKILL; no fallback O_CREAT |
| FUSE open(O_TRUNC), truncate | WAL retenido y OFD share/range locks | Rechazo con WAL corrupto y conflicto share |
| FUSE read/write, release, fsync | FD adquirido con lease en open; direct_io evita cache de páginas; OFD lock por rango | Sin WAL por bloque de datos; fsync explícito para durabilidad; power cut no probado |
| FUSE unlink, rmdir, rename | Delegación al Win32 runtime; ChangePath retiene ahora WAL en C: y MoveFileEx usa journal | SIGKILL de namespace + test broker rename/delete; recuperación Win32 |
| FUSE mkdir | WAL + namespace_lock + alias y fsync | Pruebas SIGKILL legacy; no journal independiente de mkdir FUSE |
| FUSE linkat | Legacy: WAL + refresh; servicio UID: EOPNOTSUPP | Rechazo hasta contrato FILE_SHARE_DELETE/hardlink mediado |
| FUSE chmod | Servicio UID: EPERM; legacy: chmod POSIX | Permisos físicos del servicio no modificables por cliente |
| FUSE utimens, setxattr, removexattr | Servicio UID: EOPNOTSUPP | Se deniegan: no hay aún evaluación SID/ACL + transacción de metadatos |
| FUSE getxattr/listxattr | Sólo lectura, oculta user.ntnative.* | Smoke de privacidad y aceptación Fedora |
| Win32 SetFileAttributes | Guard WAL C: + fsync de inode tras chmod/xattr | SIGKILL post operación del broker |
| Win32 SetEndOfFile / NtSetInformationFile EOF / FileInfoByHandle clase 6 | Mismo helper con file_lock/WAL/OFD range lock y fsync | Test native set, SIGKILL tras EOF |
| Win32 SetFileTime / FileInfoByHandle clase 0 | Legacy modifica inode; remote HANDLE sin implementación específica | PENDIENTE: transacción con timestamps, controles de privilegio y crash |
| Win32 SetFileInformationByHandle clase 4 (delete-pending) | Registro durable de pending por inode y OFD lease | Recuperación al reinicio; revisar interacción con rename bajo WAL |
| CopyFile/CopyFileEx | Secuencia CreateFile/WriteFile/DeleteFile | NO atómico ante SIGKILL/corte; destino parcial posible |
| CreateSymbolicLink / reparse | Capa de enlace simbólico; guard WAL por symlink | Reparse restantes y journaling completo PENDIENTE |

## Inyección de fallos disponible

* `NT_FS4_TEST_CRASH` sólo se usa en worker `--test`; fases `after-open`,
  `after-write`, `after-rename`, `after-delete`, `after-attribute`,
  `after-native-set`. `tests/fs4_handle_broker_test.py` reinicia un worker
  frío y comprueba datos, alias, limpieza de locks y permisos observables.
* `NT_FS4_FUSE_TEST_CRASH` sólo está armada dentro de `--selftest`; fases
  `post-lease`, `post-link`, `post-fsync` y `post-alias` del staging FUSE.
  `tests/fs4_fuse_stage_sigkill.py` ejecuta SIGKILL, recupera el WAL
  y verifica que una entrada no se publica antes de tener lease.
* Persisten los fixtures de WAL/rename/pendings de `dos83_namespace_sigkill`
  y `dos83_pending_delete_sigkill`; no sustituyen las pruebas físicas.

## Evidencia y límites

Una prueba de SIGKILL verifica muerte del proceso y reentrada del broker.
No prueba pérdida instantánea de energía ni semántica exacta de power-loss.
El modo Fedora físico exige /dev/fuse, SELinux Enforcing, cuenta UID separada
y v3 activo. Su script prueba accesos negativos y recopila etiquetas, pero
las denegaciones pueden venir de DAC; deben revisarse dominio SELinux y AVC.

NO EJECUTADO físicamente en esta sesión: entorno disponible Debian 13,
sin SELinux Enforcing y sin /dev/fuse. Falta también un Windows NTFS real
para pruebas diferenciales y validación de herencia de HANDLE entre procesos.

No reemplazar una prueba real por green de GitHub Actions. La CI actual
puede tener fallos ajenos en check-pe-apc; documentar resultado exacto.
## Orden de locks FUSE multihilo

FUSE create/mkdir/link adquieren ahora namespace_lock **antes** del
flock físico del WAL, como unlink/rmdir/rename. En el hilo que posee WAL,
la asignación de alias usa su gate por directorio sin volver a tomar
dos83_lock, evitando la inversión de locks. `fs_open(O_TRUNC)` y `fs_truncate`
no esperan namespace_lock mientras retienen WAL. No afirmar ausencia de
bloqueos mutuos sin estrés en hardware multihilo.
