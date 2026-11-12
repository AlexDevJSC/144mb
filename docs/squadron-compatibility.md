# Compatibilidad nativa de Squadron121 en Linux

Auditoría de las importaciones directas del ejecutable AMD64
`Squadron121_DX12.exe` de la versión 2026.10.19-rc4. SHA-256
`cfc4bbeb6948490f72ca63b031c9339c7eb68a9ef40f0b54b78da58cd8355d01`.

| Biblioteca | Pendientes iniciales | Cubiertas antes de esta rama | Cubiertas en esta rama |
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

El auditor resuelve las 324 importaciones directas del binario, incluidos
los 16 ordinales. El resultado requiere FluidSynth, SDL2_image,
vkd3d y Vulkan, el compilador DXBC de vkd3d-utils y el DXC nativo de Linux.

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

## Portabilidad del compilador DXC en Fedora

El fixture PE `tests/pe/shader_fixture.c` compila HLSL con `IDxcCompiler3`,
incluye `color.hlsli` desde el disco C: del invitado, y verifica que
`IDxcResult::GetOutput(DXC_OUT_OBJECT)` devuelve en UTF-16 el nombre
`résultat.dxil`. En Fedora 45 con DXC Linux 1.8.2505.1, pasar el nombre
Unicode directamente al compilador nativo producía
`0x80AA000C (DXC_E_STRING_ENCODING_FAILED)`. Forzar una configuración
regional UTF-8 no lo solucionó; usar un nombre ASCII sí. Se corresponde
con el problema de rutas Unicode descrito en
[DirectXShaderCompiler #7394](https://github.com/microsoft/DirectXShaderCompiler/issues/7394).

El adaptador `src/runtime/dxcompiler.c` conserva sin modificar los
argumentos del PE; para un valor no ASCII de `-Fo`, transmite a DXC una
representación ASCII de ese nombre y mantiene una copia del original.
Cuando el invitado consulta el nombre del objeto compilado, devuelve
el UTF-16 original mediante su interfaz COM. Esta compatibilidad está
acotada al nombre de salida `-Fo` de `IDxcCompiler3` y no promete
normalizar todas las rutas de DXC ni repararlas en su biblioteca.

El RPM empaqueta las pruebas `%check` con LTO y `-Werror`. La prueba
`tests/compat_batch_test.c` inicializa explícitamente las estructuras
que `GetSystemTimeAsFileTime` puede dejar intactas si falla el reloj.
La CI de Fedora también ejecuta `check-pe-shaders` después de instalar
DXC desde su paquete de Linux con SHA-256 verificado.

## Reproducir en Linux

```sh
make -j2 all check check-pe check-pe-import check-pe-image check-desktop check-pe-native
make -j2 check-pe-console check-pe-displayconfig check-pe-platform check-pe-seh
make -j2 check-pe-graphics check-pe-wic check-pe-shaders
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
OpenSSL 3.5.7, FluidSynth 2.4.4, SDL 2 offscreen, Mesa lavapipe 25.0.7,
vkd3d 1.19 y DXC 1.8.2505.1. Se repitieron las pruebas del parser y del
ABI con UBSan. Mines conserva 197 de 197 importaciones directas.

## Arranque del ejecutable original

El cargador prepara TLS y los parámetros de proceso que el PE consulta
mediante GS y el PEB. Squadron121 entra en su código principal, abre las
fábricas DXGI y busca un adaptador D3D12. En Codex solo se detectó Mesa
lavapipe. DXGI lo identifica como adaptador software, que el juego omite.
El ejecutable informa `No Direct3D 12 Feature Level 12_0 adapter found.`
El despacho C++ con `STATUS_UNWIND_CONSOLIDATE` entrega esa excepción al
manejador del juego y se comprobó la aparición del error del propio juego.

El modo Autoplay Lab del mismo ejecutable completa partidas sin gráficos.
Se probaron diez semillas del nivel 1 con el agente heurístico incorporado.
Seis partidas terminaron en derrota y cuatro alcanzaron el límite de tiempo.
El juego guardó un JSON válido, devolvió código 0 y confirmó las diez
comparaciones de determinismo. Se corrigió `CreateFileW` para aceptar el
atributo estándar de herencia que usa el CRT al abrir ese archivo.

Otra ejecución probó los 32 niveles con tres semillas por nivel y hasta
ocho continuaciones. De 96 partidas, 28 despejaron el tablero, 47
terminaron en derrota y 21 alcanzaron el límite de tiempo. El programa
escribió los 96 resultados en JSON y devolvió código 0. Por ejemplo, en
el nivel 1 la semilla 2 despejó el tablero en 7787 ticks.

```sh
SDL_VIDEODRIVER=offscreen pe-isolated-run /ruta/a/Squadron121_DX12.exe \
  --bench squadron-headless.json --agent heuristic --channel structured \
  --clock step --levels 1 --seeds 10 --seed-base 1 --max-ticks 36000 --determinism-all
```

Los datos del juego se disponen en el disco C: del invitado para esta
prueba. El resultado se escribe en la raíz de ese disco persistente.

Bubblewrap no pudo configurar el namespace de red estándar en Codex.
La prueba usó un arnés local con namespaces de usuario, montaje, PID,
IPC y UTS, y seccomp para bloquear sockets. Se usaron los datos del juego
y el ejecutable original. Las DLL de Windows del paquete quedaron fuera.

`check-pe-graphics` verifica la creación de un dispositivo D3D12, la
copia de datos y la sincronización de una cola mediante Vulkan. Queda
pendiente la presentación de fotogramas a través de `IDXGISwapChain` y
el acceso a una GPU física en el sandbox. El perfil de Streamline devuelve
`eErrorFeatureMissing` para las extensiones de proveedor como DLSS y Reflex.
