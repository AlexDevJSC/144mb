# FileSystem4 — núcleo de políticas DACL (primera capa verificable)

**Alcance:** motor binario puro de validación/evaluación de
\`SECURITY_DESCRIPTOR_RELATIVE\` y construcción de DACL heredadas, diseñado
para su incorporación posterior a todas las rutas de mutación PE/FUSE.

**No es todavía \`SetFileSecurityW\`, ni supone que una ACL guardada se
esté aplicando al abrir, escribir, renombrar o borrar archivos.**

## Contrato de entradas

- Descriptor de seguridad Windows *self-relative*, revisión 1, hasta
  2048 bytes; offsets DWORD-aligned y dentro del buffer, owner/group
  SID de longitud válida, DACL explícita, ACL revisión 2/4, hasta 128 ACE.
- SID con autoridad de 48 bits y entre 0 y 15 subautoridades. **Los SID
  pueden ser arbitrarios**: el evaluador no depende de un catálogo
  codificado, compara bytes con los SID presentes en el token autenticado.
- ACE soportadas: \`ACCESS_ALLOWED_ACE_TYPE\` (0) y
  \`ACCESS_DENIED_ACE_TYPE\` (1), en orden de aparición y con máscara
  de acceso de 32 bits. La primera denegación que cubra derechos aún
  pendientes rechaza; los ALLOW reducen derechos pendientes. Las ACE
  \`INHERIT_ONLY\` no afectan el acceso al objeto actual.
- Flags \`OBJECT_INHERIT\`, \`CONTAINER_INHERIT\`,
  \`NO_PROPAGATE_INHERIT\`, \`INHERIT_ONLY\`, \`INHERITED_ACE\` admitidos
  con comprobación de bits reservados.
- NULL DACL, DACL ausente, SACL, ACE object-specific/callback/claims,
  SID malformados, flags de auditoría en DACL, offsets solapados y ACL
  truncadas o con padding inesperado se **rechazan**. En particular
  *una ACE desconocida nunca se ignora como si concediese acceso*.
- Token: hasta 32 SID previamente **autenticados por quien llama al
  evaluador**. El motor NO fabrica un token Windows a partir de un UID
  Linux ni acepta un SID de identidad enviado por un PE sin
  autenticación. Llamar \`fs4_acl_check\` sin token válido falla.

## Herencia

\`fs4_acl_inherit\` construye un descriptor independiente con owner
\`S-1-22-1-uid\`, grupo \`S-1-22-2-gid\` y DACL heredada:

- Archivo: ACE con \`OI\`, se eliminan OI/CI/NP/IO y se marca
  \`INHERITED_ACE\`.
- Directorio: ACE con OI o CI; las que tienen OI sin CI permanecen
  \`INHERIT_ONLY\` para seguir propagándose; con NP se eliminan las
  marcas propagables en el hijo.
- \`CREATOR_OWNER\` (\`S-1-3-0\`) y \`CREATOR_GROUP\`
  (\`S-1-3-1\`) se sustituyen por el propietario/grupo autenticado del
  hijo sin escribir fuera del buffer.
- Si no se heredan ACE, se produce una DACL **vacía**, que deniega
  acceso por defecto. No se inventa una DACL de concesión total.
- La función no realiza mutación en disco ni cambia la DACL parental.

## Trabajo que sigue bloqueado

El servicio IPC v3 aún necesita:
1. Identidad de token Windows autenticada, con SID de usuario, grupos,
   privilegios, owner override y ACL de objetos (NO equivalente a
   \`SO_PEERCRED.uid\`).
2. Almacenamiento protegido y durable con WAL, y autorización real
   \`READ_CONTROL\` / \`WRITE_DAC\`; tratamiento de claves heredadas en
   create/mkdir antes de publicación.
3. Enlace del evaluador a **todos** los caminos de autorización de
   Win32, FUSE, rename, delete, metadatos y handles ya abiertos, sin
   nuevos bypasses de ambient UID.
4. Semántica de ACE objeto/callback/condicional, SACL, herencia
   automática/protegida, generic mapping y derechos especiales para
   reclamar compatibilidad NTFS integral.
5. Fixtures Win32 contra Windows real y estrés de accesos entre procesos.

**No conectar \`SetFileSecurityW\` para devolver TRUE mientras los puntos
anteriores no garanticen que el descriptor se hace cumplir.**

## Pruebas

\`make -j2 check-fs4-acl-policy\` ejecuta un binario C con \`-Werror\` y
comprueba DACL con SID arbitrarios, DENY/ALLOW, owner/group,
CreatorOwner, OI/CI/NP, herencia en archivos/directorios, corrupción de
offsets, NULL DACL, ACE desconocidas, longitudes y datos adversarios.

CI \`filesystem4-linux\` lo ejecuta además de las pruebas de broker
existentes; registrar resultado exacto antes de cualquier afirmación
de implementación terminada.
