# Componente 4 / problema 5 — puente de excepciones hardware PE64

Fecha: 18-11-2026. Continuación de la entrega del despachador especial
en #181 y del saneamiento del ejecutor PE en #184. **Contrato experimental,
no afirmación de compatibilidad Windows universal.**

## Delimitación de autoridad

El lanzador confiable carga y valida los intervalos PE .text y el
directorio .pdata/.xdata antes de llamar a
`nt_runtime_enable_software_seh`. Solo entonces instala, una vez por
proceso, manejadores Linux SA_SIGINFO|SA_ONSTACK para
SIGSEGV/SIGILL/SIGFPE/SIGBUS/SIGTRAP. Su implementación vive en
`src/runtime/guest_hardware_fault.c` y NO es un import Win32.

Cada hilo PE obtiene **dos pilas anfitrionas dedicadas**, una para
señales y otra para el despacho de excepciones; la segunda evita
invocar Win64 o pthread desde un manejador asíncrono. Las pilas se
restituyen antes de destruir GS/TEB. Los fallos fuera de código PE
fijado, durante un import ELF, sin TEB válido o en una sesión
comprometida se rechazan con terminación irreversible de *ese proceso
invitado*. No se interrumpe un hilo ajeno ni se manipula su pthread.

El handler únicamente verifica el origen kernel `si_code>0`, el
`RIP` perteneciente al código PE autorizado y la fase TLS GUEST
publicada antes de cada entrada y salida de import; copia GPR,
`siginfo` y el XSAVE entregado por el kernel a almacenamiento TLS
prealocado y reescribe `ucontext` para retornar en una pila anfitriona
de despacho. Sin malloc, mutex, IPC, VEHs ni APIs Windows desde la
señal. Un `kill/tgkill` que finge SIGSEGV o SIGILL se rechaza.

## Mapeo y restitución

| CPU Linux | Excepción NT |
| --- | --- |
| SIGSEGV SEGV_MAPERR/SEGV_ACCERR | STATUS_ACCESS_VIOLATION 0xC0000005; `REG_ERR` distingue lectura, escritura y ejecución, `si_addr` indica dirección |
| SIGILL ILL_ILLOPC/ILLOPN/ILLADR/ILLTRP | STATUS_ILLEGAL_INSTRUCTION 0xC000001D |
| SIGILL ILL_PRVOPC/PRVREG | STATUS_PRIVILEGED_INSTRUCTION 0xC0000096 |
| SIGFPE INTDIV/INTOVF y errores flotantes reconocidos | NTSTATUS correspondiente a división, overflow o excepción FP |
| SIGBUS BUS_ADRALN | STATUS_DATATYPE_MISALIGNMENT 0x80000002 |
| SIGBUS BUS_ADRERR | STATUS_ACCESS_VIOLATION sin inventar parámetros de acceso |
| SIGTRAP BRKPT/TRACE | STATUS_BREAKPOINT / STATUS_SINGLE_STEP |

Los códigos positivos desconocidos, el origen usuario y la información
de contexto inexistente producen rechazo. El dispatcher convierte el
ucontext en CONTEXT AMD64, distingue **RIP hardware exacto** de la
dirección de retorno software `RIP-1`, y entrega la excepción a la
secuencia Windows existente: VEH → SEH por .pdata/.xdata →
UnhandledExceptionFilter. Si un handler elige una continuación o
`EXECUTE_HANDLER`, el runtime restaura **solo las extensiones SIMD**
(YMM/ZMM, según CPU) del XSAVE del kernel y luego aplica el
FXSAVE/CONTEXT Win64 aprobado por el handler y los GPR. Los componentes
de XSAVE que cambien otros estados (PKRU, AMX, MPX y no revisados) se
rechazan expresamente; no deben restaurarse desde una pila anfitriona. Cualquier unwind no local envenena la frontera de
imports para impedir nuevos paros `ptrace` que presuman un host frame
que ya no existe.

## Pruebas

```sh
make -j2 check-hardware-fault-classifier
make -j2 check-pe-hardware-seh
make -j2 check-pe-seh
make -j2 check-pe-apc check-guest-boundary
```

`tests/pe/hardware_seh_fixture.S` provoca **tres excepciones CPU
reales**, no llamadas artificiales a RaiseException:
1. UD2/illegal instruction.
2. DIV por cero y SIGFPE.
3. Lectura NULL y SIGSEGV, comprobando el
   `EXCEPTION_RECORD.ExceptionInformation` de acceso y dirección.

Cada función contiene su propia scope `__C_specific_handler` Win64 y
debe elegir `EXECUTE_HANDLER` para que el ejecutable retorne 0.
Además, el test de clasificador rechaza `SI_USER`, `SI_TKILL`,
códigos desconocidos y comprueba lecturas/escrituras/ejecución.

## NO resuelto

- **APC especial asíncrona en cualquier instrucción PE**: el despacho
  de APC públicas sigue limitado a retornos de importación verificados.
  Detener de forma universal otro TID requeriría un scheduler/NT kernel
  supervisor con estado por hilo, suspensión transaccional, estado
  completo de XSAVE, CET, TLS y retorno de excepción.
- **Unwind PE ↔ ELF arbitrario**: no se intenta caminar un frame ELF ni
  liberar locks Linux de una importación inesperada. Toda pérdida de
  frontera detiene el invitado. Esto es **denegación segura**, no
  implementación completa de SEH mixto.
- **SEH hardware de DLL dinámicas/JIT y todo código nativo**: solo se
  permite el ejecutable PE principal con alcance fijado por el
  lanzador. Módulos no fijados, páginas W→X y contextos alterados son
  rechazados.
- **XSTATE no SIMD o modificado por el handler**: PKRU, AMX, MPX y
  extensiones futuras se rechazan. El handler no puede editar YMM/ZMM
  por un CONTEXT Windows ampliado en este perfil.
- **Pruebas diferenciales Windows y hardware Fedora KDE**: un runner
  Ubuntu/Fedora en contenedor no las sustituye. SHSTK activo se rechaza
  en la puerta PE; no se pretende soporte CET.
- **Anidamiento de fallos hardware**: si un handler provoca otro fallo
  antes de restaurar el primero, se rechaza, no se fabrican stacks de
  excepciones reentrantes arbitrarios.

**Componente 4: 96 % → 96 % (+0)**. Un prototipo nuevo sometido a pruebas
no acredita equivalencia universal ni cierra el problema 5.
