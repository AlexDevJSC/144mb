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
señal POSIX de tiempo real (prefiere SIGRTMIN+4 y busca otro número
disponible si ya está ocupado) **solo si ningún componente host lo había
registrado**, y asigna a cada hilo invitado su propia pila de
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
espera alertable. Si el anfitrión ha ocupado la señal preferida,
el runtime busca otra libre hasta SIGRTMAX sin sustituir SIG_IGN ni
manejadores ajenos. Si no queda ninguna, conserva solo el modo cooperativo.
La selección no es una reserva atómica frente a otro código host que
registre manejadores simultáneamente y no garantiza señales desbloqueadas.

## Recuperación limitada de fronteras VEH/SEH

Se implementa una reconciliación **estricta por procedencia**:
cuando SIGILL/SIGFPE/SIGSEGV proviene de RIP PE, sin import
ELF abierto, el runtime marca temporalmente HOST durante el
dispatcher VEH/SEH y autoriza volver a GUEST **solo después** de
que el Win64 handler seleccione un RIP PE válido. Esto permite
recuperar varios fallos hardware sucesivos sin envenenar falsamente
la frontera.

Para `RaiseException`, la única excepción recuperable es la
entrada **naked** del import Win64 auditado, con un solo slot sintético
de retorno, validado contra el manifiesto de código del cargador.
No cruza destructores, mutex ni cleanup del host: solo retira ese
slot y reautoriza tras seleccionar el RIP PE. Las llamadas ELF
anidadas y los callbacks que abandonan importaciones generales
continúan denegados. El test de frontera verifica que un segundo
slot impide esta reconciliación.

**Esto NO es un desenrollado PE↔ELF arbitrario.** Se evita cruzar
frames host sin contrato de limpieza; no se inventa una tabla `.pdata`
para ELF ni se promete invocar `pthread_cleanup` ajeno.

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

## Corrección del puente nativo y XSTATE (18 de noviembre)

Los runs especializados sobre CI Linux mostraron que la presencia del
componente PKRU en el XSAVE del kernel no implica que el programa haya
cambiado sus permisos. El helper revisado
`src/runtime/guest_xstate_policy.h` usa el descriptor CPUID XSTATE
subleaf 9 y RDPKRU para aceptar **solo** el PKRU guardado que coincida
con el PKRU corriente de ese mismo hilo. Se comprueba de nuevo tras el
callback. Nunca ejecutamos XRSTOR con la máscara PKRU desde la pila
anfitriona ni aceptamos un cambio de permisos del callback.
La CI especializada de
[44316321810](https://github.com/AlexDevJSC/144mb/actions/runs/44316321810)
acreditó que las pruebas de fallos CPU reales, entrega de señal
asíncrona en bucle sin imports y supervisor terminaron con éxito.

El PE independiente con `NT_TEST_ASYNC_SEH` mostró un segundo límite
real: `__C_specific_handler` se llamaba desde el despachador ELF a
través del thunk de importación Win64, por lo que el retorno sintético
apuntaba al propio host. La nueva ruta
`verified_c_specific_import_thunk` verifica las instrucciones
`FF 25 disp32` dentro del alcance ejecutable PE, la dirección de IAT
validada y el destino exacto del export nativo
`vcruntime140.dll!__C_specific_handler`. **Únicamente ese handler
resource-free** se invoca directamente desde el despachador, sin
fabricar un desenrollado de un frame ELF. La
[CI 44317135537](https://github.com/AlexDevJSC/144mb/actions/runs/44317135537)
demostró la recuperación SEH de callback APC bajo ese contrato acotado.
En ese mismo run falló una prueba previa de RtlUnwind(Ex), por lo que
no se declara CI general verde. Se incorporó una reconciliación
específica de los imports naked y auditados `RtlUnwind(Ex)`,
manteniendo el rechazo de imports y callbacks con recursos host.
Exigir nueva prueba real antes de aceptar ese último cambio.

Las trazas instrumentadas de la regresión de hardware se compilan solo
con `NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS`; no se activan en el runtime
distribuido. No se relaja el contrato de señales ajenas a invitado,
direcciones dinámicas sin registrar ni cambios PKRU.

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
