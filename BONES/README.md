# BONES — Núcleo de Proyecto 1.44

Responsable: Bones (programación del núcleo, entidades, estructuras, físicas y contratos de integración).

Versión de referencia: 0.3.0 (C11). API de Bones v2; foundation.h experimental pendiente de revisión de Soul.

Este directorio contiene la base modular del motor y se organiza en `include/`, `src/`, `modules/`, `tests/`, `scripts/`, `tools/`, `examples/` y `docs/`.

- Bones es autoridad de entidades, transformaciones, simulación y colisiones.
- Eye implementa gráficos y materiales.
- Ear implementa audio.
- Brain implementa IA.
- Skin implementa interfaz y el sistema de guardados asignado por dirección.
- Mouth implementa conectividad y multijugador.
- Soul revisa integración y regresiones.

Los conectores de módulos no son las implementaciones completas de sus especialistas. El ejecutable Windows de 1,44 MB todavía no ha sido validado.

Para compilar y probar en Linux: `make test` y `make core`. Para Windows revisar los scripts de `scripts/`.

Se conserva la arquitectura de un único ejecutable final; los módulos son unidades de código fuente enlazadas, no DLL dinámicas.
