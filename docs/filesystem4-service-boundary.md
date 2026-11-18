# FileSystem4/P4 — aislamiento del backing con identidad de servicio

Estado: existe una implementación opt-in de UID de servicio separado
para clientes POSIX mediante FUSE, con socket Unix autenticado y backing
privado (véase filesystem4-dedicated-storage-service.md). No se han instalado
reglas SELinux ni verificado físicamente el montaje Fedora; tampoco existe
todavía el adaptador IPC de handles del runtime PE. Por tanto NO hay garantía
universal para todos los clientes Windows/Linux.

## Condición que falta

Hoy nt-dos83-fuse, nt-dos83-mediator y el runtime PE acceden al mismo
árbol Linux real. Landlock sólo limita procesos que atraviesan su propio
launcher y descendientes. Si el cliente posee los inodos del backing,
incluso permisos 0500 no son una barrera: ese UID puede cambiar chmod
o acceder por otro bind/mount. Los flocks/OFD son cooperantes.

Un servicio que use UID dedicado sin adaptar el runtime PE tampoco
resolvería la situación: pe-isolated-run necesita manipular C: y el
registro OFD. Cambiar solamente el propietario rompería funcionalidad
o forzaría de nuevo permisos amplios inseguros.

## Arquitectura completa prevista: implantación parcial

1. Proceso de almacenamiento de confianza como UID dedicado, con backing
   privado (0700, sin grupo común con clientes). Administrador configura
   DAC y política SELinux explícita; ningún sudo automático del juego.
2. Única entrada de clientes mediante socket Unix autenticado: SO_PEERCRED
   (UID/GID/PID), permisos y token/capacidad por sesión, expiración y
   revocación; prohibido conceder autoridad por un PID/TID sin contexto.
3. Servicio abre raíces mediante FDs fijados, openat2
   RESOLVE_BENEATH|NO_SYMLINKS|NO_XDEV y verifica identidad antes y después
   de mutaciones. Gestiona journal, xattrs, share leases y locks sobre
   inodos físicos originales.
4. El runtime PE utiliza un adaptador de almacenamiento mediante IPC
   para abrir, leer, escribir, renombrar, truncar y bloquear. Ni el PE
   invitado ni el lanzador host reciben un bind escribible del backing
   original. El sandbox Bubblewrap sigue independiente del supervisor
   de hilos del núcleo.
5. El daemon FUSE actúa como otro cliente autenticado del mismo servicio,
   con mapeo explícito de identidades y garantías de coherencia del
   caché para archivos NT marcados; FUSE allow_other, default_permissions
   y etiqueta SELinux requieren revisión y denegación comprobada.
6. Cambios de credenciales, sesiones expiradas, procesos que mueren,
   caída del servicio y reinicio jamás permiten que el acceso externo
   pase silenciosamente al backing directo.
7. Prueba adversaria obligatoria: un cliente Linux fuera de la sesión
   intenta open/renameat2/unlinkat/truncate/xattr sobre el backing, incluso
   con symlinks, FDs heredados, hardlinks y montajes alternativos. Todo
   debe resultar denegado por DAC/SELinux salvo capacidad de servicio.

## Auditoría implantada (lectura exclusivamente)

    python3 scripts/fs4-backing-audit.py --lower /carpeta/C: --view /carpeta/view
    make check-fs-backing-audit

Registra propiedad UID/GID, permisos, SELinux visible y tipo de montaje
FUSE en el namespace del proceso. Si el UID del cliente es dueño del
backing, la prueba identifica una ausencia de separación. El comando
devuelve **3 = incompleto** incluso si no detecta ese problema: no puede
demostrar reglas de servicio, etiquetas SELinux, ni negación real a otro
UID sólo leyendo stat/mountinfo. No se debe interpretar su salida como
certificación de un servicio inexistente. No realiza cambios de estado.

## Hitos pendientes tras el servicio Linux

A. Especificación binaria IPC versionada, estado de sesión, control de
capacidad y pruebas de autenticación adversaria.
B. Migración del backing y WAL a servicio UID separado, con SELinux
Enforcing y sin acceso directo del runtime PE o FUSE.
C. Sustituir operaciones de archivo PE y FUSE por backend común
sin perder semántica de handles, borrados pendientes ni Ids.
D. Matriz bajo apagados inesperados, recuperación, multicliente,
regresión Windows y denegación del UID host no autorizado.

No procede declarar terminado el aislamiento sin todos esos hitos.

## Broker Win32 HANDLE v3 (18-11-2026)

Nuevo proceso C de confianza `nt-storage-handle-broker` bajo el UID
`ntnative-storage`: recibe el socket autenticado por SO_PEERCRED en el
servicio y ejecuta las APIs Win32 reales `nt_CreateFileW`, `nt_ReadFile`,
`nt_WriteFile`, `nt_CloseHandle`, `nt_DeleteFileW`, `nt_MoveFileExW`,
`nt_SetEndOfFile` y `nt_LockFile/nt_UnlockFile`. De esta forma conserva
el mismo sistema físico de leases OFD, delete-pending y WAL del runtime;
**no** hay ficheros temporales ni envío de FD de backing por SCM_RIGHTS.

El contrato binario fijo v3 se prueba con sockets Unix de tipo SEQPACKET.
Cada conexión tiene nonce de sesión, como máximo 32 capacidades de handle
aleatorias, 256 peticiones, 2048 bytes por E/S y 120 segundos de vida.
Una petición con nonce incorrecto revoca la conexión. Las rutas C:/ se
validan y el backend mantiene la restricción de raíz por FD del runtime.
En producción consulta la vista FUSE viva en cada operación y el servicio
mata trabajadores al detenerse. Los binarios confiables exigen propiedad
root y no ser escribibles por clientes.

**Desactivado por defecto:** requiere la opción explícita de administrador
`NT_STORAGE_EXPERIMENTAL_HANDLES=1`. El PE aún **no utiliza** ese protocolo:
ni el supervisor `--service-socket --pe` ni Bubblewrap levantan un PE usando
esta vía. Integrar el adaptador PE, ampliar funciones (atributos, directory
enumeration, reparse y ACL), probar 2/4 procesos y Fedora SELinux Enforcing
son requisitos previos para habilitar cualquier modo PE generalista.

## Lanzador PE experimental por IPC

`nt-fs-supervisor --view /run/ntnative-storage/view --service-socket
/run/ntnative-storage/control.sock --pe --experimental-pe-ipc -- game.exe`
requiere el socket v1 autenticado, el handshake v3 activo con UID de
servicio verificado mediante SCM_CREDENTIALS, FUSE vivo, Bubblewrap y la
instalación administrada que permite `NT_STORAGE_EXPERIMENTAL_HANDLES=1`.
No admite `--lower` ni comparte otros directorios/raíces DOS/UNC.

El launcher crea `/game` con `--tmpfs`, monta sólo el ejecutable PE en
lectura y un socket del broker en `/run/ntnative-storage/socket`. Ya no
monta el C: físico en este modo. El runtime redirige CreateFile/ReadFile/
WriteFile/CloseHandle/DeleteFile/MoveFile/SetEndOfFile/SetFilePointer/LockFile
al IPC versionado. Algunas operaciones como FindFirstFile, atributos y
security descriptors aún se rechazan explícitamente: sin ellas un PE real
puede fallar en vez de fingir compatibilidad. El modo PE general sigue
desactivado por defecto.

Es imprescindible ejecutar pruebas adversarias en Fedora físico (SELinux
Enforcing, montajes alternativos, cierres y reconexiones) antes de
considerarlo seguro para uso de propósito general.

## FUSE: leases OFD también para archivos no marcados

En modo de servicio UID, FUSE adquiere ahora el registro OFD de FILE_SHARE
para todos los opens/creates/truncates, sin depender de un xattr opt-in.
Además usa direct_io y evita que la caché de página salte los chequeos de
locks por rango. El nuevo test de C: PE debe comprobar conflictos cruzados
con un cliente Linux de la vista física en Fedora; un CI sin montaje FUSE
no prueba esta garantía. La ventana de creación antes de adquirir lease
aún requiere una transacción unificada para poder declararse atómica.
