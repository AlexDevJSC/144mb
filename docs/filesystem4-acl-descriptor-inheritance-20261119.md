# FileSystem4 — generación de descriptores hijos y evaluación de DACL

**Estado de esta revisión: módulo ACL interno probado; no autorización NTFS
en producción.** La revisión extiende `nt_acl_engine`, ya presente en
`main`, y evita exponer operaciones de escritura de permisos antes de
tener una identidad NT confiable y una política que cubra PE y FUSE.

## Contratos implementados

- `nt_acl_validate_descriptor` valida el `SECURITY_DESCRIPTOR_RELATIVE`
  reconocido por el motor actual: propietario/grupo, offsets relativos,
  ACL_REVISION 2, ACE de acceso allow/deny, tamaños y SID binarios. SACL,
  ACE de objeto, condicionales, callback y semánticas desconocidas se
  rechazan, no se ignoran.
- `nt_acl_access_check_mapped` permite traducir `GENERIC_READ`,
  `GENERIC_WRITE`, `GENERIC_EXECUTE` y `GENERIC_ALL` utilizando un
  `nt_acl_generic_mapping` aportado **por el servicio**, para el tipo
  concreto de objeto. Máscaras genéricas no mapeadas o mapas recursivos
  siguen `UNSUPPORTED` (sin concesión). Se conserva la API antigua,
  que rechaza GENERIC sin un contrato.
- `nt_acl_inherit_descriptor` recibe el SD padre y SID de propietario y
  grupo **autenticados** por el llamador. Calcula y serializa un SD hijo
  autocontenido, con DACL heredada según OI, CI, NO_PROPAGATE,
  INHERIT_ONLY, INHERITED. Sustituye placeholders `CREATOR_OWNER`
  S-1-3-0 y `CREATOR_GROUP` S-1-3-1 en ACE heredadas, ajustando los
  tamaños y offsets. Una DACL vacía sigue denegando todos los derechos;
  NULL DACL no se interpreta como DACL vacía. La salida informa tamaño
  requerido antes de escribir y rechaza solapamiento directo de buffers.
- No modifica backing, chmod, POSIX ACL, xattrs ni permisos existentes.
  Las nuevas APIs no admiten un SID declarado por el PE como identidad
  autenticada.

## Bloqueos para implementar SetFileSecurity/DACL completo

El contrato existente de FileSystem4 ya tiene operaciones FUSE ordinarias
`utimens` y `setxattr/removexattr` para `user.*` **no reservado**. Aún
rechaza `chmod` de servicio para evitar alterar DAC 0700 del UID aislado.

**No publicar SetFileSecurity, NtSetSecurityObject ni virtual chmod hasta
completar todas estas condiciones simultáneamente:**

1. **Token NT autenticado del cliente**: UID procedente de SO_PEERCRED
   (broker) y fuse_get_context (FUSE), pero sin fingir que UID equivale a
   SID de dominio Windows o a grupos de un token Windows. Resolver
   identidad primaria, grupos habilitados/deny-only, grupos restringidos,
   privilegios, deny/allow y CREATE/OWNER por proceso/sesión. La lista de
   SIDs nunca puede proceder de una petición guest no verificada.
2. **Persistencia y transacción**: descriptor binario validado y
   checksummed sobre backing UID exclusivo, fijación por (dev,ino),
   transacción/recuperación con namespace WAL, fsync, protección de xattr,
   identidad tras rename/hardlink, propagación del SD hijo y rechazo
   fail-closed ante xattr corrupto o falta de journal.
3. **Autorización transversal**: comprobar READ_CONTROL, WRITE_DAC,
   WRITE_OWNER, DELETE, lectura, escritura, atributos y directorios
   en CreateFile/NtCreateFile, rename, unlink, hardlink, FUSE
   open/create/read/write/truncate/chmod/utimens/xattr, y FDs/handles ya
   abiertos. El UID servicio no debe conferir privilegios implícitos.
4. **Semánticas avanzadas**: orden ACE, GenericMapping por clase de
   objeto, herencia protegida, SACL/auditoría y privilegios, SID arbitrarios
   de autenticación externa, propietario, grupos restringidos y
   reparse/junction sin seguir host symlinks.
5. **Pruebas**: PE real, dos identidades/varios procesos PE y FUSE,
   denegaciones adversarias, ACL padre/hijo, cambio concurrente DACL
   tras abrir un HANDLE, recovery SIGKILL y referencia NTFS Windows.
   Las pruebas virtualizadas del parser no constituyen esta aceptación.

Habilitar solo el almacenamiento o lectura del SD, mientras las
mutaciones siguen usando privilegios del UID servicio, sería un bypass.
Por eso esta revisión no eleva el estado a “DACL NTFS completo”.

## Verificación

```sh
make -j2 check-nt-acl-engine
make -j2 check-fs4-security-metadata check-fs4-fuse-metadata
```

`tests/nt_acl_engine_test.c` incorpora casos sobre `GENERIC_*`, SID
de longitud variable, tamaño del SD hijo, CREATOR_OWNER/GROUP y
denegaciones. Se revisa la CI del SHA final. La aceptación física de
Fedora no se incluye porque el propietario no la prioriza; no equivale
a aprobar aislamiento SELinux ni pruebas de power-loss.
