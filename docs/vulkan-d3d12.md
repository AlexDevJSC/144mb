# Puente D3D12 y diagnóstico Vulkan — etapa 0

## Punto de partida

En `feature/squadron-native` (PR #47), el ejecutable
`Squadron121_DX12.exe` 2026.10.19-rc4 registra 288 de 324
importaciones directas. La carga se detiene en
`sl.interposer.dll!D3D12CreateDevice`. El cargador ya procesa la
plantilla TLS estática; el fallo no procede del TLS de la revisión
anterior.

## Diagnóstico nativo entregado

`nt-vulkan-probe` carga **solo** `libvulkan.so.1` del Linux anfitrión
mediante `dlopen`, obtiene las funciones de Vulkan con
`vkGetInstanceProcAddr` y:

1. Comprueba la versión de la **instancia del cargador**, diferenciándola
   de las capacidades individuales de cada GPU.
2. Crea una instancia de consulta y enumera los dispositivos físicos.
3. Consulta si cada dispositivo anuncia `VK_KHR_swapchain`.
4. Comunica los errores reales de la API y libera la instancia/biblioteca.

No requiere bibliotecas de Windows, no ejecuta código PE y no altera
los registros de importaciones del runtime. El subconjunto de ABI del
cargador está declarado en código propio; la interfaz usa la ABI nativa
de Linux, no `NT_MS_ABI`.

En Linux x86-64:

```sh
make -j2 all check
build/nt-vulkan-probe
```

Resultado del entorno local de desarrollo: compilación GCC 14.2,
`-std=c17 -Wall -Wextra -Wpedantic -Werror`, prueba PASS. El cargador
informó versión 1.4.309 y `VkResult=-9` al consultar el dispositivo
(sin controlador/dispositivo Vulkan utilizable en dicho entorno).
Esto **no** es una medición de la GPU AMD del usuario.

Las pruebas de CI añaden `tests/vulkan_mock_loader.c`, una
biblioteca Vulkan simulada que se carga solo durante la suite Linux y
Fedora. Verifica el recorrido completo del diagnóstico para cuatro
situaciones deterministas:

| Simulación | Estado esperado |
| --- | --- |
| API 1.3 y dispositivo con `VK_KHR_swapchain` | `NT_VULKAN_PRELIMINARY_READY` |
| API 1.2 | `NT_VULKAN_API_BELOW_1_3` |
| Dispositivo sin `VK_KHR_swapchain` | `NT_VULKAN_NO_SWAPCHAIN` |
| Ningún dispositivo físico | `NT_VULKAN_NO_PHYSICAL_DEVICE` |

El mock **no es un controlador gráfico** y no constituye ejecución
D3D12. Se usa únicamente para verificar los caminos del diagnóstico,
incluidos los sistemas de CI sin GPU.

## Por qué NO se resuelve todavía D3D12CreateDevice

`D3D12CreateDevice` exige crear y devolver una interfaz
`ID3D12Device` válida con su ABI COM y los servicios que esperan la
aplicación y Streamline. Un `VkInstance` o `VkDevice` no puede
devolverse como si fuera un puntero COM. Registrar un stub que
devuelva `E_NOTIMPL` o una estructura ficticia aumentaría
artificialmente el auditor y no permitiría ejecutar el juego.

Esta prueba solo mide la disponibilidad de un punto de partida Vulkan.
Ni Vulkan 1.3 del cargador ni `VK_KHR_swapchain` garantizan que una GPU
pueda crear un dispositivo D3D12 o satisfacer los requisitos de
VKD3D-Proton. No se han medido extensiones adicionales, colas gráficas,
feature levels, soporte de shaders, DXGI, intercambio de imágenes ni
presentación Wayland.

### Siguientes entregas con criterio verificable

- Seleccionar un backend D3D12 real (adaptación explícita de
  VKD3D-Proton/libvkd3d o implementación propia) y fijar versión,
  interfaces, procedencia y obligaciones de licencia.
- Validar adaptador físico, características y extensiones de Vulkan,
  crear dispositivo y colas, y probar recursos y comandos sobre Linux.
- Exponer ABI `D3D12CreateDevice` con `REFIID`, parámetros,
  valores HRESULT, propiedad COM, `QueryInterface`, `AddRef` y
  `Release` correctos y un fixture PE de MinGW-w64 que lo use.
- Implementar DXGI (fábrica, adaptador, swapchain) y el contrato de
  `sl.interposer.dll` necesario para Squadron121; incorporar
  compiladores D3DCompile y DXC de forma verificable.
- Solo **después** registrar cada símbolo como nativo, medir la
  nueva cobertura sobre el mismo PE e intentar iniciar y jugar.

## Alcance y licencia

La prueba usa únicamente `libvulkan.so.1` del sistema Linux con
resolución dinámica y código fuente MIT del repositorio. La biblioteca
Vulkan del sistema y sus controladores mantienen sus licencias.
No incorpora código de Wine, Proton, VKD3D-Proton, DXVK ni NVIDIA.
Los candidatos y la política de reutilización se detallan en
`docs/reuse.md`.
