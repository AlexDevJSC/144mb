# Cargador PE aislado en Linux — fase experimental

## Estado y alcance

Se ha añadido un mapeador genérico inicial para **PE32+ AMD64** y un
lanzador `pe-isolated-run`. No utiliza Wine, Proton ni un kernel Windows:
las instrucciones AMD64 del invitado se ejecutan en el procesador Linux,
y las importaciones resueltas llaman a equivalentes del runtime propio.

La cobertura **197/197 importaciones directas** de Mines procede del
auditor previo y mide símbolos con equivalente registrado. El fixture sintético verifica el mapeo de
varias secciones, una relocación DIR64, la IAT, la plantilla y callbacks TLS, el puente GS/TEB/PEB mínimo y una llamada MS x64 real
y el rechazo temprano de imágenes malformadas. Posteriormente se completaron
una victoria y una derrota del Mines original con un arnés aislado en Linux
fuera de pantalla. El usuario ha confirmado ambos finales en su portátil
Fedora KDE y ha comunicado fallos de menús. La revisión de esos fallos y el
alcance funcional están en [Compatibilidad de Mines](mines-compatibility.md).

### Operaciones implementadas

- Validación de MZ, PE32+, máquina AMD64, encabezados opcionales,
  tamaño, alineaciones, sección ejecutable y entry point.
- Carga de hasta 96 secciones y 256 MiB de imagen; inicialización
  de datos y espacio sin inicializar; permisos finales por página,
  con prohibición de secciones escritura+ejecución (W^X).
- Intento de usar la base preferida sin sobrescribir mapeos existentes,
  o relocaciones de tipo DIR64 en memoria privada.
- Importación por nombre u ordinal, enlazada al registro propio de
  equivalentes Win32. Rechazo explícito de funciones no soportadas.
- Preparación de un TEB mínimo por hilo mediante GS de Linux y un PEB
  compartido que contiene la base de la imagen y los parámetros iniciales
  del proceso. Los parámetros AMD64 normalizados incluyen rutas UTF-16,
  línea de comandos, directorio, entorno inicial y manejadores estándar.
  El PE de pruebas verifica los offsets de Win32 y que dos hilos
  observan el mismo PEB. Se copia la plantilla TLS del ejecutable
  al slot 0 de cada hilo, con hasta 4 KiB de datos y relleno y alineación
  de hasta 16 bytes. Se validan las direcciones de callbacks antes de
  su entrega. Se restaura el GS anterior al salir del código invitado.
- Transferencia de control a un punto de entrada de aplicación con
  ABI Microsoft x64, usando el trabajador de pruebas y SDL en Linux.
- Límite de archivo de 128 MiB y disco C: persistente por usuario con el EXE
  montado en solo lectura. `APPDATA`, `LOCALAPPDATA`, `USERPROFILE`, `TEMP`
  y `TMP` apuntan a directorios del perfil invitado.

### Ejecución de prueba

En Linux x86-64:

```sh
make -j2 all check check-pe check-pe-image check-pe-import
sudo dnf install bubblewrap
build/pe-isolated-run /ruta/completa/programa.exe
```

Usa una sesión Wayland válida. El lanzador publica únicamente el socket
Wayland elegido dentro del contenedor. Para diagnósticos sin interacción
gráfica, se puede usar `SDL_VIDEODRIVER=offscreen`, pero esto **no permite
jugar**. El worker se invoca automáticamente dentro de Bubblewrap,
no ejecutándolo directamente con `--worker`.

Las pruebas `make check-pe-external` ejecutan fuera de Bubblewrap **solo**
el PE controlado que MinGW-w64 genera durante la compilación. No son
una autorización para ejecutar PE de terceros sin aislamiento.
`make check-pe-isolated` intenta comprobar ese mismo ejecutable
dentro de Bubblewrap; si la política Linux impide preparar el sandbox,
la prueba declara `SKIP` y no cuenta como validación del aislamiento.

El entorno de Bubblewrap monta `/usr` y las bibliotecas del host en
solo lectura junto con la configuración de fuentes. Ofrece `/proc` y
`/dev` aislados, crea `/tmp` temporal y desactiva la red.
El perfil persistente se monta en `/game`, con el archivo PE en solo lectura.
El worker cierra los descriptores heredados de preparación desde el número 3
antes de cargar código invitado. Exige
PID 1, `no_new_privs` y una raíz tmpfs como comprobación del entorno
esperado. Estas comprobaciones no constituyen una atestación del sandbox. Si falla la
creación del sandbox se rechaza la ejecución. Esto reduce el acceso del
invitado, pero el prototipo **no es un aislamiento de seguridad auditado**
y no debe emplearse para ejecutar software malicioso.

### Perfil persistente del usuario

El lanzador usa `$XDG_STATE_HOME/nt-native-linux/drive-c`. Cuando la variable
está vacía o contiene una ruta relativa, usa
`$HOME/.local/state/nt-native-linux/drive-c`. El directorio es compartido por
las aplicaciones del usuario y conserva preferencias y otros archivos de C:
entre procesos. La carpeta personal de Linux mantiene su ubicación habitual
fuera del sistema de archivos invitado.

| Variable invitada | Ruta Windows |
| --- | --- |
| `USERPROFILE` | `C:\Users\guest` |
| `APPDATA` | `C:\Users\guest\AppData\Roaming` |
| `LOCALAPPDATA` | `C:\Users\guest\AppData\Local` |
| `TEMP` y `TMP` | `C:\Temp` |

El preparador abre los directorios respecto de descriptores de sus padres.
El perfil y los directorios estándar deben pertenecer al usuario y tener
permisos privados. Las aperturas de esos componentes rechazan enlaces
simbólicos. Las creaciones concurrentes aceptan `EEXIST` y vuelven a validar
el directorio. La entrada del worker cierra los descriptores heredados para
que la aplicación use exclusivamente los archivos visibles en el sandbox.
Las rutas Win32 conservan `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS` y
`RESOLVE_NO_XDEV` dentro del disco invitado.

El preparador devuelve al llamador un descriptor de directorio con
`FD_CLOEXEC`. El lanzador lo duplica por encima de los descriptores de
entrada y salida estándar y emplea `--bind` con su ruta `/proc/self/fd/N`.
El directorio padre del disco queda fuera del montaje invitado. Un fallo
de preparación cancela el arranque y conserva los archivos existentes
para el siguiente intento.

Los valores siguen las carpetas por usuario documentadas en
[KNOWNFOLDERID](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid).
La elección del directorio Linux sigue la
[especificación XDG 0.8](https://specifications.freedesktop.org/basedir/latest/).

### Carpetas de Linux compartidas

Cada `--share Nombre=/ruta/absoluta` da acceso de lectura y escritura a
una carpeta existente de Linux durante ese lanzamiento. Las operaciones
sobre la ruta Windows acceden al archivo original en la carpeta elegida.
Puedes repetir `--share` con distintos nombres.

| Nombre | Ruta del programa Windows | Consulta de la ubicación Linux |
| --- | --- | --- |
| `Desktop` | `C:\Users\guest\Desktop` | `xdg-user-dir DESKTOP` |
| `Documents` | `C:\Users\guest\Documents` | `xdg-user-dir DOCUMENTS` |
| `Downloads` | `C:\Users\guest\Downloads` | `xdg-user-dir DOWNLOAD` |
| `Music` | `C:\Users\guest\Music` | `xdg-user-dir MUSIC` |
| `Pictures` | `C:\Users\guest\Pictures` | `xdg-user-dir PICTURES` |
| `Videos` | `C:\Users\guest\Videos` | `xdg-user-dir VIDEOS` |

```sh
build/pe-isolated-run --share "Documents=$(xdg-user-dir DOCUMENTS)" /ruta/al/programa.exe
```

El lanzador rechaza la carpeta personal completa y sus directorios
ancestros. Cada carpeta elegida tiene un montaje independiente dentro
del sandbox. El runtime elige el descriptor según el prefijo de la ruta
Win32 y aplica las mismas comprobaciones `openat2` dentro de ese
directorio. Las rutas descendientes conservan las restricciones actuales
de caracteres ASCII y distinción de mayúsculas del sistema de archivos.
`MoveFileExA` entre raíces distintas devuelve `ERROR_NOT_SAME_DEVICE`.
Si la ruta Windows ya existía en el perfil, al lanzar con `--share` se
usa la carpeta de Linux. En un lanzamiento sin esa opción se vuelve a
ver el contenido del perfil persistente.

`SHGetKnownFolderPath` admite esas seis carpetas, `Profile`,
`RoamingAppData` y `LocalAppData`, para el usuario actual. Acepta flags
`0` (verifica que exista el directorio) y `KF_FLAG_DONT_VERIFY`.
Devuelve una ruta UTF-16 terminada en cero que se libera mediante
`CoTaskMemFree`. Las rutas Win32 de los datos de aplicación siguen
accediendo al perfil persistente. [Contrato de SHGetKnownFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath).

`make check` comprueba la consulta de carpetas, lectura y escritura
directas, renombrado y límites entre raíces. `make check-pe-isolated`
genera un PE independiente que consulta `Documents`, lee un archivo
preparado desde Linux y añade un byte en el original.

En Linux 6.18.57 x86-64 con GCC 14.2, MinGW-w64 GCC 14 y SDL offscreen,
pasaron `make -j2 all check check-pe check-pe-import check-pe-image
check-desktop check-pe-external` y su repetición con UBSan. El entorno de
pruebas impidió configurar `NETLINK_ROUTE` en Bubblewrap. Un arnés local
con namespaces de usuario, montaje, PID, IPC y UTS y seccomp para bloquear
sockets ejecutó el PE de carpetas y dos arranques de Mines con preferencias
persistentes. El auditor registró 197 de 197 importaciones de Mines y 71 de
324 de `Squadron121_DX12.exe` con un equivalente registrado. La auditoría
de Squadron121 fue de solo lectura.

### Fuera de alcance y riesgos pendientes

- TLS de DLL adicionales, plantillas fuera de los límites del cargador,
  importaciones retardadas,
  PEB/TEB completos (hay solo estructuras mínimas), llamadas dinámicas no incluidas en las 197
  importaciones directas de Mines, compatibilidad de DLL externas.
- Entrega general de SEH y fallos de memoria del código invitado; el
  backend existente solo admite los perfiles descritos en la documentación.
- Inicialización Win32/CRT completa, identidad de módulos, recursos
  no cubiertos por los iconos, sonido y gráficos fuera del perfil SDL/Cairo.
- En instalaciones donde el kernel o la política deniegan los namespaces
  sin privilegios, Bubblewrap falla; el programa no retira el aislamiento
  para continuar.
- Prueba manual con el `mines.exe` del SHA-256
  `3ba695d8bccac02cb9e6d63e630b50be15b729a952769814bbb684f9c9f096f3`.
  Registrar el primer error, las funciones resueltas dinámicamente y
  si se puede abrir una partida, revelar casillas y salir sin fuga.

## Reproducción y licencia

```sh
make -j2 BUILD_DIR=build-ubsan CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' check-pe-image
```

El cargador y el lanzador están licenciados bajo MIT. Las dependencias
SDL2, Cairo, libffi y Bubblewrap son externas y conservan sus licencias.
No se incorporan DLL ni implementaciones propietarias de Windows.

### COM e imágenes

El perfil WIC de `CoCreateInstance` activa `IWICImagingFactory` con
SDL2_image. Incluye decodificación desde un archivo invitado, consulta
del tamaño y conversión de píxeles RGB/BGR y RGBA/BGRA. Respeta la
inicialización COM por hilo y mantiene los píxeles vivos mientras
existan referencias al frame o al conversor. La prueba
`make check-pe-wic` crea, decodifica y convierte un BMP desde un PE AMD64.
La dependencia [SDL2_image](https://github.com/libsdl-org/SDL_image)
conserva las licencias de los codecs empaquetados por la distribución.

### Procesos hijos y shell

`CreateProcessW` puede lanzar otro PE AMD64 del disco invitado en el mismo
sandbox. Admite un ejecutable explícito o el primer argumento de una línea
de comandos, un directorio de trabajo invitado y el entorno heredado.
El padre conserva manejadores de espera y el código de salida de 32 bits.
Las opciones de seguridad, la herencia arbitraria de manejadores, las
consolas separadas y los bloques de entorno personalizados quedan fuera
del perfil inicial.

`SHParseDisplayName` genera un PIDL para rutas de archivos o directorios
invitados. `SHOpenFolderAndSelectItems` muestra una carpeta SDL con selección
de un elemento y `ShellExecuteW` admite abrir carpetas y ejecutar un PE
con el mismo mecanismo de procesos hijos. Los verbos y asociaciones del
escritorio anfitrión quedan fuera de este perfil.

`make check-pe-platform` verifica el shell desde un PE generado con MinGW-w64. El caso de
procesos hijos de `make check-pe-isolated` prueba argumentos Unicode,
directorio de trabajo y código de salida superior a 255.

El lanzador pasa los argumentos que siguen al EXE a la línea de comandos
Win64. Un PE de prueba verifica argumentos vacíos, Unicode, espacios,
comillas y barras invertidas. `CreateFileW` conserva `HANDLE_FLAG_INHERIT`
en el manejador invitado cuando recibe un descriptor de seguridad nulo.
`GetHandleInformation` expone ese bit. Los descriptores Linux siguen
cerrándose al ejecutar otro proceso.

### Consola

`AttachConsole` admite el terminal Linux heredado del padre. `AllocConsole`
crea un seudoterminal Linux y una ventana SDL con salida y entrada de texto.
`GetConsoleWindow` devuelve el HWND de esa ventana. Un terminal heredado
carece de HWND invitado. `make check-pe-platform` prueba apertura, título,
entrada, salida y cierre desde un PE. `make check` comprueba UTF-8 y título
en un terminal heredado.

### Excepciones AMD64

`RaiseException`, `RtlUnwind` y `RtlUnwindEx` usan las tablas AMD64 del PE
y la pila Linux del hilo invitado. El perfil busca filtros Win64, ejecuta
bloques de finalización y restaura registros al reanudar o transferir el
control. Solo recorre código de la sección ejecutable de entrada y aplica
límites a los marcos, la memoria legible y el anidamiento. Las señales
síncronas de Linux y los formatos de unwind encadenados quedan fuera de
este perfil. `make check-pe-seh` comprueba continuación, captura,
finalización, ambos métodos de unwind y el callback de consolidación de
excepciones C++ desde un PE AMD64.
