# Core 4 — siguiente fase de hardening (problemas 1–3 del plan operativo)

> **Estado:** cambios propuestos en rama separada. La ejecución de las
> pruebas y la aceptación en Fedora pertenecen al mantenedor. No declarar
> PASS, porcentaje de compatibilidad o activación de producción sin ellas.

## 1. Ciclo de vida del supervisor y protocolo

**Riesgo anterior:** el RPC genérico esperaba 1 segundo y podía recibir
una respuesta corta, larga, atrasada o inesperada sin revocar la sesión.
Un paquete tardío podía corresponder a la solicitud anterior. El RPC de
control v4 ya revocaba la sesión en varios casos, pero sus verificaciones
de longitud se efectuaban con `recv` y no detectaban explícitamente
`MSG_TRUNC` como contrato de `SOCK_SEQPACKET`.

**Cambio:** `receive_exact_reply` usa `recvmsg` y compara el tamaño
exacto y los flags `MSG_TRUNC|MSG_CTRUNC`. Tanto el RPC ordinario como
el de control usan esta operación. Cualquier respuesta ambigua, EOF,
fallo de envío o expiración del plazo cierra y revoca el canal; el
padre desadjunta a los hilos que todavía controla como parte del
tratamiento de EOF existente. Una denegación bien formada conserva
el socket reutilizable, sin escribir los parámetros de salida.

El plazo de respuesta es 4 segundos (stop del trazador hasta 1,5 s);
no es garantía de tiempo real. El contrato sigue sin autorizar
operaciones `ptrace` por defecto, incluso si el socket está adjunto.

**Prueba añadida:** `thread_broker_transport_failure_test.c` simula
un padre directo que emite paquetes sobredimensionados, denegación
seguida de corrupción, magic inválido, ACK tardío y `GET_CONTEXT`
incompleto. El cliente debe invalidar el canal y preservar outputs.

**Pendiente:** pruebas con perturbación masiva de señales y salida del
supervisor en cada punto intermedio; un protocolo con timeout no
convierte operaciones de mutación en transacciones recuperables.
La prueba del mismo broker con múltiples hilos PE es obligatoria
antes de habilitar suspensión genérica.

## 2. Frontera PE ↔ runtime: rango real de DLLs

El cargador conoce los mapeos de las DLL importadas durante
`pe_image_load`, antes de ejecutar el invitado. El padre recoge una
lista acotada y verificada de **sólo DLLs fijadas/pinned antes del
fork**, que no se pueden descargar durante la vida del invitado.
Los rangos deben corresponder a código PE cargado, no solaparse con
el ejecutable ni con otro rango, y caber en un máximo de 64 DLLs.

El manifest se copia al estado del proceso padre ANTES de crear su
servidor autenticado. `nt_supervisor_get_guest_context` vuelve a
comprobar los registros kernel: fase `GUEST` estable, RIP dentro del
ejecutable o de uno de los rangos de arranque, GS_BASE igual al TEB
registrado y RSP válido. La fase sola nunca autoriza el stop.

`LoadLibrary` después del fork, DLLs explícitas no pinned, imágenes
sin sección de código admitida y regiones JIT RW→RX **no se añaden**
a la lista. La descarga de una biblioteca que se hubiese registrado
como segura no está permitida: ese alcance sólo se concede a las DLL
pinned del loader inicial. No se introduce un mecanismo dinámico de
autorización basado en información escrita por el PE.

Se añaden pruebas de manifiesto inválido y su revocación al cambiar
el PE; el test de control demuestra autorización cuando el RIP queda
dentro de un rango adicional fijado por el padre y denegación para
regiones no reconocidas y un JIT real.

**Pendiente:** auditoría completa de callbacks TLS/DllMain/SEH/VEH
sobre cargas dinámicas, y protección host frente a código PE
arbitrario malicioso capaz de alterar memoria del propio proceso.
Una prueba con memoria compartida no constituye aislamiento de
seguridad por sí sola.

## 3. Windows AMD64 CONTEXT: grupo flotante de sólo lectura

`GetThreadContext` / `NtGetContextThread` sobre un hilo realmente
suspendido pueden solicitar ahora `CONTEXT_FLOATING_POINT` (0x8).
El supervisor usa `PTRACE_GETREGSET(NT_PRFPREG)` para recuperar
la imagen FXSAVE de 512 bytes del hilo real: x87, XMM0–XMM15 y
`MXCSR`. Se publica el `CONTEXT` al caller **sólo** si tanto
la validación de RIP/GS/TEB como el GETREGSET de FP tienen éxito;
un error no devuelve un contexto parcial.

La escritura con `SetThreadContext` / `NtSetContextThread` sigue
admitiendo **sólo registros enteros** en un stop supervisado.
Escrituras de FXSAVE, AVX/YMM, XSTATE, DEBUG, CONTROL/RIP/RSP y
segmentos siguen rechazadas. No afirmar soporte de AVX/XSAVE por
obtener la imagen FXSAVE clásica.

Las pruebas de runtime, broker IPC y PE32+ comprueban lectura de
`MXCSR`, que coincide con la posición de FXSAVE, y denegación de
escritura de FP y de lectura de registros de depuración.

## Verificación manual en Fedora

Ejecutar sin `sudo`:

```sh
make -j2 check-broker-transport-failure check-guest-boundary
make -j2 check-supervisor-control-ipc check-runtime-supervisor-win32
make -j2 check-pe-async-supervisor check-pe-thread-supervisor
make -j2 check check-pe

# Batería repetida; la suite de control ya realiza 32 ciclos por ejecución.
bash scripts/check-core4-manual-hardening.sh 100

# Objetivo orientativo >=10.000 ciclos de control (313 x 32).
bash scripts/check-core4-manual-hardening.sh 313

# Aceptación aislada real (no equivale a las pruebas de GitHub Actions).
bash scripts/fedora-thread-supervisor-acceptance.sh
cat build/supervisor-fedora-acceptance/result.txt
```

Si Bubblewrap devuelve `INCOMPLETE`, **no** clasificarlo como
`PASS`. Una tasa del 100 % de estas pruebas mide únicamente esos
casos, no el porcentaje de APIs de Windows completamente compatibles.

No habilitar `nt_runtime_enable_supervisor_thread_control()` en el
lanzador de aplicaciones arbitrarias como consecuencia de esta PR.
La autorización continúa siendo sólo del anfitrión controlado.


## Revisión de cierre: contratos adicionales, NO equivalentes a Windows completo

### Mutaciones de estado y vida de `HANDLE`

`TerminateThread` y `NtTerminateThread` sólo pueden consumir la
suspensión cooperativa de arranque; ahora rechazan si
`supervisor_async_suspended` es verdadero. El cierre de un HANDLE
adquiere el mismo `supervisor_gate_lock` que Suspend/Resume/Get/Set,
siempre antes de `wait_lock`. Esto impide que otro hilo cierre el
HANDLE mientras un RPC remoto vuelve con éxito y está a punto de
confirmar la cuenta local. Un duplicado mantiene vivo el objeto
canónico y puede completar un ResumeThread anidado: fixture real
`tests/runtime_supervisor_win32_test.c`.

Un intento de mutación remota cuya respuesta sea denegada o se pierda
**nunca inicia una suspensión cooperativa en esa misma llamada**.
Si el broker cierra el canal por timeout, EOF, respuesta malformada o
falta de sincronía, se mantiene el rechazo. Nada en este flujo
autoriza señalización de un Linux TID no registrado.

### CONTEXT: readback, reversión y último recurso

`nt_supervisor_nt_set_context` captura el PRSTATUS original,
escribe el grupo INTEGER, vuelve a leer PRSTATUS del kernel y exige
igualdad exacta de todo el registro observable. Si falla, intenta
restaurar PRSTATUS original bajo el mismo stop y lo vuelve a leer.
Sólo después de una confirmación completa se borra
`unsafe_register_state`; ninguna restauración parcial se anuncia
como éxito.

En caso de rollback no demostrable el broker intenta terminar
**sólo** al proceso hijo sujeto al `pidfd` original y no permite
DETACH voluntario de registros posiblemente corruptos. Si el
`pidfd_send_signal` falla, conserva la traza para que
`PTRACE_O_EXITKILL` termine al hijo al salir el broker. El backend
no presume recuperación limpia en ese caso y responde como hilo en
terminación.

`tests/supervisor_context_rollback_fault_test.c`, compilado en
**dos binarios distintos con macros únicamente de test**, comprueba:
1. El kernel aceptó una escritura pero se inyecta readback incoherente:
   el registro original debe quedar restaurado.
2. La misma escritura con rollback deliberadamente fallido: sólo
   puede terminar el hijo fijado por pidfd, con resultado SIGKILL.

### CET y una DLL realmente ejecutada

La import gate vuelve a consultar Shadow Stack antes de su epílogo.
Adicionalmente, **sólo en el fork con concesión HOST explícita para
control asíncrono**, se instala un filtro seccomp obligatorio que
rechaza `arch_prctl` de mutación SHSTK (códigos mayores o iguales a 0x5000,
excepto la consulta de sólo lectura `ARCH_SHSTK_STATUS`). El filtro rechaza ABI de
syscall x32 y requiere `PR_SET_NO_NEW_PRIVS`; si no puede instalarse,
el hijo NO ejecuta el fixture. No se desactiva el CET existente.

El test de runtime ejecuta los cuatro `arch_prctl` directos de
SHSTK_ENABLE/DISABLE/LOCK/UNLOCK y exige `EPERM`, aunque la CPU
o kernel de CI no tengan CET físico. Esto prueba la denegación por
kernel y no una función wrapper de usuario.

El PE32+ `async_supervisor_fixture.exe` ahora importa
`guest_companion.dll!GuestHotLoop` y crea un hilo cuyo PC real cae
en el bucle de código de **la DLL**, no del EXE. La DLL queda
pinned al resolver la IAT antes del fork y el padre conserva un
rango inmutable propio. El fixture comprueba un stop real, su
RIP de DLL y la reanudación sin pérdida del hilo. Las DLL
`LoadLibrary` tardías y las páginas JIT no adquieren esta garantía.

### Alcance autorizado, pendientes y decisión de liberación

La seguridad de una suspensión depende también de que el PE no
modifique deliberadamente el runtime que ejecuta dentro de su mismo
proceso. **No** hay aislamiento antimalicia completo de memoria
PE/host; por ello se mantiene `pe-isolated-run` sin el grant de
suspensión asíncrona. Un resultado positivo sobre DLL pinned no
justifica autorizar DLL dinámicas no registradas ni JIT.

SetThreadContext para RIP/RSP, DEBUG, FXSAVE/AVX/XSTATE sigue
rechazándose. Las APC especiales fuera de alertable waits siguen
no soportadas. Se puede cerrar **el contrato experimental acotado**
cuando sus pruebas de código y de núcleo sean positivas; nunca
inferir de ello «70% de Windows».

Comandos específicos para la aceptación manual del mantenedor:

```sh
make -j2 check-supervisor-context-rollback
make -j2 check-supervisor-control-ipc check-runtime-supervisor-win32
make -j2 check-pe-async-supervisor
make -j2 check-guest-boundary
bash scripts/check-core4-manual-hardening.sh 313
bash scripts/fedora-thread-supervisor-acceptance.sh
```

El test físico Bubblewrap con SELinux debe clasificarse como
INCOMPLETE si no puede ejercer la parada por política del anfitrión.
