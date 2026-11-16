# Etapa 4 / problema 6: aceptación Fedora físico, multigrupo y Windows diferencial

**Responsable:** núcleo NT/Win32, procesos e hilos. **Estado:** instrumentado;
hardware físico y paridad de planificación **NO verificados** en esta entrega.
Esta comprobación no habilita la transición al problema 5 (APC avanzadas).

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

1. **Windows 11 / Windows Server 2022 y posteriores:** procesos e
   hilos pueden abarcar por defecto todos los grupos. Las APIs
   tradicionales operan sobre el **grupo principal** del hilo o
   proceso. `GetThreadGroupAffinity` describe el grupo principal, no
   necesariamente toda la afinidad transgrupo del hilo. El prototipo
   Linux actualmente **rechaza** una afinidad que abarque varios
   `cpu_set_t` grupos, porque no conserva un grupo primario separado.
   La prueba con más de 64 CPUs debe detectar esta divergencia, no
   adjudicarle un PASS de paridad.
2. **Topología:** `group = linux_cpu_id / 64` es una convención interna,
   no garantiza numeración compacta ni correspondencia con topología
   Windows en hardware/VM distinto. `CPU_SETSIZE` de glibc limita el
   perfil actual. La fotografía inicial del cpuset no sigue hotplug.
3. **Boosts de prioridad:** Windows dispone de aumento dinámico de
   prioridad ante entrada, finalización de espera, etc., con controles
   `Get/SetThreadPriorityBoost` y `Get/SetProcessPriorityBoost`.
   `nice` de Linux es por TID y estático para el perfil existente.
   **No implementar un booleano que afirme que controla boosts cuando
   Linux no aplica un mecanismo observable equivalente.** El control
   de Windows se prueba en un hilo de prueba; nuestro runtime sigue
   `UNSUPPORTED`.
4. **Quantum:** `QueryThreadCycleTime` y `GetThreadTimes` miden trabajo,
   no el quantum de CPU del planificador de Windows. Linux
   `sched_rr_get_interval` y su política `SCHED_RR` tampoco
   constituyen traducción general de las reglas del planificador NT.
   Se necesita diseño y medición diferencial de latencia/reparto en
   tareas controladas antes de ofrecer contrato de quantum.
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

## Modelo compacto de grupos sobre cpuset disperso (implementación propuesta)

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

## Referencias técnicas, no evidencia de hardware

- [Microsoft: Processor Groups](https://learn.microsoft.com/en-us/windows/win32/procthread/processor-groups)
- [Microsoft: SetThreadAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)
- [Microsoft: GetProcessAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask)
- [Microsoft: GetThreadGroupAffinity](https://learn.microsoft.com/en-us/windows/win32/api/processtopologyapi/nf-processtopologyapi-getthreadgroupaffinity)
- [Microsoft: Priority Boosts](https://learn.microsoft.com/en-us/windows/win32/procthread/priority-boosts)

**Estado de entrega:** instrumentos de evidencia creados, no
comparación física ejecutada. No aceptar 100 % de componente 4 ni
pasar a APC avanzadas basándose en esta PR.
