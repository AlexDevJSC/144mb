# Capa Kernel/User en Fedora

El lanzador ejecuta PE AMD64 dentro del entorno aislado existente. El
runtime de usuario resuelve los módulos del invitado y conecta sus
contratos con los servicios del kernel y del escritorio Linux.
La instalación de Fedora incorpora esta capa con el mismo RPM y el mismo
instalador de usuario del runtime.

```mermaid
flowchart TD
    PE["Programa PE AMD64"] --> Loader["Resolución de módulos"]
    Loader --> NT["NTDLL, KERNEL32 y KERNELBASE"]
    Loader --> USER["USER32 y win32u"]
    NT --> Kernel["Relojes, hilos y descriptores Linux"]
    USER --> Desktop["SDL y escritorio Fedora"]
```

| Contrato compartido | Implementación Linux |
| --- | --- |
| Importación PE por nombre u ordinal y `LoadLibrary`/`GetProcAddress` | Registro de módulos incorporados. Los nombres de API sets reconocidos se resuelven al mismo módulo que sus nombres canónicos. `KERNELBASE` reutiliza los equivalentes Win32 del sistema. |
| `NtQuerySystemTime` y `GetSystemTimeAsFileTime` | `CLOCK_REALTIME`, época de 1601 y unidades de 100 ns. |
| `NtDelayExecution`, `Sleep` y esperas de objetos | Plazos relativos monotónicos, plazos absolutos de reloj de pared y esperas de glibc. |
| Eventos y cierre | NTDLL y Win32 comparten la tabla de objetos. El cierre de archivos usa los mismos descriptores confinados del runtime. |
| `GetLastError`, `SetLastError` y las variantes `Rtl*LastWin32Error` | El DWORD `LastErrorValue` vive en `GS:0x68` del TEB de cada hilo invitado. |
| Consultas USER32 y win32u | `NtUserGetCursorPos`, `NtUserGetForegroundWindow`, `NtUserGetAsyncKeyState` y `NtUserGetKeyState` usan el escritorio SDL del runtime. |

## ABI y alcance

- Los puntos de entrada usan el ABI Microsoft x64. `NTSTATUS`, `ULONG` y
  los estados de evento tienen 32 bits. `BOOLEAN` ocupa 8 bits, los handles
  ocupan 64 bits y `OBJECT_ATTRIBUTES` ocupa 48 bytes. Las salidas deben ser
  escribibles. Las funciones `Nt*` devuelven `NTSTATUS` y mantienen
  `LastErrorValue`. Los adaptadores Win32 traducen sus propios errores.
- Los eventos son anónimos y pertenecen al proceso. Se aplican los derechos
  de espera y modificación del handle. `NtDuplicateObject` y `DuplicateHandle`
  crean handles distintos para el mismo estado de evento, hilo o proceso;
  cerrar el original no invalida el duplicado. Se admiten derechos reducidos
  para eventos y se comprueba que no superen los del handle original.
  `GetHandleInformation` expone el indicador de herencia del duplicado.
  Los handles de archivo se duplican mediante `fcntl(F_DUPFD_CLOEXEC)` y
  comparten posición de lectura/escritura. Para archivos únicamente se admite
  `DUPLICATE_SAME_ACCESS`, sin ampliación ni reducción arbitraria de derechos.
  `DUPLICATE_CLOSE_SOURCE` cierra el origen tras duplicar con éxito.
  La duplicación se limita a handles dentro del proceso invitado actual:
  NO hay broker de objetos entre procesos, y el indicador de herencia no
  implementa todavía herencia real hacia procesos hijos.
  Se pueden esperar hasta 64 objetos
  de la misma tabla, incluidos eventos, hilos y procesos hijos. La espera
  múltiple solo consume autoeventos si se satisface el conjunto solicitado.
  El cierre concurrente despierta al hilo y devuelve handle inválido.
  Los eventos con nombre, los APC y `/dev/ntsync` quedan fuera de este perfil.
- Los API sets se seleccionan por nombre completo y versión registrada.
  Se acepta la ruta virtual `C:\Windows\System32\` para los módulos
  incorporados. El mismo registro sirve para el auditor, la IAT y las
  consultas dinámicas. Una DLL o función desconocida produce el error
  correspondiente.
- Las consultas de escritorio usan la ventana, el foco y el teclado del
  invitado que conoce SDL. Las llamadas directas a números de syscall NT,
  los controladores Windows y la carga general de DLL PE quedan pendientes.

## TLS dinámico del TEB AMD64 (implementación incremental)

El TEB invitado reserva ahora espacio suficiente para los 64 `TlsSlots`
nativos de Windows x64 en `GS:0x1480` y el puntero a
`TlsExpansionSlots` en `GS:0x1780`. El runtime expone `TlsAlloc`,
`TlsFree`, `TlsGetValue` y `TlsSetValue` tanto por KERNEL32 como
por su alias KERNELBASE. Puede asignar los **1088 índices** (64 directos
y 1024 adicionales), reservando la memoria de expansión individualmente
por hilo solo cuando se utiliza. Los valores son independientes de la
plantilla TLS estática del PE accesible desde `GS:0x58`.
Las entradas y salidas anidadas conservan el hilo invitado actual y
restauran el estado previo de GS.

La asignación de índices es compartida por el proceso y está
sincronizada. Una generación asociada a cada índice impide que liberar y
reasignar un slot revele valores antiguos de otros hilos. `TlsGetValue`
devuelve `NULL` y limpia `LastError` cuando el índice válido está vacío.
El código PE generado para la prueba lee directamente `GS:0x1480`
y `GS:0x1780`, crea hilos adicionales, verifica el aislamiento y
comprueba la reutilización y el agotamiento de los 1088 índices.

**No es un TEB completo:** faltan el gestor de TLS estático y
dinámico de DLL adicionales, la política completa del cargador Ldr,
campos adicionales de TEB/PEB y servicios NT asociados. Los 1088 slots
Win32 cubren `TlsAlloc` y sus operaciones afines; no completan el ABI
del sistema de hilos de Windows ni sus interacciones con el cargador.

Ejecutar `make check-pe-teb-tls` desde Linux comprueba la implementación
Win32 desde un PE32+ AMD64 compilado con MinGW. La CI lo incluye en Ubuntu,
UBSan y Fedora.

## Verificación

`make -j2 check check-pe check-pe-import check-pe-kernel-user` ejecuta dos
PE MinGW-w64 de control sobre Linux. El primero combina importaciones
NTDLL, KERNEL32, KERNELBASE y un API set de sincronización. Comprueba
relojes, espera múltiple, derechos, hilos y escrituras directas al TEB.
El primer PE también duplica eventos y hilos con handles independientes,
comprueba límites de permisos y cierre del original, y duplica un descriptor de
archivo para verificar que puede seguir usándose después del cierre del
original. No se prueban aquí objetos con nombre ni handles compartidos
entre procesos, que siguen fuera del alcance.

El segundo combina USER32, win32u y un API set de escritorio, y prueba
también el cargador dinámico y los errores de módulo o símbolo ausente.
Se ejecutaron en Linux 6.18.57 AMD64 con GCC 14.2 y MinGW-w64 GCC 14,
también con `-fsanitize=undefined -fno-sanitize-recover=all`.

Las referencias de ABI son las cabeceras `winternl.h`, `ntdef.h` y
`ntstatus.h` de MinGW-w64 y los contratos públicos de Windows. El mapa de
módulos y las firmas win32u se contrastaron con Wine 11.0.
[NtCreateEvent](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntcreateevent) ·
[WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects) ·
[Windows API sets](https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets) ·
[Wine 11.0 API sets](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/apisetschema/apisetschema.spec) ·
[Wine 11.0 win32u](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/win32u/win32u.spec).
