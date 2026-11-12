# nt-native-linux

Base experimental de una capa de compatibilidad NT y Win32 integrada en
Linux. El objetivo es ejecutar aplicaciones de Windows en Fedora KDE Plasma
manteniendo el kernel y los controladores de Linux. Cada implementación
reproduce un contrato observable mediante recursos del anfitrión. El objetivo
inicial es x86-64 con gráficos AMD mediante amdgpu y Mesa RADV.

## Estado de la base 0.1.0

- `NtQueryPerformanceCounter` en C, respaldado por `CLOCK_BOOTTIME`.
- Un programa de prueba PE32+ AMD64 que llama a esa implementación mediante
  el ABI Microsoft x64. El anfitrión ejecuta el código de un binario de prueba
  generado e incrustado durante el build.
- Un segundo PE de consola, compilado con MinGW-w64, importa
  `ntdll.dll!NtQueryPerformanceCounter`. Linux resuelve esa importación con
  la implementación propia.
- El runtime expone equivalentes con alcance probado en Linux para las 197
  importaciones directas de Mines. Un PE de control verifica la llamada
  Win32 al reloj. Otro abre y pinta una ventana SDL, crea controles y
  resuelve 21 importaciones, incluida `COMCTL32.dll #17`.
- `pe-import-audit` lee las importaciones directas de un PE AMD64 y
  cuenta las que tienen un equivalente registrado.
- Un microbenchmark y un spec RPM para el comando ELF de diagnóstico.
- Existe un primer cargador PE32+ AMD64 con relocaciones DIR64, IAT y
  permisos W^X, más un lanzador Linux con Bubblewrap.
  Se verificaron victoria y derrota de Mines en Linux fuera de pantalla.
  El usuario ha confirmado también ambos finales en su portátil Fedora KDE.
  La revisión posterior amplía los diálogos y los menús a partir de los fallos
  comunicados. El alcance probado se detalla en `docs/mines-compatibility.md`.
  El cargador prepara la plantilla TLS del ejecutable en cada hilo
  invitado. TEB/PEB completos y procesos NT generales siguen pendientes.
- El lanzador proporciona un perfil persistente de usuario para las variables
  `APPDATA`, `LOCALAPPDATA`, `USERPROFILE`, `TEMP` y `TMP`. Se verificó el
  guardado y la recuperación de preferencias de Mines y de un PE independiente.
- Las aplicaciones pueden acceder directamente a carpetas elegidas de Linux
  mediante sus rutas de usuario de Windows. El runtime implementa el
  subconjunto de `SHGetKnownFolderPath` que corresponde a esas carpetas.

`nt-vulkan-probe` comprueba el cargador y los dispositivos Vulkan.
El perfil D3D12 crea dispositivos, colas y recursos a través de vkd3d y
Vulkan. El perfil DXGI enumera adaptadores Vulkan. La presentación de
fotogramas mediante DXGI queda pendiente.

## Compilar y probar

Para instalar desde GitHub en Fedora x86-64, usa el
[instalador para tu usuario](packaging/fedora/README.md#instalación-para-tu-usuario).

En Linux x86-64 con GCC, GNU Make, GNU binutils, pkg-config, SDL 2, Cairo, libffi y OpenSSL 3 de desarrollo:

```sh
make
make check
make check-pe
make check-pe-image
build/nt-vulkan-probe
make benchmark
# Con MinGW-w64: make check-pe-external (PE de escritorio generado)

```

`make check-pe` requiere que `ld -V` liste `i386pep` y que `objdump -i`
liste `pe-x86-64`. El build genera `build/nt-counter.exe` y
`build/pe-host`. Ejecuta `build/pe-host` para repetir la prueba. El PE
espera una función entregada por el anfitrión y no tiene un punto de entrada
de aplicación Windows convencional. Todas las pruebas se ejecutan en Linux.

Con MinGW-w64 AMD64 instalado, `make check-pe-import` genera los
ejecutables de prueba NT y Win32, muestra sus tablas de importación y
los ejecuta en los anfitriones correspondientes. Cada anfitrión ejecuta
exclusivamente el PE generado e incrustado en el build.

El mismo lanzador sirve para cada EXE compatible. Requiere Bubblewrap.

```sh
build/pe-isolated-run /ruta/al/programa.exe
build/pe-isolated-run --share "Documents=$(xdg-user-dir DOCUMENTS)" /ruta/al/programa.exe
```

El acceso a carpetas de Linux se habilita por lanzamiento. La cobertura
funcional se comprueba con cada aplicación. [Límites y modelo de aislamiento](docs/isolated-loader.md).

Con MinGW-w64 y una sesión Wayland o X11 activa, `make check-desktop`
prueba la ventana desde el anfitrión Linux y desde un PE. En CI se usa
`sh tests/pe/run_wayland.sh make -j2 check-desktop` con Weston.

Usa el auditor para medir la cobertura de un archivo externo.

```sh
make
build/pe-import-audit --unresolved /ruta/a/mines.exe
```

Squadron121 DX12 tiene 324 de 324 importaciones directas con un equivalente
registrado cuando están instalados los backends requeridos. El ejecutable
llega a la selección de adaptador. La prueba en Codex se detuvo porque el
juego requiere Direct3D 12 Feature Level 12_0 y descarta el único adaptador
de ese entorno, Mesa lavapipe. [Cobertura y prueba de arranque](docs/squadron-compatibility.md).

El auditor lee una copia del archivo de hasta 128 MiB. Informa sobre
las importaciones directas y devuelve 0 si todas tienen un equivalente,
1 si quedan llamadas pendientes y 2 si el formato o la lectura fallan.
El indicador opcional añade la biblioteca y el símbolo de cada importación
pendiente.

La prueba Linux y el comando de diagnóstico se compilan por separado del PE.
Puedes probar el destino del paquete sin privilegios con `make install
DESTDIR="$PWD/build-stage" PREFIX=/usr`.

## Navegación

| Ruta | Contenido |
| --- | --- |
| [include/ntnative](include/ntnative) y [src/runtime](src/runtime) | ABI interno y código de compatibilidad |
| [tests](tests) | Contratos, PE controlados y medición en Linux |
| [Arquitectura](docs/architecture.md) | Límites entre el kernel, el runtime y el escritorio |
| [Reutilización y licencias](docs/reuse.md) | Inventario, fuentes y dependencias |
| [Decisiones](docs/decisions.md) | Fundamento, alcance y condiciones de revisión |
| [Validación](docs/validation.md) | Pruebas realizadas y plan comparativo |
| [Cargador aislado](docs/isolated-loader.md) | Mapeo PE, prueba sintética, ejecución y límites |
| [Compatibilidad de Mines](docs/mines-compatibility.md) | Importaciones, errores y límites verificados en Linux |
| [Backlog](docs/backlog.md) | Tareas ordenadas y definición del siguiente PR |
| [Fedora](packaging/fedora/README.md) | Compilación RPM y validación en VM |
| [AGENTS.md](AGENTS.md) | Instrucciones reproducibles para Codex |

El código y la documentación originales están bajo la [licencia MIT](LICENSE).
Las dependencias de la plataforma conservan sus licencias. La comparación con
Wine, Proton y ReactOS se documenta por separado del código que se ejecuta.
