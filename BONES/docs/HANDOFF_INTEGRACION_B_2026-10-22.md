# BONES 0.4.3-B — Ear 0.4 y Brain 0.3.1

Base: revisión A commit `0a5e5d3a1018a0b8a32fefc7085ed72e6221dd76`. Contratos públicos Bones API 2 y ModKit ABI 1 SIN cambios.

Integración realizada:
- `init_runtime()` crea con Foundation un NPC `BONES_KIND_NPC` / `owner=0` antes de registrar ModKit. Posición demostrativa (1.35,0,0) frente a la casa; Brain 0.3.1 (proveedor implementación versión 4) decide las órdenes `submit_move`, NO Bones.
- Bones aplica las órdenes con su física existente y colisionadores de la casa. ID, propietario, ticks y secuencias siguen gestionados por Foundation.
- `ear_audio_pump()` se llama en cada iteración de presentación del host, **incluso en menú sin ticks**. Ear 0.4 (implementación 4) mantiene WinMM de forma independiente, produce música procedural y escucha eventos físicos de salto y aterrizaje.
- Título de ventana muestra diagnósticos reales de Brain (órdenes aceptadas) y estado de dispositivo de Ear. Ausencia de dispositivo no impide iniciar el mundo.
- El módulo Ear libera salida y buffers mediante su `stop` en `bones_mod_host_destroy()`.

Prueba conjunta:
`make -C BONES host_audio_brain_test` enlaza las implementaciones **reales**. Verifica que Brain envía órdenes aceptadas durante 240 ticks, el actor de Foundation cambia de posición, no penetra colisionadores de casa/sala, su owner sigue 0, y Ear recibe salto y aterrizaje. `BONES/scripts/test_host_audio_brain_msvc.bat` compila y ejecuta lo mismo en MSVC.

Límites:
- Sin navegación avanzada ni detección visual de sonidos. En un runner CI puede no haber dispositivo de audio: `device_ready=0` NO equivale a no compilar ni a fallo de gameplay, y no confirma audibilidad.
- Skin actual muestra que las opciones de sonido siguen pendientes: Bones no ha diseñado ni modificado sus widgets; Skin debe publicar controles que llamen al contrato ya disponible de Ear. Petición a Soul/Skin en el PR.
- El NPC está en un punto fijo de la escena de demostración; no implica spawns procedurales ni pathfinding completo.
- Prueba humana Windows sigue necesaria para cámara, NPC visual, música y efectos auditivos.

**Revisión de Soul:** confirmar que este puente compila y se escucha en dispositivo real, no marcar reproducción audible solo por CI. B es dependiente de A y permanece sin merge.
