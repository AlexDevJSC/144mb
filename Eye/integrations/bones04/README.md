# Paquete instalable Eye para Bones 0.4

El conector canónico es `modkit.c` junto con `eye_modkit_ext.h` y los
6 ficheros fuente y 6 cabeceras `Eye/src` + `Eye/include`.

Para instalar sin editar Bones, copiar esos **14 archivos** directamente a
`BONES/modules/eye/`. Los scripts de Bones ya compilan `modkit.c` y enlazan
el rasterizador software. No se necesitan DLLs ni dependencias de terceros.

La instantánea completa de estos archivos está publicada también en
`BONES/modules/eye/` para construcción inmediata desde este repositorio.

Consultar `Eye/docs/INTEGRACION_BONES_MODKIT_0_4.md`.
