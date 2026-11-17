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
