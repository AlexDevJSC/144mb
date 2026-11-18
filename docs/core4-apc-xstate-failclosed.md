# Componente 4 — problema 5: APC avanzada experimental, restauración o cuarentena

**18-11-2026 · Alcance: supervisor AMD64 de PE de laboratorio.**
No es una implementación de APC especiales genéricas de Win32. Las APIs
`NtQueueApcThreadEx` y `QueueUserAPC2` mantienen el rechazo seguro de
`SPECIAL_USER_APC` fuera de los contratos ya probados. No se activa el
supervisor `ptrace` para PE arbitrarios.

## Vulnerabilidad de ciclo de vida anterior

`nt_supervisor_invoke_trusted_guest_apc()` ya permitía al padre ejecutar
una APC en un PE de prueba sin espera alertable, sobre una pila de
invitado independiente y con una trampa INT3 controlada. Capturaba GPR
y `NT_X86_XSTATE`, pero después de `PTRACE_SETREGSET` el resultado de la
escritura no se cotejaba con `GETREGSET` y había caminos de error que
retornaban mientras el programa podía conservar RIP/RSP/XSTATE de la
APC inyectada. `nt_supervisor_release()` podía intentar desadjuntar
un hilo parado con registros no restaurados.

## Nuevo contrato acotado

- Identificar y detener solo un TID del hijo directo, bajo la frontera
  PE/runtime existente y con `pidfd` fijado por el supervisor.
- Antes de cambiar RIP/RSP: capturar XSTATE completo acotado a 64 KiB,
  comprobar que el kernel acepta escritura de la instantánea e
  inmediatamente volver a leerla. Si falta la garantía, cuarentena.
- Escribir la entrada y pila APC solo tras la validación. Leer de nuevo
  todo el GPR, comparar exactamente y denegar la ejecución ante una
  transformación inesperada del kernel.
- Al recibir INT3, comprobar RIP/GS/TEB de la región autorizada y la
  confirmación lógica de la APC. Restaurar **XSTATE y GPR** en el hilo
  detenido, verificar nuevamente ambos conjuntos y solo entonces CONT.
- Después de cualquier mutación ambigua, pérdida de traza, timeout
  de APC o señal inesperada, marcar `compromised` y solicitar SIGKILL
  **exclusivamente** sobre el `pidfd` del hijo identificado. No CONT,
  DETACH ni reutilización de TID; mantener `PTRACE_O_EXITKILL` hasta
  muerte comprobada del proceso o finalización del trazador.
- Un hijo que haya terminado realmente deja de ser un tracee activo;
  no inventar un contexto recuperable ni reanudarlo.
- Una señal que interrumpe `waitpid` con `EINTR` no equivale a pérdida
  de seguimiento: reintentar sin ampliar el plazo monotónico original.

## Pruebas y aceptación

```sh
make -j2 check-pe-thread-supervisor
make -j2 check-pe-thread-supervisor-apc-failure
```

Ambas usan PE32+ auténtico construido en la propia compilación Linux.
La primera valida ejecución de APC sobre hilo invitado, retorno a la
instrucción PE interrumpida y progreso sin espera alertable.
La segunda añade una macro **exclusivamente de compilación de tests**
que altera la comprobación de finalización después del INT3 real,
cuando el estado GPR/XSTATE ya ha sido reemplazado. La regresión
exige error, prohibición de CONT/DETACH, solicitud SIGKILL aceptada por el\n`pidfd` y mantenimiento de `PTRACE_O_EXITKILL` hasta terminar el trazador.\nLa prueba no finge que un `waitpid` intermedio con `WIFSTOPPED` sea\nconfirmación de muerte: esa comprobación terminal continúa pendiente.

El workflow experimental ejecuta ambas. Compilar no equivale a aprobar:
registrar PASS de la CI del último commit, y **INCOMPLETE** si
Bubblewrap/SELinux o la máquina física Fedora no permiten el ensayo.
No desactivar SELinux, aislamiento ni protecciones para hacer pasar
la suite. No se lanza ni instala nada en el equipo Fedora del usuario.

## Estado y pendientes

- **Madurez componente 4** (estimación histórica, escala FileSystem 4):
  **96 % → 96 % (+0 puntos)**. Se elimina una ruta de fallo insegura
  de la APC experimental pero **no** se cierra aún APC especial pública.
- Pendientes específicos del problema 5: restauración ante señales
  concurrentes con estado arbitrario, pila y trampolín general con
  retorno seguro, APC especiales de Win32, entrega no alertable
  de múltiples hilos y procesos, derechos y cancelación en desconexión,
  y referencia diferencial en Windows.
- Dependencias posteriores: CONTEXT AMD64 completo (problema 2),
  frontera de suspensión genérica (problema 1), seguridad y
  objetos NT entre procesos (problemas 4/3).
- **Puerta Fedora física:** pendiente y separada del trabajo de desarrollo;
  ninguna prueba omitida se presenta como aprobada.
