# Compatibilidad de Mines en Linux

El auditor leyó **197 importaciones directas** en 15 bibliotecas de
`mines.exe`. Esta etapa agregó **74 equivalentes con pruebas en Linux**.
Quedaron **93 importaciones sin resolver**, incluida una por ordinal.

| Estado | Antes | Ahora |
| --- | ---: | ---: |
| Importaciones con un equivalente registrado | 30 | 104 |
| Importaciones sin resolver | 167 | 93 |

El recuento describe los puntos de entrada disponibles y sus perfiles
probados. Las búsquedas que el programa haga en tiempo de ejecución
quedan fuera de la medición. El análisis de Mines fue de solo lectura.
La ejecución de código PE se limitó a los binarios de control del proyecto.

## Equivalentes probados

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

## Errores reproducidos

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

## Reproducir la prueba

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
del resumen. El inventario de abajo se obtuvo de esas filas.

## Qué impide el arranque

- El cargador actual ejecuta PE de laboratorio generados durante el
  build. Mines necesita un cargador general con relocaciones, TLS,
  estado de proceso e hilo NT y aislamiento del invitado.
- La superficie GDI de memoria y los objetos USER32 de esta etapa
  necesitan una ventana, entrada, recursos y pintura conectados al
  escritorio para mostrar el juego. También faltan texto, fuentes,
  formas adicionales, diálogos e impresión.
- La apertura de archivos por handle, la salida de proceso y el
  arranque y excepciones del CRT requieren servicios del invitado
  adicionales. Los resultados de la auditoría solo identifican las
  dependencias directas que todavía faltan en el registro.

## Inventario pendiente

| Biblioteca | Pendientes | Símbolos |
| --- | ---: | --- |
| `USER32.dll` | 45 | `AdjustWindowRectEx`, `BeginPaint`, `CheckDlgButton`, `CloseClipboard`, `CreateWindowExA`, `DefDlgProcA`, `DefWindowProcA`, `DestroyWindow`, `DrawMenuBar`, `EmptyClipboard`, `EnableWindow`, `EndPaint`, `GetClientRect`, `GetDC`, `GetDlgItemTextA`, `GetKeyboardState`, `GetMenu`, `GetMenuItemRect`, `GetSysColor`, `GetWindowLongPtrA`, `GetWindowRect`, `InvalidateRect`, `IsDialogMessageA`, `IsDlgButtonChecked`, `LoadCursorA`, `LoadIconA`, `MessageBeep`, `MessageBoxA`, `OpenClipboard`, `RegisterClassA`, `ReleaseCapture`, `ReleaseDC`, `SendDlgItemMessageA`, `SendMessageA`, `SetCapture`, `SetClipboardData`, `SetForegroundWindow`, `SetMenu`, `SetWindowLongPtrA`, `SetWindowPos`, `SetWindowTextA`, `ShowWindow`, `SystemParametersInfoA`, `TranslateMessage`, `WinHelpA` |
| `KERNEL32.dll` | 8 | `CreateFileA`, `GetModuleFileNameA`, `RtlCaptureContext`, `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `SetUnhandledExceptionFilter`, `TerminateProcess`, `UnhandledExceptionFilter` |
| `GDI32.dll` | 14 | `CreateFontA`, `CreateFontIndirectA`, `Ellipse`, `EndDoc`, `EndPage`, `ExtTextOutW`, `GetDeviceCaps`, `GetTextExtentPoint32A`, `GetTextExtentPoint32W`, `GetTextMetricsA`, `Polygon`, `Polyline`, `StartDocA`, `StartPage` |
| `COMDLG32.dll` | 3 | `GetOpenFileNameA`, `GetSaveFileNameA`, `PrintDlgA` |
| `COMCTL32.dll` | 1 | `#17` |
| `VCRUNTIME140.dll` | 3 | `__C_specific_handler`, `__current_exception`, `__current_exception_context` |
| `api-ms-win-crt-stdio-l1-1-0.dll` | 6 | `__acrt_iob_func`, `__p__commode`, `__stdio_common_vfprintf`, `__stdio_common_vsprintf`, `__stdio_common_vsscanf`, `_open_osfhandle` |
| `api-ms-win-crt-math-l1-1-0.dll` | 2 | `__setusermatherr`, `_fdopen` |
| `api-ms-win-crt-runtime-l1-1-0.dll` | 11 | `_c_exit`, `_configure_narrow_argv`, `_exit`, `_get_narrow_winmain_command_line`, `_initialize_narrow_environment`, `_register_thread_local_exe_atexit_callback`, `_seh_filter_exe`, `_set_app_type`, `_wassert`, `exit`, `terminate` |
