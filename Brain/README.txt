PROYECTO 1.44 — BRAIN v0.2.0 | IA Y SIMULACIÓN PARA BONES

REQUISITOS
  C11; sin bibliotecas ajenas al estándar C para el módulo portable.
  Compilación Windows prevista mediante MSVC (no ejecutada en esta sesión).

QUÉ HACE
  - Genera intenciones de comerciantes, aldeanos y guardias según reloj/amenaza.
  - Mantiene stock, producción, consumo, precios y compras idempotentes en RAM.
  - Mantiene relaciones entre facciones mediante eventos de rama/secuencia.
  - Calcula hashes de la parte de mundo que Brain posee.
  - Entrega a Bones intenciones abstractas; no conoce coordenadas de rutas.

ESTRUCTURA
  include/brain/brain.h                   API pública v1
  include/brain/brain_visual.h            NUEVO contrato de referencias Eye
  src/brain_clock.c                     Reloj
  src/brain_random.c                    Aleatoriedad indexada
  src/brain_economy.c                   Economía agregada
  src/brain_npc.c                       IA de horarios/prioridades
  src/brain_world.c                     Estado, eventos, deduplicación
  src/brain_bones.c                     Adaptador para Bones
  src/brain_visual.c                    NUEVO códec BVR1 + overrides sparse
  demo/brain_demo.c                     Demostración de compra y reloj
  demo/bones_stub_integration.c         Ejemplo que FINGE Bones
  tests/test_brain.c                    Pruebas automatizadas
  tests/test_visual_refs.c              NUEVO reconstrucción sin píxeles
  spec/CONTRATO_BRAIN_EYE_REFERENCIAS_v1.txt  NUEVO acuerdo propuesto
  spec/CONTRATO_BRAIN_BONES_v1.txt       Integración y límites
  docs/                                 Plan, estado, decisiones y changelog

EJECUTAR EN LINUX
  make test
  make demo
  make bones-demo
  make all    # genera build/libbrain.a (artefacto de desarrollo)
  make sanitize   # ASan+UBSan, nuevos contratos visuales

EJECUTAR EN WINDOWS
  Abrir x64 Native Tools Command Prompt de Visual Studio:
     scripts\build_windows_msvc.bat
  Esto crea test_brain.exe, test_visual_refs.exe y brain_demo.exe;
  script NO validado en Windows aquí.

ESTADO DE CALIDAD
  Tests C11 GCC y ASan/UBSan pasan en Linux. Compatibilidad Windows por
  verificar. No está integrado con Bones real porque no se dispone de su API.
  Existe un serializador experimental SOLO de referencias visuales BVR1;
  no existe un gestor general de guardados, red o merge PBW. El dueño del
  sistema SAVE todavía no está asignado en el protocolo de responsabilidades.
  El ejecutable completo de 1,44 MB NO está ensamblado ni medido.

REGLA DE CAMBIO
  Toda modificación del código debe actualizar estado y changelog y, si
  corresponde, ADR, contrato público y pruebas. Ver BRAIN_OBJETIVOS_Y_NORMAS.txt.

EXTENSIÓN v0.2.0 — SOLICITUD DE EYE
  - Las variaciones decorativas se derivan; no se guardan por superficie.
  - Los cambios semánticos se pueden transportar como overrides por ID estable.
  - La cabecera versiona por separado mundo y generador de Eye.
  - EYE1/EYE2 se identifican por números que DEBE comunicar Eye. El test usa
    1001/1002 como etiquetas simuladas, NO IDs reales del renderer.
  - El códec produce 48 + 56*n bytes sin texturas ni píxeles.
  - Eye y Bones no están integrados realmente en este ensayo.
  - Ver spec/CONTRATO_BRAIN_EYE_REFERENCIAS_v1.txt para roles y límites.

NUEVO v0.3 — BONES v0.4.0
  Paquete autocontenido del módulo: bridge/bones_0_4/modules/brain/.
  Copiar modkit.c, brain.h, brain_npc.c y README.txt a BONES/modules/brain/.
  Ejecutar make modkit-test para tests del contrato ABI simulado; para
  verificar integración completa ejecutar make modkit_test desde BONES/.
  Detalles y límites: docs/CONTRATO_BRAIN_BONES_MODKIT_0_4.txt.
  Los módulos anteriores de economía/BVR1 permanecen sin integrarse en host.
