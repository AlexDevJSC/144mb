# Direct3D 12 sobre Vulkan

El diagnóstico `nt-vulkan-probe` carga el Vulkan de Linux, comprueba la versión
del cargador y enumera los dispositivos físicos y su soporte de
`VK_KHR_swapchain`. La suite prueba sus rutas de error con un cargador
simulado. La prueba de Direct3D 12 usa el ICD Vulkan real.

Si pkg-config detecta `libvkd3d`, `libvkd3d-utils` desde 1.2 y `vulkan`, el
runtime publica la creación de dispositivos D3D12, la serialización de root
signatures y la creación de fábricas DXGI. También atiende los cinco puntos de
entrada D3D12 y DXGI que Squadron121 importa desde `sl.interposer.dll`. La
fábrica enumera dispositivos Vulkan, devuelve adaptadores con vtables COM
Microsoft x64 y proporciona un LUID al dispositivo D3D12 del mismo adaptador.

`make check-pe-graphics` compila un PE AMD64 con MinGW-w64. El PE consulta el
LUID, crea una root signature, ejecuta una copia de búfer con una cola D3D12,
espera la fence mediante un evento invitado y comprueba los datos copiados.
La prueba pasó en Linux x86-64 con GCC 14.2, vkd3d 1.2 y Mesa lavapipe.
Para elegir el ICD de prueba, usa `VK_DRIVER_FILES` con la ruta de su JSON.

La fábrica cubre enumeración y consulta de adaptadores. Las salidas DXGI y
la presentación mediante swapchain siguen pendientes. La fase de creación
puede informar que un feature level no está disponible en el adaptador.

El runtime enlaza las bibliotecas ELF de [vkd3d](https://gitlab.winehq.org/wine/vkd3d)
y del [cargador Vulkan](https://github.com/KhronosGroup/Vulkan-Loader).
Las bibliotecas conservan sus licencias LGPL-2.1-or-later y Apache-2.0,
respectivamente. Mesa conserva su licencia de distribución. El puente
del runtime es código propio MIT.

## Compilación de shaders

`D3DCompile` usa la exportación de libvkd3d-utils cuando está disponible.
`DxcCreateInstance` abre `libdxcompiler.so` y comprueba que ofrece
`IDxcCompiler3` e `IDxcUtils`. Las interfaces invitadas convierten argumentos
y resultados UTF-16 y conservan los HRESULT y la propiedad COM. El include
predeterminado lee archivos del disco invitado mediante la apertura confinada.
Las extensiones de reflexión y PDB de DXC siguen pendientes.

`make check-pe-shaders` ejecuta desde un PE ambos compiladores y verifica
compilación DXBC (SM5), DXIL (SM6), lectura de include, diagnóstico de código
inválido y un nombre de salida Unicode. La prueba local usó vkd3d 1.19 y el
[DXC ELF 1.8.2505.1 de Microsoft](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1).
DXC conserva las licencias del archivo upstream y el código del runtime
mantiene MIT.
