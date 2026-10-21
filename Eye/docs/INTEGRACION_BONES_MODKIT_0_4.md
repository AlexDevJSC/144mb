# Eye 0.5.1 y Bones 0.4.0 ModKit ABI 1

**Estado:** implementación de conexión estática, pendiente de CI/validación en Windows y Soul.
**Responsable:** Eye. Ningún cambio a `bones.h`, `modkit.h`, host, física, snapshots ni comunicación.

## Instalación por fuentes

La edición de distribución se **enlaza estáticamente**. Copiar el contenido de
`BONES/modules/eye/` de este repositorio, o del paquete independiente
`Eye_ModKit_Bones04_Instalable.zip`, sobre `modules/eye/` de Bones 0.4.0.

Carpeta de destino, completa:

- `modkit.c` — único punto de entrada detectado por los scripts actuales.
- `eye_modkit_ext.h` — control opcional de vistas de prueba.
- `eye.c`, `eye2.c`, `eye3.c`, `eye4.c`, `eye5.c`, `eye_tone.c` — generadores.
- `eye.h`, `eye2.h`, `eye3.h`, `eye4.h`, `eye5.h`, `eye_tone.h` — cabeceras.

**No incluir los seis generadores `.c` por separado en la línea de enlace.**
`modkit.c` los incluye en una unidad de compilación para ser compatible con el
`Makefile` y los `.bat` de Bones, que detectan únicamente `modkit.c`.
Las macros locales renombran tres ayudantes `static` homónimos; los algoritmos
históricos de las recetas NO se han modificado.

Desde la raíz de Bones:

```sh
python3 scripts/gen_modkit_registry.py
make modkit_test
make modkit_preview
```

En Windows, ejecutar `scripts\\build_windows_msvc.bat` en la consola de
herramientas x64 de Visual Studio. El script enlaza Eye al EXE único y realiza
su comprobación del tamaño PE. No garantiza que se cumpla el límite.

## Qué hace la integración

`bones_mod_eye_query` anuncia `BONES_ROLE_EYE`, ModKit ABI 1, versión de
implementación 501. `start` inicializa el renderizador P144 y reconstruye
únicamente tres materiales desde recetas de Eye4:

| ID Bones | Categoría original | Receta integrada |
| --- | --- | --- |
| 0 | STONE | panel de fachada híbrido |
| 1 | FLOOR | empedrado híbrido |
| 2 | WOOD | puerta/madera híbrida |

`present` transforma `BonesModFrame` a quads/sprites del rasterizador
existente, **sin convertirlo a `BonesScene` ni acceder al motor físico**.
Se mantiene la salida `bones_eye_mod_pixels()` para la ventana y el modo
headless existentes de Bones. No se hace lectura de archivos ni se recalculan
texturas por fotograma; `p144_renderer_init` y las recetas se ejecutan en
`start`, reconstruyendo los tres tiles de 64x64 (48 KiB de píxeles).

## Constructor de tejados

`eye5_build_roof` está incluido y probado. Opcionalmente el ejecutable host
puede incluir `eye_modkit_ext.h` y llamar a:

```c
Eye5RoofSpec spec = {2.f, 2.f, 2.5f, 1.4f, .25f, EYE5_GABLE};
bones_eye_mod_roof_preview(&spec, 0.f, 0.f, 4.f, 2u);
```

Eso registra una cubierta visual aprobada explícitamente por la aplicación.
`build_scene` añade los 9 quads o 13 superficies a través de
`services->scene_add_quad` en la fase autorizada, sin generar colisionadores.
Usar `spec=NULL` para desactivar el ejemplo. No supone que Eye conozca dónde
hay viviendas en el mundo; las coordenadas las proporciona la aplicación.

Para probar la receta EYE5 de tablillas de madera sobre el ID WOOD:

```c
bones_eye_mod_roof_texture_preview(1);
/* hacer una o más capturas */
bones_eye_mod_roof_texture_preview(0);
```

Este modo cambia el aspecto de **todas** las superficies WOOD de la escena.
Es una demostración técnica, no una solución final de texturas de tejado.

## Bloqueos y tareas para Bones / Soul

- Bones 0.4 valida `material_id` solo en el rango 0..2. No se han asignado IDs
  adicionales ni se ha sobrecargado su significado silenciosamente.
- La API de ModKit no ofrece datos de edificios desde Bones a Eye. La
  demostración de tejado se solicita desde el host, no aparece sola.
- La geometría está limitada a 96 quads y 8 sprites y sin caras unilaterales.
- No existen colisionadores creados desde Eye: Bones decide transitabilidad.
- La implementación comparte el rasterizador de 320x200; no impone DLLs.
- Soul debe revisar integración, memoria, caché futura, regresiones y ABI.
- No se ha compilado el PE Windows ni verificado su instalación en FAT12.
- El objetivo de semejanza artística del 84 % sigue sin demostración.

Para material de tejado 3 y siguientes, solicitar extensión semántica de
materiales primero a Bones y validación Soul antes de cambiar el contrato.

## Pruebas

`Eye/tests/test_bones_modkit_04.c` verifica versión, arranque, framebuffer,
los dos tipos de cubiertas, inserción geométrica, rechazo de ID3, y salida.
`Eye/scripts/test_bones_modkit_04.sh` compila contra el contrato y rasterizador
reales de Bones, luego ejecuta las pruebas nativas ModKit. Además, el proveedor
se ha compilado en un ensayo local con un encabezado ModKit equivalente y un
rasterizador simulado. **Eso no constituye todavía una prueba conjunta contra
el árbol completo de Bones.**
