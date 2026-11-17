# FileSystem: sesión NT supervisada y archivos marcados

Estado: perfil de usuario explícito (no interceptación universal del kernel).
La implementación y pruebas de esta rama son independientes de Núcleo3.
Un supervisor de hilos o ptrace de Núcleo3 **no** se usa para mutaciones
de almacenamiento. FileSystem administra su propia sesión, sus leases y
los procesos que inició, sin detener arbitrariamente hilos del runtime.

## 1. Control de acceso en lanzamiento

`nt-fs-supervisor` abre una vista `nt-dos83-fuse` sólo para una
sesión nueva, comprueba el montaje y Landlock ABI >=3, y ejecuta
el comando como descendiente de `nt-dos83-landlock`.
Mantiene un lock por vista para impedir montajes concurrentes y ejecuta
`nt-dos83-mediator --scan` antes de abrir la sesión, rechazando
reconciliaciones que fallan. Es una comprobación preventiva, no un WAL.
No ejecuta el comando ante ausencia de FUSE, Landlock, montaje verificable
ni si el montaje desaparece; detecta la muerte del daemon y finaliza
el grupo del proceso que lanzó. El cierre desmonta la vista y detiene
su propio daemon. SIGTERM/SIGHUP activan la limpieza supervisada, que
intenta terminar el grupo completo del invitado incluso si su proceso
principal terminó y dejó hijos en segundo plano. No es una garantía
contra descendientes que se cambien voluntariamente de sesión/grupo.
El montaje físico y la liberación de recursos deben validarse en Fedora.

```bash
make -j2 all check-fs-supervisor check-dos83-fuse
mkdir -p "$HOME/nt-lower" "$HOME/nt-view"
nt-fs-supervisor --lower "$HOME/nt-lower" --view "$HOME/nt-view" -- /bin/sh
# Dentro de la shell lanzada, usar $NT_FS_VIEW para escribir y explorar.
```

La utilidad no se activa con sólo ejecutar un binario de Linux arbitrario:
debe elegirse como punto de entrada. Un PE lanzado con
`pe-isolated-run` usa ya su propio sandbox Bubblewrap; el presente
supervisor no sustituye ese confinamiento ni controla directamente su
árbol `/game`. No se incorpora una dependencia accidental de ptrace,
privilegios de root ni de los componentes de Núcleo3.

**Límite crítico:** Landlock sólo confina el proceso lanzado y sus
descendientes. Otros procesos con acceso directo al backing pueden
escribir en él. Para garantizar mediación frente a cualquier proceso del
mismo usuario hace falta un servicio con UID propio y política DAC/SELinux
administrada; no está implementado aquí. Tampoco se garantiza terminar
subprocesos que abandonen deliberadamente el grupo de procesos.

## 2. Perfil explícito NT-compatible en archivos

Archivo físico ordinario (sin marcador): comportamiento FUSE/POSIX
actual, sin reescribir semánticas de Linux.

Archivo físico marcado `user.ntnative.fs_compat=1`: antes de conceder
`fs_open`, FUSE toma un lease OFD en el **mismo registro que CreateFile**;
los accesos de Linux se traducen a lectura/escritura y comparten los
permisos por defecto `FILE_SHARE_READ|WRITE|DELETE`. De este modo una
apertura NT sin permiso de compartición puede rechazar una apertura
Linux posterior, y la apertura Linux activa puede impedir una apertura
NT cuyo `dwShareMode` excluya su acceso. Los locks de rango mediante
`LockFile/LockFileEx` se consultan antes de I/O FUSE en el FD marcado.

Las aperturas FUSE de archivos marcados utilizan `direct_io` para impedir
que el caché de páginas del kernel satisfaga una lectura sin llamar al
callback que valida los bloqueos por rango. Esto puede limitar `mmap`
sobre esos handles y no sustituye la mediación de los accesos directos
al backing por procesos ajenos a la sesión.

El marcador sólo puede ser asignado por un operador con acceso al backing:

```bash
# Debe existir ya el fichero, dentro del grant C: autorizado:
nt-dos83-mediator --root "$HOME/nt-lower" --mark 'C:/datos.db'
```

El truncado por ruta también toma el lease antes de modificar el contenido,
además de consultar los locks NT sobre el rango de bytes.
Los clientes de FUSE no pueden leer, enumerar ni modificar metadatos
`user.ntnative.*`, ni acceder a `:ntnative-file-leases`.
Marcadores malformados fallan cerrados. Para prevenir pérdida de datos
se comprueba la compartición **antes** de permitir O_TRUNC.

El marcado requiere que no haya clientes Linux ajenos al protocolo con
FDS previos: un proceso que ya tenía el archivo abierto, o escribe
directamente fuera del montaje, no puede ser revocado por este bridge.
No se pretende transformar toda la semántica POSIX en NTFS ni importar
ACL Windows, oplocks o exclusiones globales al kernel Linux.

## 3. Pruebas automatizadas

```bash
python3 -m unittest discover -s tests -p 'test_fs_supervisor.py'
make check-dos83-fuse
# En Fedora usuario final:
bash scripts/fedora-p2-3-acceptance.sh
```

El autotest C de FUSE se ejecuta aun si `/dev/fuse` está restringido:
prueba conflicto con `CreateFileW`, denegación antes de `O_TRUNC`,
semántica del lease, bloqueo de rango, liberación, marcador y directorio
privados. El test montado añade 257 entradas paginadas, metadatos POSIX,
hardlinks, marcador confiable, negativo de marcador corrupto y 32 renombrados
concurrentes. Las pruebas de supervisor incluyen grants disjuntos,
aliases mediante symlinks, ausencia de directorio y ausencia de montaje.

Un `SKIP mounted-FUSE` en CI **no es** una prueba de montaje correcta;
la aceptación en Fedora con SELinux Enforcing no se considera realizada
hasta ejecutarla en el equipo correspondiente.

## 4. Recuperación tras caída (todavía sin cerrar)

El supervisor proporciona un **límite de vida útil** a la sesión FUSE,
pero no convierte la secuencia xattr/renameat2 en una transacción durable.
El protocolo de renombrado del runtime todavía puede dejar metadatos
staging obsoletos si el sistema cae durante una operación. Un journal
persistente, recuperación verificada y pruebas de cortes de energía siguen
siendo trabajo pendiente. No se debe marcar FileSystem 100 % sólo por
haber implantado un monitor de procesos.

## 5. Semánticas POSIX

Se corrige la enumeración `readdir` para devolver cookies `telldir`
resumibles, sin exponer los directorios reservados. `fsync` sincroniza
datos, `flush` sigue siendo no-op intencional y **no** promete durabilidad:
la llamada POSIX a flush no equivale a fsync. No se anuncian
`RENAME_EXCHANGE`, todos los callbacks de bloqueo FUSE, ni enlaces
simbólicos arbitrarios como soportados.

## Criterio de finalización

El 100 % del alcance FileSystem exige integración de rutas de ejecución,
recuperación duradera de nombres, matriz de concurrencia con límites,
y pruebas sintéticas de desarrollo completas. Las pruebas en el equipo
Fedora del usuario son la validación final aparte. El porcentaje actual
se debe reevaluar tras comprobar el resultado de CI.
