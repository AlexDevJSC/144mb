# FileSystem4 — FUSE/UID: metadatos ordinarios con control de leases

**Estado:** PR de desarrollo. No equivale a montar FUSE en Fedora físico
ni implementa ACL NTFS editables.

## Semántica habilitada

- \`utimens\` en archivos regulares: \`UTIME_NOW\` y \`UTIME_OMIT\`
  admitidos; rango FILETIME 1601+ y resolución 100ns validada.
  Se abre un HANDLE Win32 real de escritura en el propio UID del
  servicio y se llama a \`SetFileTime\`, que toma el WAL de C:,
  comprueba sus permisos de HANDLE y sincroniza el inode antes de
  responder. Un HANDLE con \`FILE_SHARE_WRITE\` denegado provoca error,
  nunca bypass mediante \`futimens\`.
- \`utimens\` en directorios: se usa FD fijado de directorio abierto con
  \`openat2\`, bajo mutex FUSE/WAL del namespace y \`fsync\` del FD.
  No se trata como un HANDLE regular. Las rutas con symlink intermedio,
  reparse o cruce de mount quedan denegadas por la frontera existente.
- \`setxattr/removexattr\`: exclusivamente \`user.*\` normal, con tamaño
  de valor <= 4096 bytes y flags validados. WAL/namespace_lock
  cubre el syscall y el fsync; sobre archivos regulares se exige
  además un lease OFD \`NT_FS_WRITE | NT_FS_SHARE_ALL\` para no saltar
  \`FILE_SHARE_WRITE\` o delete-pending. En directorios aplica guard
  de namespace/WAL; no se simula un lease de archivo.
- \`user.ntnative.*\` es namespace interno del runtime, **oculto**
  para lectura/lista y denegado para escritura/borrado. \`security.*\`,
  \`system.*\`, \`trusted.*\` y ACL POSIX no son permisos concedidos a
  los clientes FUSE. El backing sigue 0700 y del UID servicio.
- El FUSE \`chmod\` **permanece EPERM** para el servicio: cambiar el DAC
  físico del backing permite deshacer la frontera de seguridad. Una
  implementación futura necesitará un modo virtual auditado y evaluado
  también en cada ruta Win32, no solo mostrar \`st_mode\` distinto.

## Fallos y durabilidad

El WAL de namespace ya existente es una **barrera de serialización y
admisión**, no un registro undo universal de cambios de xattr/timestamps.
\`fsync\` completado precede el resultado de éxito. Si falla fsync
después del syscall, la mutación puede estar observable parcialmente y
se devuelve error: no se inventa atomicidad. La caída del proceso no
prueba pérdida de alimentación, y los flocks solo gobiernan clientes
cooperantes en el servicio UID.

La política de seguridad NTFS por SID/ACE arbitrarios, herencia completa,
\`READ_CONTROL/WRITE_DAC\`, SACL y tokens Windows autenticados no forma
parte de este cambio. Aceptar una DACL editable sin hacerla cumplir en
todas las rutas PE/FUSE sería un bypass de seguridad.

## Pruebas

\`\`\`sh
make -j2 check-fs4-fuse-metadata
make -j2 check-fs4-handle-broker check-dos83-fuse
make -j2 check
\`\`\`

\`--selftest-metadata BACKING\` exige un usuario no-root y backing
owner-only y **no monta FUSE**. El test invoca las operaciones reales
FUSE en modo de servicio test: lectura/escritura/borrado xattr, época
FILETIME, fsync, denegación de xattrs reservados y un HANDLE Win32
que niega \`FILE_SHARE_WRITE\`. La CI separada registra SKIP de montajes
físicos. La batería Fedora física se actualiza para exigir las operaciones
positivas, pero no se ha ejecutado aquí.
