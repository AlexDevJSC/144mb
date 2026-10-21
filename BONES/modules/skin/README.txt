SKIN 0.3.0 — Bones ModKit 0.4 ABI1
===============================
Este subdirectorio contiene el adaptador especializado de Skin, sincronizado
con Skin/v0_3/modules/skin/. Su autor es Skin, no el núcleo Bones.
El registro automático detecta modkit.c y la función bones_mod_skin_query.
Sus .c auxiliares se incluyen desde modkit.c (unidad de compilación única),
porque Makefile y scripts Windows de Bones compilan únicamente modkit.c.
NO añadir estos auxiliares a la lista de compilación por separado.
El módulo provee lógica de interfaz UI2, no altera físicas ni renderizado 3D.
El EXE principal de Bones aún no invoca la composición 2D: ver
Skin/v0_3/docs/CONTRATO_COMPOSICION_MODKIT.md y solicitar a Soul revisión.
