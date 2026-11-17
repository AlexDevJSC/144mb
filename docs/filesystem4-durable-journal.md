# FileSystem4 — Journal DOS 8.3 para renombrados del runtime

Estado: implementación inicial; pendiente de validación física Fedora.
No afirma equivalencia NTFS ni recuperación general tras cortes eléctricos.
Describe únicamente los renombrados del broker MoveFileEx dentro de una
misma raíz autorizada y sus xattrs de alias.

## Contrato e invariantes

Cada grant mantiene :ntnative-file-leases/dos83-rename-v1.wal (0600) en un
directorio reservado (0700). La carpeta está oculta en FUSE y al sistema
DOS. El archivo permanece truncado a cero con fsync entre transacciones,
sin borrar ni reemplazar un inode que otros procesos pudieran tener abierto.

- Una operación registrada por grant; flock(LOCK_EX) sobre el WAL durante
  preparación, renameat2, sincronización y limpieza. Orden de locks:
  WAL, luego directorios. No serializa a programas Linux no cooperantes.
- Antes de modificar xattrs se persiste un registro con magic, checksum
  FNV-1a, dev/ino de raíz, archivo y ambos padres, víctima si existe,
  rutas UTF-8 relativas, alias y preimagen del xattr destino. Enteros
  little-endian explícitos. Nunca se serializa el padding de estructuras C.
- Barreras: fsync(WAL) -> modificación de xattr -> fsync(inodo) ->
  renameat2 -> limpieza alias -> fsync(inodos/directorios) ->
  ftruncate(WAL,0) + fsync(WAL). Una barrera fallida deja trabajo pendiente.
- El modo nt-dos83-mediator --root DIR --recover reproduce el journal
  únicamente si las identidades de ambos directorios, del archivo y
  de la víctima son demostrables. --scan recupera antes de refrescar.
  El supervisor invoca --scan antes de montar la sesión; el worker PE
  ejecuta recovery de C: antes de instrucciones invitadas.
- Si el origen todavía posee el inodo previsto, se revierte el xattr
  destino preparado. Si el destino lo posee y el origen desapareció,
  se completa limpieza del alias antiguo. Si el resultado no se puede
  determinar, el WAL queda intacto y nuevos renombrados fallan cerrados.
- El checksum detecta corrupción accidental, no manipulación deliberada
  de un atacante con acceso al mismo UID. Nunca es una ACL NTFS.

## Pruebas

make check-dos83-wal crea cinco snapshots descartables de interrupción:
antes del staging, después del staging, después de renameat2,
registro corrupto y desaparición de ambas entradas. Verifica identidad,
contenido, rollback, forward recovery y negativa de reutilizar el WAL.
Son estados de disco sintetizados, NO cortes de energía reales ni SIGKILL
sincronizado en el proceso real.

make check-dos83-mediator, make check-dos83-fuse y las suites PE siguen
siendo obligatorias. SKIP no equivale a PASS de integración.

## Pendientes reales — no declarar 100 %

1. Sólo están cubiertos los renombrados MoveFileEx del broker cooperante:
   NO la generación perezosa de alias, todos los unlink, hardlinks,
   reparse/junction ni operaciones POSIX directas.
2. Algunos archivos/directorios pueden negar el fsync de los inodos a
   pesar de que renameat2 fuese posible. Fallar cerrado es deliberado.
3. Otros programas con acceso directo al backing pueden saltarse WAL y
   los flocks: hace falta UID de servicio y política DAC/SELinux para
   imponer mediación a todo proceso ajeno al runtime.
4. Los xattrs de víctimas reemplazadas con hardlinks supervivientes
   requieren una política de recolección de restos.
5. El recovery automático inicial abarca C:. Otras unidades autorizadas
   requieren preflight por su raíz física antes de ser utilizadas.
6. Aceptación pendiente en Fedora real con SELinux Enforcing, reinicios,
   SIGKILL, pérdida de montaje, escenarios simultáneos y fallos de disco
   en cada barrera; comparación de resultados frente a NTFS en Windows.
7. No están cerrados la conexión universal de la vista FUSE con el
   lanzador PE, operaciones concurrentes bajo fallo ni la semántica
   avanzada de permisos/casos y puntos de análisis NTFS.

No se modifican contratos de ptrace, hilos, APC o supervisor del núcleo.
