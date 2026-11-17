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

En Linux AMD64 con compilador C, binutils GNU (incluida la emulación `i386pep`), pthread y soporte kernel de `pidfd_open`, `PTRACE_SEIZE` y `PTRACE_GETREGSET`:

```bash
bash scripts/check-thread-supervisor.sh
BUILD_DIR=build-supervisor-ubsan CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
  bash scripts/check-thread-supervisor.sh
```

Para ejecutar la prueba de aceptación con registro automático del entorno en Fedora KDE, sin elevar privilegios:

```bash
bash scripts/fedora-thread-supervisor-acceptance.sh
cat build/supervisor-fedora-acceptance/result.txt
```

El script genera `environment.txt` (commit, kernel, Fedora, SELinux, Yama, arquitectura y compilador), `test.log`, `pe-test.log` y `result.txt` con los códigos de las dos pruebas. También puede ejecutarse mediante `make check-thread-supervisor` sin crear el informe de aceptación.

En Fedora, ejecutar **sin `sudo`**. Registrar `uname -a`, `/proc/sys/kernel/yama/ptrace_scope` si existe, estado SELinux y código final. La CI tiene un workflow de Ubuntu dedicado; el RPM Fedora y el PE launcher no activan esta función experimental.

**Criterios de resultado:** código 0 y las líneas `PASS supervisor: ...` y `PASS supervisor PE32+: ...` significan que ambos experimentos se ejecutaron; código 77 significa `INCOMPLETE` por bloqueo de host; otros códigos significan `FAIL`. No elevar permisos, desactivar SELinux ni modificar `ptrace_scope` para conseguir un resultado positivo.

## Etapa 2 — ejecución real de PE32+ y comprobación de contexto (17-11-2026)

Se ha añadido una prueba reproducible que compila un **PE32+ AMD64 auténtico con GCC + GNU objcopy/ld (emulación i386pep)**, reutilizando el procedimiento de `nt-counter.exe`, lo carga con el parser y el mapeador existentes `src/pe/image.c` (no un mapeador de laboratorio distinto) y ejecuta su punto de entrada en un pthread con `nt_guest_thread_enter` y **GS apuntando al TEB del runtime**. El hilo huésped no realiza llamadas al host ni mantiene locks del runtime durante su bucle de prueba. Un segundo hilo del proceso hijo mantiene un contador de actividad Linux independiente.

- `tests/pe/supervisor_pe_fixture.c`: ejecutable PE32+ freestanding que aumenta un contador en memoria compartida y termina cuando el anfitrión solicita salir; no contiene importaciones Win32, callbacks ni CRT.
- `tests/pe/supervisor_pe_thread_test.c`: cargador PE y anfitrión de prueba. El proceso padre es el supervisor y el hijo directo ejecuta el PE en su segundo hilo. Se registra TID real, rango de código PE y dirección del TEB antes de adjuntar `ptrace`.
- Dos ciclos positivos comprueban `RIP` **dentro del código PE mapeado**, `RSP != 0`, `GS_BASE == TEB`, contador PE detenido, contador Linux del líder avanzando, escritura/lectura/restauración de `RAX` con `PTRACE_SETREGSET`, y reanudación con incremento posterior del contador PE.
- El padre hace `PTRACE_DETACH` antes de pedir terminación y comprueba que el PE retorna y el proceso hijo sale sin error. El test de identidad del supervisor y las rutas de error siguen siendo independientes.
- `make check-pe-thread-supervisor` es explícito: no se añade a `make all`, `make check`, `pe-isolated-run` ni al RPM. El workflow dedicado ejecuta el test PE con y sin UBSan.

Para ejecutar ambas pruebas sobre Linux AMD64 con GCC y binutils `i386pep`:

```bash
make check-thread-supervisor
make -j2 check-pe-thread-supervisor
make -j2 BUILD_DIR=build-supervisor-pe-ubsan \
  CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' check-pe-thread-supervisor
```

El script de aceptación Fedora ahora ejecuta **ambos** tests y guarda por separado `test.log`, `pe-test.log`, los códigos individuales y `result.txt`. Requiere `gcc`, `make` y `binutils` con soporte PE/COFF; **nunca** necesita `sudo` para ejecutar las pruebas, alterar políticas Yama/SELinux ni adjuntarse a un proceso ajeno.

**Límite de interpretación:** esto demuestra que el supervisor detiene y restaura **código PE real en un bucle aislado y sin callbacks al host**. No prueba todavía el paso seguro a `SleepEx`/Win32, la existencia de una frontera de locks general, la propagación de señales Linux, APC especiales o `SetThreadContext` para ejecutables arbitrarios. El punto de entrada recibe una estructura de prueba por RCX, es decir, una **ABI experimental distinta del punto de entrada usual de Windows**; no se modifica la ABI pública del cargador de producción. El proceso PE de este test no se lanza mediante Bubblewrap. No certificar compatibilidad general ni Fedora físico con base en la CI.

**Estado porcentual:** componente 4: 96 % → 96 % (+0 pp). Aporta evidencia nueva para la arquitectura, pero no amplía todavía las APIs Win32 disponibles en programas reales.

## Etapa 3 — frontera explícita guest/host y prueba de rechazo (17-11-2026)

La frontera experimental se define en `include/ntnative/guest_boundary.h` con estados
`UNREGISTERED → GUEST → HOST → GUEST → FINISHED` y contador monotónico de
transiciones. El estado es **asesoramiento de un host de confianza, NO protección
contra un PE arbitrario malicioso**. La autoridad real sobre el PC/GS sigue siendo
el kernel Linux por `PTRACE_GETREGSET`.

`nt_supervisor_interrupt_guest()` suspende temporalmente un TID previamente
registrado y sólo devuelve éxito si **simultáneamente**: (a) se confirma el
`PTRACE_EVENT_STOP`, (b) el estado indica `GUEST` con generación estable,
(c) `RIP` está en el rango ejecutable PE autorizado,
(d) `GS_BASE` coincide con el TEB registrado y (e) `RSP` es no nulo.
Un rechazo de frontera intenta `PTRACE_CONT` **antes de devolver error
`EBUSY`**. En caso de error al reanudar, el fallo no se oculta: el
caller sólo puede destruir su propio proceso hijo controlado.

`nt_supervisor_get_guest_context()` y `nt_supervisor_set_guest_rax()`
exigen estar detenido y pasar la misma verificación, de modo que no
puede escribirse siquiera `RAX` en un hilo detenido en host.
Son **interfaces del supervisor de prueba**, no implementaciones nuevas
de `GetThreadContext`/`SetThreadContext` públicas.

El fixture PE32+ entra en una callback anfitriona con ABI Win64 que
**retiene deliberadamente un mutex pthread**. El test solicita
detención en ese instante y exige `EBUSY`, reanudación automática
y ausencia de bloqueo; a continuación falsea el indicador de estado
`GUEST` y repite el intento: la verificación independiente de `RIP`
también debe rechazarlo. Finalmente se libera el mutex, el PE vuelve
a ejecutar código de invitado y las operaciones de contexto autorizado
siguen funcionando. Esta prueba negativa es obligatoria y no se
sustituye por la simple ausencia de un hang.

### Validación Bubblewrap/Fedora

`scripts/check-pe-thread-supervisor-bwrap.sh` ejecuta el **harness y
el PE en el mismo namespace de usuario/PID de Bubblewrap**; el
supervisor sigue siendo el padre directo del hilo invitado. El montaje
de laboratorio permite sólo `/usr` y el repositorio en solo lectura,
`/proc`/`/dev` y un `/tmp` temporal. No se monta el host `/home`,
no se permiten redes y no se exige `CAP_SYS_PTRACE`. Es un **sandbox de
pruebas controladas**, no reemplaza la configuración del launcher
de producción `pe-isolated-run`.

Ejecutar en Fedora KDE físico, **como usuario normal**:

```bash
sudo dnf install -y gcc make binutils bubblewrap
git switch feature/core4-guest-host-boundary-bwrap-20261117
make -j2 check-pe-thread-supervisor
bash scripts/fedora-thread-supervisor-acceptance.sh
cat build/supervisor-fedora-acceptance/result.txt
```

`sudo` sólo se muestra para la instalación de paquetes, **nunca para
ejecutar el supervisor o alterar SELinux/Yama**. Los informes guardan
tres estados individuales: supervisor Linux, PE32+ nativo y
PE32+ dentro de Bubblewrap. Resultado global `PASS` requiere los
tres códigos 0. Una restricción de `ptrace`/user namespaces da
`INCOMPLETE`/77, y no se maquilla como éxito. Otros fallos son
`FAIL`. La ejecución de CI en un runner Ubuntu **no certifica Fedora
físico**, y se registra por separado.

### Puerta para APC y funciones Win32 reales

La arquitectura demuestra el requisito necesario para contextos de
hilos invitados en una ventana controlada, pero faltan dependencias:

1. **Mediar todas las entradas PE→API anfitriona y callbacks de host→PE**:
   publicar `HOST` **antes** de tomar locks, y `GUEST` sólo tras
   liberarlos; evitar la llamada a cualquier callback PE bajo locks
   internos. Una verificación de `RIP` por sí sola es insuficiente
   si una callback vuelve a PE con un mutex del host aún adquirido.
   El fixture prueba únicamente la transición controlada.
2. **Supervisor hijo real del sandbox y broker autenticado**: alojar el
   supervisor en el mismo namespace que los hilos PE reales, con mapeo
   TID→HANDLE/identidad generacional y comprobaciones NT de derechos.
   No leer directamente `TID` suministrados por PE ni usar `ptrace`
   desde un hilo del mismo proceso invitado.
3. **`SuspendThread`/`GetThreadContext`**: añadir un backend opcional
   que traduzca `STATUS_NOT_SUPPORTED` al fallar el gate y preserve
   los contadores actuales, validando `CONTEXT_AMD64`, flags y vida
   de cada HANDLE. Mantener el camino cooperativo actual como fallback.
4. **`SetThreadContext`**: no habilitar escrituras generales hasta
   contar con captura/restauración verificada de entero, control,
   `RFLAGS`, SIMD/FXSAVE, TLS/GS y pruebas de flujo de retorno PE.
   El roundtrip de `RAX` del experimento no equivale a este contrato.
5. **APC especiales**: sólo es seguro introducir ejecución de una APC
   fuera de esperas alertables cuando exista un trampoline AMD64 que
   preserve **todo el contexto** (incluido XSTATE requerido), alineación
   Microsoft x64, retorno, manejo de señales, cancelación y estados
   de desconexión. No reutilizar la cola APC normal para simularlas.

Por estos motivos **no se cambia el comportamiento de las funciones
públicas `NtQueueApcThreadEx`, `QueueUserAPC2`,
`SuspendThread`, `GetThreadContext` ni `SetThreadContext`** en
esta contribución. Activarlas en producción basándose sólo en el
prototipo aumentaría el riesgo de corrupción/lockups.

**Componente 4: 96 % → 96 % (+0 pp).** La evidencia estricta de la
frontera no equivale a paridad de APIs ni a aceptación en hardware físico.

## Etapa 4 — puente NT acotado y APC especial en PE controlado (17-11-2026)

`src/supervisor/nt_supervisor_nt_bridge.[ch]` añade un **adaptador
experimental en el proceso supervisor externo**, con códigos
`NTSTATUS`, comprobación de derechos y semántica de contador de
suspensión (hasta 127). Sólo funciona sobre **un TID hijo registrado
y detenido mediante la frontera**:

- `nt_supervisor_nt_suspend_thread` y `...resume_thread`: permisos
  `THREAD_SUSPEND_RESUME`, `previous` real, suspensiones anidadas
  y reanudación sólo tras el último descenso. Se comprueba que el
  contador del PE no avanza mientras sigue suspendido.
- `nt_supervisor_nt_get_context`: obtiene `RIP`, `RSP`,
  `RFLAGS` y registros generales realmente suministrados por Linux,
  traducidos al `CONTEXT` AMD64 del proyecto con `CONTROL`,
  `INTEGER` y `SEGMENTS`. No finge soporte de debug/FP/XSTATE en
  `GetThreadContext`.
- `nt_supervisor_nt_set_context`: permite la escritura comprobable
  de registros enteros (excepto los punteros de control `RIP/RSP`,
  y `RBP`). Las escrituras generales de control, `GS` y SIMD
  permanecen denegadas. El test escribe `RAX`, lo lee y restaura
  antes de reanudar.
- `nt_supervisor_nt_queue_trusted_apc`: ejecuta una APC **sin espera
  alertable** mediante `ptrace`, exclusivamente en un PE fixture
  sin importaciones y con pila APC propia, guard page, trampolín
  Microsoft x64, finalización `INT3` y captura/restauración real
  de `NT_X86_XSTATE` y todos los registros generales. Nunca
  entrega el `SIGTRAP` al invitado. Rechaza ejecución en secciones
  críticas, stacks no autorizados o estados incompletos. El test
  exige progreso del PE después de la APC.

**Esto no es aún conectar las exportaciones Win32 reales**:
`nt_SuspendThread`, `nt_GetThreadContext`, `nt_SetThreadContext`,
`nt_NtQueueApcThreadEx` y `nt_QueueUserAPC2` siguen usando
los contratos previos. Los nuevos métodos viven **en el supervisor
externo**, y todavía requieren un broker IPC autenticado entre cada
HANDLE NT y el TID correspondiente, más mediación general de
transiciones y callbacks, antes de que sean el backend del runtime.
Un proceso Linux no puede hacer `ptrace` de otro hilo de su propio
grupo como si fuera su trazador externo.

**Evidencia de CI:** <https://github.com/AlexDevJSC/144mb/actions/runs/44039343577>
PASS en test Linux, test Linux UBSan, PE32+ con puente NT/APC y
PE32+ UBSan; Bubblewrap runner Ubuntu: **INCOMPLETE** por
restricciones de namespaces/`ptrace`, no aprobación física Fedora.

**Madurez componente 4: 96 % → 96 % (+0 puntos porcentuales).**
Se amplía la prueba arquitectónica, no las exportaciones en producción.

## Riesgos todavía abiertos

1. **Frontera PE/host no demostrada.** `ptrace` puede parar el hilo mientras mantiene un mutex de Linux, una pila de la implementación o un callback hacia el runtime; esto puede bloquear el proceso. El test utiliza un bucle C libre de esos recursos. Que exista un supervisor no elimina este problema. Antes de integrarlo, definir un protocolo de estados verificable `GUEST_RUNNING`, `HOST_TRANSITION`, `HOST_CRITICAL`, `PTRACE_STOPPED` y reconocer transiciones de host sin dejar locks retenidos al parar.
2. **No existe aún integración con el lanzador PE de producción**: la segunda prueba ya ejecuta un **PE32+ real** mediante el loader de pruebas y GS/TEB, pero no integra el supervisor en `pe-isolated-run` ni prueba Bubblewrap. La ruta `pe-isolated-run` usa `posix_spawn`, herencia de FDs, GS/TEB y Bubblewrap; el supervisor debe residir en el ámbito de permisos adecuado y autenticar cada TID registrado por el broker. No usar PIDs/TIDs proporcionados sin verificar por el programa invitado.
3. **No confundir parada con semántica NT.** Faltan contadores anidados de `SuspendThread`, un registro de contextos Win64/NTSTATUS, validación de `ContextFlags`, entrega de APC/SEH, finalización y propagación de señales reales sin perderlas. La modificación de `RAX` solamente demuestra un `SETREGSET` real y su readback, no una reanudación PE después de cambiar `RIP`.
4. **Aislamiento y permisos:** Yama, SELinux, seccomp, user namespaces y Bubblewrap pueden rechazar `ptrace`; `PTRACE_O_EXITKILL` afecta al hilo trazado si muere el supervisor. No se debe intentar saltarse estas políticas.
5. **Concurrencia y vida útil:** esta versión gestiona un solo TID y un propietario; faltan registro de threads creados después, `PTRACE_O_TRACECLONE`, eventos de salida, migración de supervisor, reentrancia, test de cierre por señal y estrés bajo carga. El supervisor y el hilo trazado pueden compartir decisiones de seguridad, pero el supervisor no concede derechos NT por sí solo.

## Próxima puerta de integración

Extender el fixture **PE32+ ya operativo** al lanzador real aislado con Bubblewrap y un protocolo genérico de frontera guest/host que declare secciones críticas y evite paradas inseguras; comparar capturas y contextos. Repetir sobre Fedora KDE físico sin permisos extra y con Bubblewrap activo. Después de esa evidencia, diseñar el adaptador a `SuspendThread/GetThreadContext/SetThreadContext` y APC avanzadas con contratos NTSTATUS/LastError. Mantener el backend actual como fallback seguro mientras no exista prueba positiva de integración.

**Porcentajes:** componente 4 conserva la estimación documentada de 96 % → 96 % (+0 pp). Un fixture aislado no aumenta cobertura Win32 ni certifica cierre de fase.
