# Orden vinculante de desarrollo del núcleo NT/Win32

Decisión de dirección del proyecto — 13 de noviembre de 2026.
Este archivo fija el orden de prioridad para **los siete componentes críticos del núcleo NT/Win32** definidos en [nt-core-readiness.md](nt-core-readiness.md). No reemplaza los siete dominios de arquitectura general ni los trabajos de gráficos, integración KDE o pruebas.

## Secuencia aprobada

| Fase | Identificador de componente | Componente | Prioridad |
| --- | --- | --- | --- |
| 1 | **4** | **Procesos e hilos** | **Actual: máximo desarrollo alcanzable antes de cambiar la prioridad** |
| 2 | **5** | Memoria virtual | Después de fase 1 |
| 3 | **2** | PEB/TEB | Después de fase 2 |
| 4 | **3** | Objetos NT | Después de fase 3 |
| 5 | **1** | Cargador PE y DLL | Después de fase 4 |
| 6 | **6** | Excepciones SEH/VEH | Después de fase 5 |
| 7 | **7** | API NTDLL/Win32 | Después de fase 6 |

**Orden obligatorio: 4 → 5 → 2 → 3 → 1 → 6 → 7.**

La expresión «por completo» es una meta de desarrollo, no una declaración de que pueda alcanzarse una paridad universal con Windows en un único cambio. Antes de cambiar de prioridad, desarrollar el componente activo hasta el máximo viable y medible. Si un requisito de un componente anterior depende de otro componente todavía no priorizado, implementar **únicamente la interfaz mínima transversal necesaria**, sin trasladar silenciosamente la prioridad principal. Documentar los bloqueos irresolubles, los riesgos de seguridad y los casos sin prueba antes de avanzar. Nunca declarar un componente «100 %» por contar importaciones, superar CI o pasar un fixture controlado.

## Seguimiento porcentual obligatorio en cada entrega

Tras **cada entrega con cambios en GitHub** (PR, fusión o commit que se comunique al usuario), la respuesta final debe especificar el componente del núcleo NT/Win32 afectado y presentar explícitamente:

- **Porcentaje anterior → porcentaje actualizado (variación en puntos porcentuales)**, p. ej. «35 % → 38 % (+3 puntos)».
- **Razón concreta del cambio**: contratos añadidos o corregidos, evidencia de pruebas y límites que permanecen.
- **Referencia verificable** del cambio (PR o commit) y estado de CI. No afirmar que las pruebas han pasado cuando sigan pendientes.
- **Si la entrega no mejora funcionalidad** (documentación, refactor sin nuevas garantías o prueba fallida), reflejar «sin cambio (+0 puntos)». No forzar aumentos artificiales.
- Si un cambio afecta a varios componentes, registrar el antes/después de cada uno, manteniendo **4** como prioridad principal mientras dure su fase.

Los porcentajes son **estimaciones de madurez técnica** y no una medición automática de compatibilidad con programas Windows; el margen de error del diagnóstico inicial es considerable. No confundir «API exportada», «test verde» o «PR fusionada» con compatibilidad universal. El porcentaje debe revisarse a partir de cobertura de contratos, pruebas negativas, concurrencia, manejo de errores y comportamiento de software real.

### Línea base del seguimiento

Estimación registrada tras la integración de la PR #68, **13 de noviembre de 2026**, usando como punto de partida los porcentajes de la última evaluación arquitectónica. No se asigna retrospectivamente un incremento numérico a la PR #68 porque no se realizó una reevaluación ponderada de sus criterios en ese momento.

| Componente | Línea base estimada | Estado |
| --- | ---: | --- |
| 1. Cargador PE y DLL | 55 % | En espera de su fase |
| 2. PEB/TEB | 40 % | En espera de su fase |
| 3. Objetos NT | 30 % | En espera de su fase |
| **4. Procesos e hilos** | **35 %** | **Línea base histórica; fase actual activa** |
| 5. Memoria virtual | 35 % | Siguiente fase |
| 6. SEH/VEH | 25 % | En espera de su fase |
| 7. API NTDLL/Win32 | 20 % | En espera de su fase |

A partir de esta línea base, toda variación porcentual debe anotarse junto a la entrega que la justifique. Si el trabajo revela limitaciones previamente desconocidas o regresiones, **el porcentaje también puede disminuir**.

### Registro de evolución

- **13-11-2026, PR #71 y PR #72 fusionadas: componente 4 — 35 % → 55 % (+20 puntos porcentuales estimados).** Nuevas rutas de apertura e identidad de procesos/hilos, `NtCreateThreadEx`, terminación antes de entrada de hilo, terminación acotada de hijos por pidfd, `CREATE_SUSPENDED` de procesos hijo mediante puerta del lanzador, nombres completos UTF-16 y pruebas de ciclo de vida. Quedan especialmente suspensión asíncrona y contexto de hilos activos, herencia de handles, seguridad/token NT, servicios generales entre procesos, planificación, interacciones complejas de APC y tests diferenciales Windows. **Esta mejora es de madurez del subconjunto, no de rendimiento (no hay benchmark 2×).** Pruebas Linux y Fedora de la rama de PR #72: exitosas en [CI 43341757354](https://github.com/AlexDevJSC/144mb/actions/runs/43341757354). Este resultado no valida los casos explícitamente pendientes ni los tests omitidos por limitaciones del entorno.

- **13-11-2026, PR #74 fusionada — afinidad y ciclo de vida: 55 % → 60 % (+5 puntos porcentuales estimados, CI validada).** `SetThreadAffinityMask`, `NtSetInformationThread` clase 4 y `GetProcessAffinityMask` aplican y consultan afinidad real Linux en el perfil representable de 64 CPU, con derechos y errores explícitos; prueba de 48 hilos y alias. Sigue pendiente la compatibilidad con múltiples processor groups, prioridades y contextos de hilos activos. Resultados: [Linux, UBSan y Fedora correctos (run 43346528358)](https://github.com/AlexDevJSC/144mb/actions/runs/43346528358). Prueba positiva de afinidad Linux y 48 ciclos de vida ejecutada sin SKIP; ello no demuestra grupos Windows múltiples ni planificación completa.

- **13-11-2026, PR #76 fusionada — planificación y tiempos: 60 % → 66 % (+6 puntos porcentuales estimados).** Añade prioridades reales de hilos por `nice` con derechos, `GetThreadTimes` y `GetProcessTimes` con FILETIME y tiempos CPU de Linux. No se incluyen contextos arbitrarios, suspensión asíncrona ni herencia general. [CI Linux/UBSan/Fedora superada (run 43483478807)](https://github.com/AlexDevJSC/144mb/actions/runs/43483478807).

- **13-11-2026, PR #78 integrada — consultas nativas: 66 % → 68 % (+2 puntos porcentuales estimados).** `NtQueryInformationThread(ThreadBasicInformation)` deja de devolver prioridad 8 y afinidad 1 ficticias; consulta el planificador Linux para hilos activos y usa una captura final del propio hilo para consultas posteriores a su terminación. El perfil sigue acotado a CPU group 0 y valores de prioridad representables.

- **13-11-2026, PR #79 fusionada — creación de hilos en el proceso propio: 68 % → 70 % (+2 puntos porcentuales estimados, CI Linux/UBSan/Fedora correcta).** `CreateRemoteThread`, `CreateRemoteThreadEx` y `NtCreateThreadEx` aceptan el proceso actual a través de handles reales con `PROCESS_CREATE_THREAD`; nunca habilitan la inyección en otro proceso Linux o Windows, ni atributos extendidos no implementados. Se prueban creación suspendida y derechos.

- **13-11-2026, preparación de pruebas físicas Fedora (esta PR): 70 % → 70 % (+0 puntos).** Se añade `scripts/fedora-check-stage4.sh`, que ejecuta pruebas de runtime Linux, PE Windows, UBSan y procesos hijo aislados, y rechaza explícitamente suites omitidas (`SKIP`). Un script de pruebas no añade contratos al runtime y no justifica aumentar el porcentaje.

- **14-11-2026, fase 4 — flags y vida útil de handles (PR en curso): 70 % → 72 % (+2 puntos estimados, sujeto a CI).** `SetHandleInformation`, `GetProcessHandleCount` y cierre protegido por `HANDLE_FLAG_PROTECT_FROM_CLOSE` para handles de archivos/objetos NT; duplicación con handles reales del proceso propio bajo `PROCESS_DUP_HANDLE`. Tests PE verifican protección, independencia entre alias, errores y cierre. La herencia de handles entre procesos y el broker NT siguen pendientes.

### Formato de cierre para cambios futuros

> **Componente 4 — Procesos e hilos:** 35 % → XX % (**+N puntos porcentuales**, estimación).\
> **Avances:** [contratos demostrados].\
> **Pendientes:** [limitaciones reales].\
> **Verificación:** [pruebas ejecutadas, resultado y enlace a PR/CI].

## Criterios de avance y salida de fase

1. Listar contratos implementados, contratos sin implementar, supuestos y limitaciones explícitas.
2. Verificar ABI AMD64, NTSTATUS/LastError, derechos de acceso, referencias, fugas, concurrencia, carrera de cierre y estados de terminación.
3. Incorporar PE controlados que prueben tanto casos positivos como negativos, y comparación con Windows real cuando sea factible.
4. Ejecutar suites de Linux, UBSan y Fedora, y pruebas manuales necesarias en Fedora KDE. Una prueba omitida (SKIP) no cuenta como validación.
5. Revisar el aislamiento y no degradar las capacidades que ya funcionan de Mines y los fixtures externos.
6. Registrar evidencia y bloqueos pendientes por fase. Sólo pasar a la siguiente fase si el máximo desarrollo razonablemente validable se alcanzó, o si existe un bloqueo explícito que no puede levantarse dentro de esta fase.

## Fase activa: 4 — procesos e hilos

Estado de partida documentado: procesos PE hijo acotados, handles de hilo, creación suspendida, TLS/FLS, APC de usuario acotadas y esperas alertables.

Backlog específico de la fase, sin asumir que todos los puntos sean realizables de inmediato:

- Suspensión/reanudación y conteos, incluido el tratamiento seguro del estado inicial frente a suspensión asíncrona de un hilo ejecutándose.
- Vida útil de hilos y procesos; handles cerrados/duplicados; consistencia de IDs, códigos de salida y permisos.
- Contextos de hilo, acceso a hilos externos, cancelación, terminación y semántica de APC que aún no cubrimos.
- Herencia de handles, entorno y parámetros de procesos hijo, coordinación y observación entre procesos.
- Pruebas de concurrencia y cierre, creación fallida, carreras entre el lanzador y sus hilos y respuesta coherente de errores.

**Regla de seguridad:** no intentar suspender un pthread arbitrario mediante señales de Linux ni presentar un mecanismo cooperativo como suspensión asíncrona general de Windows. Fallar explícitamente cuando no se pueda mantener el contrato.

En cada PR del componente activo actualizar este archivo o [nt-core-readiness.md](nt-core-readiness.md) con nuevos logros, pruebas y límites. Al cerrar la fase 4, priorizar **memoria virtual (5)** sin volver a reorganizar la secuencia salvo nueva instrucción del responsable del proyecto.
