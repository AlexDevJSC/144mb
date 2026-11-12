# Núcleo NT/Win32: estado verificable y criterios de salida

Este informe documenta el alcance **implementado** del prototipo AMD64 de
`main` y la rama incremental `feature/nt-core-native-process-memory`.
No es una declaración de compatibilidad universal con Windows. Una API
registrada no se considera compatible sin pruebas de argumentos, efectos,
errores, vida útil y concurrencia.

| Componente crítico | Implementado en esta etapa | Fuera de alcance o aún pendiente |
| --- | --- | --- |
| 1. Cargador PE y DLL | PE32+ AMD64, relocaciones DIR64, exportaciones de DLL por nombre/ordinal y resolución de *forwarders* hacia un callback; imports directos de módulos internos | Carga automática de DLL PE externas y sus dependencias, DllMain general, TLS de DLL, descarga de módulos, imports retardados efectivos, circularidad y DLL arbitrarias |
| 2. PEB/TEB | PEB/TEB mínimos por hilo; 1088 índices Win64 TLS; `RtlGetCurrentPeb`; `NtQueryInformationProcess/Thread` para las clases básicas del proceso/hilo actual | Estructuras completas, PEB_LDR_DATA, offsets/estado avanzado, campos de versiones, otros tipos de consultas y objetos de otros procesos |
| 3. Objetos NT | Eventos, esperas, alias de handles y eventos con nombre dentro del proceso; `NtOpenEvent`, `NtQueryEvent`, `OpenEventW`; derechos de consulta/modificación | Namespaces entre procesos, objetos globales de sesión, nombres Unicode arbitrarios, mutexes/semaforos NT completos, broker/IPC y acceso de seguridad NT avanzado |
| 4. Procesos e hilos | Procesos hijo PE acotados, hilos pthread, TLS, FLS, espera, duplicación local; `CREATE_SUSPENDED`, `ResumeThread`, `NtResumeThread` al iniciar un hilo | Suspensión asíncrona de hilos en ejecución, contexto externo, APC, herencia general de handles, tokens, procesos arbitrarios y atributos de seguridad |
| 5. Memoria virtual | `VirtualAlloc/Free/Protect/Query` en reservas propias y puente `NtAllocate/Free/Protect/QueryVirtualMemory` para proceso actual | Vistas de sección compartidas, acceso a memoria de otros procesos, PAGE_GUARD, MEM_RESET, regiones no gestionadas y semántica NT completa de memoria |
| 6. SEH | `.pdata/.xdata` AMD64 para las imágenes soportadas; excepciones software, filtros/desenrollado del perfil, registro y eliminación de VEH ordenados para `RaiseException` | Traducción general de SIGSEGV/SIGILL/SIGFPE, encadenamiento completo de unwind, vectored continue handlers, excepciones de DLL externas, SEH universal |
| 7. API NTDLL/Win32 | Registro compartido API sets/KERNEL32/KERNELBASE/NTDLL y API nuevas de proceso, objetos, memoria, suspensión y VEH | Cobertura funcional completa de bibliotecas Windows, APIs dinámicas no probadas y sus combinaciones, MSI, .NET, drivers Windows |

## Pruebas ejecutables

Las nuevas pruebas generadas con MinGW-w64 se ejecutan con nuestro cargador
PE32+ AMD64 sobre Linux. Se incluyen en la CI Ubuntu, UBSan y Fedora 45:

```sh
make check-pe-image check-pe-native-core check-pe-named-events \
     check-pe-suspended-thread check-pe-veh check-pe-teb-tls check-pe-memory
```

- `check-pe-image`: distingue DLL/EXE, exportaciones y *forwarders*
  por nombre y ordinal; rechaza formatos inválidos.
- `check-pe-native-core`: compara PEB/TEB consultado con GS, salidas NT,
  estado de memoria y conservación de `LastError`.
- `check-pe-named-events`: apertura por nombre, alias, permisos, consultas
  y destrucción del objeto tras cerrar la última referencia.
- `check-pe-suspended-thread`: un hilo creado suspendido no ejecuta
  código invitado hasta `ResumeThread`; comprobación de códigos y errores.
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
