# Compatibilidad de Mines en Linux

El auditor leyó **197 importaciones directas** en 15 bibliotecas de
`mines.exe`. Las tres etapas agregaron **74**, **76** y **17** equivalentes.
El auditor resuelve las **197 importaciones directas**, incluida la entrada por ordinal.

| Estado | Base | Primera etapa | Segunda etapa | Tercera etapa |
| --- | ---: | ---: | ---: | ---: |
| Importaciones con un equivalente registrado | 30 | 104 | 180 | 197 |
| Importaciones sin resolver | 167 | 93 | 17 | 0 |

El recuento describe los puntos de entrada disponibles y sus perfiles
probados. Las búsquedas que el programa haga en tiempo de ejecución
quedan fuera de la medición. Las tres etapas iniciales usaron una auditoría
de solo lectura y ejecutaron los binarios de control del proyecto. Las
partidas posteriores y la revisión de menús se documentan al final.

## Primera etapa

| Bloque | Nuevos | Contrato comprobado y alcance |
| --- | ---: | --- |
| Sistema y módulos | 12 | Reloj de arranque en milisegundos módulo 2³², reloj civil en unidades de 100 ns desde 1601, hora local del anfitrión, seudohandle de proceso, inicialización de SLIST alineada, capacidades de CPU del perfil y detección de depurador mediante `/proc`. `GetStartupInfoW` entrega la configuración inicial sin atributos de lanzamiento personalizados. Las búsquedas de módulos se limitan a bibliotecas incorporadas y respetan mayúsculas en los símbolos. Cada referencia adquirida se libera por separado. |
| Memoria y mensajes de error | 6 | Objetos fijos con puntero y móviles con identificador y contador de bloqueos. El último desbloqueo móvil devuelve `FALSE` y `NO_ERROR`. `FormatMessageA` cubre un subconjunto de mensajes de sistema en inglés y cadenas literales sin escapes con `IGNORE_INSERTS`. Puede devolver un búfer que libera `LocalFree`. |
| Conversión de caracteres | 1 | ANSI 1252 y UTF-8 a UTF-16. La consulta con longitud -1 incluye el terminador. UTF-8 admite rechazo estricto o sustitución de secuencias inválidas. |
| Rutas Win32 | 4 | Creación y borrado de directorios, borrado de archivos y renombrado con o sin reemplazo. El anfitrión configura un directorio Linux para `C:` y las rutas relativas. Los nombres ASCII se comparan tal como están en el sistema de archivos Linux. Las aperturas usan `openat2` y rechazan enlaces simbólicos y cruces de montajes. El renombrado opera sobre entradas del directorio sin seguir enlaces en el nombre final. |
| C y flujos de archivos | 19 | Locale C inicial, conversiones numéricas, entorno suministrado por el lanzador, inicializadores y callbacks de cierre en el ABI del invitado. `fopen`, `fread`, `fwrite` y `fclose` operan sobre archivos regulares del directorio configurado, con CRLF y fin de archivo por Ctrl+Z en modo texto. `_cexit` ejecuta los callbacks registrados y vacía los flujos de escritura. |
| Geometría y menús | 10 | Rectángulos de 32 bits, incluidas salidas que coinciden con entradas. Menús con texto, comandos, separadores y submenús de un solo padre. Se comprueba estado anterior por posición e identificador y liberación recursiva. Máximo de 128 menús y 256 entradas por menú. |
| Mensajes y temporizadores | 5 | Temporizadores de hilo sin ventana, respaldados por `timerfd` y `poll`, con hasta 64 por hilo. `GetMessageA` recupera `WM_TIMER` o `WM_QUIT` y respeta el filtro. `DispatchMessageA` despacha el callback del invitado. Con una ventana, o sin una fuente de eventos elegible, el perfil devuelve un error. |
| Dibujo GDI | 17 | Superficies de memoria con bitmaps monocromos o RGB, plumas cosméticas sólidas o nulas, pinceles sólidos o tramados, stock básico, recorte rectangular y píxeles verificables. `BitBlt` cubre `SRCCOPY`, `PATCOPY`, `BLACKNESS` y `WHITENESS`, incluida la copia solapada. Coordenadas `MM_TEXT`, color directo y hasta 4.096 píxeles por lado con 64 MiB totales de bitmaps. `SelectClipRgn` admite restaurar el recorte con `NULL`. Los patrones y la aproximación de color monocromo son propios de este backend. |

El código nuevo usa glibc, pthreads y las interfaces del kernel Linux.
Las pruebas proporcionan punteros válidos y memoria suficiente para las
operaciones admitidas. La raíz de archivos acota las rutas de los
fixtures. El aislamiento de un PE externo requiere una frontera de proceso.

Contratos de referencia del Windows SDK, fichas de 2018 a 2025.
[Memoria](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/winbase/nf-winbase-globalunlock.md) ·
[Mensajes de error](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/winbase/nf-winbase-formatmessagea.md) ·
[Conversión UTF-16](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/stringapiset/nf-stringapiset-multibytetowidechar.md) ·
[Bitmaps](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/wingdi/nf-wingdi-createcompatiblebitmap.md) ·
[Transferencias](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/wingdi/nf-wingdi-bitblt.md) ·
[Menús](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/winuser/nf-winuser-appendmenua.md) ·
[Temporizadores](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/winuser/nf-winuser-settimer.md)

### Errores reproducidos en la primera etapa

| Prueba | Resultado observado |
| --- | --- |
| Auditoría de Mines | Estado de salida 1 con 93 importaciones pendientes. `COMCTL32.dll` pide el ordinal 17. |
| Auditoría de dos PE de control | Estado 0 con una importación resuelta en cada archivo. |
| Cabecera PE truncada | Estado 2, diagnóstico de formato inválido y lista vacía. |
| Módulo, nombre de función o handle inexistente | `ERROR_MOD_NOT_FOUND` (126), `ERROR_PROC_NOT_FOUND` (127) o `ERROR_INVALID_HANDLE` (6). |
| Reloj civil con fallo inyectado | `ERROR_GEN_FAILURE` (31) y salida `FILETIME` intacta. |
| Memoria móvil descartada o desbloqueo sin bloqueo previo | `ERROR_DISCARDED` (157) o `ERROR_NOT_LOCKED` (158). |
| Búfer corto, mensaje ausente y plantilla fuera del perfil | `ERROR_INSUFFICIENT_BUFFER` (122), `ERROR_MR_MID_NOT_FOUND` (317) o `ERROR_NOT_SUPPORTED` (50). |
| UTF-8 malformado estricto, flags desconocidos y búfer corto | `ERROR_NO_UNICODE_TRANSLATION` (1113), `ERROR_INVALID_FLAGS` (1004) o `ERROR_INSUFFICIENT_BUFFER` (122). La salida quedó intacta en los casos de capacidad y conversión estricta. |
| Ruta sin raíz configurada, destino ya existente, directorio no vacío | `ERROR_PATH_NOT_FOUND` (3), `ERROR_ALREADY_EXISTS` (183) o `ERROR_DIR_NOT_EMPTY` (145). Una ruta con `..` se rechazó con `ERROR_ACCESS_DENIED` (5) y un enlace simbólico dio `ELOOP` en `fopen`. |
| Entero fuera de rango y modo CRT inválido | `ERANGE` con saturación a 32 bits y `EINVAL`, respectivamente. |
| Handle gráfico inválido o bitmap ya seleccionado | `ERROR_INVALID_HANDLE` (6) o `ERROR_ACCESS_DENIED` (5). La reserva que supera el límite falla con `ERROR_NOT_ENOUGH_MEMORY` (8). |
| Transferencia fuera de los límites del bitmap fuente | `ERROR_INVALID_PARAMETER` (87) con la superficie de destino intacta. Una operación de rasterización fuera del perfil devuelve `ERROR_NOT_SUPPORTED` (50). |
| Menú cíclico, submenú activo y menú ya liberado | `ERROR_INVALID_PARAMETER` (87), `ERROR_ACCESS_DENIED` (5) o `ERROR_INVALID_HANDLE` (6). |
| Temporizador ya destruido y filtro invertido | `ERROR_INVALID_HANDLE` (6) o `ERROR_INVALID_PARAMETER` (87). La salida del mensaje quedó intacta al usar una ventana inválida. |

### Reproducir la primera etapa

Entorno local del 10 de noviembre de 2026. Debian 13 x86-64, Linux
6.18.57, GCC 14.2.0, glibc 2.41 y GNU binutils 2.44. Los backends de
esta etapa son relojes y memoria de Linux, aperturas de archivos con
`openat2` y temporizadores `timerfd`.

~~~sh
make -j2 all check check-pe
build/pe-import-audit --unresolved /ruta/a/mines.exe
~~~

Pasaron las pruebas nativas, el PE freestanding y los casos de ABI.
Con UBSan pasaron también los anfitriones de los dos PE que importan
el contador mediante la IAT. MinGW-w64 estaba ausente en el entorno
local. Para esos dos anfitriones se reutilizaron los PE de control
generados con binutils en Linux en la etapa anterior. GitHub Actions
compila los PE desde fuente con MinGW-w64 en Ubuntu y valida el RPM en
Fedora. Las comprobaciones locales de ASan y UBSan pasaron con
`ASAN_OPTIONS=detect_leaks=0`. También pasó la compilación con fortify,
LTO y protección de pila.

El archivo auditado tiene 353.864 bytes y SHA-256
`3ba695d8bccac02cb9e6d63e630b50be15b729a952769814bbb684f9c9f096f3`.
`--unresolved` imprime una fila TSV por importación pendiente, seguida
del resumen. El inventario se vuelve a generar con el auditor al añadir equivalentes.

## Segunda etapa

| Bloque | Nuevos | Contrato probado |
| --- | ---: | --- |
| Archivos Win32 y CRT | 3 | Cinco disposiciones de apertura de archivos regulares, lectura y escritura, compartición por inodo dentro del proceso y transferencia HANDLE → descriptor → flujo. El cierre tiene un único propietario. |
| Arranque y cierre | 8 | Ruta y argumentos del ejecutable invitado, entorno copiado, salida normal con callbacks y salida rápida con código de 32 bits. La terminación se aplica al proceso Linux del fixture. |
| Ventanas y entrada | 21 | Ventanas superiores, áreas cliente, dibujo con DC y recorte de daño, títulos ANSI 1252, mensajes de teclado y ratón, datos por ventana y área de trabajo. Los temporizadores existentes se pueden asociar a ventanas. |
| Dibujo y texto GDI | 10 | Cairo y las fuentes de Linux producen medidas ANSI y UTF-16, glifos recortados, elipses, polígonos y polilíneas. Las capacidades del dispositivo describen el raster y el DPI lógico del perfil. |
| Portapapeles | 4 | Sesión de apertura, borrado y publicación de texto ANSI o UTF-16 a través de SDL. El portapapeles adquiere la memoria móvil tras publicar el texto. |
| Entrada y salida CRT | 5 | Tres flujos estándar asignados por el lanzador, modo de confirmación con fdatasync y formatos estrechos validados mediante libffi y glibc. |
| Contexto y excepciones | 6 | Registros x86-64 y FXSAVE, búsqueda por intervalo en la tabla de funciones, filtro superior y slots de excepción por hilo. |
| Finalización y matemáticas CRT | 4 | Destructores TLS antes de los callbacks, tipo de aplicación y mensaje de aserción, y callback de dominio de sqrt. |
| Menú en ventana y colores | 6 | Barra y submenú de un nivel dibujados en la ventana SDL, áreas calculadas desde el mismo diseño y selección con WM_COMMAND. El menú reserva parte del área de la ventana y usa una paleta de invitado. |
| Impresión | 5 | La consulta del dispositivo predeterminado entrega un DC PDF, DEVMODEA y DEVNAMES. El runtime compone y entrega un PDF de varias páginas a un descriptor asignado por el anfitrión. |
| Desenrollado AMD64 | 1 | Unwind de prólogo y cuerpo para metadatos versión 1, con límites explícitos de imagen y pila. Restaura registros y obtiene datos del manejador dentro de la imagen. |
| Terminación mínima, filtro CRT y audio | 3 | La terminación mínima registra el estado y conserva los callbacks. El filtro CRT entrega las excepciones soportadas a los manejadores del hilo invitado. SDL encola un tono de aviso. |

El perfil de archivos admite acceso genérico y compartición de lectura y
escritura. La raíz de archivos, el entorno y la identidad del módulo son
proporcionados por el anfitrión. El cierre de procesos requiere el trabajador
Linux del fixture. Los handles de proceso de terceros, los descriptores de
seguridad personalizados y la expansión de comodines quedan pendientes.

Las ventanas y callbacks pertenecen al hilo que abre SDL 2. La ventana
tiene una superficie GDI que se presenta al cerrar un ciclo de pintura.
El perfil inicial usa ventanas superiores, texto ANSI y el subconjunto
de teclado indicado en las pruebas. Wayland decide la posición global
de la ventana. Las clases con menús de recursos y ventanas hijas quedan
pendientes.

### Pruebas y errores de la segunda etapa

- Linux x86-64, Debian 13, kernel 6.18.57, GCC 14.2.0, glibc 2.41, GNU
  binutils 2.44, MinGW-w64 14 y SDL 2.32.4.
- Pasaron `make -j2 all check check-pe check-pe-import` y
  `SDL_VIDEODRIVER=offscreen make check-desktop`. El PE de escritorio
  resolvió 13 importaciones reales de USER32 y GDI a través de la IAT,
  pintó una ventana y recibió el cierre desde un temporizador.
- Un acceso de archivo incompatible devuelve `ERROR_SHARING_VIOLATION`
  y conserva el contenido al intentar truncarlo. Un handle transferido
  al CRT deja de poder cerrarse con `CloseHandle`.
- La prueba de arranque observó `ERROR_INSUFFICIENT_BUFFER` al truncar
  la ruta del módulo. Un intento de terminar desde el anfitrión devolvió
  `ERROR_ACCESS_DENIED`.
- El entorno local denegó sockets AF_UNIX a Weston. En Ubuntu, Weston 13 presentó la ventana del PE de control con la
  ruta de texturas de SDL y Mesa por software. En el entorno local se
  verificaron color, recorte, entrada y cierre con `offscreen`.
- Mines sigue siendo una auditoría de solo lectura. El mapa de carga
  permite las importaciones por nombre de los PE de control incrustados
  y conserva la separación entre páginas de código y datos.

Referencias de contrato del Windows SDK, fichas de 2018 a 2025.
[CreateFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea) ·
[Propiedad del handle CRT](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/open-osfhandle) ·
[GetModuleFileNameA](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamea) ·
[Ventanas y mensajes](https://learn.microsoft.com/en-us/windows/win32/winmsg/windowing) ·
[SDL 2](https://github.com/libsdl-org/SDL/tree/release-2.32.4).

El perfil GDI usa fuentes del sistema y puede obtener métricas diferentes a las
fuentes instaladas en otros entornos. La alineación inicial es superior
izquierda y las opciones de texto comprobadas son recorte y fondo opaco.

La prueba del CRT reprodujo `D%d%n` y `%2x%2x%2x` de Mines. El
puente conserva el tamaño de `long` en LLP64, prueba truncamiento y
rechaza formatos fuera del subconjunto antes de llamar a glibc. El
portapapeles se verificó en SDL `offscreen` con transferencia de propiedad
y acentos. Las pruebas de excepción ejercitan entradas controladas,
contexto por hilo y devolución de filtros. La entrega de fallos de un PE externo, los epílogos y el encadenado
de desenrollado quedan pendientes.

El menú probado acepta comandos y un nivel de submenú. El ajuste de
rectángulo se verificó con una ventana sin marco. `PrintDlgA` cubre
`PD_RETURNDEFAULT` y `PD_RETURNDC` para el dispositivo PDF. La
impresión mantiene la salida anterior hasta finalizar el documento y
la prueba comprobó las dos páginas, el título y la cabecera PDF.
El filtro del CRT usa handlers por hilo sin instalar señales en Linux.
El audio se probó en la cola SDL del backend de audio de prueba.

| Caso de error | Resultado |
| --- | --- |
| Menú adjunto a una ventana | Se rechaza la destrucción directa con `ERROR_ACCESS_DENIED`. El cierre de la ventana destruye el menú adjunto. |
| Impresión fuera del orden del documento | `ERROR_INVALID_PARAMETER` al cerrar una página inexistente o un documento con una página abierta. |
| Desenrollado en un epílogo o fuera de la pila autorizada | `ERROR_NOT_SUPPORTED` o `ERROR_INVALID_PARAMETER`, con el contexto de entrada intacto. |
| Lectura CRT con anchura de texto cero o ausente | `EINVAL` antes de llamar a glibc. |
| Código de aviso sin tono asignado | `ERROR_INVALID_PARAMETER`. |

## Tercera etapa

| Biblioteca | Nuevas | Símbolos y contrato probado |
| --- | ---: | --- |
| `USER32.dll` | 6 | `CheckDlgButton`, `DefDlgProcA`, `GetDlgItemTextA`, `IsDialogMessageA`, `IsDlgButtonChecked`, `SendDlgItemMessageA`. El diálogo llama a su procedimiento con ABI x64 y devuelve `DWLP_MSGRESULT`. Los IDs alcanzan controles hijos con texto ANSI, estados de botón, listas de opciones y notificaciones `WM_COMMAND`. Tabulador, Intro, Escape, teclado de edición y clic actúan sobre los controles dibujados con Cairo. |
| `USER32.dll` | 2 | `LoadCursorA` carga cursores de sistema SDL y `LoadIconA` crea superficies RGBA de iconos de sistema y recursos numéricos `RT_GROUP_ICON`/`RT_ICON` DIB. La clase aplica ambos objetos a su ventana SDL. |
| `USER32.dll` | 3 | `SetCapture`, `ReleaseCapture`, `SetForegroundWindow`. La captura entrega el handle anterior, enruta eventos entre ventanas de la aplicación y envía `WM_CAPTURECHANGED`. La activación depende del backend. |
| `USER32.dll` | 2 | `MessageBoxA` crea una ventana modal SDL, devuelve el ID elegido y restaura al propietario. `WinHelpA` muestra el documento de texto provisto por el lanzador y cierra su ventana con `HELP_QUIT`. |
| `COMDLG32.dll` | 2 | `GetOpenFileNameA`, `GetSaveFileNameA`. Selector de un archivo, navegación, filtros, extensión predeterminada, confirmación de sobrescritura, cancelación y salida con offsets y título. Admite `OPENFILENAMEA` AMD64 de 136 y 152 bytes. |
| `COMCTL32.dll` | 1 | `#17` resuelve `InitCommonControls` y habilita la clase `msctls_statusbar32`. La prueba verifica su registro, texto y representación en la ventana. |
| `VCRUNTIME140.dll` | 1 | `__C_specific_handler` interpreta tablas C SEH AMD64 con alcance acotado. Distingue continuación, búsqueda y selección de handler. Durante unwind invoca finalizadores y conserva el índice de la tabla. El despachador aporta el intervalo de código, la pila y la transferencia. |

El perfil de controles admite `BUTTON`, `EDIT`, `STATIC`, `COMBOBOX`,
`LISTBOX` y la barra de estado. El selector lista hasta 256 entradas de
un máximo de 4096 examinadas por directorio y mantiene los nombres del
perfil ASCII del disco C:. Valida directorios y archivos con `openat2`
confinado. Los hooks, plantillas y la selección múltiple quedan pendientes.
El resultado de Guardar selecciona una ruta y el programa invitado decide
cuándo crear o reemplazar el archivo.

El cargador de iconos acepta hasta 4 MiB de recursos de un módulo, con
iconos DIB sin compresión. Los recursos de cursor privados quedan pendientes.
La captura fuera de la aplicación depende del compositor. El visor de ayuda
cubre `HELP_CONTENTS`, `HELP_FINDER` y `HELP_QUIT` con texto preparado.
Quedan pendientes la lectura de los formatos binarios HLP y CHM, el despacho
general de excepciones y la carga de ejecutables externos.

### Verificación en Linux

El entorno local usó Linux 6.18.57 x86-64 en Debian 13, glibc 2.41,
GCC 14.2.0, binutils 2.44, MinGW GCC 14.2 con cabeceras MinGW-w64 12.0.0,
SDL 2.32.4 y Cairo 1.18.4. Se usó el backend SDL `offscreen`.
La suite local pasó también con ASan y UBSan, con la detección de fugas
desactivada. La prueba de GitHub con Weston verificó la carga nativa del
cursor de sistema.

```sh
make -j2 all check check-pe check-pe-import
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 make -j2 check-desktop
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 make -j2 \
  BUILD_DIR=build-ubsan CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
  all check check-pe check-pe-import check-desktop
build/pe-import-audit --unresolved /ruta/a/mines.exe
objdump -p build/win32-desktop.exe
```

El archivo auditado tiene SHA-256
`3ba695d8bccac02cb9e6d63e630b50be15b729a952769814bbb684f9c9f096f3`.
El auditor devolvió 197 importaciones, 197 equivalentes y 0 pendientes.
`objdump -p` confirmó el ordinal 17 en el PE de escritorio generado.
Se extrajo la sección `.rsrc` de Mines con `objcopy -O binary
--only-section=.rsrc` y se verificó que el icono 200 se decodifica
en 32 × 32 píxeles RGBA. La inspección y la decodificación leyeron datos del
archivo. Las pruebas que ejecutaron código PE usaron los fixtures del build.

| Caso probado | Resultado observado |
| --- | --- |
| ID de control ausente | `ERROR_CONTROL_ID_NOT_FOUND`. La prueba de texto también comprueba el NUL final y el límite de copia. |
| Recurso DIB truncado | La carga falla y los handles ya entregados siguen válidos. |
| Cursor en SDL `offscreen` | El backend carece de dispositivo de puntero. `LoadCursorA` informa `ERROR_GEN_FAILURE`. |
| Intento de seleccionar un enlace simbólico | La validación lo rechaza y el usuario puede cancelar sin cambiar la ruta del llamador. |
| Estructura de selector inválida y búfer pequeño | `CDERR_STRUCTSIZE` y `FNERR_BUFFERTOOSMALL`. Se escribe el tamaño requerido como WORD si hay espacio. |
| Ayuda sin documento provisto | `ERROR_FILE_NOT_FOUND`. |
| Tabla SEH inválida o destino fuera del código autorizado | `ExceptionContinueSearch` con `ERROR_INVALID_PARAMETER`, sin invocar el callback. |
| Ordinal ajeno al perfil, bits reservados activos o IAT inconsistente | El parser del fixture rechaza los bytes antes de ejecutarlos. |

Los contratos se contrastaron con las declaraciones públicas de MinGW-w64
12.0.0 en `winuser.h`, `commdlg.h`, `commctrl.h`, `cderr.h`, `winnt.h`
y `excpt.h`. El ordinal coincide con la tabla de Wine 11.0. El código
propio mantiene la licencia MIT y reutiliza las dependencias SDL y Cairo del
proyecto. [Tabla de COMCTL32 en Wine 11.0](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/comctl32/comctl32.spec).

## Partidas y revisión de menús del 10 de noviembre de 2026

Las pruebas anteriores completaron una victoria y una derrota del ejecutable
original en Linux fuera de pantalla con aislamiento. El usuario comunicó
después una victoria y una derrota en un portátil Fedora KDE Wayland y aportó
una captura con `COMPLETED!`. También informó de fallos de menús, con
`Restart` operativo. La compatibilidad se registra por funcionalidad y por
entorno junto al recuento de símbolos.

Se reprodujeron tres límites del runtime compartido.

| Fallo reproducido | Contrato y corrección |
| --- | --- |
| `GameAboutBox` y `GameConfigBox` pedían 38 bytes por ventana y `RegisterClassA` devolvía 0. | `cbWndExtra` admite de 0 a 65.536 bytes por ventana en este perfil, inicializados a cero y liberados al destruirla. `GetWindowLongPtrA` y `SetWindowLongPtrA` aceptan offsets de bytes dentro del bloque, incluidos los desalineados. La escritura devuelve el valor anterior. Ambas conservan el último error al tener éxito. Un acceso fuera del bloque devuelve 0 y `ERROR_INVALID_INDEX`. |
| Los elementos que excedían el alto del cliente quedaban inaccesibles. | La rueda desplaza el desplegable activo por sus filas. La representación y la selección usan el mismo intervalo visible. |
| Pulsar una entrada con submenú cerraba el desplegable. | Se conserva una pila de hasta 16 desplegables. Cada apertura entrega `WM_INITMENUPOPUP` con el handle y la posición del elemento. `WM_INITMENU` precede a la primera apertura y `WM_UNINITMENUPOPUP` acompaña al cierre. Escape retrocede un nivel y los elementos deshabilitados conservan su estado. |

Las pruebas genéricas comprueban bloques independientes por ventana, el
último offset válido, los errores de límites, un menú mayor que su ventana,
tres niveles de desplegables, un elemento habilitado durante la notificación
de apertura, Escape, la destrucción del propietario durante la apertura y
la entrega de `WM_COMMAND` con `lParam` igual a cero.
Se mantienen los límites de la representación dentro de la ventana SDL y
el alcance del teclado definido por esas pruebas.

El uso de `DefDlgProcA` con el bloque de diálogo reservado identifica las
ventanas de diálogo. Una ventana ordinaria con bytes adicionales conserva
la entrega de Intro y Escape a su procedimiento. La prueba de teclado cubre
esta distinción tras la revisión del PR.

En el ejecutable original se seleccionaron mediante eventos SDL de clic y
rueda `New`, `Restart`, `About`, `Specific`, `Random Seed`, `Custom`,
`Preferences`, el tablero cuadrado de 16 × 16 con 40 minas y el tablero
Snub-Square de 5 × 5. Los diálogos se abrieron y cerraron mediante su evento
de cierre. Los nueve procesos terminaron con estado 0 después del cierre
explícito de la ventana principal. Estas comprobaciones cubren la apertura,
cancelación y selección de tipos ensayadas. La prueba física de la revisión
de menús sigue pendiente.

Entorno local Debian 13 x86-64, Linux 6.18.57, GCC 14.2, MinGW-w64 14,
SDL 2.32.4 y Cairo 1.18.4, con backend `offscreen`. Pasaron
`make -j2 all check check-pe check-pe-import check-pe-image check-desktop`
y la repetición de esas suites con UBSan. Para el PE original, la restricción
de `NETLINK_ROUTE` del entorno impide `--unshare-all`. El arnés conserva
aislamiento de usuario, PID, montaje, IPC y UTS y aplica seccomp para denegar
la creación de sockets e `io_uring`. El lanzador publicado conserva su
aislamiento habitual.

Contratos consultados de Microsoft Learn para
[WNDCLASSA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-wndclassa),
[SetWindowLongPtrA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptra),
[WM_INITMENU](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-initmenu),
[WM_INITMENUPOPUP](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-initmenupopup)
y [WM_UNINITMENUPOPUP](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-uninitmenupopup).
La revisión de límites contrastó también el tratamiento de índices inválidos
de las [pruebas de ventanas de Wine 11.0](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/user32/tests/win.c).

## Preferencias persistentes del 11 de noviembre de 2026

El usuario informó del aviso `Error saving preferences` con el detalle
`Could not determine pathname for configuration files`. El ejecutable
original consulta `APPDATA` mediante `getenv`. El lanzador anterior limpiaba
el entorno y mantenía un disco C: temporal. La prueba reprodujo el aviso al
abrir `Preferences` y confirmar con Intro.

El lanzador proporciona ahora un perfil persistente compartido por usuario
y las variables estándar descritas en el
[contrato del cargador](isolated-loader.md#perfil-persistente-del-usuario).
El código del perfil se aplica a todos los ejecutables lanzados por esta
entrada. El aislamiento conserva las comprobaciones de rutas y los permisos
del EXE y cierra los descriptores de preparación antes de ejecutar el PE.

En Linux fuera de pantalla, el Mines original abrió `Preferences`, confirmó
la selección y guardó un archivo `mines.conf` de 69 bytes en el directorio
de datos de Simon Tatham. Después se cambió `highlight-flags` a `true` en
ese archivo de prueba. Un proceso nuevo abrió las preferencias y volvió a
guardarlas conservando ese valor. Ambos procesos terminaron con estado 0
después del cierre explícito de la ventana principal. El aviso de error
desapareció en estos recorridos.

Un PE independiente generado por MinGW-w64 importa `getenv`, `fopen`,
`fread`, `fwrite` y `fclose`. Dos lanzamientos aislados guardaron primero
`P` y después `PP` en su archivo de estado. Se usó el mismo lanzador y las
mismas implementaciones del runtime. Las pruebas nativas verificaron la
reapertura del perfil, cuatro preparaciones concurrentes, el fallback XDG y
el rechazo de enlaces y permisos no privados.

Se mantuvo el entorno Linux, SDL y compiladores de la revisión de menús.
Pasaron `make -j2 all check check-pe check-pe-import check-pe-image
check-desktop` y las suites con UBSan. La restricción `NETLINK_ROUTE` hizo
que `check-pe-isolated` declarase `SKIP` en este host. Para los dos PE y
las comprobaciones de Mines se utilizó el arnés aislado con seccomp descrito
en la sección anterior, atravesando la entrada normal del lanzador.
La prueba física del guardado en Fedora queda pendiente.
