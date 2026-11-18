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
make -j2 check-pe-thread-supervisor-apc-xstate-failure
```

Las tres pruebas de referencia iniciales usan PE32+ auténtico construido en la propia compilación Linux.
La primera valida ejecución de APC sobre hilo invitado, retorno a la
instrucción PE interrumpida y progreso sin espera alertable.
Las dos pruebas negativas usan macros **exclusivamente de compilación de
tests**. Una altera la comprobación de finalización después de un
INT3 real, con el GPR/XSTATE del invitado ya reemplazado. La otra
corrompe un byte del readback final de XSTATE después de las escrituras
de restauración. Ambas exigen error, prohibición de CONT/DETACH, solicitud SIGKILL aceptada por el
`pidfd` y mantenimiento de `PTRACE_O_EXITKILL` hasta terminar el trazador.
La prueba no finge que un `waitpid` intermedio con `WIFSTOPPED` sea
confirmación de muerte: esa comprobación terminal continúa pendiente.

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

## Continuación del problema 5 — cancelación y perturbaciones controladas

La variante `nt_supervisor_invoke_trusted_guest_apc_guarded` acepta **solo
desde el supervisor de pruebas** un indicador atómico de cancelación y
un descriptor de socket `SOCK_SEQPACKET` propiedad del supervisor.
Se evalúan sin adquirir mutex del invitado. El método anterior se mantiene
como wrapper sin guardas, y las exportaciones Win32 siguen intactas.

- **Cancelación antes de mutar:** devuelve `ECANCELED` sin terminar al
  hijo. El caller todavía conserva un thread stop de `ptrace` y solo
  puede reanudar después de comprobar que no se modificó su contexto.
- **Cancelación o cierre del peer tras mutar:** activa cuarentena,
  `pidfd_send_signal(SIGKILL)` y veto irreversible de CONT/DETACH.
  Ninguna devolución exitosa puede interpretarse como APC cancelada y
  reanudada sin probar el estado de GPR/XSTATE.
- **Señales y excepciones:** una señal real `SIGUSR1` dirigida al TID
  o una instrucción PE real `UD2` (stop `SIGILL`) durante la APC se
  tratan como finalizaciones **anómalas**, no como el INT3 contractual.
  Ninguna de esas pruebas demuestra entrega de SEH arbitraria.
- **Concurrencia:** la prueba PE de APC ordinarias crea cuatro threads
  Win32 reales simultáneamente, con dos APC diferentes por thread,
  comprobación de identidad y orden de cada cola. Es cobertura de la
  ruta ordinaria en producción, NO habilitación de APC especial
  concurrente sobre cuatro hilos del mismo proceso.

Pruebas nuevas:

```sh
make -j2 check-pe-thread-supervisor-apc-guards
make -j2 check-pe-apc
```

Los cinco subcasos del primer objetivo son cancelación posterior a
la mutación, desconexión del socket, `SIGUSR1` dirigido,
`UD2/SIGILL` del PE, y cancelación previa segura. Se ejecutan
mediante las compilaciones de pruebas del supervisor, nunca mediante
opciones del PE arbitrario.

**No resuelto todavía:** integrar una autorización por HWND/HANDLE
y RPC generacional para APC especial de Windows arbitrario, cancelación
en el broker real cuando RPC queda bloqueado, excepciones/SEH y unwind
del callback, restauración segura de stack/RSP para código externo,
procesos e hilos PE múltiples con APC especiales simultáneas y
terminal `waitpid` tras pérdida del trazador. Esos contratos requieren
otras fases de contexto/supervisión y no se implementan falsamente.

El nivel del componente 4 **permanece 96 % → 96 % (+0 pp)** hasta
cerrar un contrato Win32 general probado. La aceptación de Fedora
físico permanece separada de CI, como definía FileSystem 4.
