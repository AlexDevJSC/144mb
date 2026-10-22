# Traspaso Skin fase 1 a Bones y Soul — API y orden exactos

**Sin modificar núcleo ni renderizador:** todos los archivos integrables residen en `BONES/modules/skin/`; ModKit registra automáticamente `modkit.c` con `bones_mod_skin_query` ABI1. ModKit `implementation_version` = 4. Continúa sin permisos de escribir mundo, movimiento, eventos, escena o snapshots. Contrato de UI independiente de ModKit ABI.

## Inicialización

1. Crear `BonesFoundation`, registrar módulos y llamar `bones_mod_host_start(host)`.
2. Skin inicializa en `start`, muestra `SKIN2_MAIN`, bloquea movimientos al inicio.
3. Mantener estado en el hilo principal. No invocar la API mientras los módulos no hayan iniciado.

## Entrada y simulación

En el procedimiento Win32, procesar teclas en `WM_KEYDOWN` **solo en flanco** (`!(lParam & (1L<<30))`) para evitar aceptar/escapar varias veces por autorrrepetición. Mapear: VK_ESCAPE→SKIN2_BACK, 'I'→SKIN2_OPEN_INV, 'E'→SKIN2_INTERACT, VK_UP/DOWN/LEFT/RIGHT→SKIN2_UP/DOWN/LEFT/RIGHT, VK_RETURN→SKIN2_ACCEPT. Consultar `Skin2Action a=bones_skin_ui_input(key)`.

- `SKIN2_START`: host crea/reinicia mundo (si realmente soporta reinicio de sesión; en caso contrario informa no disponible). Después de éxito, `bones_skin_ui_enter_game()`.
- `SKIN2_SINGLE`: host inicia/continúa sesión individual; solo tras éxito `bones_skin_ui_enter_game()`.
- `SKIN2_MAIN_REQUEST`: host sale de la sesión si está permitido; al confirmar `bones_skin_ui_enter_main()`.
- `SKIN2_QUIT`: host solicita cierre normal.
- `SKIN2_INTERACT_REQUEST`: consultar `bones_skin_ui_state()->target_id` y validar de nuevo mediante la interfaz **autoritativa** de Bones; de no existir, dejar el panel «Esperando a Bones». No fabricar datos, no mutar propiedad/puertas/inventario.
- `SKIN2_APPLY_SETTINGS`: preferencia visual local; ni Ear ni Eye reciben ajustes inventados.
- `SKIN2_NO_ACTION`: cambios de pantalla en Skin, sin acciones de juego.

**Regla imprescindible antes de cada paso fijo:**

```c
BonesInput input = {0};
if (!bones_skin_ui_blocks_gameplay()) {
    get_gameplay_input(window, &input); /* implementación de Bones */
} else {
    release_mouse_capture_if_needed(window);
}
if (bones_mod_host_step(host, &input) != BONES_MOD_OK) {
    /* gestión de fallo central de Bones */
}
```

No llamar `bones_step` directamente; no reutilizar el input del frame anterior mientras haya menú. En sesiones online Bones/Mouth debe decidir cómo pausar la simulación compartida: Skin bloquea movimiento **local**, no congela el tiempo global.

## Datos de HUD e interacción

- Skin lee tick y actores mediante `BonesModServices` en `after_tick`. No convertir esos datos en vida/MP/oro.
- `bones_skin_ui_update(&view)` permite datos confirmados de otro sistema cuando exista, estableciendo los bits `SKIN2_AVAIL_HP`, `MP`, `GOLD`, `INVENTORY` **solo para hechos reales**. En defecto `available=0`.
- Cada tick o al cambiar objetivo, Bones informa `bones_skin_ui_target(id, valid, label)`. `valid` indica evaluación realizada por Bones de dirección, proximidad y permisibilidad. Si no hay API de objetivos, establecer `id=0, valid=0` y no se mostrará E.
- No usar `target_label` como validación. Revalidar ID al recibir E.

## Presentación con Eye: BLOQUEADA hasta entrega #3

Contrato de composición previsto (pendiente validación Eye):

```c
if (bones_mod_host_frame(host) == BONES_MOD_OK) {
    const uint32_t *eye_pixels = bones_eye_mod_pixels();  /* const, Eye-owned */
    memcpy(app_pixels, eye_pixels, 320u*200u*sizeof(uint32_t));
    bones_skin_ui_compose(app_pixels, 320, 200, 320);     /* app-owned, no depth */
    present_once(app_pixels);                            /* GDI o API de Eye */
}
```

El arreglo `app_pixels` en 320x200 necesita 256000 bytes de RAM; Eye confirma el formato BGRA/ARGB y su API de copia/presentación. Si Eye provee otra técnica sin copia, se adapta **sin tocar el Z-buffer ni permitir mutación de `const`**.

Skin no debe presentar dos veces por fotograma ni llamar internamente a Eye. El escalado se hará con viewport separado y comprobación de pixel perfect/letterbox. No reclamar integración visual final antes de que Eye entregue código, pruebas y Soul lo apruebe.

## Revisión Soul

Verificar: contrato ModKit ABI1 sin cambios, seguridad de punteros y ciclo de vida, permisos mínimos; exclusión de input durante menú; no presentación doble/tearing; memoria extra; pantalla de información con límites reales de Ear/Mouth; no fingir persistencia ni datos RPG; compatibilidad con clientes Win32, PE final y regresión de tamaño.
