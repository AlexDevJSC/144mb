# Núcleo NT/Win32: estado verificable y criterios de salida

Este informe documenta el alcance **implementado** del prototipo AMD64 de
`main` y su evolución incremental. La prioridad de los siguientes cambios está
fijada en [orden de desarrollo del núcleo](nt-core-development-order.md).
No es una declaración de compatibilidad universal con Windows. Una API
registrada no se considera compatible sin pruebas de argumentos, efectos,
errores, vida útil y concurrencia.

| Componente crítico | Implementado en esta etapa | Fuera de alcance o aún pendiente |
| --- | --- | --- |
| 1. Cargador PE y DLL | PE32+ AMD64, relocaciones, importaciones, forwarders y grafo de DLL limitado a archivos regulares del directorio aislado; `LoadLibrary/GetProcAddress/FreeLibrary`, dependencias estáticas, `DllMain` de proceso/hilo, TLS de DLL importadas antes del primer hilo (hasta 63 slots) y contador de referencias local | carga de nuevas DLL con TLS después de iniciar hilos, ciclos complejos, búsqueda general de rutas Windows, contratos completos de Ldr y concurrencia/descarga arbitraria |
| 2. PEB/TEB | PEB/TEB parciales con 1088 slots TLS Win32; tres listas de `PEB_LDR_DATA` que incluyen EXE y DLL PE locales registradas mientras están adjuntas | registro dinámico de TLS en hilos existentes, estructura completa de Ldr/PEB/TEB y objetos de procesos externos |
| 3. Objetos NT | Eventos y **semáforos** con nombre local, alias de handles y esperas múltiples, contadores y límites de semáforos, derechos y validación de colisiones entre tipos | Namespaces entre procesos, objetos globales de sesión, nombres Unicode arbitrarios, mutexes/semáforos compartidos entre procesos, broker/IPC y seguridad NT avanzada |
| 4. Procesos e hilos | Procesos hijo PE acotados con `CREATE_SUSPENDED`, terminación por pidfd y código DWORD; `OpenProcess`/`NtOpenProcess`, `OpenThread`/`NtOpenThread`, consultas de ID y permisos, flags HANDLE `INHERIT`/`PROTECT_FROM_CLOSE`, duplicación segura en el proceso propio y recuento de handles, `NtCreateThreadEx` y `CreateRemoteThread`/`CreateRemoteThreadEx` acotados al proceso propio con derechos, terminación segura de hilos antes de la entrada, afinidad real `SetThreadAffinityMask`/`NtSetInformationThread` para grupos CPU x64 representables, prioridades de hilo Win32↔nice Linux acotadas, tiempos `GetThreadTimes`/`GetProcessTimes` con CPU real, consulta nativa `THREAD_BASIC_INFORMATION` de afinidad/prioridad desde Linux en lugar de constantes, nombres UTF-16 completos, TLS/FLS, handles y **APC de usuario acotadas** entregadas mediante esperas alertables (`NtQueueApcThread`, `QueueUserAPC`, `NtTestAlert`, `SleepEx`) | Suspensión asíncrona y terminación de hilos ejecutándose, `GetThreadContext`/`SetThreadContext`, APC de kernel/variantes Ex, herencia general, prioridades, afinidad multicgrupo de CPU, tokens, procesos arbitrarios y acceso a hilos ajenos al runtime; ver [detalle fase 4](nt-stage4-process-threads.md) |
| 5. Memoria virtual | `VirtualAlloc/Free/Protect/Query` en reservas propias y puente `NtAllocate/Free/Protect/QueryVirtualMemory` para proceso actual | Vistas de sección compartidas, acceso a memoria de otros procesos, PAGE_GUARD, MEM_RESET, regiones no gestionadas y semántica NT completa de memoria |
| 6. SEH | `.pdata/.xdata` AMD64 para las imágenes soportadas; excepciones software, filtros/desenrollado del perfil, registro y eliminación de VEH ordenados para `RaiseException` | Traducción general de SIGSEGV/SIGILL/SIGFPE, encadenamiento completo de unwind, vectored continue handlers, excepciones de DLL externas, SEH universal |
| 7. API NTDLL/Win32 | Registro compartido API sets/KERNEL32/KERNELBASE/NTDLL y API nuevas de proceso, objetos, memoria, suspensión, VEH y E/S síncrona: `NtReadFile`, `NtWriteFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtFlushBuffersFile` | Cobertura funcional completa de bibliotecas Windows, APIs dinámicas no probadas y sus combinaciones, MSI, .NET, drivers Windows |

## Pruebas ejecutables

Las nuevas pruebas generadas con MinGW-w64 se ejecutan con nuestro cargador
PE32+ AMD64 sobre Linux. Se incluyen en la CI Ubuntu, UBSan y Fedora 45:

```sh
make check-pe-image check-pe-native-core check-pe-named-events \
     check-pe-suspended-thread check-pe-veh check-pe-teb-tls check-pe-memory \
     check-pe-nt-io check-pe-core-objects check-pe-apc check-pe-dll-graph
```

- `check-pe-image`: distingue DLL/EXE, exportaciones y *forwarders*
  por nombre y ordinal; rechaza formatos inválidos.
- `check-pe-native-core`: compara PEB/TEB consultado con GS, salidas NT,
  estado de memoria y conservación de `LastError`.
- `check-pe-named-events`: apertura por nombre, alias, permisos, consultas
  y destrucción del objeto tras cerrar la última referencia.
- `check-pe-suspended-thread`: un hilo creado suspendido no ejecuta
  código invitado hasta `ResumeThread`; prueba de contadores `SuspendThread`/`NtSuspendThread` hasta el límite 127, derechos de handles, rechazo explícito de suspensión de hilo en ejecución, respuestas de NTSTATUS/LastError y códigos de salida.
- `check-pe-dll-graph`: compila una DLL PE local sin CRT, otra DLL con TLS estático real
  y tres ejecutables Win64; valida imports estáticos, carga dinámica, PEB,
  DllMain, plantilla TLS distinta por hilo, callbacks, descarga y recarga,
  rutas bloqueadas y nombres con distinto uso de mayúsculas.
- `check-pe-apc`: un hilo suspendido recibe APC Win32/NT en su
  propio contexto durante esperas alertables, con orden de entrega,
  derechos THREAD_SET_CONTEXT, respuestas de tiempo y rechazo de
  APC sobre hilos terminados.
- `check-pe-core-objects`: semáforos NT/Win32 con contadores, nombres,
  derechos, espera múltiple y colisiones con eventos; apertura de hilos
  locales por ID y consulta del TEB/estado desde un handle separado.
- `check-pe-native-core` y el test nativo de GS validan las listas
  del módulo principal en PEB_LDR_DATA.
- `check-pe-image` valida el disparo explícito de DllMain y callbacks TLS;
  rechaza puntos de entrada fuera de secciones ejecutables.
- `check-pe-nt-io`: el mismo HANDLE para funciones NTDLL y Win32,
  IO_STATUS_BLOCK, EOF, posicionamiento explícito, consulta y cambio de
  tamaño, flush, derechos insuficientes y conservación de LastError.
- `check-pe-veh`: orden de los manejadores vectorizados, continuación de
  la excepción software y eliminación sin invalidar otros manejadores.

## Condición para declarar cada componente «pendiente únicamente de Fedora»

Esa etiqueta **no está alcanzada** para ninguno de los siete componentes
generales. Antes hace falta ampliar la implementación a los contratos
omitidos de la tabla anterior, desarrollar pruebas de referencia frente a
Windows real en varias versiones, pruebas concurrentes, pruebas cruzadas
con programas independientes y una auditoría de seguridad del sandbox.
Una prueba verde de RPM/CI en Fedora confirma únicamente los contratos
efectivamente ejercitados, no la paridad global con Windows ni que un
ejecutable arbitrario funcione.

No se implementan drivers de kernel Windows, bypass de anticheat ni
syscalls NT por número directo. El kernel, controladores y derechos del
anfitrión siguen perteneciendo a Linux.
