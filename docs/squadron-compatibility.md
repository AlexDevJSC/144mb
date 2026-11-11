# Compatibilidad nativa de Squadron121 en Linux

Auditoría de las importaciones directas del ejecutable AMD64
`Squadron121_DX12.exe` de la versión 2026.10.19-rc4. SHA-256
`cfc4bbeb6948490f72ca63b031c9339c7eb68a9ef40f0b54b78da58cd8355d01`.

| Biblioteca | Pendientes iniciales | Nuevos equivalentes | Pendientes |
| --- | ---: | ---: | ---: |
| KERNEL32 | 121 | 112 | 9 |
| USER32 | 47 | 44 | 3 |
| FluidSynth | 22 | 22 | 0 |
| WS2_32 | 15 | 15 | 0 |
| BCrypt | 7 | 7 | 0 |
| GDI32 y MSIMG32 | 4 | 4 | 0 |
| ADVAPI32 | 3 | 3 | 0 |
| COMDLG32 y COMCTL32 | 3 | 3 | 0 |
| IMM32 | 4 | 4 | 0 |
| SHELL32 | 4 | 1 | 3 |
| OLE32 | 3 | 2 | 1 |
| Streamline y compiladores de shaders | 20 | 0 | 20 |
| **Total** | **253** | **217** | **36** |

El binario pasó de 71 a 288 importaciones con un equivalente registrado
entre 324 directas. El resultado incluye los 16 ordinales del binario.
Los equivalentes se comprueban con sus contratos soportados. El juego
requiere también inicialización, llamadas dinámicas y los servicios
gráficos pendientes.

## Contratos comprobados

- Archivos Unicode en C: con apertura confinada, lectura, escritura,
  listado, cambio de directorio, metadatos y truncado. Se preservan
  las reglas de `openat2` del runtime. Los nombres y comodines tienen
  el alcance de mayúsculas del backend Linux.
- Heap del proceso, variables de entorno invitadas, línea de comandos,
  relojes y módulos incorporados. Las operaciones de consola admiten
  la salida por un terminal heredado. `VirtualProtect` actúa sobre la
  imagen invitada y admite permisos compatibles con W^X.
- Eventos anónimos, hilos pthread, FLS y sincronización local con futex.
  Las esperas admiten timeout y eventos de rearme automático o manual.
  La plantilla TLS estática se copia y completa con ceros en cada hilo.
  Se prueban los callbacks del proceso y del hilo con el ABI Microsoft x64.
- Ventanas y mensajes Unicode, monitores SDL, entrada relativa,
  geometría de IME, selectores y diálogos. `TaskDialogIndirect` ofrece
  botones comunes y personalizados y las notificaciones verificadas
  de creación, selección y destrucción. GDI admite DIB BGRA de 32 bits,
  selección de fuente, medidas de texto y composición alfa.
- Configuración regional de compatibilidad en-US con CP1252, CP437
  y UTF-8. Se usa la clasificación y el cambio de caja de Unicode de
  glibc. El perfil de versión declarado es Windows 10 10.0.19045.
- Registro de consulta bajo `C:\Registry\HKCU` y `C:\Registry\HKLM`.
  Cada clave corresponde a un directorio. Los valores son archivos
  `.sz` en UTF-8, `.dword` numéricos o `.bin` binarios. Por ejemplo,
  `HKCU\Software\Example` con un valor `Title` se consulta desde
  `C:\Registry\HKCU\Software\Example\Title.sz`. Las vistas viven en
  el disco del invitado. El acceso por API es de lectura.
- Winsock TCP/UDP IPv4 e IPv6 mediante sockets POSIX. El fixture
  comprueba TCP en loopback, operaciones no bloqueantes y `select`.
  Se aplica el namespace de red del lanzador.
- BCrypt usa OpenSSL para MD5, SHA-1, SHA-256, SHA-384 y SHA-512,
  con hashes de una vida y HMAC. FluidSynth usa `libfluidsynth.so.3`
  y abre los SoundFonts desde las rutas invitadas.

Las firmas y tamaños proceden de MinGW-w64 12.0 y los contratos públicos.
Los ordinales de Winsock se contrastaron con la tabla Wine 11.0. El
ordinal 345 de COMCTL32 corresponde a `TaskDialogIndirect`.
[Winsock 11.0](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ws2_32/ws2_32.spec) ·
[COMCTL32 6.10](https://geoffchappellmirror.github.io/studies/windows/shell/comctl32/history/ords610.htm) ·
[TaskDialogIndirect](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect) ·
[BCryptCreateHash](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash) ·
[FluidSynth](https://www.fluidsynth.org/api/group__synth.html).

## Reproducir en Linux

```sh
make -j2 all check check-pe check-pe-import check-pe-image check-desktop check-pe-native
build/pe-import-audit --unresolved /ruta/a/Squadron121_DX12.exe
build/native-audio-test /ruta/a/un-soundfont.sf2
```

`check-pe-native` ejecuta un PE de prueba generado por MinGW-w64. Comprueba
datos y efectos de archivos, heap, entorno, hilos, hashes, TCP local,
región, registro, ventanas Unicode y rasterización. El test de audio
carga un SoundFont de Linux a través de C: y mide las muestras de una
nota MIDI. El auditor habilita los símbolos de FluidSynth cuando
encuentra y enlaza los 22 símbolos del backend de Linux.

Pruebas locales en Linux 6.18.57 AMD64, GCC 14.2, MinGW-w64 GCC 14,
OpenSSL 3.5.7, FluidSynth 2.4.4 y SDL 2 offscreen. Se repitieron con
UBSan. El ejecutable original de Mines conserva 197 de 197 importaciones
y se verificó su flujo de preferencias en dos procesos aislados. Bubblewrap
no pudo configurar el namespace de red estándar en Codex. Las pruebas del
ejecutable original usaron el arnés local con aislamiento de usuario,
montaje, PID, IPC y UTS y seccomp para bloquear sockets.

El primer intento de carga de Squadron121 se detenía en su plantilla TLS
de 337 bytes. Tras el cambio el cargador supera esa validación y se
detiene en `sl.interposer.dll!D3D12CreateDevice`, antes de ejecutar
código del juego.

## Pendientes de la versión auditada

| Bloque | Símbolos pendientes |
| --- | --- |
| Streamline y shaders (20) | 18 importaciones de `sl.interposer.dll`, incluida la creación D3D12/DXGI, más `D3DCompile` y `DxcCreateInstance`. |
| Consola, procesos y excepciones (9) | `AllocConsole`, `AttachConsole`, `GetConsoleWindow`, `ReadConsoleW`, `SetConsoleTitleA`, `CreateProcessW`, `RaiseException`, `RtlUnwind` y `RtlUnwindEx`. |
| Topología de pantalla (3) | `DisplayConfigGetDeviceInfo`, `GetDisplayConfigBufferSizes` y `QueryDisplayConfig`. |
| Shell y COM (4) | `ShellExecuteW`, `SHParseDisplayName`, `SHOpenFolderAndSelectItems` y `CoCreateInstance`. |

La ruta gráfica requiere dispositivos D3D12, fábrica DXGI, compilación de
shaders y las interfaces de Streamline que el juego usa después de la
creación del dispositivo. Los candidatos a backend están documentados en
[Reutilización y licencias](reuse.md).
