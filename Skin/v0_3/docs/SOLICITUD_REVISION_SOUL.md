# Solicitud de Skin a Soul y Bones — ModKit 0.4.0

**Cambios entregados:** solo `Skin/v0_3/`, `Skin/README.md` y el puerto reservado `BONES/modules/skin/`. No hay alteraciones de contratos/modkit.h, físicas, renderizador, scripts ni bucle Win32.

**Comprobar en integración:**

1. El módulo queda registrado una sola vez y pasa `make modkit_test`/`make test` de Bones 0.4.0, sin avisos ni símbolos duplicados.
2. El *unity build* se compila en MSVC/MinGW sin rutas de cabeceras adicionales ni warnings bloqueantes.
3. `bones_mod_skin_query` no solicita acceso a escenas, órdenes de movimiento, guardados o datos de otros módulos; `Skin2View` nunca es un estado autoritativo.
4. Desarrollar con Bones una API de composición o un pequeño puente en la aplicación, para que teclado y capas 2D se presenten después de Eye. Sin aprobar no se debe declarar terminado el HUD dentro de la aplicación final.
5. Medir incremento real de bytes del ejecutable PE (no del ELF). Mantener constancia si el presupuesto global excede 1.474.560 bytes.
6. Decidir propietario de guardados. La documentación de Bones 0.4.0 menciona a Skin, pero el protocolo de responsabilidades confirmado delimita Skin a interfaces; no darlo por resuelto implícitamente.
7. Revisar buffer transitorio adicional de 256KB si es necesario y la accesibilidad de menús escalados.

**Estado del cambio:** listo para revisión, no certificado para producción Windows.
