# Core4 — contrato comprobable de APC especial pública y restauración AMD64

**19-11-2026.** Fase arquitectónica **4 (Procesos e hilos)**,
problema interno **5 (APC avanzadas)**. Reutiliza las rutas YA existentes
de `QueueUserAPC2` / `NtQueueApcThreadEx`, el import-return gate,
`guest_async_apc.c` y el supervisor de pruebas. **No introduce una
segunda implementación del planificador ni un enlace público a ptrace.**

## API y autoridad

- Win32 `QueueUserAPC2(PAPCFUNC, HANDLE, ULONG_PTR, flags=1)`
  devuelve `TRUE` únicamente cuando una APC especial se puede encolar.
  Ante HANDLE sin `THREAD_SET_CONTEXT`: `FALSE` /
  `ERROR_ACCESS_DENIED`. Una dirección que no está en el código
  ejecutable pinneado por el cargador: `FALSE` /
  `ERROR_NOT_SUPPORTED`. Se conservan los demás errores y el
  comportamiento `flags=0` de APC ordinaria.
- NT `NtQueueApcThreadEx(thread, reserve=1, routine, context, first,
  second)` es la contraparte nativa del mismo perfil. Sin la
  autorización y un TEB de hilo invitado, falla en vez de degradar la
  solicitud a APC normal.
- El objetivo debe ser un hilo PE perteneciente al proceso invitado
  actual. No se aceptan hilos Linux ajenos, PID/TID aportados como
  autoridad, callbacks ELF, JIT o DLL cargadas dinámicamente sin
  código ya autenticado.
- La cola admite 64 APC por hilo; la APC ordinaria no se consume
  durante entrega especial ni por esperas no alertables.

## Ejecución e integridad del contexto

Sobre un hilo PE con fase GUEST demostrada, la entrega puede hacerse
al retornar de un import Win64 sin locks host ni mediante un nudge
dirigido a su `pthread_t` controlado. En la vía de señal, el handler
solo captura el `ucontext`, comprueba el RIP invitado y transfiere el
retorno del kernel a una pila privada. El callback se ejecuta **fuera**
del handler y de `wait_lock`. El código existente reconstruye GPR,
FXSAVE y extensiones SIMD permitidas, y reanuda en RIP/RSP originales.

La nueva ampliación de `tests/pe/async_special_apc_loop_fixture.c`
mantiene registros RBX, R12–R15 y XMM6 con patrones específicos durante
un único bucle de instrucciones PE **sin imports ni syscalls**. Una
`QueueUserAPC2(...,1)` real desde otro hilo debe interrumpir ese
bucle, ejecutar la rutina solo en el TID autorizado y devolver
exactamente los valores en el código PE. La rutina también altera
deliberadamente un bit válido de redondeo MXCSR en su pila privada;
se exige que el hilo interrumpido observe su MXCSR original tras
reanudar. El código de salida 106 denuncia corrupción de GPR/XMM/MXCSR.
Una APC ordinaria encolada en paralelo **no** debe ejecutarse.

Las comprobaciones de registros son **un subconjunto observable** y
no prueban por sí solas todas las configuraciones CPU de XSAVE,
registros DEBUG, AVX/AMX o una equivalencia universal de CONTEXT NT.
Los fallos de entrega no se convierten en `PASS` por un callback
recibido en una importación posterior.

## Cancelación, restauración ambigua y separación del supervisor

El supervisor experimental `nt_thread_supervisor.c` puede detener
solamente un hilo de un **hijo directo de pruebas anclado a pidfd**
en un RIP PE autorizado. Verifica lectura/escritura real de GPR y
`NT_X86_XSTATE` antes/después del desvío de contexto, usa una pila
independiente y exige readback exacto de la restauración.

- Cancelación previa a mutaciones: `ECANCELED` sin ejecución.
- Cancelación posterior, RPC desconectado, excepción no contractual,
  confirmación de callback incorrecta o XSTATE restaurado diferente:
  no reanuda el tracee; cuarentena y petición de terminación solo
  del `pidfd` comprometido.
- Estas barreras están **probadas en el supervisor**, no dentro de
  la cola pública. No existe todavía un `CancelSpecialUserAPC`
  equivalente ni una integración genérica de `ptrace` con
  `QueueUserAPC2` para aplicaciones de terceros. Los tests no
  deben dar a entender lo contrario.

## Prueba reproducible

```sh
make -j2 check-core4-public-special-apc-contract
```

Descompuesto por frontend: `make -j2 check-pe-async-special-apc
check-pe-async-special-apc-seh` para las APIs públicas, y
`make -j2 check-pe-thread-supervisor-apc-failure
check-pe-thread-supervisor-apc-xstate-failure
check-pe-thread-supervisor-apc-guards` para los fallos/cancelación
del supervisor experimental. En cada caso debe ejecutarse un PE
real generado por el build, no una DLL propietaria ni un programa
Windows desconocido. Validación adicional en Linux:
`make -j2 check check-pe`; cuando haya MinGW:
`make check-pe-import`; repetir UBSan según `AGENTS.md`.

**Evidencia necesaria antes de aceptar:** CI Linux/PE real con
resultado de cada subtarget, supervisor experimental separado y
regresión de permisos negativos. Fedora 45 en contenedor y Windows
hosted NO acreditan un Fedora KDE físico con SELinux enforcing y
Bubblewrap sin SKIP, ni una política de Windows NT plenamente
replicada. Toda entrega debe documentar el SHA, el compilador y los
errores; sin ejecución de CI la suite solo es una propuesta verificable.

**Madurez de componente 4: 96 % → 96 % (+0 puntos)** antes de la
suite completa y diferencial. Esta contribución endurece cobertura
y definición de límites, no amplía la autoridad de la API.
