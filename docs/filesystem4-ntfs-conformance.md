# FileSystem4 — Compatibilidad NTFS avanzada: frontera verificable (P3)

Estado: PROTOTIPO, sin equivalencia certificada con Windows/NTFS. No
debe expresarse como «todo NTFS implementado» ni incorporarse a
porcentajes absolutos basados sólo en CI.

## Matriz por comportamiento

| Área | Implementado hoy en el runtime | Contrato pendiente |
| --- | --- | --- |
| Entrada Unicode | API W con UTF-16; API A con ACP inicial CP1252; plegado Unicode simple de ICU, ambigüedad rechazada | ACP dinámico/DBCS uniforme, equivalencia de normalización/casefold NTFS y nombres reservados/extensos contrastados en Windows |
| Alias DOS 8.3 | Alias por entrada de directorio en xattrs; asignaciones cooperantes con locks; renombrado del broker con WAL checksummed P1 | Cuotas, algoritmo de colisión exacto NTFS, nombres generados perezosamente con journal, todo rename/unlink/hardlink bajo WAL |
| Junctions | MOUNT_POINT sobre xattr v1 compacto y lectura histórica, destino dentro de mismo grant, no seguir symlinks Linux | Reparse tags adicionales, cadena de junctions (si se autoriza), control de ACL heredadas y semántica NTFS avanzada |
| Reparse DELETE | Sólo tag MOUNT_POINT y cabecera de 8 bytes, con reserved estrictamente cero (nuevo P3) | Validación diferencial de buffers y errores NTFS reales |
| Permisos | GetFileSecurity read-only de UID/GID/modo Linux; rechazo seguro de rutas fuera de grants; atributos Win32 xattr | ACL/DACL NTFS completas, SetFileSecurity y herencia ACE, SACL, privilegios, ID mapping, atributos de seguridad fuertes |
| Locks y compartir | OFD FILE_SHARE_READ/WRITE/DELETE, delete-pending limitado y locks de byte; FUSE sólo para archivos opt-in | Rango obligatorio frente a proceso externo no cooperante, oplocks, SMB leases, share/delete-pending completo en FS |
| Concurrencia | Locks por entrada/inodo, FUSE para clientes explícitos, tests multiproceso y WAL de rename cooperante | Garantía global frente a writers POSIX directos, SELinux+UID servicio, revisión de FUSE page cache/mmap, power-loss recovery |
| E/S | OVERLAPPED/CancelIoEx/IOCP acotados a archivos runtime | Todos los tipos NT, cancellation tras despacho, APC, pipes/sockets en IOCP, límites NT propios |

Estos son límites de FileSystem documentados en
docs/filesystem3-hardening.md, docs/filesystem-native-folders.md y
docs/filesystem-supervised-session.md. Los cambios de Núcleo3/hilos,
APC y planificador no corresponden a FileSystem4.

## Reglas nuevas verificables para FSCTL_DELETE_REPARSE_POINT

FSCTL_DELETE_REPARSE_POINT de directorio sólo acepta 8 bytes exactos:
tag 0xA0000003, ReparseDataLength=0 y Reserved=0. Un buffer que invoque
otro tag o manipule Reserved no borra la marca. El test de compatibilidad
vuelve a hacer GET tras el rechazo y confirma que la junction continúa
siendo legible. No se introducen nuevos tags ni nuevas rutas.

## Orden de ampliación de P3 tras P1 y P2

1. Crear fixtures PE independientes de comparativa Win32 (atributos,
   tamaños, códigos de error y estado de handles); registrar el resultado
   sobre Windows real y sobre NT144 con formato estructurado.
2. Completar ACL/sid mapping mediante contrato explícito, nunca
   reinterpretar chmod de Linux como una DACL NTFS completa.
3. Endurecer cambio de nombre/reparse/copia sin cruzar grants.
4. Ejecutar secuencias de carga real y simultánea de 2/4 procesos:
   renombrar, abrir, lock, truncar, unlink y reinicio concurrente.
5. Sólo cerrar cada subconjunto con pruebas verdes de código, fixture PE,
   Fedora Enforcing y Windows de referencia.

Este documento no asigna porcentajes ni garantiza Windows compatibility
más allá de los fixtures ejecutados.

## Matriz diferencial v2 — ACL y reparse (18-11-2026)

La captura Windows real se amplía con lectura de propietario del descriptor,
prueba reversible de ACE de denegación sobre un archivo descartable y
comprobación de presencia de reparse de junction y symbolic link. El
capturador registra `null` cuando el host no puede probar una función por
falta de privilegios; no equivale a `false` ni a PASS.

El probe NT144 comprueba `GetFileSecurityW(OWNER|GROUP|DACL)` y su
`SECURITY_DESCRIPTOR_RELATIVE`. Las ACE de denegación explícita, las
junctions y los symlinks todavía permanecen sin fixture comparable (`null`).
El comparador v2 **no** declara equivalencia global mientras falte una
observación o una referencia Windows v2. El esquema histórico v1 sigue
siendo legible, pero nunca satisface `complete` de la matriz avanzada.

Esto crea una puerta de medición, NO implementa ACL NTFS completas ni
una equivalencia de reparse/seguridad; no ejecutar el script de Windows
fuera de una máquina NTFS real y un directorio de prueba desechable.
