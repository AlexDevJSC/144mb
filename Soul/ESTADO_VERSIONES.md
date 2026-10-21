# Estado de versiones — Proyecto 1.44

**Corte inicial:** 21/10/2026. **Repositorio:** `AlexDevJSC/144mb`, rama `main`. Estado según el árbol de GitHub consultado y los archivos identificables del proyecto ChatGPT.

| Especialista | Última entrega ZIP localizada en archivos del proyecto | Versión declarada o deducida del nombre | Publicación visible en GitHub | Diagnóstico |
|---|---|---|---|---|
| BONES | `PROYECTO_144_Bones_Foundation_v0_3_0.zip` | 0.3.0 (coincide con `BONES/README.md`) | `BONES/` 0.3.0, solo parte del árbol | **Versión nominal coincidente; publicación incompleta** |
| EYE | `Eye_0_5_1_Tejados_Corregidos.zip` | 0.5.1 (también en `CORRECCION_EYE5_1.txt`) | No existe carpeta de código EYE independiente | **No publicada / sin comparación de código** |
| BRAIN | `PROYECTO_144_Brain_v0_2_Eye_Bones.zip` | 0.2 (nombre de entrega) | Solo `BONES/modules/brain/module.c` | **Conector Bones, no prueba de publicación del motor Brain** |
| EAR | `EAR_Motor_Audio_v0_2_Materiales.zip` | 0.2 (nombre de entrega) | Solo `BONES/modules/ear/module.c` | **Conector Bones, no prueba de publicación del motor Ear** |
| SKIN | `PROYECTO_144_Skin_UI_Integracion_Eye_v0_2.zip` | 0.2 (nombre de entrega) | Solo `BONES/modules/skin/module.c` | **Conector Bones, no prueba de publicación del motor Skin** |
| MOUTH | `MOUTH_P2P_Bones_Visual_v0_2.zip` | 0.2 (nombre de entrega) | Solo `BONES/modules/mouth/module.c` | **Conector Bones, no prueba de publicación del motor Mouth** |

## Fuentes y límites de esta revisión
- Árbol GitHub `main`: `BONES/`, `README.md`; dentro de Bones existen `include/bones/foundation.h`, `src/bones/{core,physics,world}.c` y conectores `modules/*/module.c`, entre otros documentos.
- `BONES/ESTADO_SUBIDA.md` reconoce expresamente que la carpeta está incompleta y no se ha compilado desde GitHub.
- Los ZIP anteriores se han **localizado por metadatos de archivos** del proyecto ChatGPT. No se ha ejecutado ni leído su contenido binario para esta comparación.
- Los nombres de versiones de ZIP de BRAIN, EAR, SKIN y MOUTH son referencias nominales; confirmar su API interna cuando los archivos fuente estén disponibles.
- No afirmar que se ha leído la última respuesta de cada chat ni que los ZIP más recientes están necesariamente todos indexados.

La revisión de código y las pruebas sobre las entregas completas están pendientes.
