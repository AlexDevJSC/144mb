ADAPTACION DE TEXTURAS — BONES 0.2 (contrato, sin enlace contra Bones)

La distribución de Bones 0.2 no está montada de forma accesible al compilador
actual. Su contrato de módulo v0.2 declara:
  BONES_API_VERSION=2; BONES_SUBSYSTEM_API_VERSION=1.
  BONES_SLOT_EYE=0; bones_eye_subsystem(eye) es el descriptor de Eye.
  El callback Eye.present recibe una escena BonesScene ya construida.

Integración propuesta — NO COPIAR SIN COMPROBAR CABECERAS REALES:

  #include "bones/bones.h"
  #include "eye_bones_compat.h"    // error de build si rompe versión
  #include "eye2.h"
  #include "materiales_embebidos.h"

  static EyeTextureBank materials;
  static uint32_t texture64[EYE_SIZE * EYE_SIZE];

  // Una vez, durante startup del módulo EYE:
  if (!eye_bank_open(&materials, eye_pack_embedded,
                     EYE_PACK_EMBEDDED_BYTES)) return 0;

  // En activación de región o al preparar la cache de texturas del backend:
  if (!eye_bank_render(&materials, id_material,
                       texture64, EYE_SIZE * EYE_SIZE)) return 0;

  // A continuación el propio backend Eye copia los píxeles 0xFFRRGGBB
  // en su atlas/registro de texturas y dibuja BonesScene como siempre.

LÍMITES DEL CONTRATO
- No invocar eye_bank_render por píxel o por fotograma; cachear el resultado.
- El banco de recetas pertenece a Eye; Bones asigna IDs y controla sectores.
- La enumeración de materiales y la asignación material-id a UV/atlas deben
  cotejarse con include/bones/bones.h al integrarlo en la distribución real.
- Eye no añade un segundo subsistema BONES_SLOT_EYE; sólo sustituye la fuente
  de texels dentro del Eye existente.
- No se ha validado ni compilado esta plantilla junto con libbones.a real.
- Sin el código Bones no conocemos el almacenamiento concreto del atlas ni
  podemos demostrar que TODOS los 22 índices estén direccionados por escena.
- El objetivo del EXE de 1.474.560 bytes requiere medición de release conjunta.

EYE 0.2.1 / PUNTO DE CONTACTO CON EL RENDERER REAL
- Fuente compilable de ejemplo: examples/bones_legacy_integration.c
- API portable: include/eye_bones_bridge.h y src/eye_bones_bridge.c
- El ejemplo se prueba sólo contra tests/mock_bones/.
- En el proyecto real, usar header third_party/eye/sw_renderer.h, comprobar
  P144_Renderer.tex y situar la llamada tras p144_renderer_init.
- Tres índices por defecto en el ejemplo: 0 piedra / 10 camino / 7 roble.
- El adaptador real NO debe volver a llamar p144_renderer_init por frame.
- Ante cambio de ABI Bones, fallar al compilar y renegociar contrato.
