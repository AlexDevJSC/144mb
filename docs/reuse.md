# Inventario y reutilización

Las decisiones se aplican por módulo. El árbol inicial utiliza glibc y
la ABI de Linux, con GCC, Make y GNU binutils para la compilación. Los
componentes externos de la tabla se investigaron como referencias o
dependencias futuras. El artefacto inicial no redistribuye sus binarios.

Los PE de prueba se compilan con el paquete GNU de MinGW-w64 AMD64 del
runner Ubuntu. Las bibliotecas de importación se derivan de las
definiciones de `ntdll.dll` y `kernel32.dll` de MinGW-w64, cuyo proyecto
usa ZPL-2.1 con excepciones marcadas por archivo. Los fixtures enlazan
sin CRT. El anfitrión Linux resuelve las importaciones con el runtime propio.
[Definición NT](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-crt/lib-common/ntdll.def.in) ·
[Definición Win32](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-crt/lib-common/kernel32.def.in) ·
[Licencia MinGW-w64](https://github.com/mingw-w64/mingw-w64/blob/master/COPYING).

Las ventanas del runtime usan la biblioteca SDL 2 de la distribución
mediante su ABI nativa. La dependencia usa licencia Zlib. El fixture se
probó localmente con SDL 2.32.4 y el backend `offscreen`. La CI usa
Weston como compositor Wayland de prueba.
[SDL 2.32.4](https://github.com/libsdl-org/SDL/tree/release-2.32.4) ·
[Licencia SDL](https://github.com/libsdl-org/SDL/blob/release-2.32.4/LICENSE.txt).

El rasterizador GDI consume Cairo 1.18.4 de Linux y obtiene tipografías
del sistema mediante el backend de fuentes de Cairo. Cairo tiene licencia
MPL-1.1 o LGPL-2.1-only y se enlaza como biblioteca de la distribución.
[Licencias de Cairo](https://www.cairographics.org/FAQ/#what-is-cairo-license) ·
[DejaVu Fonts](https://dejavu-fonts.github.io/).

libffi 3.4.8 invoca las funciones variádicas de glibc con el ABI nativo
después de validar y adaptar los argumentos Microsoft x64 del fixture.
Se usa el paquete del sistema con licencia MIT.
[libffi 3.4.8](https://github.com/libffi/libffi/tree/v3.4.8).

## Proyectos y dependencias

| Componente | Clasificación | Evidencia, dependencia y restricciones |
| --- | --- | --- |
| Linux, ELF y recursos | **Reutilizar directamente** | `fs/binfmt_elf.c` conserva la ruta ELF. La ABI de usuario proporciona procesos, memoria y descriptores. Kernel GPL-2.0-only con reglas por archivo. Las cabeceras UAPI con `Linux-syscall-note` se distinguen del código del kernel. [ELF v6.14](https://github.com/torvalds/linux/blob/v6.14/fs/binfmt_elf.c) · [Licencias Linux](https://docs.kernel.org/process/license-rules.html). |
| Linux `ntsync` | **Reutilizar directamente** si está disponible | La UAPI desde 6.14 proporciona eventos, semáforos, mutexes y esperas múltiples. La carga del módulo y los permisos dependen de la instalación. El controlador es GPL-2.0-only y el encabezado UAPI tiene `Linux-syscall-note`. Mantener una ruta alternativa en usuario. [UAPI 6.14](https://docs.kernel.org/6.14/userspace-api/ntsync.html) · [Driver](https://github.com/torvalds/linux/blob/v6.14/drivers/misc/ntsync.c) · [Cabecera](https://github.com/torvalds/linux/blob/v6.14/include/uapi/linux/ntsync.h) · [Paquete Fedora](https://packages.fedoraproject.org/pkgs/wine/ntsync-autoload/). |
| Wine | **Adaptar a nuestra arquitectura** | Wine 11.0 sirve de referencia y candidato a backend provisional externo. NTDLL abre y mapea imágenes, el cargador PE resuelve módulos y `wineserver` arbitra parte de los objetos y procesos. Wine 11 usa NTSYNC cuando está disponible. Su código general es LGPL-2.1-or-later con excepciones por archivo. Integrar en un límite explícito y revisar licencia, modificaciones y dependencias de cada build. [Release 11.0](https://www.winehq.org/news/2026011301) · [Cargador Unix](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.0/dlls/ntdll/unix/loader.c) · [Cargador PE](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/loader.c) · [Servidor](https://github.com/wine-mirror/wine/tree/wine-11.0/server) · [Licencia](https://github.com/wine-mirror/wine/blob/wine-11.0/LICENSE). |
| Valve Proton | **Reutilizar directamente** como referencia de juegos | `proton_11.0` prepara prefijos y empaqueta un Wine adaptado con DXVK, VKD3D-Proton y otros submódulos. El entorno objetivo de Proton 11 es Steam Linux Runtime 4. El código propio de Proton lleva BSD-3-Clause y cada dependencia conserva sus obligaciones. Evaluar parches concretos después de identificar un caso reproducible. [Launcher](https://github.com/ValveSoftware/Proton/blob/proton_11.0/proton) · [Submódulos](https://github.com/ValveSoftware/Proton/blob/proton_11.0/.gitmodules) · [Licencias](https://github.com/ValveSoftware/Proton/blob/proton_11.0/LICENSE) · [Steam Runtime](https://github.com/ValveSoftware/steam-runtime/blob/master/doc/reporting-steamlinuxruntime-bugs.md). |
| ReactOS NT y Win32 | **Investigar antes de decidir** el uso de módulos concretos | Referencia de semántica y casos límite junto con los contratos públicos. ReactOS recomienda RosBE para compilar y contiene componentes GPL 2.0, LGPL y permisivos. `ntoskrnl/ex/profile.c` muestra la separación entre sondeo de salidas y reloj. Registrar licencia por archivo antes de plantear cualquier adaptación. [README](https://github.com/reactos/reactos/blob/master/README.md) · [Fuente](https://github.com/reactos/reactos/blob/master/ntoskrnl/ex/profile.c) · [Política de propiedad intelectual](https://reactos.org/intellectual-property-guideline/). |
| Kernel, HAL y drivers ReactOS | **Descartar** como base | Están diseñados para el entorno de kernel y drivers NT. Los recursos de anfitrión siguen siendo responsabilidad de Linux y sus drivers. [Arquitectura y compilación ReactOS](https://github.com/reactos/reactos/blob/master/README.md). |
| DXVK | **Adaptar a nuestra arquitectura** | D3D9–11 y DXGI, además de D3D8 en versiones recientes, sobre Vulkan. El build usa Meson, MinGW-w64 y compilación de shaders. Licencia Zlib. Comprobar versiones y capacidades Vulkan de la GPU antes de integrar las DLL y su puente a Vulkan. [Código y build](https://github.com/doitsujin/dxvk) · [Licencia](https://github.com/doitsujin/dxvk/blob/master/LICENSE) · [Drivers](https://github.com/doitsujin/dxvk/wiki/Driver-support). |
| VKD3D-Proton | **Adaptar a nuestra arquitectura** | DLL PE de D3D12 y D3D12Core. Usa DXGI de DXVK. El build usa Meson, Ninja, MinGW-w64 y herramientas de shaders. Sus requisitos incluyen Vulkan 1.3 y extensiones concretas según la revisión. LGPL-2.1-or-later como licencia general, con auditoría de submódulos. [Código y build](https://github.com/HansKristian-Work/vkd3d-proton) · [Licencia](https://github.com/HansKristian-Work/vkd3d-proton/blob/master/COPYING). |
| Mesa RADV, Vulkan y amdgpu | **Reutilizar directamente** | `mesa-vulkan-drivers` aporta RADV en usuario sobre amdgpu y DRM. `vulkan-loader` carga el ICD. Mesa usa principalmente MIT con excepciones por archivo, el cargador Vulkan es Apache-2.0 y el firmware AMD se distribuye por separado. [RADV](https://docs.mesa3d.org/drivers/radv.html) · [Licencia Mesa](https://docs.mesa3d.org/license.html) · [Vulkan Loader](https://packages.fedoraproject.org/pkgs/vulkan-loader/vulkan-loader/) · [Firmware](https://packages.fedoraproject.org/pkgs/linux-firmware/amd-gpu-firmware/). |
| Wayland y KWin | **Reutilizar directamente** | Usar el compositor de Fedora KDE y la biblioteca cliente Wayland MIT. El puente Win32 de ventanas, entrada y presentación es código de usuario y requiere pruebas de foco, escala, portapapeles y eventos. [Arquitectura Wayland](https://wayland.freedesktop.org/architecture.html) · [libwayland-client](https://packages.fedoraproject.org/pkgs/wayland/libwayland-client/). |

## Contratos iniciales

| Interfaz | Clasificación | Primera verificación |
| --- | --- | --- |
| `NtQueryPerformanceCounter` | **Implementar por nuestra cuenta** | Valores no decrecientes, frecuencia estable y error con salida obligatoria nula. |
| `NtQuerySystemTime` | **Implementar por nuestra cuenta** | Época UTC de 1601, unidades de 100 ns, desbordamiento y precisión. |
| `NtCreateEvent`, `NtSetEvent`, `NtResetEvent`, `NtWaitForSingleObject` y `NtWaitForMultipleObjects` | **Adaptar a nuestra arquitectura** | Comparar eventos automáticos y manuales, espera múltiple, timeout, cierre concurrente y backend alternativo. |
| `NtClose` y `NtDuplicateObject` | **Implementar por nuestra cuenta** | Handles inválidos, privilegios, herencia y vida de objetos compartidos. |
| `NtCreateFile`, `NtOpenFile`, `NtReadFile` y `CreateFileW` | **Investigar antes de decidir** | Probar nombres NT, compartición, reparse points, case folding y eliminación pendiente. Decidir qué reutilizar por semántica. |
| Carga PE32+, importaciones por nombre y exportaciones NTDLL | **Investigar antes de decidir** el alcance permanente | Implementar primero la costura acotada del laboratorio. Comparar contra Wine en un conjunto más amplio antes de ampliar el cargador. |
| Drivers de Windows, DRM y antitrampas que requieran bypass | **Descartar** del alcance | Registrar el requisito que bloquea cada caso y conservar el límite de seguridad. |

## Solapamiento y medición

Wine y ReactOS implementan interfaces comunes y Proton distribuye una
variante de Wine. Esto identifica funciones comparables. La documentación
de VKD3D-Proton identifica un caso de reutilización concreta, el DXGI
de DXVK. En Wine, NTSYNC permite que ciertas esperas y señales alcancen
el objeto respaldado por un descriptor a través de `ioctl`. La gestión
de objetos sigue teniendo responsabilidades propias.

Para demostrar que puede quitarse una operación, instrumenta el mismo
binario, build, prefijo y carga con ambos caminos. En Proton 11 se puede
variar `PROTON_NO_NTSYNC=1` y comprobar el backend por trazas en una
ejecución separada. Primero compara semántica, después IPC y syscalls
observadas, latencias y consumo. La matriz registra solapamientos
funcionales y no atribuye costes a ninguna capa sin datos.

Referencias [Diseño Wine NTSYNC](https://list.winehq.org/hyperkitty/list/wine-gitlab@list.winehq.org/thread/6JWY4TJCPUJF2KTJ4627YZY6UTIXIQ6S/) ·
[Opciones Proton 11](https://github.com/ValveSoftware/Proton/blob/proton_11.0/README.md) ·
[DXGI en VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton#using-vkd3d-proton).

## Política de incorporación

### Siguiente integración tras las partidas de Mines

La corrección de menús mantiene contratos Win32 compartidos y pruebas con
menús y ventanas sintéticos. El recuento de importaciones se acompaña de
resultados de comportamiento, cancelación, errores y ciclo de vida para
medir la ampliación a otras aplicaciones.

La integración de Wine se centrará primero en las pruebas y la semántica de
ventanas, mensajes y diálogos. La selección de módulos requiere definir el
gestor de handles, la cola de mensajes y el punto de conexión con Linux.
En Wine 11.0, `USER32` declara dependencias de `gdi32`, `sechost`, `advapi32`,
`kernelbase` y `win32u`, además de otras importaciones. `win32u` incorpora
una biblioteca Unix y depende de `ntdll`. Esas dependencias delimitan el
trabajo previo a cargar módulos Wine en este runtime. [Build de USER32](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/user32/Makefile.in) ·
[Build de win32u](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/win32u/Makefile.in).

Proton reúne Wine con componentes adicionales para juegos. Su árbol incluye
DXVK y VKD3D-Proton. La integración gráfica tiene sus propios criterios de
Vulkan y Direct3D en P2-02. La aceptación de cualquier módulo reutilizado
exige fijar la revisión, conservar sus avisos y licencia, declarar sus
dependencias y ejecutar los mismos fixtures PE en Linux. El runtime de esta
revisión utiliza implementaciones propias y bibliotecas del anfitrión.
[Código y componentes de Proton](https://github.com/ValveSoftware/Proton).

### Procedencia y distribución

MIT cubre los archivos originales del proyecto. Las dependencias del
sistema se consumen mediante su ABI o en paquetes separados. Antes de
publicar una integración que redistribuya código, registra revisión,
archivos modificados, dependencias transitivas, avisos y obligaciones
de entrega de fuentes y relinkado según la licencia y el modo de
distribución. Conserva las licencias de los archivos externos y evita
trasladar código GPL o LGPL a archivos declarados como MIT. Las
implementaciones nuevas deben derivarse de contratos públicos y
comportamiento observado, con su procedencia registrada.

Referencias [MIT SPDX](https://spdx.org/licenses/MIT.html) ·
[LGPL 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) ·
[Datos de licencias de Fedora](https://packages.fedoraproject.org/pkgs/fedora-license-data/fedora-license-data/).
