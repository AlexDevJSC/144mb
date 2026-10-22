# RFC-EYE-GFX-2026-01 — Catálogo de materiales y resolución de tiles para Bones 0.4

**Estado: PROPUESTA, NO IMPLEMENTADA.** Autores de revisión: Eye → Bones → Soul → Dirección.
**Sustituye como solicitud vigente, sin borrar historial:** `PETICION_BONES_EYE_0_3.txt`, `PETICION_BONES_EYE_0_4.txt` y `RFC_BONES_EYE5_INTEGRACION.txt`.
**No forma parte del PR #12 de renderizado/menús.** No altera `BONES_API_VERSION=2`, `BONES_MOD_ABI=1`, `BonesModQuad` ni colisionadores.

## 1. Problema verificado

- `BonesModQuad.material_id` es `uint32_t`, pero el host `api_add_quad` de `BONES/src/modkit/modkit.c` rechaza material >2. El renderizador `P144_Renderer.tex[3][4096]` también contiene **tres** tiles y Eye `present` descarta material >2.
- La escena base usa `BonesQuad.material:uint8_t`; los 96 quads y 8 sprites son límite actual por *snapshot*, no por sector.
- Eye ofrece recetas `EYE1–EYE3` de 12 bytes, `EYE4` de 16 bytes, `EYE5` de 12 bytes, todas renderizan **64×64 píxeles `0xFFRRGGBB`**. EYE5 tiene cubiertas de 9 y 13 superficies sin crear colisión.

## 2. IDs estables / fuente de verdad

**Propuesta:** catálogo público y versionado `catalog_version:uint32_t` con `material_id:uint8_t` de 0..255 EN LA escena base. Reservar **0=STONE, 1=FLOOR, 2=WOOD** inalterados; 3..255 se asignan de forma explícita y determinista en la tabla de catálogo (no por orden de lectura ni hash truncado). Los IDs de catálogo NO son IDs de objeto, color, semilla, familia acústica ni estado persistido. ID desconocido o versión incompatible: señal de error y material de diagnóstico distinguible; nunca acceso fuera de `tex`.

`BonesModQuad.material_id` puede seguir siendo `uint32_t`; el rango efectivo 0..255 se pacta en la validación del host sin cambiar el layout. Si se requieren >256 IDs simultáneos en una misma escena, no truncar: nueva propuesta ABI/layout y evaluación de coste.

Para variantes por sector/semilla no crear un ID persistido por cada ocurrencia. Definir clave de receta `(catalog_version,material_id,variant_id,seed,eye_generator_version)` resuelta a receta Eye; `variant_id` y semilla **no** van dentro del ID de 8 bits. La identidad mundial procede de Bones, no del índice de la caché.

## 3. Resolución y caché

- `eye_resolve(key)` devuelve referencia lógica validada y su receta, nunca pixels persistidos.
- `eye_tile_get(key)` devuelve un tile reconstruible 64×64, `uint32_t[4096]`, **16.384 B**. Su puntero se presta; está prohibido retenerlo fuera del intervalo pactado.
- Propuesta de capacidad inicial: **16 slots = 262.144 B de datos de píxeles** más claves/edades/ocupación. Capacidad de prueba, no límite universal.
- Regla de vida: `frame_begin` obtiene conjunto de IDs usados; los tiles que se referencian durante ese frame quedan fijados (**pinned**) hasta `frame_end`; no expulsar un tile mientras rasteriza alguna cara que lo utiliza. Evitar regeneración por píxel o en cada frame.
- Si existen >16 materiales distintos visibles, estudiar loteado **sin referencias prestadas entre lotes** o elevar presupuesto; reportar sobrecapacidad en vez de reemplazar silenciosamente un tile fijado.
- Construcción CPU síncrona: puede resolver y rasterizar cara a cara con préstamo temporal y luego liberar, siempre que no queden referencias asíncronas. Su semántica debe quedar documentada y probada por Soul.
- Regeneración por semilla y recipe canonical, sin ficheros obligatorios. Resultados invariables por orden de carga A→B / B→A, reinicio y evicción.

## 4. Geometría, UV y visibilidad

- Conservar el contrato de quads con UV por vértice, near-plane y Z-buffer. En EYE5 los patrones U recorren el alero y V sigue la pendiente.
- Frontones/aguas triangulares con `v[3]=v[2]` validados por ensayo. No exigir primitiva triangular pública sin métrica que justifique coste.
- Conservar winding exterior coherente; **doble cara temporal** (`flags=0`) porque el rasterizador actual no implementa descarte por cara. Selección individual de sidedness queda en un RFC distinto, con ABI nueva si cambia layout.
- El modelado visual de 9/13 superficies no define cajas de colisión, puertas ni física; Bones determina ubicación/estado y las entidades persistidas.

## 5. Compatibilidad y negociación

- EYE1/2/3 y EYE4/5 conservan algoritmo versionado. `EYE5_VERSION=5` y `EYE2_FORMAT_MAJOR=2` son identificadores de formatos, no un esquema oficial persistido de catálogo/world.
- No sustituir silenciosamente un algoritmo antiguo si un save pide una versión distinta. Migración solo explícita, reversible y revisada por Soul.
- ModKit ABI1 no recibe texturas en `BonesModFrame`; ampliación puede implementarse por una función local al provider Eye mientras Bones mantenga `BonesQuad` y su validador, pero debe aprobarse cualquier cambio semántico público. Prohibido a Eye elevar los IDs por su cuenta en el núcleo.
- `BrainVisualRef` y `P144SurfaceBinding` acústico no cambian por esta propuesta; revisar esos contratos independientemente.

## 6. Pruebas de aceptación (NO ejecutadas; criterios acordar)

1. Escena con materiales 0, 1, 2 sigue dando el mismo hash de imagen; 3..N conservan IDs y texturas diferentes.
2. Rechazar 256, `UINT32_MAX`, recetas inexistentes y `flags` no aceptados sin overflow/corrupción.
3. Envío 96+1 quads y saturación de caché con 17+ referencias tienen errores identificables, no se leen bytes expulsados.
4. Hash por recipe + generador después de evicción/reinicio; igualdad Windows/MSVC y Linux/GCC o desambiguar divergencia legítima.
5. Tejados 2/4 aguas (9/13 superficies): winding, UV paralelas al alero, frontones, near clipping, ocultación y fallbacks.
6. Medir `sizeof(BonesScene)`, peak RAM y binary del **mismo** PE MSVC antes/después, con `/O2 /GL /MT /OPT:REF /OPT:ICF /LTCG`; no comparar bytes ELF aislados.
7. Pasar `make -C BONES modkit_test`, `Eye/scripts/test_eye_modkit_real.sh`, QA Windows y playtest visual Soul.

**Medida de referencia disponible (Eye 0.5.2 provisional):** CI Windows MSVC reportó `221184` bytes de `Proyecto144_Bones_ModKit.exe`; esto **NO es el coste adicional del RFC ni prueba de juego completo**.

## 7. Decisiones solicitadas

**Bones:** confirmar rangos/IDs 3..255, validador y API de consulta de material por escena; dar responsabilidades de sector/lotes y documentación de error/saturación. **Soul:** revisar ABI, determinismo, pinning, RAM y PE/medición reproducible. **Dirección:** autorizar cambio antes de implementar. Historial de peticiones antiguas se conserva, este RFC es la única solicitud vigente.
