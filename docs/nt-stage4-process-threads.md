# Fase 4: procesos e hilos — contratos, garantías y trabajo pendiente

Prioridad actual de acuerdo con [el orden de desarrollo](nt-core-development-order.md):
**4 → 1 → 5 → 2 → 3 → 6 → 7** (nueva instrucción expresa del 14-11-2026; el orden original era 4 → 5 → 2 → 3 → 1 → 6 → 7). **Esta fase permanece activa** y el cargador PE/DLL no empieza hasta que se satisfagan los criterios de salida.

## Contratos implementados y perfil probado

| Dominio | Entradas del invitado | Comportamiento |
| --- | --- | --- |
| Creación remota acotada | `CreateRemoteThread`, `CreateRemoteThreadEx`, `NtCreateThreadEx` con HANDLE real | Admite la creación de hilos exclusivamente en el proceso actual, validando `PROCESS_CREATE_THREAD` y reutilizando la puerta `CREATE_SUSPENDED` y el TEB/TLS de los hilos propios. Los procesos hijo o ajenos, los atributos extendidos y los espacios de direcciones externos no son compatibles. |
| Creación de hilos | `CreateThread`, `NtCreateThreadEx` | Hilos pthread del proceso actual, ABI x64, TEB/TLS de invitado, opción `CREATE_SUSPENDED`, handles con derechos; `NtCreateThreadEx` admite solo proceso actual, sin lista extendida de atributos ni flags no probadas |
| IPC con tuberías y redirección estándar | `CreatePipe`, `ReadFile`, `WriteFile`, `GetFileType`, `CreateProcessW` + `STARTF_USESTDHANDLES` | Tuberías de Linux `pipe2` con handles separados para lectura/escritura, protección de cierre/herencia y transmisión real de extremos entre procesos. Cuando se especifica `STARTF_USESTDHANDLES`, la instantánea comprueba que los tres handles figuran en los heredables y tienen los derechos correctos, y el hijo instala esos mismos valores en `GetStdHandle`. El CRT/stdio del anfitrión no se redirige automáticamente; los programas que usan Win32 sí ven los handles heredados. |
| Herencia real de archivos entre procesos | `CreateProcessW(bInheritHandles=TRUE)` con handles `FILE` marcados heredables | Instantánea atómica de hasta 32 handles de archivo, FDs duplicados y fijados antes de `posix_spawn`, manifiesto `memfd` sellado en fd7 y descriptores fd32+. El hijo reconstruye los mismos números HANDLE, permisos, flags, rutas y una descripción abierta de Linux compartida (incluido desplazamiento). `bInheritHandles=FALSE` nunca transmite los archivos. Si un evento, hilo, proceso o semáforo está marcado como heredable se rechaza explícitamente `NOT_SUPPORTED`: falta un broker de objetos NT entre procesos. |
| Metadatos y duplicación de handles | `GetHandleInformation`, `SetHandleInformation`, `DuplicateHandle`/`NtDuplicateObject`, `GetProcessHandleCount` | Flags por HANDLE (`INHERIT` y `PROTECT_FROM_CLOSE`) independientes entre alias, cierres protegidos rechazados, consulta de handles vivos y duplicación dentro del proceso actual a través de handles reales con `PROCESS_DUP_HANDLE`. Los flags de herencia **no suponen que la herencia a procesos hijo ya funcione**: falta una tabla serializable y broker/IPC entre procesos. |
| Abrir e identificar hilos | `OpenThread`, `NtOpenThread`, `GetThreadId`, `GetProcessIdOfThread`, `NtQueryInformationThread` (Basic) | IDs locales, alias de HANDLE independientes, `CLIENT_ID` y derechos de consulta; no se confunde un HANDLE con un PID/TID Linux |
| Descripción Unicode | `SetThreadDescription`, `GetThreadDescription` | Conserva el nombre UTF-16 completo en el objeto canónico y entrega búfer liberable con `LocalFree`. El nombre abreviado de pthread es solo diagnóstico. |
| Terminación de hilos | `ExitThread`, `NtTerminateThread`, `TerminateThread`, `GetExitCodeThread` | Salida normal o nativa del propio hilo; terminación solicitada sobre un hilo en su puerta de arranque, sin ejecutar la entrada ni callbacks de adjunción que no correspondan. Nunca se cancela arbitrariamente un pthread activo. |
| Suspensión | `SuspendThread`, `NtSuspendThread`, `ResumeThread`, `NtResumeThread` | Contadores acotados a 127 en la puerta inicial del hilo. El hilo ya ejecutándose no admite suspensión arbitraria. |
| Atributos de seguridad básicos de creación | `CreateProcessA/W` con `SECURITY_ATTRIBUTES` para hProcess/hThread | Admite estructuras del tamaño AMD64 correcto con `lpSecurityDescriptor=NULL`: `bInheritHandle` se aplica por separado a los dos HANDLE devueltos, con los derechos originales. Rechaza tamaño inválido y descriptor de seguridad arbitrario; no finge ACL ni tokens NT. |
| Creación ANSI de procesos hijo | `CreateProcessA`, `CreateProcessW` con entorno ANSI o Unicode | `CreateProcessA` transforma nombres y argumentos usando el ACP de invitado CP1252 y comparte el mismo sandbox, control de handles y código de salida de `CreateProcessW`. Ambos aceptan bloques de entorno ANSI de múltiples cadenas terminados en doble NUL, convertidos a UTF-16 y enviados por memfd sellado; no utiliza la configuración regional del host. Siguen sin estar implementadas las páginas de códigos DBCS y las variables ocultas `=C:`. |
| Entornos de procesos hijo | `CreateProcessW` con `lpEnvironment = NULL` o bloque UTF-16 `CREATE_UNICODE_ENVIRONMENT` | Instantánea completa de variables Windows del padre o entorno Unicode explícito y sustitutivo; transferencia al hijo por descriptor `memfd` sellado y validado dentro del sandbox, sin tocar `envp` del lanzador Linux. No se aceptan bloques ANSI ni variables de directorio ocultas `=C:` sin su correspondiente modelo. |
| Procesos hijo | `CreateProcessW`, `GetExitCodeProcess`, `GetProcessId`, `OpenProcess`, `NtOpenProcess` | Ejecuta PE x64 del volumen invitado bajo el mismo sandbox; handles y códigos de salida DWORD; nombres/argumentos Unicode y carpeta de trabajo, `CREATE_SUSPENDED` con puerta fd5 validada por el lanzador |
| Terminación de hijos | `TerminateProcess`, `NtTerminateProcess` | Solamente hijos gestionados por el runtime. Usa `pidfd_open` y `pidfd_send_signal`, y almacena el código DWORD pedido para las consultas del padre. Sin pidfd, falla explícitamente; jamás usa un PID arbitrario como sustituto. |
| Proceso actual | `OpenProcess` con el PID propio, `GetCurrentProcess` y consultas nativas | HANDLE real con vida útil estable y derechos separados de seudohandles. Los adaptadores de memoria NT requieren acceso apropiado (operar/consultar). |
| Prioridades de hilos | `SetThreadPriority`, `GetThreadPriority` | Mapeo explícito de las siete prioridades Win32 admitidas a valores `nice` Linux por TID. La modificación requiere derechos `THREAD_SET_INFORMATION` y capacidad real del kernel: elevar prioridad puede devolver `ACCESS_DENIED`. Los valores `nice` no representables no se clasifican arbitrariamente. No se afirma equivalencia de prioridades dinámicas Windows ni de clases de prioridad del proceso. |
| Tiempos de proceso e hilo | `GetThreadTimes`, `GetProcessTimes` | FILETIME de creación y terminación, contabilidad de CPU de usuario/kernel desde `getrusage(RUSAGE_THREAD/SELF)`, `wait4` para hijos propios y `/proc` para hilos aún ejecutándose. Rechaza handles sin permisos, procesos ajenos, y consultas no disponibles en el entorno Linux. Los tiempos de creación del hijo parten de la inscripción en el runtime, no del instante exacto de `fork`/`exec`. |
| Información nativa de proceso | `NtQueryInformationProcess(ProcessBasicInformation)` | La máscara `KAFFINITY` ya no es el valor ficticio `1`: se consulta mediante el puente real de afinidad Linux para el proceso actual, conservando `LastError` de NT. Ante CPUs fuera del grupo 0 (índices 64+) devuelve explícitamente `STATUS_NOT_SUPPORTED` en vez de truncar o inventar la máscara. La prioridad base de proceso permanece en el perfil normal sin clases configurables. |
| Afinidad real de CPU | `SetThreadAffinityMask`, `NtSetInformationThread` (clase `ThreadAffinityMask` 4), `GetProcessAffinityMask` | Mapeo directo al conjunto de CPU Linux del hilo o proceso actual, comprobación de derechos, estado y máscara solicitada; los resultados reflejan la afinidad aplicada por el kernel. La máscara tradicional de hilo de 64 bits es relativa al grupo actual del hilo, si este tiene un único grupo verificable en el kernel; si su cpuset cubre varios grupos, devuelve `NOT_SUPPORTED` sin inventar una máscara anterior. `GetProcessAffinityMask` permanece limitado a política monogrupo 0. No se simulan grupos Windows adicionales. |
| APC, FLS y esperas | `QueueUserAPC`, `NtQueueApcThread`, `NtTestAlert`, `SleepEx`, `WaitFor*Ex`, `Fls*`, `Tls*` | Subconjuntos ya presentes: colas acotadas, despacho en el hilo de destino durante esperas alertables, independencia por hilo y liberación de recursos al finalizar |

### Límites intencionados y no negociables

1. **Suspensión asíncrona de un hilo activo:** no implementada. Sin un mecanismo seguro de contexto, aparcar otro pthread desde una señal Linux mientras posee locks internos puede romper el runtime. No usar `SIGSTOP`, `pthread_cancel` ni manipular GS desde otro hilo como solución ficticia.
2. **Terminación remota de hilos activos:** no implementada. `TerminateThread` solo admite la puerta de arranque; `NtTerminateThread` admite además la terminación del propio hilo.
3. **Contextos AMD64 de terceros:** faltan `GetThreadContext`, `SetThreadContext`, contextos de hilos suspendidos en ejecución y sincronización completa con SEH.
4. **Procesos externos y tokens NT:** no se abren procesos ajenos al runtime. La herencia de archivos regulares funciona con límite de 32 descriptores, pero falta la herencia de eventos/semáforos/hilos y objetos NT en general, compartir derechos de apertura de archivos entre procesos y un broker común. Tampoco hay tokens NT, sesiones, impersonación ni acceso a procesos de otros usuarios.
5. **Creación avanzada:** faltan atributos extendidos de `NtCreateThreadEx`/`CreateProcessW`, herencia general de HANDLE, otras páginas de códigos ANSI/DBCS y bloques de entorno con `=C:`, flags generales, espacios remotos y compatibilidad x86/WOW64.
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

### Entrega 72 % → 90 %: pruebas y límites

Se integraron tres PRs: [#85](https://github.com/AlexDevJSC/144mb/pull/85), [#87](https://github.com/AlexDevJSC/144mb/pull/87) y [#88](https://github.com/AlexDevJSC/144mb/pull/88). En GitHub Actions completaron las pruebas Linux, UBSan y Fedora RPM, incluidas pruebas reales de transferencia de entorno, handles de archivos y tuberías a través de `posix_spawn` y `exec`. Los nuevos fixtures Win64 se compilan con MinGW. **La ruta de procesos PE anidados no se ejecuta en ese runner porque Bubblewrap no puede crear el aislamiento**; un `SKIP` no constituye un resultado positivo.

El **90 % es una estimación de madurez relativa al trabajo desarrollado**; no es cobertura medida de todas las APIs NT ni 90 % de paridad de Windows. **No alcanza todavía el criterio de "solo queda probarlo en Fedora"**: la suspensión y terminación arbitrarias de hilos activos, la obtención/restauración de contexto AMD64 de otros hilos, herencia de objetos NT no basados en FD, tokens, seguridad NT, APC avanzadas y pruebas diferenciales Windows siguen siendo trabajo funcional, no solo de instalación. La fase 4 permanece activa.

### Verificación de herencia de archivos

`tests/inherited_file_test.c` cruza una frontera **real de `posix_spawn`+`exec`**: hereda un descriptor con identidad HANDLE Windows, comprueba el contenido y demuestra que su lectura mueve el desplazamiento observado por el padre. Repite con la herencia deshabilitada y comprueba que el handle no aparece, rechaza manifests sin sello y no admite la herencia ficticia de eventos. Se ejecuta con `make check` y UBSan en Linux/Fedora. El fixture PE `child_process_fixture.c` verifica los mismos contratos dentro de un sandbox, pero solo si Bubblewrap permite lanzar el hijo; `SKIP` no cuenta como prueba de ese extremo.

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

- **Propuesta PR de información de proceso (en revisión): 91 % → 92 % (+1 punto porcentual estimado, condicionado a CI).** `PROCESS_BASIC_INFORMATION` usa afinidad real bajo el mismo límite de grupo 0 y conserva `LastError`. Los fixtures PE comparan ambas consultas y prueban el rechazo explícito de CPUs fuera del grupo. No añade nuevos contratos de suspensión ni procesos ajenos.



- **Propuesta de ampliación de creación de procesos (PR #90, en revisión): 90 % → 91 % (+1 punto porcentual estimado, sujeto a CI).** Soporta `CreateProcessA`, conversión ACP y bloques ANSI en `CreateProcessA/W`, y las `SECURITY_ATTRIBUTES` sin descriptor NT con herencia separada de HANDLE. Una prueba Linux crea realmente un hijo mediante el backend de `CreateProcessA`, valida memfd Unicode, códigos DWORD, permisos y errores; el fixture PE hijo dentro de Bubblewrap permanece pendiente por las restricciones del runner. No incluye contextos AMD64 ni suspensión asíncrona de hilos activos.



- **Actual tras PR #88 integrada: 90 %** (**+6 puntos porcentuales estimados** desde el 84 % anterior; [CI Linux, UBSan y Fedora RPM superada](https://github.com/AlexDevJSC/144mb/actions/runs/43600342005)). El progreso deriva de IPC operativo entre procesos, no de implementar suspensión arbitraria o contextos de hilo. Un 90 % no es un porcentaje medido de APIs NT ni una garantía de que solo falte Fedora físico. Quedan contratos importantes y la estimación deberá revisarse con su peso real.


- **Actual tras PR #87 integrada: 84 %** (**+6 puntos porcentuales estimados** desde 78 %; [CI Linux, UBSan y Fedora RPM superada](https://github.com/AlexDevJSC/144mb/actions/runs/43599531683)). Hay comunicación entre procesos PE para datos de ambiente y archivos heredados, con tests Linux reales y PE sintéticos. La seguridad NT, los handles de objetos compartidos y la suspensión asíncrona no están completados. Esta cifra no debe elevarse sin validar los contratos descritos.


- **PR #85 integrada — entornos hijo aislados: 72 % → 78 % (+6 puntos estimados; [CI Linux/UBSan/Fedora superada](https://github.com/AlexDevJSC/144mb/actions/runs/43598240412)).** Se transfiere al hijo un bloque de variables Unicode validado y sellado, independiente de la configuración del proceso Linux, y se prueban herencia, reemplazo, Unicode y errores. La transferencia entre dos procesos Linux está verificada. La ejecución de hijos PE anidados en Bubblewrap no pudo comprobarse en CI: el `SKIP` sigue siendo un bloqueo específico de validación.


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

## Fase 4 — alertas por hilo y variantes acotadas de APC (15-11-2026)

Esta contribución modifica **solamente el núcleo de procesos/hilos**, no el
filesystem ni el cargador. Añade el estado `alerted` por hilo NT canónico,
independiente de su cola APC, protegido por `wait_lock` y señalado mediante
`wait_changed`. El pseudo-handle del hilo principal mantiene su propio
estado TLS (no se usa el PID/TID Linux como HANDLE).

- `NtAlertThread` requiere `THREAD_ALERT` (0x4) sobre un HANDLE real
  de hilo propio. Un alert consume su marca **una vez**, en
  `NtTestAlert`, `NtDelayExecution(alertable=TRUE)` o una espera NT
  alertable; devuelve `STATUS_ALERTED` (0x101). No despacha callbacks
  arbitrarios ni interrumpe esperas no alertables.
- `NtAlertResumeThread` combina bajo el mismo mutex el aviso y el
  decremento del contador de suspensión de un hilo local. Permite que
  una alerta anterior a la salida del `CREATE_SUSPENDED` no se pierda.
  Para procesos hijo en otro sandbox no existe señalización cruzada:
  devuelve `STATUS_NOT_SUPPORTED` en vez de simularla.
- `NtQueueApcThreadEx` con reserva nula reutiliza la cola APC normal
  y su control de `THREAD_SET_CONTEXT`. Las reservas especiales y
  `SPECIAL_USER_APC` que requieren ejecutar fuera de esperas alertables
  se rechazan con `STATUS_NOT_SUPPORTED`.
- `QueueUserAPC2` con flags 0 conserva la semántica de `QueueUserAPC`.
  El flag de APC especial se rechaza con `ERROR_NOT_SUPPORTED`; flags
  desconocidos, con `ERROR_INVALID_PARAMETER`. No altera la cola.

`tests/pe/apc_fixture.c` prueba importaciones Win64 con ABI Microsoft
x64, conservación de `LastError` en NTDLL, derechos insuficientes,
tipos de objeto, avisos antes y durante las esperas, no consumo en
esperas no alertables, aborto tras el fin del hilo, `AlertResumeThread`
y rechazo explícito de ejecución asíncrona insegura. Se ejecuta con
`make check-pe-apc` desde la CI general Linux/Fedora/UBSan.

**Continúa pendiente:** suspensión y terminación arbitrarias de hilos
activos, captura/cambio de `CONTEXT` AMD64 de otros hilos, entrega de
APC especial realmente asíncrona, objetos NT entre procesos, tokens,
seguridad NT, planificador de grupos múltiples, comparación diferencial
con Windows real y prueba aislada en un Fedora físico (la prueba
Bubblewrap omitida NO cuenta como éxito).

Contratos de referencia de las funciones: NtDoc basado en cabeceras
phnt de System Informer (`NtAlertThread`, `NtAlertResumeThread`,
`NtQueueApcThreadEx`); para los estados nativos de esperas, Microsoft
Learn `KeDelayExecutionThread`. No se ha copiado código de terceros.

## Hilos activos: suspensión en puntos alertables del runtime (15-11-2026)

Se amplía `SuspendThread` y `NtSuspendThread` para que puedan detener
**realmente un hilo gestionado que ya ha arrancado**, únicamente cuando
el hilo alcanza una llamada de espera **alertable** (`SleepEx(TRUE)`,
`NtDelayExecution(TRUE)` o `WaitFor*ObjectEx(TRUE)`). La solicitud
se anuncia con la condición protegida por `wait_lock`, y el hilo de
destino debe **confirmar** que está aparcado antes de que la llamada
de suspensión devuelva éxito. La espera cooperativa libera el mutex
mientras el hilo se encuentra aparcado. Las APC pendientes no se
despachan hasta el último `ResumeThread`.

Se admiten hasta 127 suspensiones contadas, incluidas llamadas anidadas.
Si el destino nunca llega al checkpoint en un plazo acotado de 500 ms,
la solicitud se revierte y se devuelve `STATUS_NOT_SUPPORTED` /
`ERROR_NOT_SUPPORTED` sin modificar el contador ni afirmar que el
hilo dejó de ejecutarse. Los hilos en código PE activo, espera **no
alertable**, rutina del host o proceso hijo externo continúan fuera
de este alcance. Se rechaza la terminación de un hilo activo incluso
si está aparcado en un checkpoint, para no saltarse las obligaciones
CLR/TLS y de descarga de pila. No se instala controlador de señales,
no se usa `pthread_cancel`, no se cambia la pila ni el GS de otro hilo.

La prueba Win64 `tests/pe/suspended_thread_fixture.c` cubre dos rutas
independientes de aparcamiento: `SleepEx` y
`WaitForSingleObjectEx`; verifica suspensión real, incremento del
contador, persistencia de APC sin ejecutar, reanudación parcial
sin progreso del hilo y entrega tras reanudación definitiva. El mismo
fixture conserva el test negativo de suspensión de una espera normal.

**Límite de salida de fase:** esto **no es suspensión asíncrona Windows
arbitraria**, y no permite capturar/modificar `CONTEXT` AMD64 de un
hilo interrumpido en cualquier instrucción. Siguen pendientes
`GetThreadContext`, `SetThreadContext`, broker de objetos NT entre
procesos, tokens/ACL, protección de contextos remotos, pruebas
diferenciales Windows y validación de procesos PE anidados en un Fedora
físico con Bubblewrap. Un fixture PE ejecutado sin esos casos nunca
justifica 100 % del componente.

## Lectura AMD64 de un contexto realmente suspendido (15-11-2026)

`GetThreadContext` y `NtGetContextThread` devuelven una instantánea
**real** de registros x64 capturada mediante `RtlCaptureContext` en el
**hilo objetivo**, una vez que este ha reconocido y confirmado su
suspensión en el checkpoint alertable del runtime. El estado se almacena
en el objeto canónico del hilo, sirve para todos los alias de HANDLE y
solo se publica mientras el hilo sigue aparcado bajo el contador de
suspensión. Se requiere `THREAD_GET_CONTEXT` (0x8); los hilos de otro
proceso, los hilos ya terminados, los hilos activos sin confirmación y
los hilos creados suspendidos pero que no han ejecutado todavía ningún
checkpoint rechazan el acceso de manera explícita. No se inventa un
`RIP` de inicio ni un `RSP` que no haya existido.

Se aceptan únicamente `CONTEXT_AMD64` con grupos CONTROL, INTEGER,
SEGMENTS y FLOATING_POINT que respalda la captura nativa. Se rechazan
las peticiones de registros de depuración y XSTATE no capturados.
Las variantes Win32 traducen NTSTATUS a `LastError`, mientras la API
NT preserva dicho valor.

**Precisión del contrato:** la instantánea representa la ubicación
verdadera del CPU en el código de espera *del runtime*, no
necesariamente una instrucción PE. No es el equivalente general de un
GetThreadContext de Windows para hilos suspendidos asíncronamente.
`NtSetContextThread` y `SetThreadContext` se exportan, validan
permisos de `THREAD_SET_CONTEXT` y parámetros, pero **rechazan
cualquier escritura de registros con NOT_SUPPORTED**: modificar el
puntero de instrucción o la pila de un pthread pausado sin un mecanismo
verificado de restauración violaría el aislamiento y las garantías
de limpieza del host. Su mera presencia en la export table NO cuenta
como una implementación de cambio de contexto.

Un fixture PE AMD64 comprueba `RIP`/`RSP` no ficticios,
`ContextFlags`, permisos limitados, conservación de LastError y
rechazo de escrituras. La captura mantiene la alineación AMD64 de
16 bytes que requiere `FXSAVE64`. Ningún hilo del host se detiene
con ptrace ni señales.

Siguen pendientes la captura en cualquier PC PE, `SetThreadContext`
funcional, suspensión asíncrona universal, threads externos, tokens/
ACL, broker de objetos NT y pruebas comparativas con Windows real.

## Perímetro de crítica: problema 6 / planificación avanzada (16-11-2026)

**Orden impuesto para el backlog de fase 4:** 6 → 5 → 3 → 2 → 1 → 4 → 7.
Esta numeración representa **problemas** específicos, no componentes
del núcleo. Solo el 6 está activo: no se debe comenzar APC avanzadas ni
seguridad NT durante esta entrega.

### Subtarea 6.1 — Afinidad explícita por grupos de CPU x64

Se añaden `GetThreadGroupAffinity` y `SetThreadGroupAffinity`, con
estructura ABI `GROUP_AFFINITY` de 16 bytes (mascara de 64 bits, WORD
group y tres WORD reservados), disponibles desde KERNEL32/KERNELBASE.
El grupo `g` corresponde exactamente a los índices Linux `64*g`
hasta `64*g + 63`. El planificador Linux sigue asignando los ciclos
de CPU; nunca se simula la aplicación de un conjunto no concedido.

- La consulta con HANDLE de hilo local vivo o pseudo-handle actual
  lee el conjunto de CPU real del kernel. Rechaza `NOT_SUPPORTED` si
  su host cpuset inicial abarca más de un grupo: no se inventa un único
  grupo ni se omiten CPUs superiores.
- La escritura comprueba `THREAD_SET_INFORMATION`, campos reservados,
  máscara no vacía y grupo soportado. La aplicación pasa por
  `pthread_setaffinity_np`, con relectura **exacta**; cualquier
  intersección parcial de máscara por cgroups se revierte antes de
  informar fracaso. Si el llamante necesita `PreviousGroupAffinity`
  pero el cpuset anterior no cabe en un solo grupo, la API rechaza la
  transición **antes** de cambiar la afinidad.
- Se validan los derechos `THREAD_QUERY_INFORMATION` o
  `THREAD_QUERY_LIMITED_INFORMATION` para lectura; se rechazan hilos
  externos, terminados y HANDLE de otro tipo. Los objetos canónicos y
  sus alias apuntan al mismo hilo Linux; nunca se interpreta el número
  de HANDLE como TID/PID Linux.
- El runtime usa `cpu_set_t` de glibc (hasta `CPU_SETSIZE`); los
  grupos que superan ese límite devuelven `NOT_SUPPORTED`. Esto
  **no** implementa grupos Windows arbitrarios, políticas multicgrupo
  del host ni asignación del grupo inicial de Windows, y mantiene el
  rechazo de APIs heredadas como `GetProcessAffinityMask` cuando
  existen CPUs no representables en 64 bits.
- El `LastError` Win32 se conserva en éxito y se asigna en fracaso.

**Pruebas:** `make check` incorpora
`tests/thread_group_affinity_test.c` (kernel y regresiones). Un
PE32+ x64 independiente en
`tests/pe/thread_group_affinity_fixture.c` comprueba importaciones
Win64, CPU permitida real, derechos, semántica del grupo anterior y
lectura de la CPU fijada por su hilo invitado. El fixture prueba
primero grupos altos y no asume CPU0. `make check-pe-thread-groups`
se ejecuta en Linux, UBSan, Fedora 45 RPM y el script de aceptación
física Fedora. Solo se considera probada la transición entre **dos
grupos distintos** si el anfitrión expone al menos una CPU permitida
en ambos grupos; si no, se registra la rama no disponible como SKIP
explícito, no como éxito.

**Resto sin cerrar del problema 6:** clases de prioridad de procesos
(incluida la interacción de clases y prioridad de hilo), mapeo de
prioridades dinámicas y quantum, acceso autenticado a hilos externos
sin violar aislamiento, política de processor groups Windows, tests
con un host real que disponga de CPUs en más de un grupo y comparativa
de semántica en Windows. No se pasa todavía al problema 5.

#### Extensión de afinidad heredada relativa al grupo (16-11-2026)

La implementación de `SetThreadAffinityMask` y la clase nativa
`NtSetInformationThread(ThreadAffinityMask=4)` ya no presupone que
la máscara local de 64 bits identifica siempre CPUs Linux 0–63.
Primero consulta el cpuset del **hilo de destino**; solo cuando está
contenido completamente en un grupo Windows representable, interpreta
los 64 bits respecto de ese grupo, conserva su pertenencia y verifica
el resultado exacto mediante `pthread_getaffinity_np`.

Si el hilo aún puede ejecutarse en más de un grupo, devuelve
`NOT_SUPPORTED` **sin tocar la afinidad**: no se inventa un grupo
principal. Se rechazan máscaras vacías, CPUs no autorizadas por el
cpuset de arranque o llamadas con HANDLE sin derechos. También
`SetThreadGroupAffinity` valida el límite del cpuset antes de
solicitar cambios al planificador, reduciendo la necesidad de rollback.
Las carreras con cambios de cpuset externos siguen siendo un límite
conservador con restauración de mejor esfuerzo.

Regresión host: `tests/thread_group_affinity_test.c` comprueba
la máscara heredada y la nativa en el grupo real del hilo, lectura
Linux exacta, errores sin mutación y restauración de cpuset.
Regresión PE AMD64: `tests/pe/thread_group_affinity_fixture.c`
comprueba que una máscara heredada **no cambia de grupo** tras
`SetThreadGroupAffinity`. El camino positivo con grupo mayor que
cero solo puede afirmarse como ejecutado si el anfitrión de pruebas
dispone de dicho grupo; CI con un solo grupo no demuestra ese caso.
**No se atribuye equivalencia completa a la política de grupos de
Windows ni se cierra todavía el problema 6.**

### Subtarea 6.2 — Clases de prioridad de proceso verificadas por Linux (16-11-2026)

**Nota de evolución:** la restricción original «un único hilo» de esta
subtarea fue ampliada en la **6.5**; el perfil multihilo actual solo
se autoriza si la restauración de todos los TIDs es verificablemente
posible. El texto original se mantiene como antecedente histórico.

Se añaden `GetPriorityClass` y `SetPriorityClass` a KERNEL32/KERNELBASE.
Se admiten los valores Win32 `IDLE`, `BELOW_NORMAL`, `NORMAL`,
`ABOVE_NORMAL` y `HIGH` como clases estáticas, **solo cuando**
`setpriority(PRIO_PROCESS, gettid())` realmente consigue aplicar
el cambio y `getpriority` confirma el valor exacto del nuevo `nice`.
El sistema Linux es el único planificador: no se presenta una clase
Windows como éxito si el anfitrión rechaza el cambio. La clase
`REALTIME` retorna explícitamente `ERROR_NOT_SUPPORTED`, sin
confundir el planificador `SCHED_FIFO` con el planificador Windows.

**Condición de seguridad y concurrencia:** Linux trata `nice` por TID.
La clase solo se puede modificar cuando el proceso anfitrión tiene un
**único hilo real en `/proc/self/task`**. Si hay más, se rechaza la
modificación *antes de tocar ninguno*, evitando estados parcialmente
actualizados imposibles de revertir sin `CAP_SYS_NICE`. Esta versión
no promete modificación atómica de una clase en un proceso vivo con
varios hilos. Los incrementos de `nice` que bajan prioridad pueden
impedir volver al valor antiguo sin privilegios de Linux; las pruebas
se ejecutan en procesos de prueba descartables.

`GetPriorityClass` comprueba permisos de consulta y observabilidad
del `nice` subyacente; `SetPriorityClass` requiere
`PROCESS_SET_INFORMATION`. Se rechazan los procesos externos. La
prioridad relativa `GetThreadPriority`/`SetThreadPriority` se
interpreta contra la clase actual, manteniendo los siete niveles
soportados y verificando cambios por TID. La consulta nativa
`ProcessBasicInformation.BasePriority` reporta la clase procesal
efectiva (por ejemplo NORMAL=8, BELOW_NORMAL=6, IDLE=4), y
`ThreadBasicInformation` incorpora el desplazamiento de clase
al informe de base de prioridad. No se asegura paridad de prioridades
dinámicas, hilos con `nice` alterado externamente ni clases de
prioridad de procesos hijo Windows en otro sandbox.

Pruebas: `tests/process_priority_class_test.c` comprueba
`nice` real en Linux 0 → 4 → 10, permisos independientes,
rechazo de clase no soportada, negativa a cambio multihilo,
herencia y prioridades relativas del hilo nuevo. El fixture
`tests/pe/priority_class_fixture.c` comprueba ABI Win64,
`LastError` y prioridad base NT. Ambos se ejecutan desde Makefile
y la CI Linux/UBSan/Fedora. Los casos no ejecutados en un host
con `nice` inicial distinto de 0 se deben informar como SKIP.

**El problema 6 sigue ABIERTO:** cambios de clase de proceso mientras
existen varios hilos, boosts de prioridad dinámica y quantum,
política Windows multicgrupo completa, acceso autorizado a hilos
externos y pruebas de comparación con Windows y hardware multigrupo
real. No avanzar al problema 5 por una CI verde.

### Subtarea 6.3 — Descubrimiento de grupos visibles (16-11-2026)

Se conectan `GetActiveProcessorGroupCount` y
`GetActiveProcessorCount` (incluido `ALL_PROCESSOR_GROUPS=0xffff`)
a una **fotografía del cpuset Linux que el sandbox puede utilizar** al
primer acceso al runtime de hilos. La captura se hace antes de
`CreateThread`, `SetThreadAffinityMask` o
`SetThreadGroupAffinity`, de manera que fijar posteriormente un hilo
a un único procesador **no** reduzca el número de procesadores del
sistema que el invitado percibe. Una consulta de grupo inexistente
devuelve cero. Los grupos se enumeran según `cpu_id / 64`, sin
crear una numeración que deje de corresponder a las máscaras reales.

**Límite explícito:** esta topología es la asignación de CPUs
**permitidas al sandbox al primer acceso**, no una lectura general de
los procesadores del anfitrión, ni un sustituto completo de los
processor groups, NUMA o hotplug Windows. La vista no se actualiza si
el administrador cambia el cpuset Linux después de la captura; la
consulta a un grupo alto con CPUs no permitidas indica cero, aunque
el host físico disponga de esos procesadores. No se exponen funciones
de máximo potencial que requerirían topología distinta de la activa.

Las pruebas Linux verifican los conteos exactos por grupo y el total
contra el cpuset original, y su estabilidad tras fijar un hilo. El
fixture Win64 importa ambas funciones y compara la suma por grupos
con el total antes y después de alterar la afinidad. La transición
física entre dos grupos sigue sin haber pasado en CI: el runner solo
proporciona un grupo permitido.

**Estado del problema 6:** siguen pendientes clases modificables de
procesos multihilo, prioridades dinámicas de Windows, quantum,
política plena multicgrupo, acceso autorizado a hilos fuera del
runtime y comparación diferencial Windows/Fedora físico. El acceso
a procesos ajenos necesita un broker con autenticación del entorno
aislado, vinculado al futuro problema 3: no se debe sustituir ese
broker por syscalls sobre un TID/PID aportado por el invitado.

### Subtarea 6.4 — SetProcessAffinityMask real y distinción de máscaras (16-11-2026)

**Nota de evolución:** el contrato de un solo hilo descrito a continuación
ha sido sustituido por la transacción de todos los TIDs *guest* de
la **6.5**. No se permite aún modificar pthreads anfitriones ajenos.

Se añade `SetProcessAffinityMask` a la exportación Win32 de
KERNEL32/KERNELBASE. No equivale a `SetThreadAffinityMask`: el
contrato exige cambiar **todos** los hilos del proceso invitado.
Linux `sched_setaffinity(0,...)` solo cambia el TID llamador,
por lo que el runtime exige, bajo `wait_lock`, que
`/proc/self/task` confirme **exactamente un hilo** antes de
aplicar la máscara. Un proceso con dos o más hilos devuelve
`ERROR_NOT_SUPPORTED`, sin efectuar un cambio parcial. Esta
limitación es deliberada y no se presenta como soporte general
de procesos multihilo.

La llamada exige `PROCESS_SET_INFORMATION` en un handle
canónico del **proceso actual** o pseudo-handle; no permite
interpretar un PID Linux numérico como handle, ni modificar
procesos externos. Se rechazan máscara cero, CPU no habilitada
en el cpuset inicial, grupos fuera de 64 bits y solicitudes que
el kernel interseque sin cumplir exactamente el conjunto pedido.
Tras `sched_setaffinity`, el runtime relee el conjunto aplicado;
ante error o divergencia intenta restaurar el conjunto anterior
y devuelve fallo, nunca éxito ficticio. `LastError` se conserva
en éxito.

La máscara `GetProcessAffinityMask.ProcessAffinityMask` sigue
siendo la afinidad realmente observada del único TID actual.
La salida `SystemAffinityMask` ahora refleja la **instantánea
del cpuset permitido al inicio** del sandbox (grupo 0), y no
encoge simplemente porque el invitado haya fijado el proceso a
un único procesador. Si la instantánea contiene múltiples grupos,
el API heredado se rechaza como no representable. Esto tampoco
afirma que el cpuset al inicio sea la topología física completa
de Windows o que se haya implementado el scheduler multicgrupo.

Se añaden tests `tests/process_affinity_mask_test.c` y
`tests/pe/process_affinity_mask_fixture.c`: transición
verificada por `sched_getaffinity`, separación estable de
máscara de proceso y máscara del sistema, permisos, handles
erróneos, máscara vacía y denegación sin efectos parciales
al existir otro hilo Windows. Ambos se incorporan a la
CI Ubuntu/UBSan/Fedora RPM y al script de validación Fedora.

**Pendiente del problema 6:** modificar máscaras de proceso y
clases de prioridad de manera coherente para todos los hilos
activos, prioridades dinámicas/quantum, grupos Windows completos,
operaciones auténticas autorizadas sobre hilos de otro proceso
y pruebas entre varios grupos reales. Las funcionalidades
transversales al broker NT son una dependencia arquitectónica del
problema 3 y **no se implementan ad hoc**. No pasar al 5.

### Subtarea 6.5 — Transacciones de planificación multihilo (16-11-2026)

Se introduce un inventario acotado de TIDs anfitriones desde
`/proc/self/task`, aceptando únicamente el líder de proceso y los
TIDs de hilos guest cuyo objeto canónico esté vivo y registrado.
Un hilo Linux ajeno a esta tabla (p. ej. una librería auxiliar que
crea su propio pthread) hace **fallar la operación sin cambios**.
Se mantiene `wait_lock` durante la captura, los cambios y su
verificación; `CreateThread` y la finalización de un hilo controlado
se serializan en ese mismo mutex. Se comprueba de nuevo el conjunto
de TIDs antes de aceptar el resultado. Esto no es una parada
asíncrona de todos los CPUs ni hace atómica una syscall Linux
que no lo es. No se usa el HANDLE Windows como TID Linux.

**SetProcessAffinityMask multihilo:** admite varios hilos guest
controlados, incluidas las creaciones suspendidas, y modifica cada
TID con `sched_setaffinity`, previa lectura de sus afinidades
reales y posterior comprobación exacta. Cualquier error o intersección
parcial inicia un rollback de todos los TIDs ya modificados.
Los hilos extraños, grupos no representables, cambios inesperados
de tareas o permisos insuficientes se rechazan. `GetProcessAffinityMask`
informa ahora de la **política de afinidad del proceso** guardada
separadamente, no de la afinidad del pthread llamador: una afinidad
individual más estrecha no cambia la máscara del proceso.
`SystemAffinityMask` sigue limitado a la instantánea del cpuset
permitido al sandbox. Las modificaciones al cpuset realizadas desde
fuera del runtime no pueden reflejarse automáticamente.

**SetPriorityClass multihilo:** puede aplicar cambios de `nice`
verificados a todos los TIDs guest controlados únicamente si los
permisos efectivos del kernel y `RLIMIT_NICE` permiten tanto el
avance como la eventual restauración. En particular, bajar
prioridad (`nice` mayor) en un proceso Linux no privilegiado puede
ser irreversible: la operación **se rechaza antes de cambiar ningún
TID**, preservando la garantía de no dejar una clase intermedia.
Los cambios aprobados guardan cada `nice` anterior, verifican los
valores pedidos y aplican rollback si falla un hilo. La consulta de
clase y los niveles relativos de hilos conservan su mapeo estático.
Esto no representa cambios Windows dinámicos, MMCSS, quantum,
REALTIME ni atomicidad exacta del instante de cada TID; la semántica
positiva multihilo requiere privilegios Linux realmente disponibles.

**Validación:** se añade `tests/process_priority_multithread_test.c` para comprobar ambos TID antes/después, conservar prioridades relativas y diferenciar explícitamente éxito condicionado por permisos de Linux frente a `SKIP` de la ruta positiva con rechazo sin efectos. Se amplían
`tests/process_affinity_mask_test.c` y
`tests/pe/process_affinity_mask_fixture.c` para demostrar cambios
reales del conjunto de CPUs en un hilo guest adicional suspendido,
verificación del TID, reversión a la política inicial y lectura
de ambas máscaras Win32. Los tests originales de clases de prioridad
siguen demostrando que la solicitud multihilo se niega sin privilegios
de rollback. No se declara soporte general de procesos/hilos externos.

**Pendiente en el punto 6:** comprobar éxito de clases multihilo
en anfitrión con `CAP_SYS_NICE`/RLIMIT apropiado, manejar cambios
del cpuset en caliente y políticas de grupos Windows más completas,
boosts/quantum, broker autenticado entre procesos y comparación
diferencial en Windows/Fedora físico. No se avanza al punto 5.
