# Core4 — autenticación parental de APC, ciclo de vida y contextos AMD64 acotados

Fecha: 23 de noviembre de 2026. Componente **4: procesos e hilos**.
Este documento corresponde a la PR #218 y se basa en los contratos
`thread_broker_ipc`, `guest_async_apc`, `thread_supervisor_control_server`
y `nt_supervisor_nt_bridge` existentes.

## Frontera de autoridad

La función pública `QueueUserAPC2(flags=1)`, la nativa
`NtQueueApcThreadEx(reserve=1)` y la extensión
`NtNativeQueueSpecialApc` comparten `queue_apc_common`.
Cuando el hijo PE mantiene una sesión autenticada con el padre:

1. El runtime exige un HANDLE local con `THREAD_SET_CONTEXT` y callback
   en el inventario de código ejecutable que el cargador fijó.
2. **Fuera de `wait_lock`**, y bajo el mismo `supervisor_gate_lock`
   que protege el cierre de HANDLE, obtiene una capacidad de
   `SUP_OPEN` autenticada. La respuesta incluye PID/TID/generación.
   Revalida la identidad del HANDLE tras el RPC para evitar reutilización.
3. El nuevo `SUP_APC_AUTHORIZE` transporta el callback y la generación
   exacta a través del socket AF_UNIX SOCK_SEQPACKET privado. Se exige
   `SCM_CREDENTIALS` en el mensaje, secreto de sesión, pidfd del hijo
   directo, capacidad `THREAD_SET_CONTEXT` viva y TEB del hilo.
4. El padre compara la dirección con el inventario inmutable de rangos
   de código PE y DLL iniciales. No acepta stack, Linux TID/PID o un
   supuesto rango de código aportados por el PE.
5. Tras el ACK, el runtime inserta el trabajo en la cola canónica del
   hilo. La ejecución continúa utilizando el mecanismo **existente**
   del propio hilo PE: import-return o señal dirigida a pthread con
   pila aislada. El padre **no ejecuta ni inyecta** esa APC con ptrace.
6. Inmediatamente antes del callback y al volver de él, el hilo
   vuelve a comprobar su capacidad autenticada, generación y código.
   Si pierde la autorización antes de comenzar, no ejecuta el callback.
   Si pierde el ACK de retorno, no publica `COMPLETED` y registra
   `ABORTED` (resultado ambiguo).

Si no existe broker en un ejecutable de pruebas autónomo, se mantiene
el comportamiento local histórico, sin fingir autorización del padre.
El pseudo-HANDLE de hilo propio conserva su comprobación local TEB;
aún no está instrumentado como lease de supervisor. La nueva operación
RPC **no es una puerta de PTRACE**; no concede `SUP_SUSPEND` ni
`SUP_SET_CONTEXT` al invitado.

**Límite importante:** estas comprobaciones autentican la identidad y
el código en cada cruce, pero NO son un registro independiente de
START/DONE realizado por el padre. La comprobación real de ejecución
está en la cola del hilo invitado y su recibo local; no debe
confundirse con una certificación de CPU obtenida por ptrace.

## Ciclo de vida y cancelación

Para la extensión propia de NTNative (no ABI nativa de Windows):

- `NtNativeQueueSpecialApc` entrega un ticket monotónico con
  `THREAD_SET_CONTEXT`, ligado al objeto canónico del hilo.
- `NtNativeQuerySpecialApc` exige el mismo derecho y devuelve:
  `1 PENDING`, `2 RUNNING`, `3 COMPLETED`,
  `4 CANCELLED` antes de comenzar,
  `5 COMPLETED_AFTER_REQUEST` y `6 ABORTED`.
  Guarda hasta 64 estados terminales por hilo. Un recibo expirado
  devuelve `STATUS_NOT_FOUND`.
- `NtNativeCancelSpecialApc` extrae de forma atómica una APC pendiente
  y devuelve `STATUS_SUCCESS`; si está ejecutándose, registra la
  solicitud y devuelve **`STATUS_PENDING`**, no éxito.
- `NtNativeSpecialApcCancelRequested` deja que el callback activo
  consulte voluntariamente esa solicitud. Debe liberar sus propios
  recursos y retornar normalmente; entonces queda registrado como
  `COMPLETED_AFTER_REQUEST`.
- Si termina el hilo durante el callback o falla la autorización,
  la ruta no anuncia una finalización verificada.

No existe un mecanismo seguro de `pthread_cancel`, `SIGKILL` de
un TID, restauración forzada de pila Win64 arbitraria o reversión de
efectos que el callback haya producido. **No está implementada la
cancelación forzosa segura de un callback no cooperativo.**

## Grupos AMD64

El supervisor padre mantiene su regla de hijo directo autenticado,
hilo en fase GUEST verificada, `PTRACE_GETREGSET`,
`PTRACE_SETREGSET`, lectura completa y rollback, o cuarentena pidfd
cuando el rollback no puede demostrarse.

| Grupo solicitado | Escritura disponible | Límite |
| --- | --- | --- |
| `CONTEXT_INTEGER` | Sí, GPR reales con readback/rollback | Sin cambio de RIP/RSP |
| `CONTEXT_FLOATING_POINT` | Sí, FXSAVE de 512 bytes con MXCSR | No equivale a todo XSAVE |
| `INTEGER | FLOATING_POINT` | Sí, transacción dual-regset, rollback de ambos | No es atomicidad de syscall única |
| `CONTEXT_CONTROL` | Sí, **solo indicadores aritméticos** CF/PF/AF/ZF/SF/OF de RFLAGS | RIP/RSP/RBP, selectores, IF/TF y bits privilegiados: denegados |
| `CONTEXT_DEBUG_REGISTERS` | Lectura de DR0–DR3/DR6/DR7 y escritura de **direcciones inactivas** DR0–DR3 | Solo rangos PE fijados; activación DR7, DR6 y watchpoints: denegados |
| `CONTEXT_SEGMENTS` | Lectura existente | Escritura FS/GS/segmentos denegada |
| `CONTEXT_XSTATE` | El supervisor de laboratorio ya captura XSAVE de manera privada para restauración de su propia APC | Falta ABI `CONTEXT_EX`/buffer variable, metadatos por CPU, protocolo autenticado específico y validación PKRU/AVX/AMX |

La API Win64 actual utiliza `nt_context64` con 512 bytes FXSAVE,
y **no implementa la representación variable CONTEXT_EX de Windows**.
Devolver éxito para `CONTEXT_XSTATE` a partir del array fijo de
vectores existente sería incorrecto. La pila, el estado de GS y las
señales del anfitrión no se modifican para disimular ese límite.

## Pruebas y criterios

```sh
make -j2 check-pe-apc check-pe-special-apc-lifecycle
make -j2 check-runtime-supervisor-launch check-runtime-supervisor-win32
make -j2 check-supervisor-control-ipc check-supervisor-mixed-rollback
make -j2 check
```

Se incluyen un PE freestanding que cancela antes de iniciar el hilo,
otro que solicita cancelación cuando el callback ya empezó y espera el
retorno voluntario, observaciones de estados terminales, autorización
negativa con HANDLE de consulta, y replay de generación inválida.
La prueba de supervisor para contextos añade cambios de RFLAGS y de
un DR0 inactivo, lecturas reales posteriores, restauración y rechazos
de RIP/RSP/IF, DR7 y dirección host ajena.

El `make check` **no es por sí solo evidencia** de aceptación en
Fedora 45 físico con SELinux enforcing, Bubblewrap sin SKIP,
grupos CPU reales >64 ni programas de terceros. Exigir además CI de
Linux, Windows diferencial pertinente y supervisor experimental antes
de comunicar un contrato como validado.

### Qué impide declarar «solo queda Fedora 45»

Quedan por implementar, demostrar y comparar con Windows:

- Estado START/DONE/CANCEL gestionado independientemente en el
  supervisor padre, con ledger autenticado y confirmaciones de ejecución
  inequívocas, no solo revalidación de leases.
- Cancelación activa **no cooperativa**: solo podría admitirse
  con un protocolo de recursos/stack/unwind que no permita dejar mutex
  del host o side effects invitados corruptos.
- Escritura general validada y reversible de CONTROL (RIP/RSP),
  SEGMENTS (FS/GS), DEBUG (DR7 activo) y XSTATE extendido
  (CONTEXT_EX), con pruebas de errores, derechos y fallos.
- La relación entre la cola pública del hijo y el supervisor ptrace
  al ejecutar callbacks fuera del camino de import-return; hoy el
  padre **autoriza**, pero no despacha esos callbacks.
- Pruebas negativas y diferenciales adicionales y aceptación física.

Por ello el componente 4 mantiene por ahora la estimación documental
**96 % → 96 % (+0 puntos)**: los cambios añaden garantías medibles,
pero no autorizan declarar equivalencia NT universal ni que únicamente
falta la comprobación en Fedora 45.
