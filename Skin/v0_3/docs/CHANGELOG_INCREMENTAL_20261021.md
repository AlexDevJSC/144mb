# Registro de cambios incremental — Skin v0.3 (ModKit implementaciones 4→5)

### Fase 1b: integración Eye PR #12, bajo revisión
- Nuevo test conjunto de `eye_display_prepare` (reproducción funcional test-only Eye 0.5.2) con Skin ModKit y menús existentes.
- Capturas a 320x200 y escalado; comprueba transparencia, inmutabilidad de la escena fuente y sentinel Z-buffer, HUD ampliado y pausa.
- Actualización de `Skin/v0_2/Makefile` para crear `build/` en ambos targets bench; `make bench` desde directorio limpio = PASS (Linux).
- Documento de traspaso Eye/Bones/Soul, sin modificar núcleo ni Eye.

### Fase 3: Mouth LAN v0.4 Session API 1, interfaz solamente
- Pantallas Crear LAN / Unir IP:puerto / Estado / Desconectar.
- `Skin2LanView` simplificada en `Skin2Ui` con phase/error/port/world_identity_known; sin sockets.
- Entradas de IPv4/puerto acotadas, sin ejecución remota: intenciones de UI al anfitrión.
- `skin_mouth_bridge.h` opcional enlazado por host a `mouth_session_get_status`; no se incorpora código Mouth a Skin.
- `tests/test_lan_ui.c` prueba límites, validación de entrada, estados y dependencias.
- La implementación ModKit incrementa implementation_version a 5; ABI sigue 1 y state_schema_version sigue 0.

### Fase 2: Ear — SIN IMPLEMENTAR
- API Ear 0.4 publicada en PR #13, pero sigue pendiente de aprobación Soul. Las opciones UI de audio conservan el aviso de pendiente, no se muestran como audibles. No se modifica Ear.

### Bloqueos generales
- PR Eye #12 y PR Bones #21 experimentales; integración en el EXE, selector E y GDI real pendientes de Bones/Soul.
- No se modificaron guardados, red, simulación de mundo, economía ni físicas.
