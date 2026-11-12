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
