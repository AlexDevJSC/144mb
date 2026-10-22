# Eye 0.5.2 — Fase 1: diagnóstico reproducible de parpadeos

Referencia revisada: `BONES/third_party/eye/sw_renderer.c`, `BONES/modules/eye/modkit.c`, `BONES/src/app/win32_main.c` en `AlexDevJSC/144mb` (21/10/2026).

## Hallazgos en código (no confundir con diagnóstico confirmado del playtest)

1. Eye ejecuta `p144_renderer_begin(...,0x15202d)` en `Eye.present` y luego `p144_renderer_crosshair`. Un triángulo recortado por el near plane, una superficie fuera del frustum o una oclusión total dejan normalmente fondo azul oscuro y la retícula, NO un framebuffer enteramente negro. El near plane usado por la ruta de triángulos es `z=0.08f`. El sprite se descarta por completo si uno de sus cuatro vértices queda detrás de ese plano: una limitación del renderer, pero tampoco justifica un fotograma negro completo.
2. La textura se genera en `eye_start` (`prepare_textures`), no por fotograma; ninguna regeneración se observa en `eye_present`. Fallos de recarga durante un frame no son una explicación viable para un borrado completo con este código.
3. `win32_main.c` valida `WM_PAINT` sin dibujar: `BeginPaint(); EndPaint();`. Si Windows invalida la ventana entre presentaciones, el área afectada puede quedar sin contenido visible. En `present` se borra **directamente** la ventana con `PatBlt(...,BLACKNESS)` antes de `StretchDIBits`, usando un HDC de ventana. Esto constituye una **ventana temporal negra observable** si Windows presenta la superficie entre ambas operaciones. Este es un **riesgo real de GDI confirmado por lectura del código**, todavía no una prueba causal de cada parpadeo reportado.
4. No existe comprobación del resultado de `StretchDIBits`. Un fallo o un redibujado interrumpido dejan el borrado negro visible. Al minimizar o cambiar el tamaño, el HDC y las dimensiones pueden cambiar durante la operación.

## Prueba automatizada (Eye, sin Windows)

Ejecutar desde la raíz del repositorio (o desde cualquier carpeta, porque el script se reposiciona automáticamente):

```sh
sh Eye/scripts/test_eye_display.sh
```

Prueba: 64.000 píxeles negros (`all_black=1`); 64.000 del color de limpieza de Eye (`clear_only=1`); fondo Eye + retícula (`other_rgb=16`, no visible geometría 3D); base GAME; copia oscurecida; fondo de menú estable; guardia de desbordamiento; signo de yaw/pitch y salto vertical a yaw/pitch=0. La prueba de cámara reutiliza la **fórmula analítica** del transformador, NO el ejecutable real de Bones.

## Prueba reproducible de integración (requiere Windows y compilación de Bones)

1. Compilar Bones con este módulo Eye. Añadir temporalmente trazas externas al renderizado: `bones_eye_mod_frame_stats(&stats,&serial)` **justo después** de `bones_mod_host_frame`. Guardar `serial`, `black_rgb`, `clear_rgb`, `other_rgb`, `hash` junto con tiempo/tick; nunca calcular sobre la ventana GDI.
2. Presentar el frame en la ventana. Instrumentar en `win32_main.c` el retorno de `StretchDIBits` y `GetLastError` si falla; trazar `WM_PAINT`, `WM_SIZE`, `WM_ERASEBKGND`, activación de la ventana y el rectángulo cliente. **Solo Bones puede incorporar esas trazas**, dado que esa fuente es suya.
3. Caminar, girar, saltar desde reposo y cambiar el tamaño de la ventana hasta reproducir el fallo. Registrar vídeo y tiempo si es posible.
4. **Frame negro antes de GDI**: `stats.all_black=1`. Verificar inmediatamente si la imagen se borró antes del render, si hay escritura errónea o puntero inválido. Distinguir de `other_rgb=16` (solo retícula, geometría ausente por near plane/frustum).
5. **Frame correcto antes de GDI, ventana negra**: `stats.all_black=0` con hash/no-claro estables y presentación GDI fallida o `WM_PAINT` sin redibujar => problema de presentación de Bones/Win32. Mantener la imagen compositada anterior y redibujarla íntegra al recibir `WM_PAINT`.
6. **Geometry drops**: si `other_rgb` cae a 16 al cruzar el near plane, registrar cámara/quads; no corregirlo alterando cámara, gravedad, colisiones ni input.
7. **Yaw/pitch**: fotografiar un hito fijo y comprobar movimientos opuestos al girar/subir mirada, sin invertir signos. **Salto vertical desde reposo**: con `vx=vz=0` y pitch=0 verificar que el punto fijo no presenta deslizamiento horizontal; el punto puede variar en X por perspectiva **si pitch != 0** y los rayos rotan con verticalidad; no confundir con desplazamiento físico lateral. Validación final por Bones/Soul.

## Petición mínima a Bones (propiedad de la presentación Win32)

- No dibujar `PatBlt(BLACKNESS)` **antes** de `StretchDIBits` en el DC de ventana. Eliminar el borrado frontal entre capas.
- Componer todo en una superficie privada y dibujar **una única imagen completa** mediante `BitBlt` o `StretchDIBits` sobre el DC de destino en una sola operación. Si se necesitan barras negras, dibujarlas en el backbuffer (sin exponer medio frame).
- En `WM_PAINT`, volver a presentar la **última imagen completa** del buffer de la aplicación, no limitarse a `BeginPaint`/`EndPaint`.
- Registrar el resultado/errores de GDI durante el diagnóstico; no cambiar físicas/cámara; revisar eventuales redibujados en movimiento/tamaño.
- Si Bone decide modificar el bucle Win32, solicitar revisión de Soul. Este PR no altera `win32_main.c`.

**No declarado cerrado**: causa concreta de cada parpadeo, Windows/GDI real, cámara de Bones durante playtest, métricas de PE final.
