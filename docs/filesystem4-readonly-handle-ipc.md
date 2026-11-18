# FileSystem4 — IPC read-only con UID dedicado

Protocolo experimental v2, exclusivamente de **lectura**. Un cliente autenticado
por el SO_PEERCRED existente abre un SOCK_SEQPACKET y envía
`{"version":2,"op":"session"}`. El servicio exige el montaje FUSE activo,
devuelve un token de sesión aleatorio y sólo permite `open/read/close/end`.
Un `open` de `C:/ruta` devuelve un token aleatorio ligado a esa conexión,
**nunca un FD del backing**. Lecturas pread con offset y máximo 2048 bytes,
base64. Cierre de la conexión revoca todos los handles. Límite de 32 handles,
128 peticiones y 15 segundos.

El directorio físico pertenece al UID del servicio (0700). Las rutas se
resuelven en el servicio mediante FDs de directorio fijados, O_NOFOLLOW,
mismo st_dev, sin seguir enlaces ni permitir directorio de metadatos privado.

NO es aún un adaptador `CreateFileW` de PE; no admite creación, escritura,
truncado, borrado, ACL, share-leases NT ni semántica NTFS completa. El
lanzamiento PE con servicio sigue rechazado. Aceptación de seguridad requiere
nuevos tests de credenciales, reinicios, Fedora real con SELinux y, antes de
escritura, transacciones y leases compartidos con FUSE.

## Admisión de seguridad adicional

En instalación real el protocolo v2 está **desactivado por defecto**;
sólo una configuración explícita y experimental del administrador
(`NT_STORAGE_EXPERIMENTAL_READONLY=1` en el servicio) lo habilita.
El canal rechaza archivos marcados como `user.ntnative.fs_compat` y
cualquier error al comprobar ese xattr: sin contrato NT FILE_SHARE no puede
leerlos eludiendo los leases autoritativos. Esta restricción es temporal,
no un reemplazo del backend NT que falta.
