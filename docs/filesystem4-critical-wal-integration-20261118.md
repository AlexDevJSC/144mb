# FileSystem4 — integración crítica WAL (18-11-2026)

Estado: propuesta para revisión; pruebas originales en la rama b29eaffe1eed313b3a1ccc47dbddf3079b4dd587;
la integración contra main requiere nueva CI. No supone aceptación física Fedora.

## Alcance incorporado

- Admitir nuevas mutaciones Win32 sólo cuando el journal de la raíz sea seguro.
  Un WAL no vacío, inválido o ilegible debe bloquear creación, truncado,
  eliminación y actualización de alias, además de renombrado.
- Poisoning de almacenamiento ante error de fsync de journal/inodo:
  impedir reutilizar el WAL con tamaño aparente cero después de fallo.
- Asegurar la sincronización de alias 8.3 por inodo y validar SIGKILL real
  en generación perezosa, CreateFile, truncado, mkdir, rmdir, unlink y
  disposición diferida, sin confundir muerte del proceso con corte eléctrico.
- Recuperación de marcadores de borrado diferido antes de exponer la vista FUSE.
- Preservar íntegros los cambios recientes del núcleo de hilos/IPC de main;
  el Makefile combina las baterías de ambas ramas.

## Evidencia y comandos

Rama de origen: b29eaffe1eed313b3a1ccc47dbddf3079b4dd587. Sus workflows «Build and test» y
«Experimental thread supervisor» finalizaron con éxito.

En la nueva rama, ejecutar:

```sh
make check-dos83-alias-durability check-dos83-wal-io-failure
make check-dos83-pending-delete-durability check-dos83-namespace-durability
make check-dos83-pending-wal-admission
make check
```

## Límites y riesgos que siguen abiertos

- No se restaura el contenido anterior de un archivo truncado;
  la barrera intenta mantener el estado del namespace y la detección
  de recuperación, NO es una transacción de datos de usuario.
- Las garantías no cubren modificaciones POSIX directas por procesos
  que evitan la mediación; el adaptador de handles PE→servicio UID
  independiente todavía no está implementado.
- No hay certificación de caída de energía, hardware de almacenamiento,
  ACL NTFS, reparse completos, SMB ni aceptación Windows/Fedora física.
- Si una barrera de sincronización o WAL se vuelve incierta,
  el comportamiento esperado es bloquear operaciones, no declarar éxito.

No declarar FileSystem4 100% hasta cerrar los defectos de integridad e
interconexión. Las pruebas físicas corresponden al paso final separado.
