# Compatibilidad de Mines en Linux

El auditor leyó **197 importaciones directas** en 15 bibliotecas de
`mines.exe`. La primera etapa agregó **74 equivalentes** y la segunda agregó **61**.
Quedan **32 importaciones sin resolver**, incluida una por ordinal.

| Estado | Base | Primera etapa | Segunda etapa |
| --- | ---: | ---: | ---: |
| Importaciones con un equivalente registrado | 30 | 104 | 165 |
| Importaciones sin resolver | 167 | 93 | 32 |

El recuento describe los puntos de entrada disponibles y sus perfiles
probados. Las búsquedas que el programa haga en tiempo de ejecución
quedan fuera de la medición. El análisis de Mines fue de solo lectura.
La ejecución de código PE se limitó a los binarios de control del proyecto.

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
- El entorno local denegó sockets AF_UNIX a Weston. La prueba de
  presentación Wayland se ejecuta en la CI de Linux. El backend local
  `offscreen` permitió verificar color, recorte, entrada y cierre.
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
contexto por hilo y devolución de filtros. La entrega de fallos PE
y el desenrollado general quedan pendientes.

## Inventario pendiente

| Biblioteca | Pendientes | Símbolos |
| --- | ---: | --- |
| `USER32.dll` | 20 | `AdjustWindowRectEx`, `CheckDlgButton`, `DefDlgProcA`, `DrawMenuBar`, `GetDlgItemTextA`, `GetMenu`, `GetMenuItemRect`, `GetSysColor`, `IsDialogMessageA`, `IsDlgButtonChecked`, `LoadCursorA`, `LoadIconA`, `MessageBeep`, `MessageBoxA`, `ReleaseCapture`, `SendDlgItemMessageA`, `SetCapture`, `SetForegroundWindow`, `SetMenu`, `WinHelpA` |
| `KERNEL32.dll` | 1 | `RtlVirtualUnwind` |
| `GDI32.dll` | 4 | `EndDoc`, `EndPage`, `StartDocA`, `StartPage` |
| `COMDLG32.dll` | 3 | `GetOpenFileNameA`, `GetSaveFileNameA`, `PrintDlgA` |
| `COMCTL32.dll` | 1 | `#17` |
| `VCRUNTIME140.dll` | 1 | `__C_specific_handler` |
| `api-ms-win-crt-runtime-l1-1-0.dll` | 2 | `_c_exit`, `_seh_filter_exe` |
