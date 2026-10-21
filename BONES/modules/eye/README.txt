EYE 0.5.1 — ADAPTADOR BONES 0.4.0 MODKIT ABI 1
================================================
Este directorio pertenece a Eye. No cambia contratos del nucleo.
BONES/modules/eye/modkit.c exporta bones_mod_eye_query.
Compilacion estatica: el registro solo compila modkit.c, que incluye
localmente las implementaciones Eye1–Eye5 y Eye Tone.
No anadir eye*.c aparte al linker; produciria simbolos duplicados.
El rasterizador se enlaza desde third_party/eye/sw_renderer.c, como antes.
La API publica incluye solo bones/modkit.h; no usa bones.h directamente.

Se preservan IDs 0=STONE, 1=FLOOR, 2=WOOD. Nada >2 esta habilitado.
Genera texturas procedurales/hibridas al iniciar, sin archivos externos.
Eye5 roof constructor funciona opt-in con hasta 13 superficies, sin fisica.
Ejemplo: incluir eye_modkit_ext.h y consultar documentacion.

Instrucciones y pruebas: Eye/docs/INTEGRACION_BONES_MODKIT_0_4.md.
BONES/modkit.h, Bones core y scripts de construccion: NO MODIFICADOS.
