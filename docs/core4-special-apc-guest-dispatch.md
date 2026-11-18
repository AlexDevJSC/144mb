# Componente 4 — problema 5: despachador Win64 de APC especial en puntos seguros

**18-11-2026 · Perfil implementado: retorno de importación PE bajo frontera de confianza.**

Esta entrega introduce por primera vez una ruta **Win32 pública acotada**
de APC especial sobre hilos ejecutados por el runtime Linux. No equivale
a entregar APC en cualquier instrucción del PE como hace Windows. El
contrato opt-in solo puede ejecutarse cuando el cargador PE activó la
puerta Win64 de importaciones y registró intervalos ejecutables que no
puede modificar un PE invitado.

## Separación de colas y permisos

- `QueueUserAPC2(callback, thread, data, flags=1)` y
  `NtQueueApcThreadEx(thread, reserve=1, callback, ...)` admiten
  APC especiales **solo** para hilos del proceso invitado actual,
  con `THREAD_SET_CONTEXT`, TEB activo e intervalo ejecutable PE
  conocido. Un HANDLE terminado, ajeno o de tipo incorrecto mantiene
  sus errores NT reales. Una rutina de un módulo dinámico sin grant
  seguro, de código JIT o de un puntero ELF se rechaza con
  `NOT_SUPPORTED`; otros flags/reservas siguen rechazados.
- La cola especial es distinta de la APC normal. `NtTestAlert` y
  esperas alertables consumen exclusivamente las APC normales;
  la especial se procesa en los retornos seguros de imports Win64.
- Límite de 64 entradas por cola y por hilo. Las entradas especiales
  se limpian al terminar el hilo; no queda un callback vivo por
  reutilización de un HANDLE.

## Punto de entrega y ABI

`nt_guest_boundary_import_leave` finaliza la importación y retira la
última entrada en su stack privado de retornos ANTES de llamar al
dispatcher `nt_runtime_special_apc_checkpoint`. La asamblea
`guest_boundary_import.S` sigue reteniendo los registros GPR y
XMM0..15 del import que estaba retornando y entrega el valor de retorno
original después de ejecutar la APC. El dispatcher:

1. Comprueba el contrato **local** de guest-entry activo, TEB
   presente, cero niveles de importación host, CET sin SHSTK
   incompatible y ausencia de transferencia no local/SEH envenenada.
   Cuando existe una sesión de supervisor, exige además fase
   `GUEST` acreditada; **sin supervisor**, el shared phase es
   `UNREGISTERED` por diseño y no se finge otra fase para `ptrace`.
2. Extrae como máximo 8 APC de la cola **del mismo hilo** bajo
   `wait_lock`, y libera siempre el mutex antes de ejecutar la rutina
   Win64 real del PE.
3. Impide reentrada cuando el propio callback llama a imports,
   `QueueUserAPC2` o a funciones que vuelven a pasar por la puerta.
   Las APC sobrantes esperan al siguiente punto de entrega.
4. Si una excepción Windows lleva a transferencia de pila no local,
   el runtime existente marca el límite como comprometido para
   suspensiones asíncronas: nunca inventa un frame de host o
   reanuda un tracee `ptrace` con la frontera falsa.

**Importante sobre SEH:** las excepciones software lanzadas mediante
`RaiseException` dentro de una APC usan el mismo despachador VEH/SEH,
`RtlCaptureContext` y `RtlRestoreContext` existente del PE. Se
prueban **por separado** un VEH que devuelve `CONTINUE_EXECUTION` y
una recuperación de frame SEH que hace `EXECUTE_HANDLER`.
NO se capturan por esta entrega señales `SIGSEGV/SIGILL/SIGFPE` de
Linux ni se implementan SEH universal, `__try/__except` en todo
callback, unwinding a través de cualquier puente de importaciones,
CET SHSTK o bibliotecas ELF. Cualquier pérdida de evidencia de retorno
es una denegación; no se introducen manejadores de señales asíncronos.

## Cobertura PE de la entrega

```sh
make -j2 check-pe-apc
make -j2 check-guest-boundary
make -j2 check
make -j2 check-pe
```

`tests/pe/apc_fixture.c` conserva las pruebas anteriores de APC
normales, `NtTestAlert`, `NtDelayExecution`, ALERT y cuatro hilos
ordinarios. Ahora prueba además:

- Dos APC especiales a la propia thread, invocadas desde imports Win32
  **sin** entrar en espera alertable ni consumir APC normales.
- Entrega de dos callbacks especiales heterogéneos en orden FIFO a
  un hilo PE secundario, que usa `Sleep` **no alertable**, con prueba
  de TID y estado final.
- Una excepción software `RaiseException` dentro del callback
  especial, manejada con VEH `EXCEPTION_CONTINUE_EXECUTION`.
- **Un segundo PE32+ independiente** (`special_apc_seh_fixture.S`) que
  solicita una APC especial sobre el mismo hilo sin espera alertable,
  ejecuta `RaiseException` dentro de una rutina con unwind Win64
  `.pdata/.xdata` y la recupera mediante el filtro incondicional
  `__C_specific_handler` (SEH de frame). Si el import retorna por la
  ruta normal en vez del handler, falla. Un contador del propio PE
  confirma el único salto legítimo al handler.
- Comportamiento negativo de HANDLE/scope protegido; variante sin
  cargador PE mantiene rechazo de especial.

## Límites, fase y próximos trabajos

No se promete una APC especial *universal*: un hilo con un bucle PE
sin imports (o atrapado para siempre en una espera no alertable
interna al host) no alcanza ningún punto de entrega. No se inyecta
`RIP/RSP` arbitrario por señales ni se invoca el supervisor
experimental sobre PE desconocidos. Las rutas de SEH no locales aún
precisan un trampolín propio y desenrollado de frames para mantener
compatibilidad completa con Windows. Esto depende de contexto AMD64
integral y del objetivo arquitectónico 6 (SEH/VEH), que no se
declara cerrado por esta prueba.

La PR #178 de cancelación de RPC del broker sigue independiente:
la entrega especial cooperativa no necesita sus cambios para
funcionar, y no puede sortear la autenticación del broker.

**Madurez componente 4, criterio FileSystem 4: 96 % → 96 % (+0 pp)**
hasta acreditar un contrato de APC especial general con ejecución
independiente de los puntos de importación y cobertura de excepción
equivalente a Windows. No usar un fixture favorable como sustituto
de Fedora KDE físico con Bubblewrap/SELinux ni de Windows diferencial.
