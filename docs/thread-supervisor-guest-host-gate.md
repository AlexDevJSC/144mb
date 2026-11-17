# Componente 4 — Problema 1: frontera PE ↔ runtime

## Superficie y contrato

El runtime NO ejecuta Win32 bajo un kernel Windows. El PE32+ AMD64
llama directamente a código máquina Linux `ms_abi` que puede tomar
mutexes y entrar en funciones nativas. Por tanto, la existencia de
`RIP` dentro de la imagen PE no basta para garantizar que suspender
ese hilo sea seguro: una callback invitada puede haber sido invocada
desde una función del host que **todavía conserva un mutex**.

La protección de las **importaciones soportadas por el resolvedor
propio** se instrumenta ahora en un punto único:

`src/runtime/exports.c` →
`nt_guest_boundary_wrap_import()` →
`src/runtime/guest_boundary_import.S` →
`nt_guest_boundary_import_enter/leave()` →
función Win32/NTDLL del runtime.

El mismo puente se aplica a nombres, ordinales y `GetProcAddress`,
sin lista manual de firmas. En un proceso de pruebas sin invitado el
resolvedor conserva sus direcciones originales, como antes. El
launcher PE explícitamente habilita el gate **antes** de rellenar
cualquier IAT.

### ABI y protección de memoria

La puerta es de ABI Microsoft x64, sin conocer las firmas:
salva/restaura RCX/RDX/R8/R9, el resto de registros de paso/salida
necesarios, RDI/RSI y XMM0–XMM15 antes de llamar al publicador SysV
C. Los argumentos en pila y los 32 bytes de shadow space no se
desplazan: el destino ve la llamada original. Al retornar, el epílogo
restaura RAX/RDX, registros XMM y no volátiles, y salta al retorno
original del invitado.

Los pequeños stubs se crean usando una página privada RW, se
escriben y se protegen RX **antes** de ser visibles a los PE. No se
solicita un mapeo simultáneamente escribible y ejecutable. La
instrucción ENDBR64 permite el destino de salto en sistemas con IBT.
Si el kernel indica que **CET SHSTK** está habilitado, la puerta
rechaza la función antes de ofrecerla a un PE: la técnica actual
reemplaza temporalmente un retorno de la pila ordinaria y NO es
compatible con shadow stacks. No se relajan protecciones ni se
desactiva SHSTK para pasar las pruebas.

Los registros SIMD altos AVX-256/512 y XSTATE completos no son
atributos de retorno garantizados de la convención Microsoft x64;
no deben confundirse con el contexto completo que exige una APC
general. Las funciones con unwinding no local pueden omitir el
epílogo: entonces la fase permanece HOST (fallo conservador), y
la reutilización de esa pila de retorno está condicionada a
completar la gestión de excepciones/señales del **problema 4**.

## Estado compartido, propiedad y decisiones

El lanzador real `nt_runtime_run_fixture` crea
`MAP_ANONYMOUS|MAP_SHARED` antes de crear el proceso que ejecuta
el PE. El padre supervisor recibe el puntero por una estructura de
launch de confianza, no desde un mensaje PE. El padre conserva el
`pidfd` y sólo admite hilos del registro autenticado del problema
2. La estructura tiene hasta **256 slots**; cada hilo se registra
después de crear el TEB e instalar GS, y queda inicialmente `HOST`.
El slot se invalida en la terminación del hilo.

`GUEST` sólo se publica alrededor de entradas revisadas:
el entrypoint del ejecutable PE y la función de inicio real de
`CreateThread`. Las importaciones entran en `HOST` antes de
ejecutar código anfitrión y vuelven a `GUEST` sólo cuando la
pila de importaciones anfitrionas queda vacía. Una callback PE
invocada desde una importación del host —incluso bajo mutex—
permanece `HOST`, independientemente de su `RIP`.
Las callbacks de TLS/DllMain/VEH/SEH/FLS invocadas durante tareas
anfitrionas permanecen conservadoramente `HOST`, incluso si el
host ya había liberado sus mutexes; esto reduce disponibilidad de
suspensión, no seguridad.

El RPC adicional `SUP_PROBE_GUEST` aplica **simultáneamente**:

1. `SCM_CREDENTIALS`, secreto de sesión y `pidfd` del hijo,
   más el capability de acceso y generación del TID concreto.
2. Slot de ese TID presente y TEB publicado de forma coherente.
3. Fase `GUEST` estable antes y después de leer `GETREGSET`,
   con la generación de transición sin cambiar.
4. `RIP` dentro de la sección ejecutable del **PE principal
   validado por el padre** y `GS_BASE == TEB`, `RSP != 0`.

Ante un rechazo la parada temporal se reanuda antes de responder,
y el supervisor se desadjunta. El diagnóstico está desactivado por
defecto; exige un flag concedido por el host que el PE no puede
solicitar en RPC. El camino Win32 productivo de suspensión/APC
continúa sin `ptrace` automático.

## Pruebas e incertidumbre

```bash
make check-guest-boundary
make check-pe-thread-supervisor
make check
make check-pe
```

`guest_boundary_import_test` verifica ABI con ocho argumentos (más
allá de los cuatro registros), retorno flotante en XMM0, anidación,
preservación del estado por pthread, callbacks bajo mutex y limpieza.

`supervisor_pe_thread_test` ahora utiliza **el mismo gate y tabla de
slots de producción** con un PE32+ real. El supervisor valida
RIP/GS/TEB, prueba que el hilo Linux líder continúa mientras el
invitado está detenido, rechaza la parada durante callback host bajo
mutex, rechaza fase falsificada y restaura contexto/APC del fixture
como en la etapa anterior.

**No equivale a compatibilidad Windows universal**: sólo cubre las
importaciones resueltas mediante el registro propio; no media
`syscall` arbitrario, saltos host no declarados, código JIT,
retornos PE no estándar, funciones que dependan de CET SHSTK,
SEH no local o DLLs ejecutables todavía no inscritos en los rangos
confiables del padre. Estos casos deben **denegar** suspensión en
vez de asumir seguridad. Un PE adversarial que pueda escribir
memoria del mismo proceso puede alterar su slot; por tanto, el slot
no es un límite de seguridad frente a ese adversario y se debe
exigir igualmente RIP/GS y el ownership autenticado.

CI Ubuntu no sustituye la aceptación física Fedora KDE +
Bubblewrap bajo SELinux. El problema 4 (señales, fallos y ciclo de
vida) y el problema 3 (APIs públicas Win32) siguen separados.
