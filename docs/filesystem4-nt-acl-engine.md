# FileSystem4 — núcleo de seguridad NTFS nativo (prototipo)

Esta rama añade un evaluador de SECURITY_DESCRIPTOR_RELATIVE con ACL_REVISION
2, SID binarios, ACE ACCESS_ALLOWED/ACCESS_DENIED, DACL nula o vacía y
herencia explícita OI/CI/INHERIT_ONLY/NO_PROPAGATE. Las ACE se evalúan en su
orden de almacenamiento contra un conjunto de SID de token autenticados;
una ACE deny sobre los derechos todavía pendientes prevalece sobre allow.
El lector comprueba offsets, tamaños, alineación, límites SID y ACE.
Los descriptores de auditoría SACL y ACE/callback condicionales se rechazan.

El token explícito admite ahora `NT_ACL_SID_DENY_ONLY` por SID. Un grupo
marcado así activa ACE de denegación, pero nunca satisface una ACE de
concesión. Los atributos desconocidos se rechazan y la salida `granted`
se pone a cero ante fallo. La compatibilidad con inicializadores existentes
se conserva: flags sin especificar son cero (SID habilitado). **El origen y
autenticación del token siguen siendo obligación del llamador confiable;
el PE invitado no puede autoasignarse grupos ni derechos.**

Los derechos GENERIC_* se rechazan de momento: la expansión depende de la
GenericMapping del tipo de objeto y no es seguro fingir un equivalente.
La función de herencia devuelve sólo un ACL hijo, no persiste SD ni hereda
propietario o SID. Con DACL NULL no genera una ACL heredable.

Comprobación: make check-nt-acl-engine

Este cambio solo fortalece el evaluador aislado y sus regresiones. No
habilita token impersonation, grupos deshabilitados/restringidos,
privilegios, auditoría ni seguridad NT global. **Componente 4:
96 % → 96 % (+0 puntos)**; no existe aún nuevo contrato de token
autenticado aplicable a procesos Windows externos.

**La política NO está conectada al acceso real del filesystem**:
faltan token SID autenticado desde servicio UID, traducción de dominios y
grupos, persistencia segura del descriptor en xattrs, SetFileSecurity,
acceso READ_CONTROL/WRITE_DAC/WRITE_OWNER, evaluación al crear/abrir y
al editar directorios, enforcement entre FUSE/PE, herencia con CREATOR_OWNER,
SACL/privilegios y fixtures contra Windows NTFS. Se mantiene fail-closed al
encontrar constructos no implementados. Esto es una pieza de implementación
verificable, no un motor NTFS completo ni una certificación de permisos.
