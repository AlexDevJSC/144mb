# Problema 5 — Cancelación de RPC autenticada mientras está en curso

Fecha: 18-11-2026. El código revisado es `src/runtime/thread_broker_ipc.c`.
Se desarrolla como continuación independiente de la PR #177 y sobre el
`main` en que las PR #172 y #175 ya estaban fusionadas.

## Fallo observable

El runtime serializa las llamadas cliente→broker por `client_lock`.
Antes de este cambio, `nt_broker_client_detach()` esperaba el mismo
mutex durante todo `receive_exact_reply(..., 4000)`. Si el broker
externo mantenía el canal abierto sin responder, el hilo que deseaba
cancelar podía quedarse bloqueado hasta cuatro segundos.
Además, no había primitiva privada de cancelación que invalidase un
ACK tardío durante la recepción de una operación de control.

## Contrato nuevo (solo runtime anfitrión)

`nt_broker_client_abort_inflight(void)` admite ejecución desde otro
pthread de confianza sin esperar a `client_lock`. Un segundo mutex
`client_abort_lock` serializa la vigencia del descriptor del socket,
el estado `client_rpc_inflight` y el indicador
`client_rpc_aborted`. Orden de locks: `client_lock` antes de
`client_abort_lock`, **nunca al revés**.

- El propietario del RPC publica el estado `inflight` antes de enviar
  el paquete. La cancelación marca `aborted` y usa
  `shutdown(SHUT_RDWR)` sobre el socket aún protegido; jamás hace
  `close(fd)` mientras el otro hilo puede estar usando ese número.
- La ruta de lectura se desbloquea sin extender el timeout de 4 s.
  El propietario vuelve a tomar `client_abort_lock`, comprueba si hubo
  cancelación y rechaza cualquier reply, incluso uno que haya llegado
  simultáneamente. El cliente destruye la sesión ambigua y borra
  `client_secret`; una llamada posterior no puede aceptar un ACK viejo.
- `nt_broker_client_detach()` solicita el aborto **antes** de tomar
  `client_lock`. No destruye recursos arbitrarios del anfitrión ni
  interpreta TID o FD suministrados por un PE invitado.
- Una cancelación sin RPC pendiente devuelve `false`, sin apagar
  un socket válido. El punto de linealización es la retirada del estado
  `inflight`: una operación completada *antes* de esa retirada puede
  devolver éxito legítimo; una cancelación recibida *durante* el RPC
  no puede devolver éxito.
- Los argumentos de salida de operaciones de consulta y control no
  se actualizan cuando no hay ACK válido.

## Prueba positiva negativa reproducible

```sh
make -j2 check-broker-transport-failure
make -j2 check-supervisor-authenticated-ipc
make -j2 check-supervisor-control-ipc
make -j2 check check-pe
```

`tests/thread_broker_transport_failure_test.c` añadió dos casos
sobre procesos padre/hijo y dos pthreads en el hijo, con AF_UNIX
`SOCK_SEQPACKET`. El padre lee el RPC y **no responde ni cierra
el socket**, y avisa por un pipe de control de que la petición fue
recibida. La operación de consulta se cancela explícitamente; la de
suspensión se cancela mediante `nt_broker_client_detach()`.
Se exige salida del hijo en menos de 1500 ms, canal invalidado,
ausencia de una segunda operación válida y conservación exacta de
las salidas anteriores. Un fallo de la prueba no se convierte en SKIP.

Las suites especializadas del supervisor ejecutan este test. Debe
anotarse el identificador de CI y el estado del último commit antes
de darlo por validado.

## Cierre de ACK ambiguo de SET_CONTEXT en el broker padre

La ruta del **padre** se refuerza además de la cancelación del cliente:
si un `NT_BROKER_RPC_SUP_SET_CONTEXT` autenticado consigue escribir
y verificar un CONTEXT pero el socket ya presenta
`POLLRDHUP/POLLHUP/POLLERR` antes de responder, o si falla el envío
de la respuesta, el broker no entrega un falso éxito. Invoca
`nt_supervisor_control_server_abort_unacked_context` sobre la
identidad NT ya comprobada (TID + generación), marca el tracee como
`compromised` y envía `SIGKILL` **solo al `pidfd` del hijo directo**.
No permite `PTRACE_DETACH` con registros modificados sin ACK;
`PTRACE_O_EXITKILL` permanece armado durante la salida del trazador.

La regresión de integración usa un proceso Linux real con un hilo
guest registrado, su TEB/GS, un `PTRACE_INTERRUPT` real y una escritura
`PTRACE_SETREGSET` verificada. Un hook de compilación **solo para
tests** publica una barrera después de esa escritura y antes del ACK.
El proceso cliente la observa y cierra la sesión desde otro pthread.
La prueba exige muerte del hijo por `SIGKILL`, no una finalización
ordinaria ni un `PTRACE_DETACH`. El hook se excluye por completo de los
objetos de producción.

```sh
make -j2 check-supervisor-control-lost-ack
```

No se afirma que un `PTRACE_INTERRUPT` ya en curso se interrumpa
instantáneamente: esta garantía es una recuperación de un ACK perdido
**después** de que `SET_CONTEXT` haya sido aplicado y verificado.

## Contratos aún NO completados

1. **Cancelación instantánea de `ptrace` durante su ejecución.**
   El cliente cancela la espera RPC y el padre ahora pone en cuarentena
   un `SET_CONTEXT` aplicado si se pierde su ACK, pero una operación
   `PTRACE_INTERRUPT` puede continuar hasta su plazo acotado. Para
   abortarla inmediatamente faltan checkpoints de cancelación en el
   supervisor y una transición de suspensión/reanudación transaccional.
2. **APC especiales genéricas de Windows.** `QueueUserAPC2` special y
   `NtQueueApcThreadEx` special siguen rechazadas en runtime; no
   se inyecta código de PE arbitrario sin preservar pila, XSTATE,
   callbacks, locks, unwind, TEB y SEH.
3. **Recuperación SEH completa de una APC interrumpida.** Existe
   emulación SEH/VEH acotada con su propio ámbito arquitectónico
   (componente 6). No hay todavía una transición general del
   `ptrace` detenido hacia un frame de excepción NT para todos los
   códigos y todas las imágenes, ni ejecución segura en callbacks
   con importaciones o desplazamiento de pila no local.
4. **APC especiales multihilo y entre procesos.** PR #177 ensaya
   las APC especiales en PE controlado y prueba colas APC ordinarias
   por cuatro hilos; eso no acredita inyección especial general.
5. **Validación física Fedora KDE/Bubblewrap y Windows diferencial.**
   No se sustituye por un test Linux con runner de contenedor.

**Escala FileSystem 4 — componente 4: 96 % → 96 % (+0 puntos).**
Se resuelve un contrato de cancelación cliente→broker y se mantiene
la fase 4 abierta: faltan contratos públicos y aceptación hardware.
