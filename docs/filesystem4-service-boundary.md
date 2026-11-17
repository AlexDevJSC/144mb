# FileSystem4/P4 — aislamiento del backing con identidad de servicio

Estado: investigación de contrato y auditoría de sólo lectura.
No existe todavía un servicio de almacenamiento separado, ni reglas
SELinux instaladas, ni una garantía universal de mediación. Una vista
FUSE por sí sola no revoca rutas/FDs directos de otros procesos.

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

## Arquitectura propuesta, no implementada

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

## Hitos de implementación futura

A. Especificación binaria IPC versionada, estado de sesión, control de
capacidad y pruebas de autenticación adversaria.
B. Migración del backing y WAL a servicio UID separado, con SELinux
Enforcing y sin acceso directo del runtime PE o FUSE.
C. Sustituir operaciones de archivo PE y FUSE por backend común
sin perder semántica de handles, borrados pendientes ni Ids.
D. Matriz bajo apagados inesperados, recuperación, multicliente,
regresión Windows y denegación del UID host no autorizado.

No procede declarar terminado el aislamiento sin todos esos hitos.
