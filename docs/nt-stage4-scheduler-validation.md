# Etapa 4 / problema 6: aceptación Fedora físico, multigrupo y Windows diferencial

**Responsable:** núcleo NT/Win32, procesos e hilos. **Estado:** instrumentado;
hardware físico y paridad de planificación **NO verificados** en esta entrega.
Esta comprobación no habilita la transición al problema 5 (APC avanzadas).

## Contraste funcional de prioridades y afinidades con Windows nativo (19-11-2026)

Este contrato añade un diferencial **mismo código fuente Win32** para
cuatro suites PE existentes, sin sustituir sus expectativas ni
reescribirlas para favorecer a Windows o Linux:

| Fixture PE | Contrato observable y frontera |
| --- | --- |
| `thread_group_affinity_fixture.c` | `Get/SetThreadGroupAffinity`, CPU realmente ejecutado, máscaras relativas a grupo, derechos de HANDLE y parámetros inválidos |
| `process_affinity_mask_fixture.c` | `Get/SetProcessAffinityMask`, permisos reducidos, máscaras, propagación a worker y restauración |
| `priority_class_fixture.c` | `Get/SetPriorityClass`, prioridades relativas de `Get/SetThreadPriority`, `NtQueryInformationProcess` y denegación sobre handles limitados |
| `priority_boost_boundary_fixture.c` | Presencia y respuesta de `Get/SetThreadPriorityBoost` y `Get/SetProcessPriorityBoost` |

En CI, `scripts/windows-run-stage4-scheduler.ps1` compila **las cuatro
fuentes exactas** con MSVC como EXE nativos (el mismo fixture de
grupos ya existía; se añaden tres). Ejecuta únicamente procesos de
prueba Windows y **guarda cada código de salida, incluido el distinto
de cero**. Algunos fixtures expresan restricciones específicas del
runtime Linux (por ejemplo `ERROR_NOT_SUPPORTED` para *priority
boost* si la extensión no está habilitada); un Windows auténtico puede
satisfacer la API y fallar legítimamente esas expectativas: ese es
precisamente un dato diferencial, no una avería que se deba ocultar.

`scripts/linux-run-stage4-scheduler-functional.sh` ejecuta los
cuatro objetivos `make check-pe-thread-groups`,
`check-pe-process-affinity`, `check-pe-priority-class` y
`check-pe-priority-boost` sobre el runtime Linux real y archiva
resultados y logs independientes. Este perfil utiliza Linux de CI
hosted, **no Fedora físico**. Cada fixture altera solamente el proceso
de laboratorio que crea; no cambia prioridad, cpuset ni derechos del
anfitrión fuera de él.

Además, la sonda nativa independiente
`tests/windows/stage4_scheduler_probe.c` comprueba **mutaciones
reversibles del propio proceso/hilo** sin suponer la clase de
prioridad inicial del runner. Para `PROCESS_PRIORITY_CLASS_ROUNDTRIP`
alterna entre NORMAL y BELOW_NORMAL, verifica la lectura y restaura
la clase original. Para `THREAD_PRIORITY_ROUNDTRIP` modifica la
prioridad de un worker `CREATE_SUSPENDED`, comprueba la lectura y
restaura el valor original antes de `ResumeThread`.
`PROCESS_AFFINITY_ROUNDTRIP` reduce la máscara del propio proceso
a uno de sus CPU permitidos, verifica y restaura; registra `SKIP`
cuando la API legacy no es representable en topología multigrupo.
Los resultados de ese binario se archivan en
`stage4-windows-native.txt` y no convierten un fixture antiguo
que sale en 10 en una falsa coincidencia. El comparador exige estos
tres hechos nativos y `NT_SCHEDULER_POLICY_PARITY=UNVERIFIED`.

`scripts/compare-stage4-priority-affinity.py` exige que las pruebas
Windows y Linux provengan del mismo commit SHA, rechaza resultados
omitidos y deja tres categorías: `SAME_SOURCE_CONTRACT_CONSISTENT`,
`OBSERVED_DIFFERENCE_CHECK_FIXTURE_ASSUMPTIONS` o
`NOT_DEMONSTRATED`. No atribuye una discrepancia a un defecto Linux
sin revisar el código de salida exacto. El informe está disponible como
`build/stage4-priority-affinity-differential.json` junto con
`stage4-windows-priority-affinity.txt`,
`stage4-linux-priority-affinity.txt` y logs originales.
El resultado máximo es *observación funcional limitada*,
**no paridad de planificador NT**.

La política dinámica escalonada de prioridades, *boost/decay* exacto,
quantum NT, selección por topología Windows real, CPU Sets, prioridades
efectivas y equidad entre cargas continúan **no demostrados**. La CI de
Windows utiliza un kernel auténtico en VM; ni eso ni Fedora RPM en
contenedor prueban rendimiento o aislamiento en dos máquinas físicas
homologables. No se altera el 96 % documental del componente 4 hasta
la revisión de los resultados y criterios de aceptación física.

## Comparación explícita de CONTEXT_FLOATING_POINT (19-11-2026)

La CI ejecuta una nueva fuente **nativa Windows compilada con MSVC**:
`tests/windows/stage4_fp_context_probe.c`. Crea un hilo Windows
`CREATE_SUSPENDED`, obtiene su `CONTEXT_FLOATING_POINT`, intenta
modificar solamente el modo de redondeo MXCSR, comprueba la lectura
posterior y restaura el valor previo antes de permitir su salida.
Registra incluso fallos de `GetThreadContext`/`SetThreadContext`,
sin alterar la evidencia para favorecer al backend Linux.

En paralelo, `make check-runtime-supervisor-win32` registra en CI
el resultado observable del supervisor Linux sobre un hilo PE real.
`scripts/compare-stage4-fp-context.py` exige el esquema y origen
Windows auténticos y etiqueta `NOT_DEMONSTRATED` si el runtime aún
no demuestra el roundtrip FP o si Windows devuelve un error.
Cuando ambos consten como correctos genera
`CONSISTENT_NARROW_PROFILE`, **no** equivalencia NT.
Ambos ejecutan escenarios diferentes (Windows: hilo creado suspendido;
Linux: hilo PE detenido mediante supervisor autenticado): esta comparación
se limita a las operaciones FP comunes y exige ampliar los fixtures
para cubrir casos idénticos, grupos combinados, otros registros y errores.
GitHub Windows-hosted es un kernel Windows real en VM, no validación
física. Tampoco acredita Fedora/SELinux en hardware, AVX/XSAVE ni
derechos de procesos arbitrarios.

Artefactos CI:
`stage4-windows-fp-context.txt`,
`stage4-linux-context.log` y
`stage4-context-differential.json`. La CI **no convierte diferencias en
PASS**. Componente 4: **96 % → 96 % (+0 pp)** por instrumentación,
sin afirmar paridad de contextos.

## Qué cuenta como evidencia

| Prueba | Entorno | Evidencia válida | Qué NO prueba |
| --- | --- | --- | --- |
| `tests/thread_group_affinity_test.c` y PE controlado | Linux/CI | `pthread_setaffinity_np` real, grupos por CPU ID y errores | Política de grupos de Windows |
| `scripts/fedora-stage4-multigroup-acceptance.sh` | **Fedora KDE x64 instalado en máquina física**, terminal Plasma | cpuset con >=65 CPUs **permitidas**, al menos dos grupos `cpu_id/64`, `cross-group=yes`, pruebas del runtime/PE/UBSan/Bubblewrap en host | Que Linux emule la topología de Windows 11 o que tenga permisos para prioridad reversible |
| `tests/windows/stage4_scheduler_probe.c` | Windows real, compilado MSVC | `GetActiveProcessorGroupCount`, `GetThreadGroupAffinity`, `GetProcessGroupAffinity`, `SetThreadGroupAffinity`, `SetThreadAffinityMask`, `Get*PriorityBoost` y QueryThreadCycleTime | Quantum de NT, igualdad de reparto de CPU, paridad con Linux |
| `windows-native-scheduler` GitHub Actions | Windows **hosted VM** | Código ejecutado por kernel Windows auténtico y artefacto con resultados | Aceptación de máquina **física** ni multicpu >64 si el runner no tiene esos procesadores |
| `tests/pe/thread_group_affinity_fixture.c` idéntico en ambos entornos | PE en Linux y compilación nativa MSVC en Windows | Código de contrato común, exit code de Windows preservado como `MATCHED` o `DIVERGED` sin ocultar un desajuste | Que una diferencia sea automáticamente un error de Linux; el fixture puede incorporar una hipótesis errónea |
| `scripts/compare-stage4-scheduler.py` | Cualquier Python 3 | Comparación de invariantes estructurales y listado de brechas explícitas | Igualdad de rendimiento entre hardware o scheduler distinto |

## Ejecución sobre Fedora KDE físico

Desde una sesión gráfica Plasma local, con GCC, Make, MinGW-w64,
Bubblewrap y Python 3 instalados, y con el mismo commit que se está
probando:

```bash
bash scripts/fedora-stage4-multigroup-acceptance.sh
# evidencia: build/stage4-physical-multigroup/{fedora-stage4-facts.txt,fedora-stage4.log}
```

El script **no pide sudo**, no cambia SELinux, no desactiva Bubblewrap y
no altera la política global del anfitrión. Si detecta virtualización,
sólo una sesión no Plasma, menos de 65 CPUs **permitidas** o menos de
dos grupos reales, termina con `INCOMPLETE` (exit 3). La
identificación de hardware físico por `systemd-detect-virt` es una
**heurística**, no prueba forense suficiente. El director debe verificar
independientemente que el dispositivo es físico. El script rehúsa
certificar la transición si el test no muestra `cross-group=yes`, o
si no se ejecuta la prueba aislada PE con Bubblewrap.

Un equipo con más de 64 CPU físicas pero cpuset limitado por su
administrador a 32 no supera el criterio: la aplicación **no tiene
acceso** a esos procesadores. Una máquina con CPUs Linux 0 y 64
pertenece a dos grupos `cpu_id/64`, pero eso **no** acredita más
de 64 CPUs; se exigen ambos requisitos.

Las prioridades multihilo pueden registrar
`PRIORITY_MULTITHREAD_POSITIVE=SKIP_NO_CAPABILITY` sin privilegios
`CAP_SYS_NICE` o `RLIMIT_NICE`; una prueba de afinidad válida no
convierte ese SKIP en validación positiva de prioridad.

## Ejecución sobre Windows auténtico

Con Microsoft Visual C++ Build Tools y Windows SDK instalados, en
PowerShell:

```powershell
./scripts/windows-run-stage4-scheduler.ps1 -RequireMultiGroup -RequirePhysical
# evidencia: build/stage4-windows-native.txt y build/stage4-windows-meta.txt
```

Sin las opciones estrictas, el script también funciona en el runner
Windows de la CI, pero un equipo de un grupo registra
`CROSS_GROUP=SKIP`; su prueba no puede cerrar el caso >64 CPU.
`-RequirePhysical` rechaza modelos de VM conocidos, pero
**no certifica** físicamente el hardware. Es necesaria verificación
independiente del dispositivo. La ejecución nativa y su compilación
MSVC prueban únicamente comportamiento de Windows; no ejecutan el
runtime Linux.

Además, `scripts/windows-run-stage4-scheduler.ps1` compila con
`/Dimport_fixture_entry=main` la misma fuente
`tests/pe/thread_group_affinity_fixture.c` que Linux ejecuta como
PE controlado. Guarda su código de salida en
`build/stage4-windows-linux-fixture.txt`; el resultado
`DIVERGED` **no se transforma en éxito** ni hace fallar la recolección
de evidencias del sistema Windows. Primero se revisa la suposición
exacta del fixture y se contrasta con la documentación oficial.

Con ambas evidencias físicas verificadas:

```bash
python3 scripts/compare-stage4-scheduler.py \
  --windows build/stage4-windows-native.txt \
  --fedora build/stage4-physical-multigroup/fedora-stage4-facts.txt \
  --require-multigroup --output build/stage4-comparison.json
```

Comparar los **contratos** y los errores, no los números de CPU
del sistema ni sus tiempos absolutos. Si faltan observaciones o
permanecen diferencias críticas, el informe debe etiquetarse
`NOT_DEMONSTRATED` aunque todas las pruebas funcionales disponibles
hayan pasado. No publicar porcentajes como medición de compatibilidad.

## Primer contraste real del mismo fixture (Windows CI, 16-11-2026)

La ejecución nativa Windows de la fuente compartida
`tests/pe/thread_group_affinity_fixture.c` devolvió primero **19**.
La causa fue una expectativa **demasiado estricta** del fixture:
ante un puntero `NULL` en `SetThreadGroupAffinity` y
`GetThreadGroupAffinity`, Windows nativo devolvió `ERROR_NOACCESS`
(**998**), mientras el backend Linux devuelve
`ERROR_INVALID_PARAMETER` (**87**). Las llamadas fallan en ambos
casos. Se ajustó el fixture para comprobar fracaso y aceptar ambos
errores; no se modificó el runtime para falsificar la respuesta
nativa. Evidencia: [GitHub Actions 43841045953](https://github.com/AlexDevJSC/144mb/actions/runs/43841045953),
job Windows nativo (4 CPU, un grupo). Una coincidencia posterior
del fixture no constituirá equivalencia de todos los códigos
`LastError` ni comportamiento multigrupo.

## Implementación incremental del grupo principal

Se ha añadido al runtime una identidad `primary_group` separada del
cpuset Linux completo para hilos invitados propios. Esta evolución
permite que las llamadas de afinidad **de hilo** trabajen sobre el
grupo principal incluso cuando el cpuset permite ejecutar en varios
grupos. Se han incorporado consultas de grupos realmente asignados con
`GetProcessGroupAffinity` y una política acotada de proceso por grupo
principal que permite `GetProcessAffinityMask` y, bajo transacción
verificable de TIDs guest, `SetProcessAffinityMask` en el caso
inicial multigrupo sin cambios explícitos externos. Esto no equivale
a una implementación completa de CPU Sets, NUMA, grupos de procesadores
compactos de Windows ni hotplug. Sigue pendiente un mapa virtual
compacto de grupos para subconjuntos
Linux con CPUs no contiguas. Tampoco se ha probado el cambio entre
grupos físicamente distintos en CI. La diferencia con Windows 11 es
ahora más acotada, pero sigue sin cerrarse.

## Diferencias detectadas por revisión contractual

1. **Windows 11 / Windows Server 2022 y posteriores:** el runtime
   conserva un grupo principal por hilo y distingue este grupo de
   la afinidad completa de Linux. Admite grupos compactos en su cpuset
   inicial y operaciones sobre la máscara del grupo principal, incluidas
   pruebas sintéticas de 80 CPU. **No** emula todavía la política
   completa de CPU Sets, NUMA, particiones, hotplug o asignaciones
   multicgrupo reales contrastadas en Windows físico.
2. **Topología:** los procesadores Linux permitidos se enumeran como
   posiciones invitadas compactas independientes del ID del host.
   `CPU_SETSIZE` de glibc sigue acotando el perfil y el snapshot
   inicial no sigue los cambios de cpuset/hotplug. La topología
   compacta no pretende reproducir la organización NUMA de Windows.
3. **Boosts de prioridad:** Windows incluye aumentos dinámicos con
   reglas por tipo de evento y duración. Un backend **experimental**,
   opt-in y con privilegios reversibles, eleva en un paso `nice` de un
   hilo invitado tras un despertar real, verifica el efecto y restaura
   su prioridad base automáticamente. La prueba Linux positiva pasa
   en CI, pero no reproduce todas las fuentes ni curvas de NT.
   El perfil predeterminado conserva el rechazo explícito para evitar
   una promesa falsa de paridad.
4. **Quantum:** la misma fuente C de prueba mide reparto de tiempo CPU,
   contención y latencia de dos hilos fijados a una CPU en Windows
   nativo y Linux CI. Ambos informes y el comparador están integrados
   y se etiquetan `NT_QUANTUM_VERIFIED=NO`. Las medidas de CPU y
   latencia **no identifican** la longitud del quantum NT ni
   demuestran equivalencia de los planificadores.
5. **Hilos externos / broker:** `OpenThread` o un identificador del
   invitado **no** autoriza `pthread_setaffinity_np`,
   `setpriority` ni `pidfd_send_signal` sobre un PID/TID arbitrario
   del host. El broker propuesto requiere:
   - Canal AF_UNIX privado del sandbox con verificación `SO_PEERCRED`
     y pertenencia a la misma sesión de juego/runtime; identidad
     estable del proceso hijo mediante `pidfd`, nunca sólo PID.
   - Tabla central de objetos con ID/generación no reutilizables,
     derechos `THREAD_QUERY_INFORMATION` y
     `THREAD_SET_INFORMATION` y separación entre handles por
     proceso. Verificación de propietario, ACL/token NT (no disponible)
     **antes** de seleccionar un TID.
   - Copia/verificación de parámetros, límites de mensaje, rechazo de
     destinos no inscritos y carreras de cierre/terminación. Mantener
     SELinux y namespace del host; ninguna función que dé permisos
     adicionales a partir de valores del invitado.
   - Negativa explícita `NOT_SUPPORTED` hasta que exista broker
     autenticado y tests de aislamiento. Esta dependencia pertenece
     transversalmente al futuro problema 3 (objetos entre procesos);
     sólo se documenta la frontera en el problema 6.

## Modelo compacto de grupos sobre cpuset disperso (implementado y probado en CI)

El snapshot al arrancar enumera los procesadores Linux permitidos en orden
ascendente y les asigna índices **guest densos** `0..N-1`, independientemente
del identificador físico Linux. Windows `GROUP_AFFINITY`,
`GetActiveProcessorGroupCount`, `GetActiveProcessorCount`,
`GetCurrentProcessorNumber[Ex]`, `Get/SetThreadGroupAffinity`,
`SetThreadAffinityMask` y la política de proceso traducen mediante
dos tablas inmutables: `guest_ordinal → linux_cpu_id` y
`linux_cpu_id → guest_ordinal`.

Ejemplo: los 80 procesadores Linux permitidos con IDs 24–63, 72–103 y
200–207 se presentan como 64 CPU en grupo guest 0 y 16 en grupo guest 1.
La posición guest (grupo 1, bit 0) se traduce realmente al CPU Linux 96;
el CPU Linux 200 se presenta como (grupo 1, bit 8). Una selección de CPU
no anunciada tras la captura se rechaza (no se concede acceso al host).
No se modifica SELinux ni la política de CPU del proceso ajeno.

La prueba `process-group-synthetic-80cpu-test` intercepta las consultas
y operaciones de afinidad sobre **este test exclusivamente**, comprueba
enumeración, traducción en ambos sentidos, grupo principal y masks. Las
pruebas de afinidad reales usan Linux CPUs permitidos pero comparan la
numeración invitada compacta. Ello comprueba el contrato del mapeo sin
ser una prueba de hardware físico >64 CPU. La topología fija todavía no
gestiona hotplug, CPU Sets, NUMA ni numeración exacta de Windows sobre
sistemas con distinta topología. No cambia por sí sola el avance de
prioridades dinámicas, quantum o broker de hilos externos.

## Boost dinámico Linux medible, condicionado (experimento 16-11-2026)

El backend opcional `NT_NATIVE_EXPERIMENTAL_BOOST=1` activa el
mecanismo de incremento **real**, en un paso de `nice` Linux, al
despertar de una espera Win32/NT que efectivamente bloqueó el hilo
invitado y terminó satisfactoriamente. Un hilo auxiliar del mismo
runtime, con acceso exclusivo a la tabla de hilos invitados bajo
`wait_lock`, devuelve automáticamente la prioridad base tras
20 ms de tiempo monotónico, incluso cuando el invitado queda
ejecutando código CPU-bound. El monitor nunca acepta un PID/TID
del invitado para `setpriority`. No emite señales, no habilita
`SCHED_FIFO/SCHED_RR`, no se ejecuta con permisos adicionales ni
interfiere intencionadamente con hilos externos al runtime.

El backend solo se considera disponible cuando Linux permite
restaurar el valor de `nice` (CAP_SYS_NICE o `RLIMIT_NICE` adecuado),
se verifica el valor realmente aplicado y se ha solicitado mediante
la variable de entorno. De otro modo los controles Win32
`Get/SetThreadPriorityBoost` y `Get/SetProcessPriorityBoost`
continúan devolviendo `ERROR_NOT_SUPPORTED`, sin salidas inventadas.
La desactivación restaura los boosts en curso y comprueba sus efectos.
Las consultas de prioridad de base no muestran el boost transitorio.
Cambiar la clase o base mientras exista un boost en ejecución falla
expresamente para no sobrescribir una prioridad que debe restaurarse.
Cualquier error de rollback interrumpe nuevas activaciones.

`tests/dynamic_priority_boost_test.c` mide la transición real
`nice_base → nice_base-1 → nice_base` del mismo TID del hilo
invitado después de una espera de evento NT. El job Linux de CI
exige la ruta **positiva** bajo `sudo prlimit --nice=40:40`;
las ejecuciones ordinarias no privilegiadas pueden registrar `SKIP`
y no cuentan como aceptación positiva.

Esto es una **aproximación limitada del scheduler de Windows**,
no el algoritmo de boost/decay escalonado NT, no cubre todas
las fuentes de boost (entrada UI, I/O prioritario, MMCSS, etc.)
y **no equivale a quantum NT**. El experimento no está habilitado
por defecto y no justifica por sí solo paridad del sistema.

## Medición diferencial de competición sobre una CPU

`tests/bench/scheduler_quantum_observation.c` es la misma fuente C
compilada con MSVC en Windows nativo y GCC en Linux. Ejecuta dos
hilos CPU-bound con afinidad real a una única CPU accesible y registra
tiempo de CPU de cada hilo, tiempo de pared, razón de equidad y
latencia desde la liberación de una barrera hasta la primera ejecución.
`scripts/compare-stage4-quantum.py` valida ambos informes, rechaza
mediciones nulas o esquemas inconsistentes y genera un JSON cruzado
(`scheduler-quantum-differential` en CI). Ambos artefactos preservan
`NT_QUANTUM_VERIFIED=NO`. Una ejecución de éxito es únicamente
evidencia de medición: el intervalo de quantum, reglas NT, escalado
por prioridad y comparabilidad entre CPU y entornos **no** están
demostrados. No se altera el scheduler del anfitrión ni se pide
`CAP_SYS_NICE` para este benchmark.

## Paso 6: frontera IPC autenticada para hilos de procesos invitados

Se añade `src/runtime/thread_broker_auth.c` como **contrato Linux
interno** de acceso read-only, sin integrarlo aún en `OpenThread` ni
conceder HANDLE NT de procesos remotos. Es una dependencia transversal
mínima para un futuro registro central de objetos NT, no la implementación
completa de ese broker.

- Un proceso padre registra un hijo a partir de identidad local de
  `fork`/lanzamiento legítimo y guarda un `pidfd` abierto y vivo. Jamás
  adquiere autoridad interpretando un PID/TID recibido de una aplicación.
- Un `AF_UNIX/SOCK_SEQPACKET` cerrado al `exec` recibe mensajes acotados
  con credenciales **SCM_CREDENTIALS por mensaje**. A diferencia de
  `SO_PEERCRED` en un socketpair establecido antes del fork, estas
  credenciales reflejan el emisor real del mensaje. Compara PID/UID/GID
  con el hijo registrado, su pidfd vivo y un secreto aleatorio de 128 bits
  procedente de `getrandom` sin fallback predecible.
- El protocolo fija versión, formato de 32 bytes y operación de consulta
  de identidad. Únicamente concede derechos
  `THREAD_QUERY_INFORMATION`/`THREAD_QUERY_LIMITED_INFORMATION`
  que el registro padre ya permitió. Rechaza escrituras y peticiones
  desconocidas, no escribe la salida en fallo y acota la recepción a 1 s.
  No produce un HANDLE o TID utilizable fuera del broker.
- `tests/thread_broker_auth_test.c` crea dos hijos propios:
  un **impostor que conoce el secreto** y un hijo legítimo. Prueba rechazo
  por credenciales del impostor, paquete truncado, versión incorrecta,
  secreto falso, `THREAD_SET_INFORMATION`, consulta permitida y
  revocación tras cierre del pidfd. No altera el scheduler de ninguno.
  Se ejecuta en `make check`, Fedora RPM y UBSan.

**Pendiente:** integrar este transporte con los procesos PE hijos
gestionados, tabla de objetos y generaciones, concesión y cierre de
handles distintos por proceso, NT ACL y tokens, ciclos de vida y pruebas
de sesión aislada. No declarar acceso a hilos remotos implementado ni
levantar la restricción actual de `OpenThread`; los PID/TID del host
aportados por el invitado siguen siendo denegados. No pasar todavía a
APC avanzadas (problema 5).

## Referencias técnicas, no evidencia de hardware

- [Microsoft: Processor Groups](https://learn.microsoft.com/en-us/windows/win32/procthread/processor-groups)
- [Microsoft: SetThreadAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)
- [Microsoft: GetProcessAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask)
- [Microsoft: GetThreadGroupAffinity](https://learn.microsoft.com/en-us/windows/win32/api/processtopologyapi/nf-processtopologyapi-getthreadgroupaffinity)
- [Microsoft: Priority Boosts](https://learn.microsoft.com/en-us/windows/win32/procthread/priority-boosts)

**Estado de entrega:** instrumentos de evidencia creados, no
comparación física ejecutada. No aceptar 100 % de componente 4 ni
pasar a APC avanzadas basándose en esta PR.

## 16-11-2026: Superficie Win32 de control de prioridad dinámica (fail-closed)

Se registran en KERNEL32/KERNELBASE las cuatro entradas
`GetThreadPriorityBoost`, `SetThreadPriorityBoost`,
`GetProcessPriorityBoost` y `SetProcessPriorityBoost`.
**No se considera implementado el mecanismo de boosts.** El runtime
comprueba los derechos del HANDLE de hilo/proceso auténtico y no
interpreta PID/TID externos; los métodos de lectura no modifican la
salida si fallan y los de escritura no alteran el planificador Linux.

En un HANDLE local válido con permisos, todos devuelven
`FALSE / ERROR_NOT_SUPPORTED`. Los handles sin derechos reciben
`ERROR_ACCESS_DENIED` y un output NULL en lectura recibe
`ERROR_INVALID_PARAMETER`. El éxito sin cambios reales sería
incompatible con el contrato dinámico de Windows: activación durante
entrada, UI y terminación de espera, y descenso posterior hacia
prioridad base. No basta con almacenar `disabled=TRUE` en una tabla
ni con asignar un valor `nice` inmutable.

Pruebas: `tests/priority_boost_boundary_test.c` valida derechos,
outputs sin mutación y conservación del `nice` del host.
`tests/pe/priority_boost_boundary_fixture.c` verifica imports,
ABI Microsoft AMD64, errores y vida útil de hilo suspendido en Linux.
Ambas rutas son parte de CI/UBSan/Fedora. Windows nativo mantiene su
prueba positiva independiente; **no** se utiliza el fixture negativo
como una prueba de paridad con Windows. El quantum y el broker siguen
pendientes. Componente 4: 95 % → 95 % (+0 pp), porque estas APIs todavía
no ofrecen compatibilidad funcional de planificación.
