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
Las rutas UNC también pueden recordarse explícitamente sin activar SMB:

~~~sh
mkdir -p "$HOME/Windows-drives/UNC-lab-assets"
pe-isolated-run --remember-unc "lab/assets=$HOME/Windows-drives/UNC-lab-assets"
pe-isolated-run /ruta/juego.exe
pe-isolated-run --unc "lab/assets=/ruta/alternativa" -- /ruta/juego.exe
pe-isolated-run --forget-unc lab/assets
~~~

La autorización UNC se almacena de forma privada en `unc.conf`, admite
hasta ocho pares `servidor/recurso` y respeta las mismas reglas de rutas
canónicas, revocación, permisos Linux/SELinux, bloqueo y escritura atómica
que el resto de concesiones. Un `--unc` explícito tiene prioridad en ese
lanzamiento; la asociación no descubre servidores ni crea conexiones SMB.
Una ruta UNC sigue representando un directorio Linux **previamente autorizado**.

La resolución de rutas ahora recuerda un directorio actual por cada unidad
DOS autorizada: al cambiar a `D:\Juegos` y después volver a `C:\`,
una solicitud `D:datos.bin` se resuelve bajo el directorio recordado para
`D:`. Las rutas comenzadas por una sola barra utilizan la unidad activa.
El directorio de trabajo **activo** se protege contra borrado y
renombrado; los directorios recordados de unidades **inactivas** son solo
rutas y no mantienen por sí mismos un bloqueo del inodo. Los datos continúan en los directorios Linux
originales, sin volúmenes emulados ni copias.

Siguen fuera de alcance SMB automático y la semántica NTFS completa de
reparse, ACL y directorios compartidos. Los accesos siguen sujetos a permisos Linux y SELinux.

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

**Política de apertura de un nombre existente.** Para `CreateFileA` con
`OPEN_EXISTING`, el runtime prefiere un nombre válido en UTF-8. Si el
archivo correspondiente no existe y esos mismos bytes también tienen otra
interpretación CP1252, comprueba el segundo nombre sobre el directorio
Linux concedido. Si ambos existen, gana UTF-8. No se hace una segunda
apertura si el nombre original existe pero falla por permisos Linux. Las
llamadas `CreateFileW` parten de UTF-16 y **nunca** se reinterpretan como
CP1252: evitar que un nombre Unicode inexistente abra por error otro
archivo es más importante que la heurística. No se hace esta suposición
para `CREATE_NEW` ni otras disposiciones de creación/escritura.
La política todavía no es universal para todas las API `A` ni sustituye
las conversiones explícitas de `MultiByteToWideChar`.

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
accesos globales al anfitrión. Las concesiones UNC y las unidades D:–Z: pueden recordarse desde el lanzador; los directorios de trabajo independientes por unidad ya se mantienen durante la vida del proceso. La gestión avanzada del namespace, la enumeración de montajes SMB y los enlaces NTFS siguen pendientes.
Los enlaces simbólicos Linux pueden identificarse como
`FILE_ATTRIBUTE_REPARSE_POINT` sin abrir ni seguir su destino;
todavía no se crean junctions/reparse points NTFS ni se soporta
`FSCTL_GET_REPARSE_POINT`.

**E/S con offset y señalización.** `ReadFile` y `WriteFile` admiten
`OVERLAPPED` para operaciones de archivo con offset explícito. Los
handles abiertos con `FILE_FLAG_OVERLAPPED` despachan la operación
a un grupo de **hasta cuatro trabajadores Linux** con cola limitada a
32 solicitudes. Las operaciones de un mismo HANDLE se ejecutan en orden,
mientras que las pertenecientes a handles distintos pueden completarse
en paralelo; se devuelve
`ERROR_IO_PENDING` y `GetOverlappedResult` permite esperar o
consultar su finalización. Se conserva el offset independiente del puntero
de archivo, se duplica el descriptor mientras la operación está viva y
se señaliza el evento opcional de Windows. Para los handles antiguos
sin ese flag se mantiene la finalización inmediata que ya utilizaban
los fixtures. `NtReadFile/NtWriteFile` todavía admiten solo finalización
inmediata, aunque pueden señalar un evento.
Se añaden `CancelIoEx` y `CancelIo` sobre las operaciones de
`FILE_FLAG_OVERLAPPED` **que todavía están en cola**. `CancelIoEx`
utiliza la identidad del HANDLE y del proceso emisor; `CancelIo` además
selecciona únicamente las solicitudes emitidas por el **hilo que llama**,
sin cancelar solicitudes de otros hilos que comparten el mismo HANDLE. Admite cancelar una solicitud concreta por dirección de
`OVERLAPPED`, o todas las encoladas de ese handle al pasar NULL.
Notifica `ERROR_OPERATION_ABORTED` y señaliza el evento; un handle
distinto del mismo archivo no puede cancelar la operación ajena. Una
solicitud que el trabajador ya empezó no se cancela mediante `pthread_cancel`
ni se reporta falsamente como cancelada: devuelve `ERROR_NOT_FOUND`
cuando no hay solicitudes en cola para ese criterio.

Se ofrece `GetOverlappedResultEx` con espera no alertable y tiempo máximo
configurable: devuelve `ERROR_IO_INCOMPLETE` para espera cero y
`WAIT_TIMEOUT` al agotarse un plazo distinto de cero, sin detener
la operación en curso. Utiliza la condición del trabajador y el reloj
real del kernel Linux; no ejecuta bucles de sondeo. La variante
`bAlertable=TRUE` se rechaza expresamente hasta disponer del comportamiento
de APC de Windows.

**IOCP básico por proceso.** Ya se ofrecen `CreateIoCompletionPort`,
`PostQueuedCompletionStatus` y `GetQueuedCompletionStatus`. Un programa
puede crear una cola, asociar un HANDLE de archivo abierto con
`FILE_FLAG_OVERLAPPED`, publicar paquetes manuales y recuperar resultados
de E/S, incluyendo la clave de finalización, bytes transferidos, dirección
de `OVERLAPPED` y errores de solicitudes canceladas. Los tiempos de espera
y el cierre del puerto se gestionan sin privilegios del anfitrión.
El bit menos significativo del evento de `OVERLAPPED` suprime el paquete
en IOCP, sin suprimir la finalización propia de la operación.

`GetQueuedCompletionStatusEx` permite extraer varios paquetes de una sola
vez, sin reordenarlos, utilizando el ABI x64 de `OVERLAPPED_ENTRY` (32
bytes por entrada). Es compatible con el consumo individual por
`GetQueuedCompletionStatus`. En las entradas, `Internal` contiene el
NTSTATUS de la operación: `STATUS_SUCCESS` para paquetes manuales y
lecturas correctas, `STATUS_CANCELLED` para una cancelación en cola y
un error NTSTATUS genérico o equivalente para otros errores. Un batch que
contiene errores sigue siendo una extracción correcta y devuelve TRUE.
Las esperas alertables se rechazan explícitamente mientras no existan
APC de E/S; las esperas de tiempo cero y expiradas devuelven
`WAIT_TIMEOUT` sin consumir ni inventar paquetes.

Esta entrega **no ofrece toda la semántica de IOCP de Windows**:
el máximo actual son 16 puertos con 256 paquetes cada uno; no se aplica
la limitación de hilos de `NumberOfConcurrentThreads`, ni están las
asociaciones con sockets y tuberías,
propiedad de puertos con HANDLEs heredados/duplicados, notificaciones
APC o cancelación garantizada de operaciones Linux ya despachadas.
Las operaciones del mismo HANDLE se serializan deliberadamente.
Las semánticas completas de cierre, señalización y desvinculación de
handles/eventos continúan pendientes.

## Coordinación de borrados entre procesos NT (extensión incremental)

La traducción de operaciones de ficheros sigue realizándose en espacio de
usuario, pero Linux mantiene los **bloqueos sobre el propio inodo**.
Cada apertura de archivo por la capa NT toma `flock(LOCK_SH|LOCK_NB)`
sobre su descriptor. Si el HANDLE niega `FILE_SHARE_DELETE`, conserva
ese bloqueo compartido hasta el último cierre de la descripción de archivo
(duplicación, herencia y caída del proceso incluidas). Si permite borrar,
libera el bloqueo de apertura inmediatamente.

`DeleteFileA/W` y el `MoveFileExA/W` ordinario adquieren un bloqueo
exclusivo **desde otro descriptor independiente**, verifican que el inodo no
haya cambiado, y mantienen el bloqueo mientras Linux ejecuta
`unlinkat` o `renameat2`. Un bloqueo denegado se comunica al PE
como `ERROR_SHARING_VIOLATION`. Los test Linux crean dos procesos reales:
uno conserva abierto el archivo con denegación de borrado mientras el otro
intenta borrarlo y renombrarlo; otro escenario demuestra que
`FILE_SHARE_DELETE` permite borrar la entrada y seguir leyendo por el
handle abierto. Tras `unlinkat`, `NtQueryInformationFile`
(`FileStandardInformation`) y `GetFileInformationByHandleEx`
(`FileStandardInfo`) exponen `DeletePending=1` mientras el último
enlace del inodo ya no existe y el handle sigue vivo. Se comprueba ese
valor en el hijo PE simulado mediante proceso Linux real. Esta
observación **no** equivale a disponer del estado de borrado pendiente
previo al `unlinkat` que ofrece Windows. No se requiere root, un
usuario NT privilegiado ni una modificación del kernel.

**Límites restantes:** la barrera original de `flock` coordina
`FILE_SHARE_DELETE`; el registro OFD descrito más abajo incorpora
`FILE_SHARE_READ` y `FILE_SHARE_WRITE` entre procesos. Tampoco implementa un namespace
`delete-pending` como Windows (rechazo de aperturas durante ese estado),
operaciones de `MoveFileEx` por copia entre raíces con el mismo bloqueo
retenido durante todo el traslado, ni atomicidad total con escritores
Linux externos. `flock` es *advisory*: un programa Linux que no coopera
puede renombrar o borrar el inodo. Un archivo sin ningún permiso Linux de
lectura o escritura puede resultar no borrable por la capa porque
`flock` necesita un FD real y no admite `O_PATH`; ese error es
conservador, no una equivalencia completa de ACL NT. No se afirma
simulación de NTFS ni que todos los programas PE funcionen.

El siguiente paso de esta línea de trabajo es ampliar el registro
compartido con un **namespace de borrados pendientes**, con restricciones
sobre reapertura, vínculos existentes y finalización, sin exponer rutas
anfitrión fuera de las concesiones ni ampliar privilegios.

## Directorio de trabajo activo protegido entre procesos

Una carpeta establecida mediante `SetCurrentDirectoryA/W` queda abierta
con un descriptor Linux y un `flock(LOCK_SH | LOCK_NB)` durante todo el
tiempo que siga siendo el directorio de trabajo activo. Otro proceso que
utilice el mismo runtime NT no puede completar `RemoveDirectoryA/W` ni
`MoveFileExA/W` sobre ese mismo inodo: esas operaciones requieren un
`flock(LOCK_EX | LOCK_NB)` independiente y responden
`ERROR_SHARING_VIOLATION` si el directorio está ocupado. Dos procesos
sí pueden mantener la misma carpeta como directorio de trabajo; el último
cierre libera su bloqueo. El directorio anterior se libera después de un
cambio válido. Sustituir el perfil privado libera los descriptores activos
del proceso.

Hay pruebas con dos procesos Linux distintos sincronizados mediante
tuberías, y otras de `C:ruta` y `D:ruta` que comprueban la memoria por
unidad y la revocación del montaje. No requiere etiquetar procesos con
privilegios ni alterar el kernel.

**Límites:** `flock` es *advisory*: un editor Linux ajeno al runtime puede
ignorar los bloqueos y renombrar el directorio. La protección es por
inodo activo y no impide necesariamente renombrar un ancestro del
directorio de otro proceso; para controlar todo el árbol de rutas y las
transiciones `delete-pending` se necesita un namespace/gestor de archivos
entre procesos de mayor alcance. Las ACL NTFS, SMB real y el resto de
las funciones avanzadas siguen fuera de este cambio.

## Registro de compartición entre procesos (nueva extensión)

El subsistema ya comprueba conjuntamente **`FILE_SHARE_READ`,
`FILE_SHARE_WRITE` y `FILE_SHARE_DELETE` entre procesos NT, sobre los
archivos Linux reales. Los controles de acceso se comparan en ambas
direcciones: el acceso solicitado no puede estar prohibido por los
handles existentes y el nuevo modo de compartición tampoco puede
denegar accesos ya concedidos. La tabla local `files[]` sigue
validando duplicación, derechos y operaciones del propio proceso.

Para la coordinación entre procesos se utiliza un registro por
`(st_dev, st_ino)` bajo `C:/:ntnative-file-leases/`. El directorio
físico tiene permisos 0700 y los archivos 0600, con comprobación
de propietario; el carácter `:` en el nombre del directorio hace que
ningún nombre DOS válido pueda abrirlo mediante las API del puente.
`flock(LOCK_EX)` serializa lectura y modificación de registros. Cada
apertura conserva un bloqueo **OFD** (`F_OFD_SETLK`) de un byte sobre
un registro propio. Antes de comprobar conflictos, se consulta el
estado real de ese bloqueo mediante `F_OFD_GETLK`: los registros
huérfanos de procesos que terminaron no se consideran activos y
pueden reutilizarse. Se admiten hasta 512 aperturas simultáneas
por inodo en este protocolo; el límite está documentado y un
agotamiento se devuelve como error, no sobrescribe un registro vivo.

Los handles duplicados comparten la descripción OFD de su apertura,
de modo que liberar una copia no extingue la autorización de las
demás. Los handles heredados por `CreateProcess` transportan
también el descriptor de la autorización en el manifiesto sellado
y los FD validados, evitando recrear de forma incorrecta una apertura
que ya existe. La caída de un proceso o el último cierre libera
automáticamente su bloqueo. La batería de pruebas abarca los
conflictos cruzados de lectura/escritura, acuerdos compatibles,
liberación tras el cierre y una prueba entre procesos reales.

**Límites:** el registro está en el perfil privado C: y precisa
permisos de lectura/escritura sobre él; los programas Linux externos
que ignoran este protocolo siguen siendo libres de actuar sobre
sus propios archivos. No es un servicio del kernel, ni un gestor
de ACL NTFS. **Delete-pending cooperativo en raíces autorizadas C:, D:–Z: y UNC.** Se añade un registro de estado
por inodo que se serializa con el mismo bloqueo `flock` y los mismos
leases OFD de `FILE_SHARE_*`. Cuando `DeleteFileA/W` encuentra al
menos un HANDLE activo que permite compartir borrado, marca el nombre
como pendiente **sin ejecutar todavía `unlinkat`**; las nuevas
aperturas `CreateFileA/W` de ese inodo devuelven acceso denegado.
El último cierre cooperante libera su lease y, si no queda ninguno,
elimina el nombre original únicamente si todavía resuelve al **mismo
dispositivo e inodo**. Los handles duplicados e hijos mantienen su
lease hasta su cierre real.

`SetFileInformationByHandle(FileDispositionInfo=4)` admite el
`BOOLEAN DeleteFile` (un byte) para un HANDLE abierto con acceso
`DELETE` (0x10000). Es posible marcar y desmarcar la disposición desde
el mismo HANDLE antes del cierre. La ruta nativa
`NtSetInformationFile(FileDispositionInformation=13)` consulta el
mismo contrato, utilizando `IO_STATUS_BLOCK` y estados NTSTATUS.
`CreateFileA/W` admite además `FILE_FLAG_DELETE_ON_CLOSE` (0x04000000)
si el acceso solicitado contiene explícitamente `DELETE`: crea
la disposición pendiente al abrir el archivo y la finaliza al último cierre.
`GetFileInformationByHandleEx` en `FileStandardInfo=1` y
`NtQueryInformationFile(FileStandardInformation=5)` reflejan el bit
`DeletePending` aunque el archivo siga enlazado en el host. La marca se guarda en el registro privado
`C:/:ntnative-file-leases/`; una caída del último proceso puede dejar
una marca pendiente, y un acceso posterior del puente intenta
finalizarla tras verificar que no queden leases.

**Límites conocidos:** la política de borrado diferido se extiende
a archivos regulares bajo C:, D:–Z: y alias UNC locales concedidos.
Cada marca nueva registra la identidad física de la raíz autorizada
para evitar que una asignación distinta de la letra o del recurso UNC
pueda borrar un archivo sustituto. Si la raíz original deja de estar
concedida, la marca puede quedar pendiente hasta restablecerla;
no hay resolución global independiente de las concesiones.
Tampoco existe atomicidad con editores Linux que no utilizan este
protocolo, ni cobertura completa para todos los tipos de objeto,
enlaces múltiples o modificaciones externas de las concesiones.
Una carrera con una sustitución externa nunca permite borrar un
inodo nuevo: la operación falla conservadoramente o descarta la
marca tras verificar la identidad. No son ACL NTFS. No impide una
modificación host deliberada del registro por el mismo usuario.
Reparse points, SMB real, 8.3 y otras semánticas avanzadas de IOCP permanecen pendientes.

## Copias con progreso y cancelación Win32 (CopyFileExA/W)

`CopyFileExA` y `CopyFileExW` comparten el mismo motor nativo,
descriptores, control de compartición, copia de atributos y confinamiento
`openat2` que `CopyFileA/W`. No ejecutan `cp`, no conceden rutas
Linux adicionales ni crean una segunda copia temporal de los datos.

El perfil implementado admite `COPY_FILE_FAIL_IF_EXISTS` (0x1),
un `LPPROGRESS_ROUTINE` con ABI Microsoft x64 y los cuatro resultados
`PROGRESS_CONTINUE`, `PROGRESS_CANCEL`, `PROGRESS_STOP` y
`PROGRESS_QUIET`. El callback recibe el tamaño total de archivo,
total transferido, tamaño/avance del único flujo regular, número de
flujo 1, `CALLBACK_STREAM_SWITCH` al comenzar y
`CALLBACK_CHUNK_FINISHED` tras cada bloque (64 KiB).
No se invoca con `file_lock` retenido; puede consultar handles
o realizar otras llamadas Win32. Se admite también la señal
`pbCancel` antes de abrir el archivo y durante la transferencia.

El callback `CANCEL` o `pbCancel` distinto de cero devuelven
`ERROR_REQUEST_ABORTED`; cuando el destino fue creado por esta
operación, se elimina la copia parcial. `STOP` devuelve el mismo
error pero **conserva el destino parcial** a petición del callback.
`QUIET` suprime avisos posteriores y completa la copia normalmente.
Los flags distintos de 0/1 se rechazan con `ERROR_NOT_SUPPORTED`
en lugar de afirmar semánticas no implementadas.

Se comprueban callbacks de ABI x64, conteo y motivo del progreso,
W/A, `FAIL_IF_EXISTS`, silenciamiento, cancelación temprana y
persistencia o eliminación del destino según STOP/CANCEL. Esta
ampliación no hace transaccional la sustitución de un destino
preexistente: si el destino existía y un fallo ocurre tras truncarlo,
puede quedar parcialmente modificado. Tampoco admite flujos
alternativos NTFS, `COPY_FILE_RESTARTABLE`, copia de symlinks,
descompresión/cifrado ni semántica completa de oplocks.

## NtCreateFile/NtOpenFile y delete-pending por identidad física (Filesystem2)

El perfil acotado de NtCreateFile/NtOpenFile acepta nombres de dispositivo
\??\C:\, \??\D:\ ... \??\Z:\ y \??\UNC\server\share\filename.
Los nombres se traducen por el mismo resolvedor seguro y las mismas
concesiones explícitas que CreateFileW; no se descubre ni monta ningún
servidor o disco por el hecho de nombrarlo. Los argumentos NT siguen
rechazando RootDirectory, EA, SecurityDescriptor, QoS y opciones ajenas
al subconjunto implementado. Se admite acceso DELETE explícito y la
opción FILE_DELETE_ON_CLOSE cuando se ha solicitado ese derecho; no
debe confundirse con una implementación de READ_CONTROL/WRITE_DAC.

Los nuevos registros delete-pending guardan el dispositivo e inodo de la
raíz Linux autorizada, además de la identidad del archivo y su ruta
normalizada. Esto permite preparar el borrado diferido sobre archivos
regulares en C:, unidades D:–Z: y alias UNC locales explícitamente
concedidos, compartiendo el registro OFD entre procesos cooperantes.
El borrado final solo opera si la concesión actual vuelve a identificar
exactamente el directorio físico original y el nombre sigue apuntando al
mismo inodo de archivo. Los registros legados de C: permanecen legibles.

**Límite importante:** la identidad física segura evita borrar un archivo
distinto al cambiar una asignación, pero NO garantiza la recogida automática
de una marca pendiente si todos los procesos perdieron la concesión original.
En ese caso el puente falla conservadoramente y puede quedar una marca
pendiente huérfana hasta que se vuelva a autorizar la raíz correcta y un
acceso cooperante ejecute la recuperación. No hay broker global, semántica
completa de vínculos duros ni coordinación con procesos Linux que ignoren
los bloqueos advisory. No se debe declarar esto equivalente a NTFS ni
aprobar en Fedora físico sin pruebas de cambio de raíz, caída y recuperación.

## Seguridad: lectura limitada de descriptores NT a partir de permisos Linux

`GetFileSecurityA/W` se exporta desde ADVAPI32 y `NtQuerySecurityObject`/`ZwQuerySecurityObject` desde NTDLL con un contrato
**solo de lectura** para OWNER_SECURITY_INFORMATION (1),
GROUP_SECURITY_INFORMATION (2) y DACL_SECURITY_INFORMATION (4). Se forma
un SECURITY_DESCRIPTOR_RELATIVE v1 de endian little, con SIDs sintéticos
`S-1-22-1-UID` (usuario Linux) y `S-1-22-2-GID` (grupo Linux).
La DACL básica incluye ACE de permitido para propietario, grupo y
Everyone, con máscaras derivadas únicamente de los bits rwx del modo.
`GetFileSecurity` comprueba acceso a la ruta mediante descriptores
confinados por openat2; no usa rutas Linux arbitrarias ni omite SELinux.
Solicitar SACL falla con ACCESS_DENIED. Si se detecta una ACL POSIX
extendida, se informa NOT_SUPPORTED en lugar de inventar una DACL
incompleta.

**No equivale a una ACL NTFS:** todavía no se implementan cambios
SetFileSecurity, READ_CONTROL, WRITE_DAC/WRITE_OWNER, ACE de denegación,
herencia, auditoría, privilegios NT, seguridad del registro ni traducción
bidireccional de identidades y pertenencia a grupos. El permiso efectivo
continúa siendo el de Linux/SELinux. Los SIDs son identificadores
sintéticos documentados, no identidades Windows reales.
La consulta nativa por HANDLE requiere actualmente el derecho FILE_READ
porque READ_CONTROL todavía no está modelado; respeta NTSTATUS y no altera
GetLastError. La consulta devuelve STATUS_BUFFER_TOO_SMALL si corresponde. La seguridad
avanzada requiere un contrato propio, pruebas con descriptores hostiles
y aceptación en Fedora antes de ampliarse.

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
solo lectura. La eliminación de un directorio de trabajo activo o de una carpeta compartida
concedida (o sus ancestros) se rechaza con un error de compartición,
igual que su cambio de nombre, para no dejar una ruta DOS inválida;
existe una regresión que comprueba el directorio de trabajo vacío.
La comprobación del nombre y el borrado siguen sin ser atómicos frente
a modificaciones concurrentes de otro proceso Linux. La marca `delete-pending` impide reaperturas entre procesos NT
cooperantes en C:, D:–Z: y UNC local autorizado, y difiere la
eliminación hasta el último cierre cuando la identidad de la raíz
original puede verificarse. Las modificaciones por programas Linux
externos y las transiciones NT más avanzadas siguen limitadas por lo
documentado en el apartado del registro de compartición.

Siguen pendientes la **implementación completa** de junctions/reparse
points, conexiones UNC/SMB de red reales, coordinación de `FILE_SHARE_*`
fuera de procesos NT cooperantes y recuperación automática de marcas
`delete-pending` cuando se pierde o se reasigna el volumen autorizado,
E/S NT/APC/IOCP completa, cancelación de operaciones ya iniciadas, ACL
NTFS y otras funciones avanzadas. La persistencia de las concesiones de
unidades y recursos UNC y los directorios actuales por unidad sí están
implementados en este cambio; esto no equivale a soporte SMB ni NTFS total. La lectura de metadatos de un symlink y la
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
