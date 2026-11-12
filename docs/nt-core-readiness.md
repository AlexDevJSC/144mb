# Núcleo NT/Win32: estado verificable y criterios de salida

Este informe documenta el alcance **implementado** del prototipo AMD64 de
`main` y la rama incremental `feature/pe-local-dll-loader` (continuación de PR #63).
No es una declaración de compatibilidad universal con Windows. Una API
registrada no se considera compatible sin pruebas de argumentos, efectos,
errores, vida útil y concurrencia.

| Componente crítico | Implementado en esta etapa | Fuera de alcance o aún pendiente |
| --- | --- | --- |
| 1. Cargador PE y DLL | PE32+ AMD64, IAT, forwarders y grafo de 64 DLL externas sin TLS desde directorio aislado; importaciones transitivas, DllMain por proceso/hilo y LoadLibrary/GetProcAddress/FreeLibrary en el perfil probado | TLS estático de DLL, SxS, rutas múltiples, dependencias cíclicas, rollback avanzado, descarga concurrente y bibliotecas con contratos Win32 no implementados |
| 2. PEB/TEB | PEB/TEB parciales, 1088 TLS, PEB->Ldr con tres listas y entrada del EXE y DLL PE adicionales adjuntadas (base, EntryPoint, tamaño y ruta UTF-16), con desvinculación al descargarse | Campos PEB/TEB y LDR_DATA completos, módulo de loader-lock universal, SxS, objetos de otros procesos y semántica de todas las consultas |
| 3. Objetos NT | Eventos y **semáforos** con nombre local, alias de handles y esperas múltiples, contadores y límites de semáforos, derechos y validación de colisiones entre tipos | Namespaces entre procesos, objetos globales de sesión, nombres Unicode arbitrarios, mutexes/semáforos compartidos entre procesos, broker/IPC y seguridad NT avanzada |
| 4. Procesos e hilos | Procesos hijo PE acotados, TLS/FLS, handles de hilos, creación suspendida, APC de usuario y esperas alertables (PR #64), más callbacks DLL_THREAD_ATTACH/DETACH para hilos invitados | Suspensión asíncrona, contextos externos, APC de kernel/variantes Ex, herencia general, tokens y procesos/hilos arbitrarios |
| 5. Memoria virtual | `VirtualAlloc/Free/Protect/Query` en reservas propias y puente `NtAllocate/Free/Protect/QueryVirtualMemory` para proceso actual | Vistas de sección compartidas, acceso a memoria de otros procesos, PAGE_GUARD, MEM_RESET, regiones no gestionadas y semántica NT completa de memoria |
| 6. SEH | `.pdata/.xdata` AMD64 para las imágenes soportadas; excepciones software, filtros/desenrollado del perfil, registro y eliminación de VEH ordenados para `RaiseException` | Traducción general de SIGSEGV/SIGILL/SIGFPE, encadenamiento completo de unwind, vectored continue handlers, excepciones de DLL externas, SEH universal |
| 7. API NTDLL/Win32 | Registro compartido API sets/KERNEL32/KERNELBASE/NTDLL y API nuevas de proceso, objetos, memoria, suspensión, VEH y E/S síncrona: `NtReadFile`, `NtWriteFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtFlushBuffersFile` | Cobertura funcional completa de bibliotecas Windows, APIs dinámicas no probadas y sus combinaciones, MSI, .NET, drivers Windows |

## Pruebas ejecutables

Las nuevas pruebas generadas con MinGW-w64 se ejecutan con nuestro cargador
PE32+ AMD64 sobre Linux. Se incluyen en la CI Ubuntu, UBSan y Fedora 45:

```sh
make check-pe-image check-pe-native-core check-pe-named-events \
     check-pe-suspended-thread check-pe-veh check-pe-teb-tls check-pe-memory \
     check-pe-nt-io check-pe-core-objects check-pe-dll-graph
```

- `check-pe-image`: distingue DLL/EXE, exportaciones y *forwarders*
  por nombre y ordinal; rechaza formatos inválidos.
- `check-pe-native-core`: compara PEB/TEB consultado con GS, salidas NT,
  estado de memoria y conservación de `LastError`.
- `check-pe-named-events`: apertura por nombre, alias, permisos, consultas
  y destrucción del objeto tras cerrar la última referencia.
- `check-pe-suspended-thread`: un hilo creado suspendido no ejecuta
  código invitado hasta `ResumeThread`; comprobación de códigos y errores.
- `check-pe-core-objects`: semáforos NT/Win32 con contadores, nombres,
  derechos, espera múltiple y colisiones con eventos; apertura de hilos
  locales por ID y consulta del TEB/estado desde un handle separado.
- `check-pe-native-core` y el test nativo de GS validan las listas
  del módulo principal en PEB_LDR_DATA.
- `check-pe-image` valida el disparo explícito de DllMain sin TLS y
  rechaza puntos de entrada fuera de secciones ejecutables.
- `check-pe-dll-graph`: prueba una DLL AMD64 real y dos EXE,
  con IAT estática, LoadLibrary/GetProcAddress/FreeLibrary, PEB,
  callbacks de hilo y resolución de referencias desde el mismo sandbox.
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
