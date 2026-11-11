# Cargador PE aislado en Linux — fase experimental

## Estado y alcance

Se ha añadido un mapeador genérico inicial para **PE32+ AMD64** y un
lanzador `pe-isolated-run`. No utiliza Wine, Proton ni un kernel Windows:
las instrucciones AMD64 del invitado se ejecutan en el procesador Linux,
y las importaciones resueltas llaman a equivalentes del runtime propio.

La cobertura **197/197 importaciones directas** de Mines procede del
auditor previo y mide símbolos con equivalente registrado. El fixture sintético verifica el mapeo de
varias secciones, una relocación DIR64, la IAT, callbacks TLS sin datos, puente GS/TEB/PEB mínimo y una llamada MS x64 real
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
- Preparación de un TEB mínimo por hilo mediante GS de Linux, PEB con base de imagen y vector TLS vacío; se restaura el GS anterior tras la ejecución normal.
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
build/pe-isolated-run /ruta/completa/Mines.exe
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
`FD_CLOEXEC`. El lanzador lo conserva hasta ejecutar Bubblewrap y emplea
`--bind` con su ruta `/proc/self/fd/N`, compatible con la interfaz existente
de Bubblewrap. El directorio padre del disco queda fuera del montaje
invitado. Un fallo de preparación cancela el arranque y conserva los
archivos existentes para el siguiente intento.

Los valores siguen las carpetas por usuario documentadas en
[KNOWNFOLDERID](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid).
La elección del directorio Linux sigue la
[especificación XDG 0.8](https://specifications.freedesktop.org/basedir/latest/).

### Fuera de alcance y riesgos pendientes

- TLS estática o con datos por hilo (solo callbacks acotados sin datos), importaciones retardadas,
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
