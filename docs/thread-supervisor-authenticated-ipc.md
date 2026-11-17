# Componente 4 — Problema 2: IPC autenticado runtime ↔ supervisor

## Contrato y topología

**Estado**: backend de control conectado en el runtime, por defecto
**sin operaciones Win32 activadas**. El código real utiliza
`src/runtime/thread_broker_ipc.[ch]` y
`src/runtime/thread_broker_registry.[ch]`, no un socket independiente
sólo creado por el fixture.

```text
pe-isolated-run / Bubblewrap
└── /runner --worker (PID 1 dentro del sandbox)
    ├── pe_image_load (todavía NO ejecuta instrucciones PE)
    └── nt_runtime_run_fixture(): proceso PADRE SUPERVISOR
        ├── socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC)
        ├── getrandom(128 bits) + fork + pidfd_open(hijo)
        ├── registro de hijo y sesión autenticada
        ├── pthread del broker (padre; autorizado a ptrace sobre su hijo)
        └── hijo PE (nt_broker_client_attach; handshake ANTES de ejecutar)
            ├── hilo inicial (TID == PID)
            ├── CreateThread → REGISTER del TID real
            └── cliente IPC → SUP_OPEN / SUP_QUERY / SUP_CLOSE
```

Para un proceso hijo creado por `CreateProcess`, el `--child` recibe
un socket autenticado desde el padre vía descriptor 8, **pero la
ejecución PE real vuelve a utilizar `nt_runtime_run_fixture()`**.
Su hijo de ejecución descarta su copia heredada del socket antiguo
antes de adjuntar la nueva sesión del padre directo. Esto evita
autorizar por accidente un nieto en un broker que sólo reconoce las
credenciales del hijo inmediato.

El servidor mantiene el `pidfd` del proceso hijo creado por este mismo
`fork()`; el cliente no puede escoger otro PID ni activar privilegios
en el servidor mediante un paquete. El nuevo protocolo es la **versión
privada 3**, con un handshake inicial y credenciales del kernel
`SCM_CREDENTIALS` comprobadas **en cada mensaje**, junto al secreto
aleatorio de sesión y la identidad viva `pidfd`. No hay socket de host
accesible por nombre ni tokens basados en tiempo.

## Operaciones del plano de control

| Operación RPC | Objetivo | Modifica un hilo |
| --- | --- | --- |
| `SUP_OPEN` (6) | Vincula un TID que el runtime ya registró a un capability por hilo, proceso y generación | No |
| `SUP_QUERY` (7) | Revalida derechos, PID hijo, registro del TID, inode /proc y generación; devuelve identidad | No |
| `SUP_CLOSE` (8) | Revoca el lease del requester | No |
| `SUP_PROBE` (9) | Diagnóstico opt-in: el padre ejecuta `PTRACE_SEIZE/INTERRUPT/GETREGSET/DETACH` y devuelve RIP auténtico | Parada **transitoria** en entorno de prueba, sin escrituras |

Los permisos de consulta del broker de procesos ajenos siguen
separados de `NT_BROKER_SUPERVISOR_SUSPEND`,
`GET_CONTEXT` y `SET_CONTEXT`. Tener un capability de consulta
de otro proceso **no** concede un capability de supervisor de ese
proceso. `SUP_OPEN` comprueba que el hilo objetivo pertenece
al **propio** proceso requester, está registrado y no ha sido reciclado.
Ni un TID forjado ni un capability de otro proceso pueden escalar.

Los capabilities son identificadores opacos de proceso: **no deben
considerarse secretos aleatorios**. La seguridad entre procesos se
fundamenta en SCM_CREDENTIALS, pidfd, secreto de sesión, comprobación
de propiedad, derechos y generación. El invitado que ya controla el
mismo proceso Windows puede consultar sus propios hilos; éste no es
un mecanismo de aislamiento contra un PE malicioso dentro de su propio
espacio de direcciones.

El servidor sólo permite `SUP_PROBE` si su propio
`peer.allowed_access` contiene
`NT_BROKER_SERVER_ENABLE_PTRACE_PROBE`. **El launcher real no lo
concede**. Sólo los tests privilegiados por el HOST de prueba lo
habilitan. Ningún paquete entrante puede cambiar esta decisión.
Un fallo de `ptrace` no genera un resultado falso: el código intenta
desvincular inmediatamente el trazador y, en un fallo irrecuperable,
únicamente puede terminar su hijo exacto identificado por pidfd.
No se permite suspender un hilo indefinidamente a través de RPC.

## Revocación y errores

- `UNREGISTER` revoca inmediatamente los leases del TID secundario.
- Un cambio de sesión/rekey invalida todos los capabilities del hijo.
- EOF, credenciales incorrectas o protocolo incorrecto cierran la
  sesión y revocan los capabilities de ese requester, antes incluso
  de esperar a la recolección del proceso hijo.
- La salida del hijo/pidfd revoca toda su identidad y sus hilos.
- Una consulta posterior con generación antigua no autoriza otro hilo
  que haya reutilizado el mismo número de TID.
- Un fallo en handshake impide la ejecución de código PE y el padre
  termina exclusivamente el proceso que acaba de crear. No se
  reutilizan PIDs/TIDs del invitado para señales del anfitrión.

## Pruebas

```sh
make check-supervisor-authenticated-ipc
make check-runtime-supervisor-launch
make check
```

`tests/supervisor_authenticated_ipc_test.c` lanza dos procesos con
secretos independientes. Verifica hilo propio registrado, rechazo
cruzado de TID, acceso denegado, capability falso, revocación por
`UNREGISTER`, registro del hilo inicial y una lectura real de RIP
mediante `ptrace` habilitada exclusivamente por el padre de prueba.
El segundo hijo comprueba que un cliente autenticado **no** puede
activar ptrace sin permiso del host.

`tests/runtime_supervisor_launch_test.c` comprueba el mismo canal
desde el `fork()` genuino de `nt_runtime_run_fixture` y un hilo creado
por `nt_CreateThread`, en dos sesiones consecutivas.

La suite `make check` y CI Fedora/RPM verifican además que el
runtime sigue construyéndose. La validación **sobre Fedora KDE físico
con Bubblewrap** pertenece al problema 5, y no debe presentarse como
hecha por superar los runners de CI.

## Límite de salida del problema 2

**Conectado:** autenticación runtime↔supervisor externo, identidad
fiable de hilos propios, capacidades con permisos y generaciones,
revocación, ciclo de vida y prueba de `ptrace` positiva en host apto.

**No conectado todavía a las exportaciones Win32:** `SuspendThread`,
`ResumeThread`, `GetThreadContext`, `SetThreadContext` y APC
especiales. Para habilitarlas universalmente es necesario completar
los problemas **1 (frontera PE/host)**, **4 (fallos, señales y
lifecycle)** y **3 (adaptador Win32)**, además de validación física.
Un socket correcto no justifica arriesgar locks del host.

Esta entrega cambia sólo el **componente 4 — Procesos e hilos**.
La madurez porcentual debe valorarse tras CI; el porcentaje histórico
anterior era 96 %, una estimación y no paridad universal con Windows.
