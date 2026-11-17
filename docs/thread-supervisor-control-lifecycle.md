# Control Win32 de hilos: problemas 4 y 3 — pruebas experimentales

## Alcance y activación

Este cambio NO activa suspensión asíncrona indiscriminada en el lanzador
PE. Los procesos que usan `pe-isolated-run` mantienen el backend
cooperativo y **rechazan** las operaciones cuyo estado real no pueda
comprobarse. Sólo un anfitrión de pruebas confiable puede llamar
`nt_runtime_enable_supervisor_thread_control()`, antes de
`nt_runtime_run_fixture()`. Ninguna importación KERNEL32/NTDLL ni
mensaje del invitado puede activar `NT_BROKER_SERVER_ENABLE_THREAD_CONTROL`.

El proceso padre posee el broker FD8 y un `pidfd` fijo del hijo.
El protocolo privado **v4**, sobre `SOCK_SEQPACKET`, autentica en
**cada petición** `SCM_CREDENTIALS`, token aleatorio de sesión,
identidad del hijo y capability ligado a TID/generación. Los mensajes
de contexto tienen tamaño fijo, alineación 16 y nunca transfieren
punteros de memoria del anfitrión. Un `HANDLE` Win32 no es un PID
Linux.

## Estado por hilo

```text
UNREGISTERED / HOST / no trusted guest image
             │    sin parada autorizada
             ▼
GUEST verified (phase, RIP executable code, GS_BASE==TEB, ownership)
             │ PTRACE_SEIZE + PTRACE_INTERRUPT
             ▼
SUSPENDED(count=1, PTRACE_EVENT_STOP)
             │ suspensión anidada → count+1, hasta 127
             │ GetThreadContext (CONTROL / INTEGER / SEGMENTS)
             │ SetThreadContext (INTEGER sólo)
             │ ResumeThread con count>1 → decremento sin ejecución
             ▼
count=1: PTRACE_DETACH desde estado detenido
             ▼
RUNNING (señales conservadas; tracer liberado)
```

`src/runtime/thread_supervisor_control_server.c` mantiene un único
trazador por TID registrado (máximo 128), exclusivamente en el
**pthread de servidor que lo creó**. Nunca se reutiliza entre
procesos ni generaciones. La última reanudación llama
`PTRACE_DETACH` mientras el objetivo sigue detenido, en lugar de
`CONT` seguido de otro `INTERRUPT` sujeto a carreras con mutexes.

`src/runtime/threads.c` adapta las exportaciones públicas:

| API | Semántica verificada en anfitrión opt-in |
|---|---|
| `SuspendThread` / `NtSuspendThread` | Detención real sólo dentro de PE confiable, contador anidado y acceso THREAD_SUSPEND_RESUME |
| `ResumeThread` / `NtResumeThread` | Cuenta anterior auténtica; último descenso desadjunta el tracer |
| `GetThreadContext` / `NtGetContextThread` | Registros del kernel en estado detenido, grupos CONTROL, INTEGER, SEGMENTS |
| `SetThreadContext` / `NtSetContextThread` | Registros enteros, comprobados y restaurables; no RIP/RSP, segmentos, debug, FP ni XSTATE |
| `NtAlertResumeThread` | Conserva el bit de alerta en la ruta de reanudación |
| `QueueUserAPC2` special / `NtQueueApcThreadEx` special | **NO SOPORTADO**: no se convierte engañosamente en APC normal |
| APC normales | Mantienen las colas y el despacho cooperativo/alertable existentes |

Un `HANDLE` se comprueba antes de abrir un capability y después de
recibir la respuesta del broker. Ningún mutex `wait_lock` queda
adquirido mientras espera la respuesta IPC. El nuevo
`supervisor_gate_lock` serializa cambios del contador asíncrono con
`ResumeThread` y operaciones de contexto, sin bloquear la cola global
de objetos NT durante llamadas al kernel remoto.

Si el padre no concede explícitamente el permiso, si falta un
`pidfd`, si el TID no está registrado o si el RIP/GS no identifica
código invitado suspendible, el backend remoto **no informa éxito**.
El programa sigue con la ruta cooperativa anterior o recibe
`STATUS_NOT_SUPPORTED` / el error Win32 correspondiente.

## Señales, fallos y vida

Un `waitpid(__WALL)` que recibe un signal-delivery-stop en lugar del
`PTRACE_EVENT_STOP` solicitado guarda la señal en el estado del
trazador. `PTRACE_CONT` y `PTRACE_DETACH` la reinyectan
exactamente una vez; los eventos inesperados no se interpretan como
paradas seguras. La terminación del hilo invalida su registro de
generación.

Ante `UNREGISTER`, EOF, paquete autenticado inválido o cierre del
canal, el servidor limpia sus trazadores. Primero intenta una
interrupción limitada y `PTRACE_DETACH`. Si falla de forma que podría
quedar un hilo detenido indefinidamente, la respuesta de emergencia
se limita al **pidfd hijo exacto** (nunca `kill(tid)` sobre un entero
Linux posiblemente reciclado). Esto es una medida de último recurso,
**no garantía de recuperabilidad sin terminación del proceso**.

## Seguridad demostrada / ausente

Pruebas:
```sh
make check-supervisor-control-ipc
make check-runtime-supervisor-win32
make check-guest-boundary
make check
```

`supervisor_control_ipc_test` comprueba el backend padre con hijos
reales y TIDs propios: denegación sin flag, contraseña de sesión,
contextos GET/SET/GET, restore antes del resume, señal SIGUSR1
encolada durante el stop, rechazo de flags de CONTROL en SET,
recuento doble, rechazo HOST, revocación y 32 ciclos de
SEIZE/GETREGSET/DETACH. Un caso desconecta el canal mientras el
hilo está suspendido y exige que vuelva a progresar tras el cleanup.
Otro ejecuta código anónimo RW→RX (JIT): la fase puede ser GUEST
pero el `RIP` queda fuera del rango PE confiable, por lo que la
operación se rechaza. También se prueba un rango tipo DLL no
registrada.

`runtime_supervisor_win32_test` pasa por las **exportaciones Win32
reales**, crea `CreateThread` y usa un rango de instrucciones
controladas y un TEB/GS sintético, con permisos de supervisor
concedidos sólo por el proceso padre. Comprueba anidamiento,
contexto AMD64 real y su restitución, transición HOST,
denegación de APC especiales y dos procesos consecutivos.

La [prueba de supervisor PE32+ real](../tests/pe/supervisor_pe_thread_test.c)
sigue corroborando el stop real en código invitado. **No debe
interpretarse como un test end-to-end de estas exportaciones Win32
dentro de un juego comercial**; el nuevo test de exportaciones usa
código AMD64 controlado en el runtime y la prueba PE32+ no activa
por defecto el backend asíncrono público.

### Métrica y activación

No existe un denominador auditado que permita afirmar un 70 % o más de
**compatibilidad Windows general**. La ejecución positiva de
`SuspendThread`, `ResumeThread`, `GetThreadContext` y
`SetThreadContext` en un fixture opt-in demuestra ese subconjunto,
no el porcentaje de todos los comportamientos Windows posibles.
RIP/RSP control, debug regs, XSTATE de CONTEXT, APC especiales,
concurrencia arbitraria, reentrancia de señales e imágenes JIT y
DLL sin alcance seguro quedan **fuera**.

El problema 4 tampoco se declara resuelto para todas las señales del
kernel (SIGSTOP de grupo, signo de excepción, salida del supervisor en
mitad de una restauración, etc.). Debe completarse el protocolo de
errores antes de autorizar PE arbitrarios o certificar Fedora físico
Bubblewrap/SELinux.
