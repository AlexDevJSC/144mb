# FileSystem4 — servicio de almacenamiento con UID independiente

## Implementado en la rama

- Servicio Python sin privilegios de root bajo la cuenta ntnative-storage,
  distinto del UID cliente. El backing C privado exige 0700 y propietario
  UID del servicio; no se entregan FDs del backing a clientes.
- El servicio arranca nt-dos83-fuse bajo su UID exclusivo. El kernel FUSE
  envía la identidad del solicitante y los 25 callbacks la validan;
  otro UID recibe EACCES. st_uid/st_gid se proyectan únicamente a la
  identidad del cliente en la vista; las escrituras físicas siguen
  perteneciendo al servicio.
- La vista rechaza chmod del backing y escritura de xattrs fuera de user.*,
  incluidos system.posix_acl*, security.* y user.ntnative.*. Esto impide
  abrir el backing 0700 mediante un chmod o ACL concedida por el cliente.
- Control IPC: AF_UNIX/SOCK_SEQPACKET activado por systemd, autenticado por
  SO_PEERCRED, una petición JSON v1 por conexión, máximo 4096 bytes.
  Se admiten status y rename. Rename ejecuta el mediador Win32 confiable
  y su journal DOS8.3; no llama os.rename sin locks/WAL. SCM_RIGHTS
  no se utiliza.
- Al arrancar, el mediador recupera/reconcilia WAL antes de montar FUSE.
  No hay modo degradado cuando falla FUSE o desaparece el montaje.
- No se alteran sistemas existentes ni usuarios del host automáticamente.

## Instalación manual, solo administrador

Requiere Fedora con FUSE3, Python3, systemd, configuración de
user_allow_other en /etc/fuse.conf (decisión explícita del administrador),
SELinux revisado, y los binarios del proyecto compilados.

1. Como root, instalar el paquete sin habilitar ninguna unidad:
       sudo make install-fs4-storage-service PREFIX=/usr/local
       sudo systemd-sysusers /usr/lib/sysusers.d/ntnative-storage.conf

2. Copiar el ejemplo packaging/systemd/ntnative-storage.conf.example a
   /etc/ntnative-storage.conf, propiedad de root y modo 0644. Sustituir los
   UID/GID numéricos por los del único usuario cliente autorizado.
   Añadir expresamente ese usuario al grupo ntnative-clients; cerrar y
   reabrir sesión para actualizar los grupos.

3. El directorio /var/lib/ntnative-storage/C debe ser propiedad de la
   identidad ntnative-storage:ntnative-storage y tener modo 0700.
   Se crea vacío si falta. No usar symlinks ni permisos compartidos.
   Para migrar archivos existentes, primero detener todos los procesos,
   recuperar WAL pendientes y cerrar FDs abiertos. Los alias DOS8.3
   dependen del dev/ino de directorio: copiar entre filesystem cambia
   esas identidades y requiere migración de xattrs, NO solo cp -a.
   Comprobar hardlinks y bind mounts alternativos antes de certificar DAC.

4. Activar SOLO el socket:
       sudo systemctl daemon-reload
       sudo systemctl enable --now ntnative-storage.socket

5. Como usuario autorizado:
       /usr/local/bin/nt-storage-control status
       /usr/local/bin/nt-storage-control rename C:/a/Original.txt C:/a/Nuevo.txt

   Vista POSIX: /run/ntnative-storage/view. El backing físico NO debe
   ser accesible al UID cliente, aunque pueda escribir en la vista.

6. Ejecutar en un Fedora físico:
       python3 scripts/fedora-fs4-service-acceptance.py

## Límites y puertas pendientes

- NO hay todavía adaptador IPC de handles para el runtime PE. NO ejecutar
  PE bajo la cuenta del servicio, ni dar al PE un bind de escritura al
  backing. El modo --pe del supervisor antiguo sigue siendo diferente,
  SIN aislamiento por UID de servicio; la nueva ruta cubre Linux/FUSE.
- La autorización actual cubre todo un UID cliente, no una sesión o token
  por proceso. Un proceso adicional del mismo UID puede usar la vista;
  otro UID no. Revocación de UID requiere cambiar configuración y reiniciar.
- ACL NTFS completas, junctions, copia entre raíces, apagado físico,
  SELinux específico del FUSE, equivalencia con Windows NTFS y captura
  de accesos a través de FDs/montajes alternativos aún NO aprobados.
- FUSE necesita user_allow_other para exponer la vista a un UID distinto;
  si está desactivado, el montaje falla cerrado. No sustituirlo por
  permisos más amplios en el backing.
- El helper fusermount3 puede requerir setuid; no imponer
  NoNewPrivileges ni montar en un namespace privado sin comprobar el
  comportamiento real en Fedora: la vista debe ser visible para clientes.
- Validación CI de IPC y permisos NO equivale a ejecución de FUSE físico,
  ni demuestra tolerancia a pérdida de energía.
