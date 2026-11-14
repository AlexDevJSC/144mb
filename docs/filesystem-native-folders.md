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

### Unidades Windows adicionales sin repetir permisos por juego

Las unidades D:–Z: siguen siendo **carpetas Linux reales** explícitamente
autorizadas. Se puede preparar una carpeta propia (no un nuevo volumen ni
un disco físico) y asociarla una sola vez:

~~~sh
mkdir -p "$HOME/Windows-drives/D:"
pe-isolated-run --remember-drive "D=$HOME/Windows-drives/D:"
pe-isolated-run /ruta/juego.exe
# Para esta ejecución, usar otra carpeta sin alterar la asignación recordada:
pe-isolated-run --drive "D=/ruta/alternativa" -- /ruta/juego.exe
# Revocar la asociación:
pe-isolated-run --forget-drive D
~~~

La ruta solicitada por Windows `D:\\Juegos\\datos.bin` se resuelve dentro
del directorio Linux autorizado para `D:`, usando descriptores Linux y
las comprobaciones `openat2`. No se copian archivos, no se crea una
partición, no se concede acceso al disco entero y no se descubre ninguna
unidad automáticamente. `C:` sigue en el perfil existente
`$XDG_STATE_HOME/nt-native-linux/C:`, sin mover ni migrar sus datos.
Las asignaciones de unidades están en el archivo privado separado
`$XDG_CONFIG_HOME/nt-native-linux/drives.conf` (o en
`$HOME/.config/nt-native-linux/drives.conf`), con los mismos límites de
propiedad/permisos, rutas canónicas, escritura atómica y revocación que
`shares.conf`. Así, los lanzadores anteriores que solo conocen
`shares.conf` no dejan de leer las carpetas habituales.

Solo acepta D:–Z: y cada carpeta debe existir antes de ser concedida.
Los recursos UNC siguen necesitando `--unc` por ejecución. No añade
SMB, unidades automáticas, directorios relativos por unidad ni semántica
NTFS de reparse. Los accesos siguen sujetos a permisos Linux y SELinux.

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

Las opciones --remember-share y --forget-share cambian la política guardada; --share solo modifica el lanzamiento actual. Una configuración inválida o un directorio que ha desaparecido provoca un error explícito y no una apertura de otra ubicación. Al guardar un permiso, el runtime ahora almacena la **ruta canónica** del directorio Linux y comprueba el inodo autorizado: sustituir después un enlace simbólico usado como alias de autorización no redirige el permiso persistente. Las seis categorías autorizables se comprueban por separado en la batería automatizada.

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
Se conservan al copiar los permisos Linux básicos (sin bits setuid/setgid)
y las fechas de acceso y modificación; también existen
`SetFileAttributesA/W` para NORMAL y READONLY. La creación de destino
no es transaccional frente a programas externos, ni se reproducen todos
los metadatos NT, el tiempo de creación, flujos alternativos o atributos
extendidos. El borrado y la sustitución siguen sujetos al modelo limitado
de compartición del runtime.

Además, se han añadido las variantes `GetCurrentDirectoryA`,
`SetCurrentDirectoryA`, `GetFullPathNameA`, `GetFileAttributesA`,
`GetFileAttributesExA` y `GetTempPathA`, junto a las funciones `W` ya
disponibles. El modo «ANSI» de esta primera implementación **acepta UTF-8**
(y lo convierte a UTF-16 para las funciones `W`); falta implementar
correctamente la página de códigos ANSI configurada por Windows.
Las rutas relativas admiten `..` **solo cuando permanecen dentro de la
raíz concedida**; si intentan subir por encima, fallan. Se distinguen
`C:\\ruta` (absoluta), `C:ruta` (relativa al directorio de trabajo de C:)
y `\\\\?\\C:\\ruta` (prefijo DOS extendido). UNC y otros volúmenes no
se traducen por accidente al sistema Linux.

En directorios Linux que ya contienen `Foo` y `foo`, la coincidencia exacta
tiene prioridad. Una petición `FOO` sin coincidencia exacta se rechaza por
ambigüedad en lugar de seleccionar un archivo arbitrariamente. Los enlaces
simbólicos no se siguen, aunque coincidan mediante variaciones de mayúsculas.

## Revisión: mayúsculas Unicode y metadatos Windows

La resolución de rutas ya usa el plegado Unicode simple de ICU, sin
depender del `locale` de la máquina Linux. Se pliegan caracteres UTF-8
completos en todos los componentes de la ruta, además de los patrones de
enumeración `*` y `?`. Estos comodines actúan por **carácter Unicode**,
no por byte UTF-8. Se conserva prioridad de coincidencia exacta; si Linux
contiene varias entradas con el mismo nombre tras plegado, se rechaza una
petición no exacta para evitar elegir una al azar. Los nombres UTF-8
inválidos solo se pueden buscar por su escritura exacta, nunca mediante un
plegado inventado. Los cambios de mayúsculas en un renombrado siguen
operando sobre el mismo directorio y el mismo inodo.

`FindFirstFileA`, `FindFirstFileExA` y `FindNextFileA` se añaden a
los métodos `W`, con nombres de salida codificados según el ACP
actualmente implementado (Windows-1252). Se admite también
`FindExSearchLimitToDirectories` sobre la enumeración Windows. El dato
devuelto incluye atributos, tamaño de 64 bits y marcas temporales
Win32, consultadas sobre el inodo real.

`GetFileInformationByHandle`, `GetFileSize`, `GetFileTime` y
`SetFileTime` complementan las consultas de información por handle
existentes. Se admiten ahora READONLY, HIDDEN, SYSTEM y ARCHIVE en
`SetFileAttributesA/W`, tanto para archivos como para carpetas
(conservando los permisos de escritura de las carpetas). HIDDEN,
SYSTEM, ARCHIVE y la fecha de creación establecida expresamente por
Windows se persisten en los atributos extendidos Linux
`user.ntnative.winattrs` y `user.ntnative.birthtime` del **mismo
archivo físico**, no en un volumen o una copia Windows. La copia nativa
reproduce esos metadatos. Las operaciones que requieran un xattr y no
puedan realizarse en el filesystem del host deben devolver error.

**Alcance del cierre de estos dos bloques:** búsqueda insensible a
mayúsculas para caracteres Unicode simples y patrones `*`/`?`;
enumeración Win32 básica A/W, identificación, tamaños, fechas y
atributos estándar sobre archivos y directorios reales, con
comprobaciones automatizadas de regresión. Cuando esos tests y la
compilación Fedora final sean correctos, quedan listos para el
*approve* manual del usuario.

## Ampliación del subsistema avanzado: conversiones, bloqueos, volúmenes y E/S

Esta vuelta amplía **cuatro bloques que todavía no están cerrados**:

**Unicode y páginas de códigos.** Además de UTF-8, CP1252 y CP437, las
conversiones `MultiByteToWideChar` y `WideCharToMultiByte` reconocen ahora
CP850, CP866, CP874, CP932, CP936, CP949, CP950 y CP1250/1251/1253–1258,
respaldadas por los conversores ICU en vez de la configuración regional del
host. Se comprueban secuencias DBCS (Shift-JIS/GBK), sustitución y decodificación
estricta; `GetCPInfo` reconoce longitudes de dos bytes y los rangos de bytes
iniciales, que también expone `IsDBCSLeadByteEx`. El ACP por defecto sigue
siendo CP1252. Las API básicas `A` del sistema de archivos también
admiten bytes CP1252 que no sean UTF-8 válido; si una secuencia es válida
como UTF-8 sigue teniendo prioridad el tratamiento UTF-8 del puente
actual. **Aún no es un modelo ACP estricto y uniforme** para todas las
API ni para las secuencias ambiguas.

**Bloqueos NT.** Las operaciones Win32 `LockFile`, `UnlockFile`,
`LockFileEx` y `UnlockFileEx` utilizan bloqueos de intervalo OFD del
kernel Linux (`F_OFD_SETLK` y `F_OFD_SETLKW`) con offset y longitud de
64 bits; los bloqueos se mantienen mientras existe la descripción de archivo
abierta. Se detectan conflictos incluso con otros descriptores del sistema
Linux que participan en el protocolo de bloqueo. Las lecturas y escrituras
del runtime consultan el rango bloqueado antes de transferir datos,
también en las operaciones NT basadas en offset. La consulta previa y la
E/S aún no forman una transacción atómica. Son bloqueos Linux
*advisory*, no una implementación completa de bloqueos obligatorios NT,
compatibilidad entre procesos del modelo `FILE_SHARE_*`, `delete-pending`
ni leases SMB.

**Otras unidades y UNC.** El código ofrece los puntos de integración
`nt_runtime_set_drive_root('D', host)` para D:–Z: y
`nt_runtime_set_unc_root("server", "share", host)` para recursos UNC
explícitos. Cada concesión abre un descriptor seguro del directorio Linux;
la resolución posterior usa ese FD y prohíbe atravesar `..` fuera de su
raíz, enlaces simbólicos y montajes no permitidos. El nombre del servidor
UNC **no origina una conexión SMB**: designa una carpeta Linux autorizada.
No se otorgan permisos automáticamente ni se lee todo el HOME. Se añaden
`GetLogicalDrives` y `GetDriveTypeW`. El lanzador permite también
**conceder unidades y recursos UNC en cada ejecución** dentro del
aislamiento Bubblewrap:

~~~sh
pe-isolated-run --drive D=/ruta/linux/autorizada \
  --unc lab/assets=/otra/ruta/autorizada -- ./programa.exe
~~~

Los directorios de origen se abren con FD y se montan bajo
`/guest-drives/D` y `/guest-unc/lab/assets`, no como
accesos globales al anfitrión. La persistencia de asignaciones UNC, la
administración avanzada de unidades y los directorios de trabajo
independientes por unidad siguen pendientes (por ahora `D:relativa`
se rechaza).
Los enlaces simbólicos Linux pueden identificarse como
`FILE_ATTRIBUTE_REPARSE_POINT` sin abrir ni seguir su destino;
todavía no se crean junctions/reparse points NTFS ni se soporta
`FSCTL_GET_REPARSE_POINT`.

**E/S con offset y señalización.** `ReadFile` y `WriteFile` admiten
`OVERLAPPED` para operaciones de archivo con offset explícito. Los
handles abiertos con `FILE_FLAG_OVERLAPPED` despachan la operación
a un **trabajador real** con cola limitada a 32 solicitudes; se devuelve
`ERROR_IO_PENDING` y `GetOverlappedResult` permite esperar o
consultar su finalización. Se conserva el offset independiente del puntero
de archivo, se duplica el descriptor mientras la operación está viva y
se señaliza el evento opcional de Windows. Para los handles antiguos
sin ese flag se mantiene la finalización inmediata que ya utilizaban
los fixtures. `NtReadFile/NtWriteFile` todavía admiten solo finalización
inmediata, aunque pueden señalar un evento.
Siguen pendientes IOCP, APC asíncronas NT, cancelación (`CancelIoEx`),
paralelismo de múltiples trabajadores y las semánticas completas de
desvinculación de handles/eventos.

## Límites actuales

Esta implementación **no equivale todavía a NTFS**. La insensibilidad de
mayúsculas incluye ahora Unicode **simple** mediante ICU; no supone
normalización Unicode NFC/NFD, ni equivalencias con expansión a varias
letras, ni identidad garantizada con todas las versiones de la tabla de
mayúsculas de NTFS. Las páginas de códigos ANSI de otras API del runtime,
la generación de nombres cortos 8.3, permisos/ACL NTFS y atributos
avanzados (reparse, cifrado, sparse, compresión) permanecen fuera de
los dos bloques básicos de esta entrega. La creación de nombres que solo difieren
por mayúsculas todavía puede sufrir una carrera entre procesos o programas
Linux que escriban simultáneamente en el directorio; la protección de
`openat2` garantiza confinamiento, no atomicidad case-insensitive de nombres.

Las operaciones `MoveFileExA/W` soportan ahora
`MOVEFILE_COPY_ALLOWED` (0x2): cuando un archivo regular se mueve entre
dos raíces Linux diferentes explícitamente autorizadas, se copia usando
handles del invitado y, solo si la copia termina, se elimina el origen.
El movimiento dentro de la misma raíz sigue usando `renameat2`. No se
aceptan enlaces simbólicos como origen ni destino. Los movimientos por
copia no son atómicos y, si el borrado del origen falla, se informa del error.

La apertura reconoce el permiso explícito `FILE_SHARE_DELETE`: si **todos**
los handles de Windows abiertos sobre ese archivo lo concedieron, se puede
renombrar o eliminar la entrada de directorio manteniendo el inodo Linux
vivo hasta cerrar esos descriptores. Si alguno no lo concede, se devuelve
un error de compartición. `DeleteFileA/W` también respeta el atributo de
solo lectura. Esto aún no implementa por completo la semántica NT de
`delete-pending` entre procesos o sobre archivos reabiertos por nombre.

Siguen pendientes la **implementación completa** de junctions/reparse
points, UNC de red y concesiones persistentes de unidades, compartición
y borrado NT entre procesos, E/S asíncrona NT/APC/IOCP completa, ACL y
otras funciones de ficheros. La lectura de metadatos de un symlink y la
asignación explícita de una carpeta host a un nombre UNC no equivalen a
esas funcionalidades avanzadas. No se modifica el kernel NT, el cargador
PE ni la red.

## Criterio de cierre

En el informe de cada vuelta, **100 % significa código implementado,
publicado y preparado para la aprobación del usuario final en Fedora**;
no significa que esa aprobación ya se haya producido. Para asignar ese
100 % a un bloque no deben quedar funcionalidades conocidas pendientes
**dentro de su alcance declarado**, y deben haber terminado las comprobaciones
automáticas aplicables. La compatibilidad completa NTFS/Windows no debe
confundirse con la conclusión de los bloques específicos de directorios
persistentes y concesión explícita de carpetas.

## Matriz de aceptación de esta vuelta

Se miden **bloques funcionales concretos**, no la compatibilidad total
con NTFS ni con todas las variantes de la API Windows:

| Bloque | Contrato que se entrega a revisión del usuario |
|---|---|
| Crear y abrir | Operaciones de archivo `CreateFileA/W` con las cinco disposiciones soportadas, variantes básicas CRT, creación de carpetas y conflictos de compartición gestionados por el runtime |
| Copiar | `CopyFileA/W` sobre los archivos reales: destino nuevo o existente, opción `failIfExists`, preservación del contenido, permisos básicos y atime/mtime; prevención de copia sobre el mismo inodo |
| Mover | `MoveFileA/W` y `MoveFileExA/W` con flags REPLACE_EXISTING/COPY_ALLOWED; cambio de escritura real en Linux, actualización de la ruta de handles abiertos y bloqueo de movimientos que rompan el cwd o las carpetas concedidas |
| Borrar | `DeleteFileA/W` y `RemoveDirectoryA/W`: comprobación de READONLY, enlaces simbólicos, directorios no vacíos y `FILE_SHARE_DELETE` en handles del proceso |
| Rutas y seguridad | `C:\\...`, `C:relativa`, `\\\\?\\C:\\...`, `..` acotado, directorios bajo FDs, búsqueda ASCII segura, prohibición de escapes a raíces compartidas, enlaces simbólicos y cruces de montajes mediante `openat2` |

Las regresiones de `compat-batch-test`, `mines-services-test` y
`guest-profile-test` cubren los escenarios anteriores. Las carpetas
concedidas son las seis *known folders* de la lista de permisos; ninguna
autoriza implícitamente el HOME completo.

Los siguientes apartados **siguen fuera del contrato de cierre de estos
bloques** y tienen porcentajes propios: opciones avanzadas de `MoveFileEx`
(como operaciones diferidas tras reinicio), copia transaccional,
`CopyFileEx` con callbacks, ACL completas, bloqueo interproceso,
`delete-pending` NT, flujos alternativos, casefold Unicode, UNC y reparse
points. Por ello, un bloque básico listo para aprobación del usuario puede
estar al 100 % sin que la compatibilidad global Windows esté al 100 %.

## Verificación

~~~sh
make build/guest-profile-test
build/guest-profile-test
make -j2 check check-pe
make check-pe-isolated
~~~

El test de perfil comprueba persistencia entre aperturas, actualización concurrente de permisos, acceso al fichero real del host, permisos privados, rechazo de compartición de $HOME y de configuraciones enlazadas simbólicamente. make check-pe-isolated necesita los requisitos del sandbox de la CI y puede indicar SKIP si el entorno no permite los namespaces necesarios.

El sistema de archivos se coordina con el lanzador y los handles NT existentes, pero no se modifican sus contratos generales. Cualquier extensión futura debe partir de pruebas con fixtures PE propios y evidencia de comportamiento Windows, distinguiendo cobertura del símbolo de compatibilidad funcional.
