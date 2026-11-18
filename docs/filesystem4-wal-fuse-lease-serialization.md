# FileSystem4 — serialización WAL, FUSE y leases (18-11-2026)

El broker PE toma el flock exclusivo del WAL con el mutex local de handles ya adquirido, antes de creación/truncado y hasta terminar registro FILE_SHARE y fsync.
En el servicio UID, FUSE crea bajo O_TMPFILE, adquiere un lease OFD y sólo entonces publica con linkat(AT_EMPTY_PATH). No hay fallback a O_CREAT cuando el FS no admite el mecanismo.
La asignación de alias con el WAL ya retenido utiliza su gate por directorio sin volver a tomar dos83_lock, evitando una inversión frente a rename (dos83_lock→WAL). En caso de fallo, sólo se revierte una entrada si su dev/ino sigue siendo el publicado.

Pruebas nuevas: dos sesiones en CREATE_NEW concurrente y liberación de FILE_SHARE tras SIGKILL. Las pruebas de CI no reemplazan Fedora físico con SELinux Enforcing.

Límites: no hay protección frente a escritores Linux con acceso directo que eludan WAL y leases; requiere DAC/SELinux real. O_TMPFILE más linkat no constituye recuperación ante pérdida de energía. Las otras mutaciones de xattrs y directorios necesitan auditoría adicional.
La herencia de HANDLE entre procesos continúa denegada: faltan tickets IPC autenticados de delegación; no sería seguro entregar FDs físicos al hijo ni recrear un HANDLE ignorando su OFD lease y offset. Async Win32/IOCP parcial no equivale a APC nativa NTFS.