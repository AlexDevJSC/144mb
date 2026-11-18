# FileSystem4 / FileSystem6 — hardlinks NTFS: contrato bloqueado

**Estado a 19-11-2026:** pendiente de implementación integral y de aceptación.
No interpretar `PASS NT144 live Win32 W-API differential probe (hardlink unimplemented)`
como paridad NTFS. Esta revisión no activa ni simula hardlinks.

## Evidencia examinada

- `tests/fs4_ntfs_probe_smoke.py` exige que `hardlink_created` sea
  `None` (desconocido/no implementado) y solo declara PASS del subconjunto
  efectivamente medido.
- `tests/test_fs4_ntfs_diff.py` exige que un informe NT144 sin hardlinks
  conserve `complete=false`, con `hardlink_created` y
  `hardlink_readable` en `unverified`.
- `docs/filesystem4-mutation-audit-20261118.md` especifica que
  `FUSE linkat` **legacy** existe bajo WAL, pero que **servicio UID**
  rechaza el enlace con `EOPNOTSUPP` a falta de un contrato de
  `FILE_SHARE_DELETE`/hardlink mediado.
- `scripts/fedora-fs4-service-acceptance.py` exige que el enlace duro
  directo en la vista UID se deniegue; mantener este rechazo hasta
  tener una implementación y pruebas equivalentes.
- `docs/filesystem4-fedora-acceptance.md` exige comparación con Windows
  real, revisión de SELinux y ensayos adversarios con segundo UID.

## Alcance y dependencias antes de implementar

El objetivo de paridad NTFS contempla los hardlinks en la referencia
diferencial de Windows. **No existe en la versión auditada un contrato
completo, ya aprobado, para crear hardlinks desde el broker v3 y
la vista FUSE UID.** Para habilitarlo es imprescindible diseñar y probar:

1. **Contrato Win32/NT y transporte.** API concreta, flags, errores,
   privilegios y límites de volumen; operación IPC versionada y
   capacidades de sesión, sin entrega de FDs del backing. La parte de
   exposición Win32 general requiere coordinación con el responsable
   del Núcleo; el almacenamiento físico corresponde a FileSystem4.
2. **Identidad y nombres.** Los dos nombres comparten `st_dev/st_ino`,
   los datos y referencias de apertura. El índice DOS 8.3 y el registro
   de alias deben permanecer por *nombre*, sin confundirlo con la
   identidad de inode. Rechazar cruzar dispositivos o raíces confinadas.
3. **Share leases y delete-pending.** Serializar `linkat` con WAL,
   `namespace_lock` y leases OFD; `FILE_SHARE_DELETE` y la eliminación
   de un nombre no deben cerrar otro enlace ni romper un HANDLE vivo.
   No reutilizar automáticamente el `fs_link` legacy sin esta revisión.
4. **ACL y metadatos.** Aplicar el control de acceso del nombre de origen
   y del directorio destino y preservar atributos/xattrs compartidos
   del inode; no exponer o permitir modificar los marcadores internos
   `user.ntnative.*`. Impedir bypass por enlaces simbólicos, `.. `,
   prefijos DOS/UNC, renombres y bind mounts.
5. **Persistencia y recuperación.** Publicar el nombre adicional solo
   después de las barreras necesarias; `fsync` del inode y directorios,
   transacción de journal/WAL recuperable y reentrada tras `SIGKILL`
   en cada fase. Un proceso Linux no autorizado no debe publicar
   nombres físicos alternativos fuera del broker.
6. **Pruebas.** Fixtures PE reales y FUSE con dos/cuatro procesos,
   `CreateHardLinkW` (cuando esté expuesto), apertura/duplicación de
   handles, renombrado/borrado diferencial, bloqueo, denegaciones y
   recuperación fría. Comparar contra JSON de Windows NTFS de la
   *misma revisión de contrato* y anotar diferencias. La prueba de
   pérdida eléctrica sobre volumen descartable sigue separada.

No basta llamar `os.link` o `linkat` sobre el backing: eso saltaría
las garantías de publicación, journal, leases y aislamiento exigidas
para el servicio UID. Tampoco se debe cambiar el probe de
`hardlink_created: null` a `true` sin que una operación PE real
reproduzca y verifique el resultado.

## Decisión de esta PR

**Bloqueado por contratos y dependencias arquitectónicas previas.**
No se cambia el comportamiento existente, las barreras FUSE UID ni
la semántica de la referencia diferencial. Seguir informando
`hardlink unimplemented`; es un límite explícito, no un test fallido
ni una capacidad aprobada.

## Invariantes de mantenimiento

El broker C cierra los handles de sesión en su salida normal,
incluyendo rechazo de nonce, límite de peticiones y cierre del peer.
Ante `SIGKILL`, el kernel cierra los FDs del worker; las pruebas
existentes comprueban nueva apertura, lease y recuperación de WAL tras
iniciar otro worker. Esto **no demuestra** pérdida eléctrica ni
ausencia universal de fuga en todos los modos de servicio.

La evidencia de Fedora 45 proporcionada por QA es de la revisión
`96199dd54f07140cd64ef27ba35d6c8ef001d0f4`. El entorno actual
de desarrollo no dispone de Fedora físico, SELinux Enforcing ni
`/dev/fuse`. No se atribuyen a FileSystem4 los fallos globales de
procesos, gráficos, APC, planificación o auditoría PE.
