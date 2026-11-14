# Fase 4: procesos e hilos — contratos, garantías y trabajo pendiente

Prioridad actual de acuerdo con [el orden de desarrollo](nt-core-development-order.md):
**4 → 5 → 2 → 3 → 1 → 6 → 7**. Esta fase permanece activa.

## Contratos implementados y perfil probado

| Dominio | Entradas del invitado | Comportamiento |
| --- | --- | --- |
| Creación remota acotada | `CreateRemoteThread`, `CreateRemoteThreadEx`, `NtCreateThreadEx` con HANDLE real | Admite la creación de hilos exclusivamente en el proceso actual, validando `PROCESS_CREATE_THREAD` y reutilizando la puerta `CREATE_SUSPENDED` y el TEB/TLS de los hilos propios. Los procesos hijo o ajenos, los atributos extendidos y los espacios de direcciones externos no son compatibles. |
| Creación de hilos | `CreateThread`, `NtCreateThreadEx` | Hilos pthread del proceso actual, ABI x64, TEB/TLS de invitado, opción `CREATE_SUSPENDED`, handles con derechos; `NtCreateThreadEx` admite solo proceso actual, sin lista extendida de atributos ni flags no probadas |
| Metadatos y duplicación de handles | `GetHandleInformation`, `SetHandleInformation`, `DuplicateHandle`/`NtDuplicateObject`, `GetProcessHandleCount` | Flags por HANDLE (`INHERIT` y `PROTECT_FROM_CLOSE`) independientes entre alias, cierres protegidos rechazados, consulta de handles vivos y duplicación dentro del proceso actual a través de handles reales con `PROCESS_DUP_HANDLE`. Los flags de herencia **no suponen que la herencia a procesos hijo ya funcione**: falta una tabla serializable y broker/IPC entre procesos. |
| Abrir e identificar hilos | `OpenThread`, `NtOpenThread`, `GetThreadId`, `GetProcessIdOfThread`, `NtQueryInformationThread` (Basic) | IDs locales, alias de HANDLE independientes, `CLIENT_ID` y derechos de consulta; no se confunde un HANDLE con un PID/TID Linux |
| Descripción Unicode | `SetThreadDescription`, `GetThreadDescription` | Conserva el nombre UTF-16 completo en el objeto canónico y entrega búfer liberable con `LocalFree`. El nombre abreviado de pthread es solo diagnóstico. |
| Terminación de hilos | `ExitThread`, `NtTerminateThread`, `TerminateThread`, `GetExitCodeThread` | Salida normal o nativa del propio hilo; terminación solicitada sobre un hilo en su puerta de arranque, sin ejecutar la entrada ni callbacks de adjunción que no correspondan. Nunca se cancela arbitrariamente un pthread activo. |
| Suspensión | `SuspendThread`, `NtSuspendThread`, `ResumeThread`, `NtResumeThread` | Contadores acotados a 127 en la puerta inicial del hilo. El hilo ya ejecutándose no admite suspensión arbitraria. |
| Procesos hijo | `CreateProcessW`, `GetExitCodeProcess`, `GetProcessId`, `OpenProcess`, `NtOpenProcess` | Ejecuta PE x64 del volumen invitado bajo el mismo sandbox; handles y códigos de salida DWORD; nombres/argumentos Unicode y carpeta de trabajo, `CREATE_SUSPENDED` con puerta fd5 validada por el lanzador |
| Terminación de hijos | `TerminateProcess`, `NtTerminateProcess` | Solamente hijos gestionados por el runtime. Usa `pidfd_open` y `pidfd_send_signal`, y almacena el código DWORD pedido para las consultas del padre. Sin pidfd, falla explícitamente; jamás usa un PID arbitrario como sustituto. |
| Proceso actual | `OpenProcess` con el PID propio, `GetCurrentProcess` y consultas nativas | HANDLE real con vida útil estable y derechos separados de seudohandles. Los adaptadores de memoria NT requieren acceso apropiado (operar/consultar). |
| Prioridades de hilos | `SetThreadPriority`, `GetThreadPriority` | Mapeo explícito de las siete prioridades Win32 admitidas a valores `nice` Linux por TID. La modificación requiere derechos `THREAD_SET_INFORMATION` y capacidad real del kernel: elevar prioridad puede devolver `ACCESS_DENIED`. Los valores `nice` no representables no se clasifican arbitrariamente. No se afirma equivalencia de prioridades dinámicas Windows ni de clases de prioridad del proceso. |
| Tiempos de proceso e hilo | `GetThreadTimes`, `GetProcessTimes` | FILETIME de creación y terminación, contabilidad de CPU de usuario/kernel desde `getrusage(RUSAGE_THREAD/SELF)`, `wait4` para hijos propios y `/proc` para hilos aún ejecutándose. Rechaza handles sin permisos, procesos ajenos, y consultas no disponibles en el entorno Linux. Los tiempos de creación del hijo parten de la inscripción en el runtime, no del instante exacto de `fork`/`exec`. |
| Afinidad real de CPU | `SetThreadAffinityMask`, `NtSetInformationThread` (clase `ThreadAffinityMask` 4), `GetProcessAffinityMask` | Mapeo directo al conjunto de CPU Linux del hilo o proceso actual, comprobación de derechos, estado y máscara solicitada; los resultados reflejan la afinidad aplicada por el kernel. Perfil AMD64 limitado a grupo 0 con CPUs Linux 0–63: si la afinidad original contiene CPUs superiores, devuelve `NOT_SUPPORTED` sin truncarla. No se simulan grupos Windows adicionales. |
| APC, FLS y esperas | `QueueUserAPC`, `NtQueueApcThread`, `NtTestAlert`, `SleepEx`, `WaitFor*Ex`, `Fls*`, `Tls*` | Subconjuntos ya presentes: colas acotadas, despacho en el hilo de destino durante esperas alertables, independencia por hilo y liberación de recursos al finalizar |

### Límites intencionados y no negociables

1. **Suspensión asíncrona de un hilo activo:** no implementada. Sin un mecanismo seguro de contexto, aparcar otro pthread desde una señal Linux mientras posee locks internos puede romper el runtime. No usar `SIGSTOP`, `pthread_cancel` ni manipular GS desde otro hilo como solución ficticia.
2. **Terminación remota de hilos activos:** no implementada. `TerminateThread` solo admite la puerta de arranque; `NtTerminateThread` admite además la terminación del propio hilo.
3. **Contextos AMD64 de terceros:** faltan `GetThreadContext`, `SetThreadContext`, contextos de hilos suspendidos en ejecución y sincronización completa con SEH.
4. **Procesos externos y tokens NT:** no se abren procesos ajenos al runtime. Falta seguridad NT, transferencia real de handles marcados `INHERIT` a procesos hijo, sesiones, permisos por token, procesos de diferentes usuarios, impersonación y broker entre procesos.
5. **Creación avanzada:** faltan atributos extendidos de `NtCreateThreadEx`/`CreateProcessW`, flags generales, espacios remotos y compatibilidad x86/WOW64.
6. **Planificación:** la afinidad de hilos en un grupo de 64 CPU usa el planificador Linux; se admiten prioridades Win32 discretas mediante `nice`. Siguen faltando los grupos de procesadores mayores de 64, prioridades dinámicas de Windows, clases de prioridad del proceso, afinidad remota, quantum y equivalencia completa de planificación.
7. **APC y señalización completas:** variantes de APC de kernel, `NtQueueApcThreadEx`, E/S asíncrona con APC, `IoCompletionPort`, suspensión de hilos remotos y carreras de desconexión.
8. **Concurrencia y seguridad:** queda una batería de estrés, pruebas diferenciales Windows reales, auditoría del sandbox y casos de carreras de salida/descarga del loader. Un fixture sintético y una CI verde no equivalen a paridad total.

### Verificación de esta entrega

- `make check`: amplía `thread_affinity_test.c` con modificación real de nice en un TID, límites de permisos, tiempos FILETIME del hilo activo/terminado y CPU de usuario/kernel. Se amplía `process_thread_test.c` con FILETIME/`wait4` del hijo activo y terminado; los PE AMD64 verifican contratos de tiempo y prioridad.


- `make check`: incluye `tests/thread_affinity_test.c` para verificar `sched_getaffinity` real, derechos, NTSTATUS/LastError, preservación de máscaras y 48 ciclos de vida de hilos con alias. El test informa `SKIP` del caso positivo si el host no expone ninguna CPU Linux por debajo de 64; el fallo explícito del grupo no representable sí se comprueba.
- `make check`: añade `tests/process_thread_test.c`, que crea hijos Linux verdaderos, verifica `pidfd`, apertura/reapertura de handles, derechos, códigos de terminación, la puerta de inicio y el desbloqueo contado.
- `make check-pe-core-objects`: PE AMD64 MinGW con `NtOpenThread`, IDs, derechos, descripción UTF-16 de longitud superior al nombre de pthread y `LocalFree`.
- `make check-pe-suspended-thread`: extiende el PE AMD64 con los errores de `SetThreadAffinityMask`, `NtSetInformationThread` y `GetProcessAffinityMask`, y verifica su caso positivo en anfitriones con cpuset representable. También prueba `NtCreateThreadEx`, `NtTerminateThread`, `TerminateThread`, creación suspendida, límites de contador, hilos activos rechazados y salida sin entrada en ejecución.
- `make check-pe-isolated`: PE de procesos hijo comprueba `CREATE_SUSPENDED`, apertura, permisos y terminación del proceso. Una prueba `SKIP` por limitaciones de Bubblewrap **no es** una validación de esta ruta.
- El estado final de los jobs Linux/UBSan/Fedora y los hallazgos de seguridad deben acompañar la PR de esta fase; no confundir compilación con validación de las rutas funcionales.

### Consultas nativas del planificador

`NtQueryInformationThread(ThreadBasicInformation)` ahora obtiene la máscara de afinidad de grupo cero y la prioridad real Linux del hilo, y conserva una captura final antes de la salida para consultas posteriores. Sin CPU del grupo representable o con valor `nice` ajeno al perfil, devuelve `STATUS_NOT_SUPPORTED`, en vez de inventar la afinidad o la prioridad. Pruebas PE comprueban la transición `NORMAL` → `BELOW_NORMAL` tanto antes como después de terminar el hilo.

### Creación de hilos mediante handles de proceso

`CreateRemoteThread` y `CreateRemoteThreadEx` aceptan un seudohandle o HANDLE real del proceso actual con derecho `PROCESS_CREATE_THREAD`. `NtCreateThreadEx` comparte esa validación. Se prueba con un PE AMD64 que las tres rutas pueden crear hilos suspendidos, reanudarlos y observar el código de salida; los handles de solo consulta o los atributos no implementados devuelven error.

### Puerta de aceptación en un Fedora físico

Se incorpora `scripts/fedora-check-stage4.sh` para una ejecución manual, sin privilegios de administrador, en Fedora x86_64 con las dependencias de compilación ya instaladas. Ejecuta:

- `make check`, incluidas las pruebas de TID, prioridad, afinidad, FILETIME y 48 ciclos de vida de hilos.
- Fixtures PE AMD64 del cargador de procesos, suspensión, APC, TEB/TLS e información nativa del hilo.
- Una repetición de los contratos de núcleo con UBSan.
- `make check-pe-isolated` a través del sandbox Bubblewrap; si hay `SKIP` o no se ejecuta el test positivo de procesos hijo, el script devuelve estado **incompleto**.

Uso desde la raíz del repositorio: `bash scripts/fedora-check-stage4.sh`.

**No se ha ejecutado este script en el Fedora físico del usuario.** La CI usa un contenedor Fedora RPM; esto no equivale a probar el escritorio, hardware y política de namespaces del equipo destino.

**El objetivo del 100 % solicitado significa que todos los contratos de procesos/hilos deben estar implementados, probados en CI y que únicamente reste esta puerta final de Fedora físico. Aún no se cumple:** siguen sin implementarse suspensión/terminación arbitrarias de hilos activos, contextos AMD64 remotos, herencia general de handles, tokens y permisos NT, procesos ajenos, APC completas y equivalencia del planificador. No se considera 100 % por el mero éxito del script ni por los fixtures sintéticos existentes.

## Madurez del componente

- **Actual tras PR #82 fusionada: 72 %** (**+2 puntos porcentuales estimados** desde el 70 %). [CI Linux, UBSan y Fedora RPM superada](https://github.com/AlexDevJSC/144mb/actions/runs/43550562358). Incorpora flags independientes por handle, rechazo de cierre protegido, duplicación con permiso `PROCESS_DUP_HANDLE` usando handles reales y contabilización de handles. **No** implementa la herencia entre procesos ni permisos NT avanzados.


- **Actual tras PR #79 fusionada: 70 %** (estimación, **+2 puntos porcentuales** respecto a 68 %; [CI Linux/UBSan/Fedora superada](https://github.com/AlexDevJSC/144mb/actions/runs/43485165821)). El soporte remoto sigue limitado al proceso actual.


- **Actual tras PR #78 fusionada: 68 %** (estimación, **+2 puntos porcentuales** respecto a 66 %; [CI Linux, UBSan y Fedora superada](https://github.com/AlexDevJSC/144mb/actions/runs/43484452467)). Corrige campos nativos previamente falsos. No completa contextos AMD64, suspensión arbitraria ni herencia general.


- **Actual tras PR #76 fusionada: 66 %** (estimación, **+6 puntos porcentuales** respecto a 60 %; [CI Linux, UBSan y Fedora correcta](https://github.com/AlexDevJSC/144mb/actions/runs/43483478807)). El incremento corresponde a planificación discreta con efectos reales en Linux y telemetría de procesos/hilos, no a suspensión asíncrona, contextos completos o herencia de handles.


- **Anterior: 35 %** (línea base tras PR #68).
- **Actual tras PR #71 y #72 fusionadas: 55 %** (estimación, **+20 puntos porcentuales**).
- **Actual tras PR #74 fusionada: 60 %** (estimación, **+5 puntos porcentuales** respecto al 55 % anterior). [CI Linux, UBSan y Fedora superada](https://github.com/AlexDevJSC/144mb/actions/runs/43346528358), incluida la ruta positiva de afinidad CPU real y 48 ciclos de hilos. No se afirma cobertura de prioridades, processor groups múltiples ni suspensión arbitraria.
- **Justificación:** ahora existen rutas conectadas de creación, apertura, identidad, consulta, suspensión de arranque, terminación y cierre tanto para hilos como para procesos hijo, y pruebas Win64/host que recorren varias de ellas.
- **Por qué no 70 % ni 100 %:** faltan contratos sustanciales de suspensión/terminación remota, captura y restauración de contextos AMD64, tokens y permisos NT, herencia de handles, servicios entre procesos, prioridades/afinidad y comparación de comportamiento con Windows real.

Este número **no mide aceleración de CPU**. No se ha realizado ni demostrado una mejora 2× en rendimiento; «duplicar porcentaje» y «duplicar rendimiento» son objetivos distintos.

La [CI del último commit de la rama de integración](https://github.com/AlexDevJSC/144mb/actions/runs/43341757354) terminó correctamente tanto en Linux como en Fedora RPM. Las pruebas omitidas por restricciones del entorno no constituyen validación. Permanecen necesarios tests diferenciales Windows y de estrés para justificar porcentajes mayores.
