# Mediación P2 de nombres DOS 8.3

## Alcance

El sistema Linux puede montar volúmenes NTFS con el controlador `ntfs3`, pero el runtime no exige que la carpeta concedida sea NTFS. En Btrfs, ext4 y otros sistemas de archivos ordinarios los nombres cortos que necesita Win32 pertenecen a nuestro runtime. No se debe confundir compatibilidad de almacenamiento NTFS con paridad de Win32.

El perfil siguiente permite dos contratos diferentes:

1. **Operaciones cooperantes (mediación previa)**: `MoveFileExW/A` dentro de un único grant y el comando `nt-dos83-mediator --move` participan en el mismo protocolo de bloqueos de directorios por `(st_dev,st_ino)`. El runtime prepara en el inodo origen el xattr de la futura entrada antes de `renameat2`, deja que Linux haga el cambio físico y confirma o revierte la preparación. Conserva el alias 8.3 anterior cuando no colisiona con otro nombre real o corto en destino; cuando colisiona, asigna un alias nuevo. También limpia el xattr de destino de un archivo reemplazado. Los hardlinks conservan sus claves por entrada. El orden canónico de bloqueos impide deadlocks ABBA entre procesos cooperantes.
2. **Escritores Linux que no cooperan (reconciliación posterior)**: `--watch` usa inotify recursivo, asocia `IN_MOVED_FROM/IN_MOVED_TO` mediante cookie y pide al runtime migrar el alias que todavía está almacenado en el inodo movido. Si la clave anterior no existía, puede generar un alias bajo el nombre nuevo. `--scan` reconstruye los watches y asigna alias que faltan, pero no puede demostrar el historial de renombres perdido. Las API internas `nt_runtime_reconcile_host_rename` y `nt_runtime_refresh_dos83_path` no son importaciones Win32/PE.

Los dos contratos siguen resolviendo rutas con FDs fijados bajo la raíz autorizada y `openat2` sin symlinks ni cruces de dispositivos.

## Uso

Tras compilar con `make`:

```sh
# Ejemplo: carpeta Linux ya autorizada como C: por el lanzador
build/nt-dos83-mediator --root /ruta/carpeta --scan

# Renombrar de forma mediada, sin usar mv:
build/nt-dos83-mediator --root /ruta/carpeta --move \
  'C:/Directorio/NombreLargo.txt' 'C:/Directorio/NuevoNombre.txt'

# Mantener reconciliación tras operaciones de programas Linux externos:
build/nt-dos83-mediator --root /ruta/carpeta --watch

# Reparación explícita de una pareja observada en otro registro:
build/nt-dos83-mediator --root /ruta/carpeta --reconcile \
  'C:/antiguo.txt' 'C:/renombrado.txt'

build/nt-dos83-mediator --root /ruta/carpeta --refresh \
  'C:/Directorio/NuevoNombre.txt'
```

La herramienta necesita permiso de lectura, búsqueda y atributos extendidos sobre el árbol. `--watch` mantiene un proceso de usuario, sin privilegios especiales y sin cambiar los permisos de los archivos; hay que lanzarlo en la sesión de usuario que ha concedido la carpeta. La entrada de comandos es UTF-8 y las rutas corresponden al C: de esa carpeta, no a rutas absolutas arbitrarias del anfitrión.

## Límites que NO se cierran

- **Inotify no autoriza ni bloquea renombres**: el escritor Linux ya ha modificado el árbol cuando recibimos el evento. Puede existir una ventana donde no se conserva el alias histórico. Tampoco permite forzar a otra aplicación a utilizar `--move`.
- Si los eventos se pierden o desbordan, si una operación mueve un fichero hacia fuera del árbol, si el dispositivo de almacenamiento no genera eventos, o si otro programa modifica de nuevo la ruta antes de reconciliarla, el sistema registra `DEGRADED` y deja de prometer la identidad corta histórica. Los accesos normales usan la validación de `dev/ino` por entrada y fallan cerrados ante ambigüedad.
- La cola de inotify no es un journal durable. `--scan` repara visibilidad y alias faltantes, **no** reconstruye un par origen/destino cuyo historial se haya perdido ni garantiza limpieza de todos los metadatos huérfanos.
- La preparación previa de xattrs no es una transacción de almacenamiento atómica junto con `renameat2`. Una caída entre operaciones puede dejar atributos de staging obsoletos en el inodo, aunque la búsqueda no debería considerarlos válidos para nombres ausentes. No se afirma recuperación idéntica a NTFS después de una interrupción eléctrica.
- Los alias y sus reglas Unicode/8.3 no se han contrastado exhaustivamente con versiones reales de Windows. `ntfs3` no se usa como dependencia obligatoria.
- **Mediación transparente de absolutamente todos los procesos Linux** exigiría prohibir el acceso directo al directorio inferior y exponer exclusivamente un sistema de archivos mediador (por ejemplo, FUSE), o una solución de kernel con una política equivalente. No se implementa aquí porque esos eran los requisitos del nivel 3, no de los niveles 1 y 2 solicitados.

## Pruebas

```sh
make check-dos83-mediator
make check
make check-pe
```

Se verifica: renombrado cooperante a otro directorio, persistencia y colisiones de alias, desaparición de alias en el directorio de origen, reconciliación post-rename simulada y una sesión real de `--watch` que recibe un evento nativo de `mv`. CI Linux y Fedora RPM son requisitos de fusión; ejecutar el script de aceptación física de FileSystem3 en el Fedora real sigue siendo un requisito aparte para validación completa de máquina, SELinux y políticas de inotify.
