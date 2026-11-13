# Fase 4: procesos e hilos — contratos, garantías y trabajo pendiente

Prioridad actual de acuerdo con [el orden de desarrollo](nt-core-development-order.md):
**4 → 5 → 2 → 3 → 1 → 6 → 7**. Esta fase permanece activa.

## Contratos implementados y perfil probado

| Dominio | Entradas del invitado | Comportamiento |
| --- | --- | --- |
| Creación de hilos | `CreateThread`, `NtCreateThreadEx` | Hilos pthread del proceso actual, ABI x64, TEB/TLS de invitado, opción `CREATE_SUSPENDED`, handles con derechos; `NtCreateThreadEx` admite solo proceso actual, sin lista extendida de atributos ni flags no probadas |
| Abrir e identificar hilos | `OpenThread`, `NtOpenThread`, `GetThreadId`, `GetProcessIdOfThread`, `NtQueryInformationThread` (Basic) | IDs locales, alias de HANDLE independientes, `CLIENT_ID` y derechos de consulta; no se confunde un HANDLE con un PID/TID Linux |
| Descripción Unicode | `SetThreadDescription`, `GetThreadDescription` | Conserva el nombre UTF-16 completo en el objeto canónico y entrega búfer liberable con `LocalFree`. El nombre abreviado de pthread es solo diagnóstico. |
| Terminación de hilos | `ExitThread`, `NtTerminateThread`, `TerminateThread`, `GetExitCodeThread` | Salida normal o nativa del propio hilo; terminación solicitada sobre un hilo en su puerta de arranque, sin ejecutar la entrada ni callbacks de adjunción que no correspondan. Nunca se cancela arbitrariamente un pthread activo. |
| Suspensión | `SuspendThread`, `NtSuspendThread`, `ResumeThread`, `NtResumeThread` | Contadores acotados a 127 en la puerta inicial del hilo. El hilo ya ejecutándose no admite suspensión arbitraria. |
| Procesos hijo | `CreateProcessW`, `GetExitCodeProcess`, `GetProcessId`, `OpenProcess`, `NtOpenProcess` | Ejecuta PE x64 del volumen invitado bajo el mismo sandbox; handles y códigos de salida DWORD; nombres/argumentos Unicode y carpeta de trabajo, `CREATE_SUSPENDED` con puerta fd5 validada por el lanzador |
| Terminación de hijos | `TerminateProcess`, `NtTerminateProcess` | Solamente hijos gestionados por el runtime. Usa `pidfd_open` y `pidfd_send_signal`, y almacena el código DWORD pedido para las consultas del padre. Sin pidfd, falla explícitamente; jamás usa un PID arbitrario como sustituto. |
| Proceso actual | `OpenProcess` con el PID propio, `GetCurrentProcess` y consultas nativas | HANDLE real con vida útil estable y derechos separados de seudohandles. Los adaptadores de memoria NT requieren acceso apropiado (operar/consultar). |
| Afinidad real de CPU | `SetThreadAffinityMask`, `NtSetInformationThread` (clase `ThreadAffinityMask` 4), `GetProcessAffinityMask` | Mapeo directo al conjunto de CPU Linux del hilo o proceso actual, comprobación de derechos, estado y máscara solicitada; los resultados reflejan la afinidad aplicada por el kernel. Perfil AMD64 limitado a grupo 0 con CPUs Linux 0–63: si la afinidad original contiene CPUs superiores, devuelve `NOT_SUPPORTED` sin truncarla. No se simulan grupos Windows adicionales. |
| APC, FLS y esperas | `QueueUserAPC`, `NtQueueApcThread`, `NtTestAlert`, `SleepEx`, `WaitFor*Ex`, `Fls*`, `Tls*` | Subconjuntos ya presentes: colas acotadas, despacho en el hilo de destino durante esperas alertables, independencia por hilo y liberación de recursos al finalizar |

### Límites intencionados y no negociables

1. **Suspensión asíncrona de un hilo activo:** no implementada. Sin un mecanismo seguro de contexto, aparcar otro pthread desde una señal Linux mientras posee locks internos puede romper el runtime. No usar `SIGSTOP`, `pthread_cancel` ni manipular GS desde otro hilo como solución ficticia.
2. **Terminación remota de hilos activos:** no implementada. `TerminateThread` solo admite la puerta de arranque; `NtTerminateThread` admite además la terminación del propio hilo.
3. **Contextos AMD64 de terceros:** faltan `GetThreadContext`, `SetThreadContext`, contextos de hilos suspendidos en ejecución y sincronización completa con SEH.
4. **Procesos externos y tokens NT:** no se abren procesos ajenos al runtime. Falta seguridad NT, herencia general de HANDLE, sesiones, permisos por token, procesos de diferentes usuarios, impersonación y broker entre procesos.
5. **Creación avanzada:** faltan atributos extendidos de `NtCreateThreadEx`/`CreateProcessW`, flags generales, espacios remotos y compatibilidad x86/WOW64.
6. **Planificación:** la afinidad de hilos en un grupo de 64 CPU ahora usa el planificador Linux. Siguen faltando grupos de procesadores mayores de 64, prioridades Windows, afinidad de procesos remotos, quantum y equivalencia completa de planificación.
7. **APC y señalización completas:** variantes de APC de kernel, `NtQueueApcThreadEx`, E/S asíncrona con APC, `IoCompletionPort`, suspensión de hilos remotos y carreras de desconexión.
8. **Concurrencia y seguridad:** queda una batería de estrés, pruebas diferenciales Windows reales, auditoría del sandbox y casos de carreras de salida/descarga del loader. Un fixture sintético y una CI verde no equivalen a paridad total.

### Verificación de esta entrega

- `make check`: incluye `tests/thread_affinity_test.c` para verificar `sched_getaffinity` real, derechos, NTSTATUS/LastError, preservación de máscaras y 48 ciclos de vida de hilos con alias. El test informa `SKIP` del caso positivo si el host no expone ninguna CPU Linux por debajo de 64; el fallo explícito del grupo no representable sí se comprueba.
- `make check`: añade `tests/process_thread_test.c`, que crea hijos Linux verdaderos, verifica `pidfd`, apertura/reapertura de handles, derechos, códigos de terminación, la puerta de inicio y el desbloqueo contado.
- `make check-pe-core-objects`: PE AMD64 MinGW con `NtOpenThread`, IDs, derechos, descripción UTF-16 de longitud superior al nombre de pthread y `LocalFree`.
- `make check-pe-suspended-thread`: extiende el PE AMD64 con los errores de `SetThreadAffinityMask`, `NtSetInformationThread` y `GetProcessAffinityMask`, y verifica su caso positivo en anfitriones con cpuset representable. También prueba `NtCreateThreadEx`, `NtTerminateThread`, `TerminateThread`, creación suspendida, límites de contador, hilos activos rechazados y salida sin entrada en ejecución.
- `make check-pe-isolated`: PE de procesos hijo comprueba `CREATE_SUSPENDED`, apertura, permisos y terminación del proceso. Una prueba `SKIP` por limitaciones de Bubblewrap **no es** una validación de esta ruta.
- El estado final de los jobs Linux/UBSan/Fedora y los hallazgos de seguridad deben acompañar la PR de esta fase; no confundir compilación con validación de las rutas funcionales.

## Madurez del componente

- **Anterior: 35 %** (línea base tras PR #68).
- **Actual tras PR #71 y #72 fusionadas: 55 %** (estimación, **+20 puntos porcentuales**).
- **Propuesto en esta entrega (afinidad, PR pendiente): 60 %** (estimación, **+5 puntos porcentuales** respecto al 55 %). Sujeta a las pruebas Linux y Fedora del commit final. No se afirma cobertura de prioridades, CPU groups ni suspensión arbitraria.
- **Justificación:** ahora existen rutas conectadas de creación, apertura, identidad, consulta, suspensión de arranque, terminación y cierre tanto para hilos como para procesos hijo, y pruebas Win64/host que recorren varias de ellas.
- **Por qué no 70 % ni 100 %:** faltan contratos sustanciales de suspensión/terminación remota, captura y restauración de contextos AMD64, tokens y permisos NT, herencia de handles, servicios entre procesos, prioridades/afinidad y comparación de comportamiento con Windows real.

Este número **no mide aceleración de CPU**. No se ha realizado ni demostrado una mejora 2× en rendimiento; «duplicar porcentaje» y «duplicar rendimiento» son objetivos distintos.

La [CI del último commit de la rama de integración](https://github.com/AlexDevJSC/144mb/actions/runs/43341757354) terminó correctamente tanto en Linux como en Fedora RPM. Las pruebas omitidas por restricciones del entorno no constituyen validación. Permanecen necesarios tests diferenciales Windows y de estrés para justificar porcentajes mayores.
