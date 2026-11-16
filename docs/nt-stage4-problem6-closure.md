# Problema 6 (fase 4): revisión de contratos e integración — 16-11-2026

Esta versión ejecuta los contratos verificables del problema **6, planificación
y compatibilidad avanzada**, sobre un Linux anfitrión. No equivale a
certificación general de Windows NT ni declara terminado el componente 4
(procesos e hilos). Respetar la cola `6 → 5 → 3 → 2 → 1 → 4 → 7`.

## Resultado por contrato

| Subtarea problema 6 | Estado/criterio | Evidencia y limitación |
| --- | --- | --- |
| 1. Prioridad multihilo | Implementado previamente para TIDs guest autenticados, con reversible nice | Pruebas positivas con RLIMIT_NICE en CI; Linux no es planificador NT |
| 2. Afinidad multihilo | Implementado para hilos controlados y CPU permitidas | Transacciones verificadas; no tocar TIDs extranjeros |
| 3. Grupos dispersos y >64 CPU | Modelo compacto + sintético de 80 CPU | Validación física de grupos >64 CPU expresamente aplazada |
| 4. Prioridades dinámicas | Boost real opt-in y decay automático con recuperación de nice | Solo con privilegio reversible; no paridad de política NT |
| 5. Quantum | Comparación nativa de distribuciones, sin parche global | No justifica reescritura; quantum Windows ≠ cuantía observada |
| 6. Hilos externos | **Ruta nueva y acotada de consulta para procesos hijos realmente lanzados** | `pidfd`, autenticación SO_PASSCRED por mensaje, RPC v2, generacionales, derechos query-only; modificaciones y token/ACL completos pendientes |
| 7. Diferencial | Windows nativo virtual y Linux CI; Fedora RPM en contenedor | No certifica hardware físico Fedora KDE, NUMA/hotplug ni Bubblewrap físico sin SKIP |

## Contrato observable de la ruta entre procesos

- El lanzador PE pasa un socket AF_UNIX `SOCK_SEQPACKET` como FD 8
  mediante `posix_spawn`; `worker_ready` cierra todo descriptor ajeno al
  manifiesto y a los canales permitidos. Antes de iniciar el PE, el proceso
  hijo **debe recibir** el saludo autenticado del padre. El token aleatorio
  de 128 bits viaja por ese socket, nunca por variables de entorno.
- En el padre, `nt_runtime_watch_child` conserva el control de ciclo
  de vida; la variante `nt_runtime_watch_child_broker` asocia identidad
  del hijo real con `pidfd`, instala sesión con secreto y arranca un
  servidor aislado por hijo. `SCM_CREDENTIALS` comprueba por **mensaje**
  PID, UID y GID del remitente; los hijos suplantadores se rechazan
  incluso si heredan el socket y conocen el secreto.
- El registro tiene cuotas finitas, generaciones monotónicas, mutex y
  referencias de handles por solicitante, con revocación al terminar
  el proceso o el hilo. Los TID secundarios anunciados por el propio
  runtime se verifican en `/proc/<PID_registrado>/task/<TID>` y su
  identidad de inode se revalida para impedir reutilización de TID.
  No hay `ptrace`, modificación de `nice` ni `sched_setaffinity`
  sobre otro proceso Linux a partir de identificadores invitados.
- Un PE hijo puede hacer `OpenThread(THREAD_QUERY_LIMITED_INFORMATION)`
  y `NtOpenThread` sobre el hilo principal o un hilo secundario
  **actualmente registrado y permitido** de un hermano dentro de la
  misma sesión runtime. Recibe un handle **local y único**; `GetThreadId`
  conserva identidad. Para objetos remotos no se simula la TEB,
  `GetExitCodeThread`, espera ni los cambios de prioridad/afinidad:
  se rechazan hasta tener el contrato completo.
- La política v2 habilita consultas limitadas entre hermanos de un
  mismo lanzador, no entre programas de distintos sandboxes. **No**
  afirma emular DACL, SID, token, impersonation o todos los permisos
  de `OpenThread` Windows: esa parte está bloqueada explícitamente
  por el problema posterior **4, seguridad NT**. La identidad compartida
  de handles genéricos requiere el problema posterior **3, objetos NT**.
  No se toman estas dependencias como implementadas.
- Un fallo de registro, autenticación, identidad, timeout o revocación
  deja inalterado el buffer de resultado y nunca habilita acciones
  sobre tareas externas al registro confiable.

## Validación y decisión de avance

- `make check`: ciclo de vida y apertura real de hijo; rechazo de
  operaciones externas. Prueba de registro: 2 hijos, ACL y 600 ciclos
  concurrentes. `thread_broker_ipc_test.c`: 2 procesos reales, hilo
  secundario registrado y consulta remota a través de SOCK_SEQPACKET.
- `make` con UBSan y Fedora RPM en contenedor verifican código Linux.
  Los fixtures PE anidados en Bubblewrap y Fedora KDE físico siguen
  pendientes de aceptación por el propietario del hardware.
- El plan permite **posponer** pruebas físicas y dependencias de los
  componentes 3/4 cuando ya se ha llegado al máximo perfil seguro y
  demostrable de este problema. No confundir el límite con compatibilidad
  Windows universal.
- Una vez que la CI del presente PR sea positiva, el trabajo siguiente
  de fase 4 es **problema 5: APC avanzadas**, no otra iteración genérica
  de planificación. El componente arquitectónico 4 permanece activo
  según su orden general.
