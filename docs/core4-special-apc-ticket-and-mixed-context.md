# Core4: cancelación de APC pendientes y contexto AMD64 multirregset

19-11-2026. Componente arquitectónico **4 — Procesos e hilos**;
problema interno **5 — APC avanzadas**. Esta entrega es **incremental**.
No se habilita `ptrace` para ejecutar solicitudes Win32 en hilos Linux
de un proceso externo sin una autoridad de broker verificable.

## Contrato de cancelación pública limitado

Las exportaciones **propias de NTNative**, presentes solamente en el
runtime del proyecto y **no en Windows NT auténtico**, son:

- `NtNativeQueueSpecialApc(HANDLE, APC_NATIVE, ctx, arg1, arg2, uint64_t *ticket)`:
  encola una APC especial de hilo del proceso propio y devuelve un
  identificador no nulo y no reutilizable para esa sesión si resulta
  exitoso. Comprueba exactamente la misma propiedad de HANDLE,
  `THREAD_SET_CONTEXT`, TEB, código PE autorizado y capacidad de cola
  que `NtQueueApcThreadEx(reserve=1)`. En fallo no escribe el ticket.
- `NtNativeCancelSpecialApc(HANDLE, uint64_t ticket)`: bajo el mismo
  mutex `wait_lock`, exige de nuevo un HANDLE con
  `THREAD_SET_CONTEXT`, encuentra la solicitud **todavía encolada**
  por su identificador y la retira antes de la fase de ejecución.
  `STATUS_SUCCESS` significa que el callback no fue retirado de la
  cola para ejecución. `STATUS_NOT_FOUND` significa que no estaba
  pendiente, **no** que un callback anterior se haya cancelado
  retroactivamente. El ticket nunca confiere derechos y no es PID,
  TID Linux, FD ni capacidad del supervisor.
- El ticket `0` es inválido. El contador no se reutiliza al cerrar
  aliases de HANDLE ni después de reciclar un slot; si agotase 64 bits,
  deja de emitir tickets en vez de colisionar. El origen de la señal
  puede quedar ya en vuelo cuando se retira la APC: el despachador
  vuelve a comprobar la cola y restaura el contexto sin callback.
- No se cancela un callback **ya extraído de la cola**. Para esa
  cancelación activa harían falta suspensión, cancelación cooperativa
  y prueba de estado de pila que este contrato **no posee**. Tampoco
  se inventa una API Windows llamada `CancelSpecialUserAPC`.

## Supervisor y combinación de grupos AMD64

El broker ya autentica proceso/hilo propietario, generación y derechos
y restringe GET/SET a un hilo PE propio suspendido por `ptrace` bajo
un supervisor padre. El nuevo perfil añade escritura simultánea
`CONTEXT_AMD64 | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT`
al supervisor y su compuerta RPC. Es una transacción de dos conjuntos
de registros Linux, **no todos los grupos CONTEXT AMD64**:

1. Captura registros GPR del hilo PE detenido, y 512 bytes
   `NT_PRFPREG` (FXSAVE), validando MXCSR.
2. Escribe `NT_PRFPREG` y `NT_PRSTATUS` bajo propiedad del
   supervisor. No ejecuta `PTRACE_CONT` entre ambas operaciones.
3. Lee de nuevo y compara **ambos regsets completos** antes de
   confirmar éxito.
4. Si una escritura o lectura es ambigua, intenta restaurar **ambas**
   imágenes y exige una lectura posterior exacta de ambas. Si la
   restauración no es verificable, marca `compromised`; el proceso
   hijo anclado a `pidfd` se pone en cuarentena y no se reanuda
   ni desadjunta con un estado de CPU desconocido.
5. El test `runtime-supervisor-win32-test` cambia RAX y MXCSR en
   un mismo RPC real, verifica lectura y restauración, y exige
   `STATUS_NOT_SUPPORTED` para CONTROL y DEBUG combinados.

**Límites:** `CONTEXT_CONTROL` escribe RIP/RSP y pila, con riesgos
de frame y propiedad de locks; `CONTEXT_SEGMENTS` incluye FS/GS
del host; `CONTEXT_DEBUG_REGISTERS` requiere un contrato de slots
y permisos de depuración; `CONTEXT_XSTATE` necesita buffer variable,
layout XSAVE, características CPU y validaciones para PKRU/AMX.
El runtime **no** autoriza su escritura todavía. No se interpreta
la representación limitada `nt_context64` como una API completa para
estos grupos.

## Verificación

```sh
make -j2 check-pe-cancel-special-apc check-pe-apc
make -j2 check-runtime-supervisor-win32
make -j2 check check-pe
```

La prueba de cancelación es un PE32+ freestanding que inicia un hilo
y lo mantiene en una espera host no alertable. Encola un callback
cancellable, comprueba el rechazo de un HANDLE solo-consulta, retira
el ticket y verifica que un ticket diferente sí se entrega en el
hilo correcto una vez finalizada la espera. Incluye token caducado,
HANDLE cerrado, doble cancelación e invalidación de parámetros.

**No confundir este resultado con una conexión pública completa
a `nt_supervisor_invoke_trusted_guest_apc_guarded`.** Ese prototipo
recibe capacidades del supervisor y señales de cancelación propias
de su sesión de laboratorio, no de un HANDLE Win32 genérico.
Invocarlo directamente desde `QueueUserAPC2` permitiría alterar
hilos que no demuestran la misma identidad, permisos y propiedad de
stacks. La integración de esa ruta y el soporte de grupos CONTEXT
restantes **siguen abiertos**; ni la validación del propio kernel
ni la CI son evidencia de compatibilidad universal Windows.

Componente 4: **96 % → 96 % (+0 pp)** mientras no haya validación
real diferencial y cierre de nuevas garantías.
