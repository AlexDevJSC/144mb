# Carpetas nativas de Fedora 45 para ejecutables Windows

Este subsistema **traduce las rutas que pide un ejecutable Windows hacia directorios reales de Linux**. No monta un NTFS emulado, no copia el contenido a un directorio temporal y no reemplaza el gestor de archivos del kernel Linux.

## Qué existe

- Una carpeta Linux normal y persistente para los datos propios del invitado: C:\Users\guest\AppData\..., almacenado bajo $XDG_STATE_HOME/nt-native-linux/C: (o $HOME/.local/state/nt-native-linux/C:).
- Carpetas explícitamente autorizadas con acceso **directo de lectura/escritura**: Desktop, Documents, Downloads, Music, Pictures, Videos. La ruta Windows C:\Users\guest\Documents se corresponde con el directorio Linux autorizado.
- A partir de este cambio, los permisos de carpeta se pueden **recordar una sola vez por usuario**. Cada ejecución posterior utiliza esa configuración sin repetir --share para cada programa.

Esta autorización no es automática. No exponemos todo $HOME ni la raíz / del host. Bubblewrap conserva el aislamiento actual, y se mantienen las comprobaciones openat2 que impiden escapar de una raíz autorizada a través de enlaces simbólicos o cruces de montaje.

## Transcripción de rutas y directorios

Este diseño no emula un volumen. La carpeta `C:` es un directorio Linux real.
El runtime recibe la ruta de Windows, elimina el prefijo `C:`, cambia
los separadores de `\` a `/`, valida los componentes y accede al mismo
fichero desde el descriptor de esa carpeta mediante APIs nativas Linux.
Esta operación pertenece al runtime de archivos, no al compilador Linux.

Ejemplo:

~~~text
Windows: C:\Program Files\Mi Aplicacion\config.ini
Fedora:  $HOME/.local/state/nt-native-linux/C:/Program Files/Mi Aplicacion/config.ini
~~~

La carpeta `C:` incluye una estructura **de directorios vacíos** para instalar:
`Windows/System32`, `Windows/SysWOW64`, `Windows/Fonts`, `Windows/Temp`,
`Program Files`, `Program Files (x86)`, `ProgramData`, `Users/guest`,
`Users/guest/AppData/Local`, `Users/guest/AppData/Roaming`,
`Desktop`, `Documents`, `Downloads`, `Music`, `Pictures`, `Videos` dentro de
`Users/guest`, además de `Users/Public` y `Temp`.

No se incluyen DLL, binarios o datos Windows simulados. La aplicación o su
instalador es responsable de crear las subcarpetas específicas que necesite.
La existencia de `C:/Program Files` por sí sola no proporciona compatibilidad
con cualquier instalador EXE o MSI.

## Conservación de perfiles existentes

Si había datos en `drive-c`, el perfil privado se renombra atómicamente a
`C:` en el mismo directorio. No se copian los datos y se conservan sus inodos,
permisos y contenido. Las aperturas concurrentes usan un bloqueo del
directorio de estado para evitar carreras entre lanzadores.

Si existen simultáneamente `drive-c` y `C:`, el lanzador falla sin borrar
ni sobrescribir ninguno. Esa situación requiere revisión manual del usuario.

## Uso en Fedora 45

En una instalación con pe-isolated-run accesible:

~~~sh
# Autorizar los Documentos nativos del usuario una sola vez:
pe-isolated-run --remember-share "Documents=$(xdg-user-dir DOCUMENTS)"

# Autorizar también Descargas:
pe-isolated-run --remember-share "Downloads=$(xdg-user-dir DOWNLOAD)"

# Ejecutar programas sin repetir esas autorizaciones:
pe-isolated-run /ruta/a/aplicacion-windows.exe

# Autorización temporal que sustituye solo durante esa ejecución la carpeta Documents recordada:
pe-isolated-run --share "Documents=/ruta/alternativa" /ruta/a/aplicacion-windows.exe

# Revocar la autorización persistente:
pe-isolated-run --forget-share Documents
~~~

Si xdg-user-dir devuelve el propio $HOME (carpeta XDG deshabilitada), el programa rechaza la autorización: no se permite compartir todo el directorio personal por accidente. Las carpetas deben existir y las rutas del host deben ser absolutas.

## Formato y duración

Las autorizaciones se almacenan en:

- $XDG_CONFIG_HOME/nt-native-linux/shares.conf cuando XDG_CONFIG_HOME es absoluto.
- $HOME/.config/nt-native-linux/shares.conf en caso contrario.

El directorio nt-native-linux es privado (0700) y el archivo shares.conf es privado (0600). La lectura rechaza enlaces simbólicos, permisos inseguros, nombres duplicados o desconocidos, rutas no absolutas y archivos excesivos. La escritura toma un bloqueo entre procesos, usa archivo temporal del mismo directorio, sincroniza y sustituye con renameat para no exponer actualizaciones a medio escribir.

Formato de texto sencillo, una línea por permiso:

~~~text
Documents=/home/usuario/Documentos
Downloads=/home/usuario/Descargas
~~~

Las opciones --remember-share y --forget-share cambian la política guardada; --share solo modifica el lanzamiento actual. Una configuración inválida o un directorio que ha desaparecido provoca un error explícito y no una apertura de otra ubicación.

**Persistencia no significa duplicación:** las modificaciones que haga el programa sobre un archivo compartido son modificaciones del mismo archivo del host. Se aplican los permisos Linux y las políticas SELinux del usuario. Los datos exclusivos del invitado permanecen en el perfil privado.

## Resolución segura de nombres sobre carpetas Linux reales

El runtime ahora resuelve los nombres de las carpetas y archivos **sin distinguir
las letras ASCII mayúsculas/minúsculas**. Por ejemplo, `C:\\Program Files\\Mi App\\Data.ini`
y `c:\\program files\\MI APP\\data.INI` apuntan al mismo archivo físico si
solo hay una entrada compatible. No se copia ni se renombra el archivo
original: las operaciones usan descriptores de directorio y `openat2` con
`RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV`.

Se aplica a lectura, escritura, creación, eliminación, cambios de nombre,
atributos y apertura de directorios, incluyendo los componentes intermedios.

También están exportadas `CopyFileA`, `CopyFileW`, `MoveFileA` y
`MoveFileW` en KERNEL32/KERNELBASE. La copia utiliza el mismo registro
de handles Win32 y las mismas comprobaciones de compartición, sin invocar
`cp` ni utilizar un directorio temporal. Se comprueba la opción
`bFailIfExists`, el contenido binario y que copiar a una variante del
mismo archivo no trunque el original. Un fallo de escritura elimina una
copia creada incompleta; la sustitución de una copia preexistente **no
es transaccional** y puede dejar contenido parcial si falla el disco.
No se garantiza todavía la reproducción de todos los metadatos NT,
flujos alternativos o atributos extendidos al copiar.

Además, se han añadido las variantes `GetCurrentDirectoryA`,
`SetCurrentDirectoryA`, `GetFullPathNameA`, `GetFileAttributesA`,
`GetFileAttributesExA` y `GetTempPathA`, junto a las funciones `W` ya
disponibles. El modo «ANSI» de esta primera implementación **acepta UTF-8**
(y lo convierte a UTF-16 para las funciones `W`); falta implementar
correctamente la página de códigos ANSI configurada por Windows.
Las rutas relativas admiten `..` **solo cuando permanecen dentro de la
raíz concedida**; si intentan subir por encima, fallan.

En directorios Linux que ya contienen `Foo` y `foo`, la coincidencia exacta
tiene prioridad. Una petición `FOO` sin coincidencia exacta se rechaza por
ambigüedad en lugar de seleccionar un archivo arbitrariamente. Los enlaces
simbólicos no se siguen, aunque coincidan mediante variaciones de mayúsculas.

## Límites actuales

Esta implementación **no equivale todavía a NTFS**. La insensibilidad de
mayúsculas implementada es **ASCII**, no el plegado ni la normalización Unicode
completos (NFC/NFD, `É/é`, etc.). La creación de nombres que solo difieren
por mayúsculas todavía puede sufrir una carrera entre procesos o programas
Linux que escriban simultáneamente en el directorio; la protección de
`openat2` garantiza confinamiento, no atomicidad case-insensitive de nombres.

Siguen pendientes junctions/reparse points, rutas UNC, todos los modos de
compartición y borrado NT, I/O asíncrono y otras funciones de ficheros. No
se modifica el kernel NT, el cargador PE ni la red.

## Verificación

~~~sh
make build/guest-profile-test
build/guest-profile-test
make -j2 check check-pe
make check-pe-isolated
~~~

El test de perfil comprueba persistencia entre aperturas, actualización concurrente de permisos, acceso al fichero real del host, permisos privados, rechazo de compartición de $HOME y de configuraciones enlazadas simbólicamente. make check-pe-isolated necesita los requisitos del sandbox de la CI y puede indicar SKIP si el entorno no permite los namespaces necesarios.

El sistema de archivos se coordina con el lanzador y los handles NT existentes, pero no se modifican sus contratos generales. Cualquier extensión futura debe partir de pruebas con fixtures PE propios y evidencia de comportamiento Windows, distinguiendo cobertura del símbolo de compatibilidad funcional.
