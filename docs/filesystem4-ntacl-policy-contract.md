# FileSystem4 — núcleo de políticas DACL (primera capa verificable)

**Alcance:** motor binario puro de validación/evaluación de
`SECURITY_DESCRIPTOR_RELATIVE` y construcción de DACL heredadas, diseñado
para su incorporación posterior a todas las rutas de mutación PE/FUSE.

**No es todavía `SetFileSecurityW`, ni supone que una ACL guardada se
esté aplicando al abrir, escribir, renombrar o borrar archivos.**

## Contrato de entradas

- Descriptor de seguridad Windows *self-relative*, revisión 1, hasta
  2048 bytes; offsets DWORD-aligned y dentro del buffer, owner/group
  SID de longitud válida, DACL explícita, ACL revisión 2/4, hasta 128 ACE.
- SID con autoridad de 48 bits y entre 0 y 15 subautoridades. **Los SID
  pueden ser arbitrarios**: el evaluador no depende de un catálogo
  codificado, compara bytes con los SID presentes en el token autenticado.
- ACE soportadas: `ACCESS_ALLOWED_ACE_TYPE` (0) y
  `ACCESS_DENIED_ACE_TYPE` (1), en orden de aparición y con máscara
  de acceso de 32 bits. La primera denegación que cubra derechos aún
  pendientes rechaza; los ALLOW reducen derechos pendientes. Las ACE
  `INHERIT_ONLY` no afectan el acceso al objeto actual.
- Flags `OBJECT_INHERIT`, `CONTAINER_INHERIT`,
  `NO_PROPAGATE_INHERIT`, `INHERIT_ONLY`, `INHERITED_ACE` admitidos
  con comprobación de bits reservados.
- NULL DACL, DACL ausente, SACL, ACE object-specific/callback/claims,
  SID malformados, flags de auditoría en DACL, offsets solapados y ACL
  truncadas o con padding inesperado se **rechazan**. En particular
  *una ACE desconocida nunca se ignora como si concediese acceso*.
- Token: hasta 32 SID previamente **autenticados por quien llama al
  evaluador**. El motor NO fabrica un token Windows a partir de un UID
  Linux ni acepta un SID de identidad enviado por un PE sin
  autenticación. Llamar `fs4_acl_check` sin token válido falla.

## Herencia

`fs4_acl_inherit` construye un descriptor independiente con owner
`S-1-22-1-uid`, grupo `S-1-22-2-gid` y DACL heredada:

- Archivo: ACE con `OI`, se eliminan OI/CI/NP/IO y se marca
  `INHERITED_ACE`.
- Directorio: ACE con OI o CI; las que tienen OI sin CI permanecen
  `INHERIT_ONLY` para seguir propagándose; con NP se eliminan las
  marcas propagables en el hijo.
- `CREATOR_OWNER` (`S-1-3-0`) y `CREATOR_GROUP`
  (`S-1-3-1`) se sustituyen por el propietario/grupo autenticado del
  hijo sin escribir fuera del buffer.
- Si no se heredan ACE, se produce una DACL **vacía**, que deniega
  acceso por defecto. No se inventa una DACL de concesión total.
- La función no realiza mutación en disco ni cambia la DACL parental.

## Persistencia interna de DACL (todavía no expuesta al invitado)

La segunda pieza interna `fs4_acl_storage.c` puede almacenar un
`SECURITY_DESCRIPTOR_RELATIVE` **ya validado** como
`user.ntnative.ntacl-v1` sobre un inode fijado. Su escritura comprueba
tipo regular/directorio y confirma `fsync` antes de éxito; su lectura
rechaza metadatos corruptos o mayores de 2048 bytes con `EUCLEAN`,
en vez de caer silenciosamente al descriptor POSIX básico.

`fs4_acl_storage_inherit_trusted` toma la DACL del directorio padre,
construye la del hijo con los SID `S-1-22-1/2` del propietario/grupo
autenticado y escribe el xattr **en un inode O_TMPFILE no publicado**.
La nueva prueba crea y publica después ese inode, reabre el nuevo nombre,
verifica permisos y rechaza un parent xattr corrompido.

**Advertencia de seguridad:** este helper no autentica el llamante,
no implementa `WRITE_DAC` ni hace cumplir la DACL en `CreateFileW`,
FUSE, rename, delete o accesos existentes. Es código interno de
persistencia y **no se ha expuesto en el broker ni a aplicaciones PE**.
El integrador final debe retener el WAL de namespace y evaluar los
derechos antes de invocarlo. `fsync` no constituye por sí solo
atomicidad multioperación ni recuperación ante corte eléctrico.

La prueba `make check-fs4-acl-storage` requiere xattrs y
`O_TMPFILE` del backing; si la plataforma no ofrece staging,
imprime **SKIP** y no simula éxito de herencia antes de publicación.

## Trabajo que sigue bloqueado

El servicio IPC v3 aún necesita:
1. Identidad de token Windows autenticada, con SID de usuario, grupos,
   privilegios, owner override y ACL de objetos (NO equivalente a
   `SO_PEERCRED.uid`).
2. Almacenamiento protegido y durable con WAL, y autorización real
   `READ_CONTROL` / `WRITE_DAC`; tratamiento de claves heredadas en
   create/mkdir antes de publicación.
3. Enlace del evaluador a **todos** los caminos de autorización de
   Win32, FUSE, rename, delete, metadatos y handles ya abiertos, sin
   nuevos bypasses de ambient UID.
4. Semántica de ACE objeto/callback/condicional, SACL, herencia
   automática/protegida, generic mapping y derechos especiales para
   reclamar compatibilidad NTFS integral.
5. Fixtures Win32 contra Windows real y estrés de accesos entre procesos.

**No conectar `SetFileSecurityW` para devolver TRUE mientras los puntos
anteriores no garanticen que el descriptor se hace cumplir.**

## Pruebas

`make -j2 check-fs4-acl-policy` ejecuta un binario C con `-Werror` y
comprueba DACL con SID arbitrarios, DENY/ALLOW, owner/group,
CreatorOwner, OI/CI/NP, herencia en archivos/directorios, corrupción de
offsets, NULL DACL, ACE desconocidas, longitudes y datos adversarios.

CI `filesystem4-linux` lo ejecuta además de las pruebas de broker
existentes; registrar resultado exacto antes de cualquier afirmación
de implementación terminada.
