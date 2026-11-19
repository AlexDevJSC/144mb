# FileSystem4 — protocolo de checkpoint físico entre arranques Fedora

**Estado:** herramienta de captura y comparación preparada; la ejecución
real en Fedora físico y un corte de alimentación no se han realizado en
esta sesión. No confundir checkpoint recuperado con recuperación de una
mutación en curso ni compatibilidad NTFS completa.

## Precondiciones de seguridad

- Únicamente un equipo **Fedora x86-64** con SELinux **Enforcing**,
  `/dev/fuse`, el servicio de almacenamiento UID instalado y una vista
  `fuse.nt-dos83` real en `/run/ntnative-storage/view`.
- Cliente normal **no root** con autorización de servicio. La comprobación
  comprueba el tipo de montaje y rechaza cualquier fallback al backing
  físico o a un directorio POSIX ordinario.
- El operador ha confirmado que el backing es una **unidad de pruebas
  descartable y restaurable** antes de ejecutar `prepare`. El script
  crea una carpeta nueva de nombre aleatorio con 64 KiB, no sobrescribe
  ni borra archivos ajenos.
- El operador coordina un reinicio o interrupción eléctrica con medios
  supervisados. **El script no apaga, reinicia ni interrumpe el sistema**.
  No provocar cortes eléctricos sobre un equipo de producción o datos
  importantes.

## Fase A: preparar datos comprometidos

Con el `main` exacto que se quiere auditar y el servicio activo:

```sh
python3 scripts/fedora-fs4-postreboot-checkpoint.py prepare \
  --physical-confirmed --disposable-confirmed
```

Se genera `.nt144-fs4-reboot-<token>/payload.bin` dentro de la vista
FUSE, se sincroniza el archivo y se guarda una evidencia JSON con
`boot_id`, UID, SHA-256, tamaño, revisión Git y versión de kernel bajo
`~/.local/state/nt-native-linux/fs4-reboot-checkpoints/`.
Conservar la ruta exacta del manifiesto que imprime el comando.
Si FUSE, UID, SELinux, `fsync` o el origen Git fallan, devuelve
**INCOMPLETE**; nunca convierte un SKIP en PASS.

## Fase B: después de otro arranque

Tras el reinicio autorizado y con el servicio UID/FUSE restablecido:

```sh
python3 scripts/fedora-fs4-postreboot-checkpoint.py verify \
  --physical-confirmed \
  --manifest "$HOME/.local/state/nt-native-linux/fs4-reboot-checkpoints/fs4-<token>.json"
```

La herramienta exige un `boot_id` diferente, el mismo UID/revisión y
la vista `fuse.nt-dos83` activa bajo Enforcing. Lee los bytes de la
vista sin seguir symlinks, verifica que el inode es regular y el SHA-256
coincide. Conserva los archivos de prueba para inspección. Un PASS
significa **solo** que el archivo sincronizado sobrevivió a otro
arranque; por diseño también ocurriría tras un reinicio limpio.

## Evidencia adicional necesaria para cerrar power-loss

1. Registro operador de la **naturaleza y hora** del corte real; no puede
   demostrarse con `boot_id` ni desde dentro del mismo anfitrión.
2. Inyección de cortes en fases de WAL/rename/delete-pending con volumen
   descartable; comparación contra el último registro durable.
3. Recuperación completa de namespace, xattrs, alias DOS 8.3 y leases
   de handles tras reconexión de clientes PE/FUSE.
4. Contextos y AVC de SELinux Enforcing, denegación real de un segundo
   UID y referencia Windows NTFS independiente de la misma revisión.
5. Ausencia de SKIP críticos en el gate
   `scripts/fedora-filesystem4-acceptance.sh`.

Esta herramienta **no ejecuta** las fases 1-5 ni certifica atomicidad
ante pérdida real de alimentación. Cerrar el hito solo cuando existan
los registros físicos con hashes y verificación humana.

### Alcance de tests automatizados

`tests/test_fs4_acceptance_gate.py` verifica la sintaxis, falta de
consentimiento, falta de manifiesto y rechazo incondicional del modo CI.
No falsifica `/dev/fuse`, la etiqueta SELinux ni la ejecución en Fedora.
