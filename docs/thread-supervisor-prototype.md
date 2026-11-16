# Núcleo NT/Win32 — supervisor externo experimental de hilos (fase 4)

**Estado:** prueba de concepto separada; no sustituye `SuspendThread`, `NtSuspendThread`, APC ni contextos del runtime PE. No está instalado ni habilitado durante ejecuciones ordinarias.

## Alcance comprobable

`src/supervisor/nt_thread_supervisor.c` permite que un supervisor Linux controle **un TID real perteneciente a un proceso hijo directo suyo**, comprobado con `/proc/<pid>/task/<tid>/status`, `PPid`, `Tgid` y `pidfd_open`. No acepta adjuntarse a procesos arbitrarios. Usa `PTRACE_SEIZE` con `PTRACE_O_EXITKILL`, `PTRACE_INTERRUPT`, `waitpid(__WALL)` para confirmar `PTRACE_EVENT_STOP`, `PTRACE_GETREGSET`/`PTRACE_SETREGSET` (solo `RAX` en este prototipo), `PTRACE_CONT` y `PTRACE_DETACH`.

- **Suspensión real:** puede detener el TID que ejecuta un bucle C sin que el hilo llegue a una espera alertable. El test mantiene un segundo hilo del mismo proceso ejecutándose y verifica que su contador sigue avanzando mientras el primero permanece inmóvil.
- **Contexto auténtico:** registra `RIP`/`RSP` no nulos; modifica y relee `RAX` mientras está detenido; restaura su valor anterior antes de reanudar. No promete que una modificación arbitraria de `RIP`/`RSP`, SIMD ni contextos PE funcione.
- **Restricción de propiedad:** sólo acepta un proceso hijo vivo del supervisor y un hilo del mismo grupo. La comprobación de `pidfd` evita tratar el número de PID como identidad reusable después de terminar el proceso.
- **No elevación:** ejecuta como usuario ordinario. Si una política de `ptrace` bloquea la prueba, informa `INCOMPLETE` y código de salida **77**, nunca `PASS`.
- **Fallo cerrado:** timeout de parada, estados inesperados, TID externo, contexto no detenido o permisos insuficientes no producen éxito. El test mata su propio proceso hijo en errores y tiene un timeout externo de 20 s.

## Reproducción

En Linux AMD64 con compilador C, pthread y soporte kernel de `pidfd_open`, `PTRACE_SEIZE` y `PTRACE_GETREGSET`:

```bash
bash scripts/check-thread-supervisor.sh
BUILD_DIR=build-supervisor-ubsan CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
  bash scripts/check-thread-supervisor.sh
```

En Fedora, ejecutar **sin `sudo`**. Registrar `uname -a`, `/proc/sys/kernel/yama/ptrace_scope` si existe, estado SELinux y código final. La CI tiene un workflow de Ubuntu dedicado; el RPM Fedora y el PE launcher no activan esta función experimental.

**Criterios de resultado:** código 0 y línea `PASS supervisor: ...` significan que el experimento se ejecutó; código 77 significa `INCOMPLETE` por bloqueo de host; otros códigos significan `FAIL`. No elevar permisos, desactivar SELinux ni modificar `ptrace_scope` para conseguir un resultado positivo.

## Riesgos todavía abiertos

1. **Frontera PE/host no demostrada.** `ptrace` puede parar el hilo mientras mantiene un mutex de Linux, una pila de la implementación o un callback hacia el runtime; esto puede bloquear el proceso. El test utiliza un bucle C libre de esos recursos. Que exista un supervisor no elimina este problema. Antes de integrarlo, definir un protocolo de estados verificable `GUEST_RUNNING`, `HOST_TRANSITION`, `HOST_CRITICAL`, `PTRACE_STOPPED` y reconocer transiciones de host sin dejar locks retenidos al parar.
2. **No existe aún integración con el lanzador PE**: el fixture crea un proceso Linux hijo con pthreads; **no ejecuta un archivo PE**. La ruta `pe-isolated-run` usa `posix_spawn`, herencia de FDs, GS/TEB y Bubblewrap; el supervisor debe residir en el ámbito de permisos adecuado y autenticar cada TID registrado por el broker. No usar PIDs/TIDs proporcionados sin verificar por el programa invitado.
3. **No confundir parada con semántica NT.** Faltan contadores anidados de `SuspendThread`, un registro de contextos Win64/NTSTATUS, validación de `ContextFlags`, entrega de APC/SEH, finalización y propagación de señales reales sin perderlas. La modificación de `RAX` solamente demuestra un `SETREGSET` real y su readback, no una reanudación PE después de cambiar `RIP`.
4. **Aislamiento y permisos:** Yama, SELinux, seccomp, user namespaces y Bubblewrap pueden rechazar `ptrace`; `PTRACE_O_EXITKILL` afecta al hilo trazado si muere el supervisor. No se debe intentar saltarse estas políticas.
5. **Concurrencia y vida útil:** esta versión gestiona un solo TID y un propietario; faltan registro de threads creados después, `PTRACE_O_TRACECLONE`, eventos de salida, migración de supervisor, reentrancia, test de cierre por señal y estrés bajo carga. El supervisor y el hilo trazado pueden compartir decisiones de seguridad, pero el supervisor no concede derechos NT por sí solo.

## Próxima puerta de integración

Crear un fixture **PE32+ real** que se ejecute en el sandbox con una señal verificable de entrada/salida de código invitado (sin intervención del programa Windows); detenerlo sólo en un punto de ejecución declarado seguro, tomar contextos y verificar que el runtime no mantiene locks. Repetir sobre Fedora KDE físico sin permisos extra y con Bubblewrap activo. Después de esa evidencia, diseñar el adaptador a `SuspendThread/GetThreadContext/SetThreadContext` y APC avanzadas con contratos NTSTATUS/LastError. Mantener el backend actual como fallback seguro mientras no exista prueba positiva de integración.

**Porcentajes:** componente 4 conserva la estimación documentada de 96 % → 96 % (+0 pp). Un fixture aislado no aumenta cobertura Win32 ni certifica cierre de fase.
