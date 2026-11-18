# Core 4 / problema 5 — APC especial asíncrona en bucles PE sin imports

18-11-2026 · implementación experimental **apilada sobre #186 y #184**.
No supone aceptación universal de todos los programas Windows.

## Contrato de ejecución

Hasta ahora `QueueUserAPC2(...,1)` y
`NtQueueApcThreadEx(..., reserve=1)` entregaban APC especiales solo
en retornos de importaciones Win64. El nuevo puente
`src/runtime/guest_async_apc.c` también permite entregarlas en
un **RIP PE en ejecución continua sin importaciones ni esperas**.

El cargador confiable fija los intervalos `.text` del PE y DLL de inicio
antes de ejecutar código invitado. El runtime reserva un número de
señal POSIX de tiempo real (SIGRTMIN+4) **solo si ningún componente host
lo había registrado**, y asigna a cada hilo invitado su propia pila de
despacho con página guardia; mantiene independiente la pila que usan
las señales de fallo de CPU de #186. No se expone a Win32 ninguna API
de señales, dirección `pthread_t`, número de TID Linux o puntero de
stack de anfitrión.

Al encolar una APC especial en un HANDLE local con
`THREAD_SET_CONTEXT`, la cola normal permanece separada y el runtime
avisa mediante `pthread_kill` **solo** al `pthread_t` que conserva la
tabla canónica de hilos propios. Se impide la entrega a hilos ajenos,
al proceso hermano y a memoria ejecutable JIT no autorizada.

El manejador POSIX:

1. Verifica `SI_TKILL` del mismo PID, la fase GUEST que no está
   en un import ELF, un `RIP` perteneciente al rango de código PE
   inmutable, una APC **realmente pendiente** y ausencia de ejecución
   anidada del mismo despachador.
2. Copia `ucontext`, GPR, FXSAVE/XSAVE del kernel a TLS y reescribe
   solo el contexto de retorno de señal para entrar en una pila de
   despacho privada, **sin llamar a APC, mutex ni malloc dentro del
   manejador de señal**.
3. Después de `rt_sigreturn`, el runtime consume la cola especial
   ya existente, libera su mutex antes de ejecutar el callback Win64 y
   limita el lote a ocho APC.
4. Reanuda en el **RIP/RSP y registros originales del bucle PE**
   mediante `nt_restore_software_context`. Restaura previamente
   únicamente extensiones XSAVE SIMD conocidas. Nunca restaura PKRU,
   AMX o estados desconocidos en una pila del anfitrión.

El handler puede ignorar la señal si llega en fase HOST: la cola
permanece pendiente para el antiguo retorno de importación. Los
callbacks ordinarios permanecen en su cola y NO se ejecutan sin una
espera alertable. Si el anfitrión ya ha ocupado la señal reservada,
el runtime no roba ese manejador y conserva el modo cooperativo.

## Excepciones dentro de la nueva pila APC

`prepare_software_stack()` reconoce la pila privada del callback y
permite que `RaiseException` busque handlers VEH/SEH en frames
**PE que estén realmente en ella**, sin fingir que el despacho SYSV
es una función Windows con `.pdata`. El puente CPU de #186 cuenta
con su propia pila separada si el callback dispara SIGSEGV/SIGILL.

Esto **no es un desenrollado PE/ELF universal**. Si una excepción
escapa de un frame PE hacia una importación ELF que conserva
un mutex, heap lock, callback nativo o recurso no registrado, el
runtime debe rechazarla. No hay garantía genérica de ejecutar
destructores arbitrarios de C++ o callbacks de `pthread_cleanup`
que no fueron instrumentados. La solución arquitectónica requiere
aislamiento continuo de stacks y/o recursos host registrados con
compensaciones verificables, no saltos de pila a ciegas.

## Evidencia y aceptación

```sh
make -j2 check-guest-async-apc-signal
make -j2 check-pe-async-special-apc
make -j2 check-pe-apc check-pe-seh
make -j2 check-guest-boundary
```

La prueba host dirigida ejecuta un bucle sin syscalls ni imports,
dispara una señal kernel real, verifica que el dispatcher utiliza
una pila distinta y **reanuda el contexto mediante la misma rutina
AMD64 assembly que el runtime PE**.

`tests/pe/async_special_apc_loop_fixture.c` genera un programa
PE32+ freestanding. Un trabajador ejecuta un bucle PE sin llamadas
a Windows ni esperas. Otro hilo le encola primero un APC normal
(que debe quedar pendiente), luego una APC especial
(`QueueUserAPC2`, `flags=1`). El callback especial verifica
el TID de destino, finaliza el bucle y devuelve el control al
hilo original. Cualquier entrega solo a import-return o en
hilo equivocado falla.

## Pendientes obligatorios

- No hay garantía frente a ejecución maliciosa que cambie su
  máscara de señales Linux directamente, o registre/modifique
  handlers sin pasar por imports. No se promete sandbox del
  mismo espacio de direcciones.
- No hay paridad con SHSTK/CET activo, ptrace externo, todos los
  formatos XSAVE, APC nested que desenrollen frames ELF o módulos
  DLL cargados después del inicio sin una autorización firme.
- Una APC entregada durante un import host queda para su retorno;
  una importación que nunca retorna **no puede ejecutarla** con
  seguridad mientras sostenga mutex o recursos del anfitrión.
- El desenrollado PE↔ELF arbitrario sigue **sin implementarse**:
  esta entrega aporta una ruta que evita cruzar esos frames al
  ejecutar callbacks asíncronos en una pila propia.
- Se exige CI PE real y repetición con sanitizers y Fedora KDE físico.
  No confundir la prueba host con validación de un programa Windows.
- Componente 4: **96 % → 96 % (+0 puntos)** hasta pruebas diferenciales
  y solución arquitectónica de los contratos universales abiertos.
