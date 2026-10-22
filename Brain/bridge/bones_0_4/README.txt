INTEGRACIÓN BRAIN v0.3.0 + BONES v0.4.0 / ModKit ABI 1

Ruta distribuible: modules/brain/{modkit.c,brain.h,brain_npc.c,README.txt}
Destino de copia: carpeta modules/brain del proyecto BONES v0.4.0.
No copiar este README ni el fixture include/bones/modkit.h al núcleo Bones.

El código del adaptador únicamente incluye modkit.h de Bones y la función
original brain_npc_decide de Brain. Las pruebas de compatibilidad utilizan
un host simulado ABI1, NO sustituyen pruebas integradas en Bones real.

Pruebas locales: desde la raíz de Brain: make modkit-test y make test.
No se ha ejecutado Windows ni un juego completo, y no se ha verificado
el tamaño de un ejecutable PE (objetivo total 1.474.560 bytes).

Fuente de decisiones y límites: docs/CONTRATO_BRAIN_BONES_MODKIT_0_4.txt.

ACTUALIZACIÓN 0.3.1:
  Incluye header privado brain_mod_diagnostics.h y test mock extendido.
  Para verificación real en Bones corregido: tests/run_real_bones_tests.sh.
  Dependencia de core Bones post-playtest sin resolver al preparar PR.
