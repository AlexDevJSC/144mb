# FileSystem4 — creación de enlaces duros mediante Win32 y WAL v3

**Estado:** contrato implementado en rama de revisión; aprobación sujeta a
CI completa, análisis de recuperación y pruebas diferenciales. No equivale
a certificar NTFS universal.

## API y ABI

- `CreateHardLinkW(LPCWSTR lpFileName, LPCWSTR lpExistingFileName,
  LPSECURITY_ATTRIBUTES lpSecurityAttributes)` y variante `A` (ACP vigente)
  se exportan desde `kernel32` y usan `NT_MS_ABI` del runtime.
- Entradas de escritura: **dos rutas DOS autorizadas**; nuevo nombre primero,
  archivo existente segundo. No se aceptan atributos de seguridad no nulos:
  `ERROR_NOT_SUPPORTED` explícito, sin ignorar ACL silenciosamente.
- Por ahora **solo C: privada** del servicio/PE. No se permite crear enlaces
  entre grants D:/UNC, carpetas mapeadas o volúmenes diferentes, ni en
  backing físico de otros UIDs. Los nombres, incluido 8.3/case-folding,
  atraviesan `guest_path`, `open_parent` y `openat2` con
  `RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV`.
- Solo archivo existente **regular**. No se siguen symlinks finales,
  junctions ni directorios. El destino debe estar ausente, incluido bajo
  comparaciones Unicode/case-insensitive, o se devuelve
  `ERROR_ALREADY_EXISTS` (183). Accesos con HANDLEs que deniegan
  `FILE_SHARE_DELETE` fallan conservadoramente (32), y un
  `delete-pending` activo impide la creación. No se entrega ningún FD.
- La semántica de ACL NT en otros grants y atributos de seguridad heredados
  no se simula. Este contrato se limita a las autorizaciones que ya aplica
  el runtime sobre la raíz C: y a la identidad de servicio UID.

## IPC

Se extiende el transporte **v3 existente** (sin alterar tamaños ni offsets
de sus estructuras): `op=24 / FS4_HARDLINK`.

- Petición de 56 bytes, `handle=0`, `offset=-1`,
  `access/share/disposition/flags/reserved=0`, `size=N`.
- Payload: `existing_utf8\0new_utf8`, N <= 2048;
  cada ruta de DOS `C:/...` debe cumplir `valid_path` y el límite
  por componente/longitud que aplica el broker. El socket es
  `AF_UNIX/SOCK_SEQPACKET` autenticado mediante las credenciales
  existentes y nonce de sesión.
- Respuesta de 32 bytes: estado Win32; sin datos, handles o descriptores.
  El worker bajo UID del servicio invoca la API W local sobre el backing.
- Un broker anterior devuelve `ERROR_NOT_SUPPORTED` al opcode 24;
  no existe fallback a `linkat` directo en el PE.

El modo de servicio FUSE mantiene **`fs_link() => EOPNOTSUPP`** para
clientes Linux: permitir `ln` arbitrario aún no forma parte del contrato
de autorización Win32; habilitarlo requerirá una interfaz de permisos
separada. Esta PR añade `CreateHardLinkW` mediante el broker, no
`FUSE linkat` libre.

## Serialización y WAL

Se conserva el WAL físico por grant `:ntnative-file-leases/dos83-rename-v1.wal`.
Su nuevo tipo de registro tiene **magic `N83LNK03`**, la misma cabecera
de 160 bytes, flag `8`, rutas origen/destino y identidades estables del
grant, directorios y archivo. La suma de comprobación FNV-1a sigue
cubriendo la identidad y las rutas. La lectura antigua v1/v2 permanece.

Orden cooperativo: `file_lock -> WAL flock exclusivo -> flock del inode
para FILE_SHARE_DELETE -> gate de delete-pending -> lock de directorio
de alias`. Se sincroniza la intención completa **antes** de `linkat`.
Tras creación se verifica (dev,ino), se sincroniza inode y directorio
de destino, se genera alias DOS 8.3 propio de esa entrada y se
sincroniza el inode; solo entonces se vacía/sincroniza WAL.

Recuperación fría:

1. Intención válida con destino ausente: no sobrevivió un nuevo nombre.
   Liberar WAL solo tras sincronizar su truncado.
2. Destino presente con el inode **exacto** registrado y fuente todavía
   identificable: terminar alias, fsync de directorio y vaciar WAL.
3. Destino extraño, root/directorio sustituido, symlink, metadatos
   truncados o checksum erróneo: **EUCLEAN**, WAL retenido, sin borrar
   ni atribuir otro inode al invitado.
4. Fallos de fsync poisonan estado o dejan la intención visible hasta
   recuperación. SIGKILL nunca implica que la transacción deba
   revertirse; el nombre puede quedar publicado si el syscall tuvo éxito.

La identidad de dos nombres es la del inode común; las claves DOS83
usan identificador del directorio + nombre y permanecen independientes.
El registro share lease es por (dev,ino), por lo que no pueden eludirse
bloqueos mediante otra entrada.

## Regresiones

- `make -j2 check-fs4-hardlink-wal`: dos workers IPC, identidad,
  lectura compartida, bloqueo y liberación, rename/delete, carrera por
  un mismo nombre, rutas maliciosas, pending-delete y seis fases
  **reales** de SIGKILL en un worker *solo test* con recuperación fría.
- `make -j2 check-pe-fs4-hardlink`: PE32+ AMD64 de laboratorio importando
  `CreateHardLinkW` por `kernel32`, comprobando
  `BY_HANDLE_FILE_INFORMATION` y comportamiento de borrado.
- `make -j2 check-fs4-ntfs-diff`: sonda de API Win32 W que exige
  creación, bytes e identidad reales; el comparador JSON mantiene
  todos los campos avanzados no verificados sin declarar paridad total.
- `make -j2 check` y batería independiente FileSystem4 de Actions.

**No se ejecuta** corte eléctrico físico ni Fedora SELinux por deseo
del propietario; no forman parte del PASS automatizado. El usuario
no solicita cerrar la aceptación física, pero es una limitación conocida.
Cualquier SKIP de FUSE/Bubblewrap/host se informa como tal.

## Límites deliberados

- No se implementa `CreateHardLinkTransactedW`, enlaces a directorios,
  cross-volume ni renombrados externos al WAL cooperativo.
- No se concede permiso de creación directa de hardlinks a clientes FUSE.
- Las ACL avanzadas, reparse, otros grants y pruebas diferenciales contra
  un Windows físico de la *misma revisión* siguen siendo extensiones
  separadas; esta implementación no implica equivalencia NTFS completa.
- Los hooks `NT_DOS83_TEST_CRASH` solo se compilan en el broker
  `nt-storage-handle-broker-crash` de test, no en producción.
