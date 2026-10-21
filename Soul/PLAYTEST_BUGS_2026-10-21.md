# Soul — Incidencias del playtest de Windows

**Fecha:** 21/10/2026 · **Proyecto:** 1.44 · **Origen:** informe directo del jugador tras instalar y ejecutar `Proyecto144_Bones_ModKit.exe` desde `C:\144\BONES\build\`.  
**Código consultado:** `AlexDevJSC/144mb`, `main`, árbol `1665ecfae5037b5f8844c2d0a3e01ec6de7e9a74`.  
**Evidencia:** inspección estática de fuentes + síntomas del jugador. **No se ha ejecutado ese EXE en su Windows ni se ha obtenido vídeo/volcado de posiciones.**  
**Regla:** confirmado en código != reproducido localmente. No cambiar módulos de especialistas sin responsable y revisión Soul.

## BUG-001 — La casa visible carece de colisiones de paredes (P1 / Bones, Eye consulta)
**Síntoma:** el jugador atraviesa paredes internas/casa. **Confirmación estática: sí, para la casa demo.**  
**Ruta:** `BONES/src/demo/test_house.c::bones_house_append_scene` crea quads en `on_scene`; `BONES/src/bones/world.c::bones_world_build_test_room` registra colliders de sala, dos volúmenes interiores y NPC, pero no los de las paredes de la casa (x=-2..2, z=2..6).  
**Esperado:** paredes sólidas y puertas definidas bloquean al jugador; paredes visibles tienen correspondencia física y la puerta decide abrir paso.  
**Repro:** iniciar demo, avanzar hacia fachadas de la casa, intentar atravesarlas a velocidad normal, sprint y salto.  
**Solución propuesta:** Bones registra colliders acordes a la geometría del edificio en la misma semilla/layout que Eye; no inferir colisiones a partir del framebuffer. Puertas: estado lógico y collider controlado por Bones; render Eye según estado.  
**Aceptar cuando:** no se atraviesan las cuatro paredes a pie/sprint/salto; no hay colisiones fantasma; interior y portal son navegables según diseño.

## BUG-002 — El ratón no gira horizontalmente (P1 / Bones app-input; Eye revisión)
**Síntoma:** pitch sí, yaw por movimiento X no. **No confirmado en código**: existe `get_input` con `mouse_dx` y `bones_physics_step_actor` suma `mouse_dx * 0.0023f` al yaw; `world_to_view` también usa yaw.  
**Sospecha:** lectura `GetCursorPos`/recaptura `SetCursorPos` en `BONES/src/app/win32_main.c`, sin entrada Raw Input ni log; posible pérdida de deltas o interferencia de foco/escala DPI.  
**Aislamiento:** girar con flechas izquierda/derecha; luego clic para capturar ratón y moverlo solo horizontalmente; registrar `mouse_dx`, yaw real y foco.  
**Aceptar cuando:** horizontal del ratón gira de forma continua en ambos sentidos; no hay saltos tras capturar/soltar ni efectos al mantener tecla E/Space.

## BUG-003 — WASD no se corresponde con la orientación percibida (P1 / Bones física + app; Eye cámara)
**Síntoma:** controles de movimiento erráticos. **Por verificar como defecto**, pues el código sí mapea W/S forward/back y A/D strafe y la base trigonométrica coincide con la transformación de Eye.  
**Rutas:** `BONES/src/app/win32_main.c::get_input`, `BONES/src/bones/physics.c::bones_physics_step_actor`, `BONES/third_party/eye/sw_renderer.c::world_to_view`.  
**Repro:** en espacio abierto, con yaw 0, 90, 180 y 270 grados, mantener W, S, A y D individualmente; grabar cambios x/z y vector de cámara. Repetir con ratón no capturado y capturado.  
**Esperado:** W hacia donde mira cámara, S hacia atrás, A izquierda y D derecha; nunca invertir ni modificar dirección por cambiar pitch.  
**Aceptar cuando:** todos los cuadrantes correctos; sin acumulación indebida de input ni cambios de dirección al pulsar combinaciones.

## BUG-004 — El salto desplaza la cámara lateralmente (P1 / Bones física + captura ratón; Eye revisar)
**Síntoma:** Space provoca desplazamiento visual lateral. **No probado**: el bloque de salto de `physics.c` solo cambia velocidad Y, pero X/Z mantienen inercia/colisiones y yaw puede estar afectado por mouse deltas.  
**Repro:** detenerse sin tocar ratón y saltar; luego saltar manteniendo W y después A; guardar x/z, yaw, pitch por tick.  
**Aceptar cuando:** salto desde reposo no añade velocidad horizontal ni giro; salto en movimiento conserva desplazamiento esperado, sin teletransporte o sacudidas laterales.  
**No imponer** inmovilización X/Z general durante el salto.

## BUG-005 — Destellos o frames negros durante movimiento (P1 / Bones ventana GDI, Eye framebuffer)
**Síntoma:** pantalla oscurecida/negra periódicamente al desplazarse. **Reproducción pendiente**.  
**Indicio verificable:** `BONES/src/app/win32_main.c::present` ejecuta `PatBlt(...BLACKNESS)` **antes de cada** `StretchDIBits`, sin doble búfer; `WM_PAINT` tampoco presenta una copia estable. Además el render hace un clear oscuro `p144_renderer_begin`.  
**Aislamiento:** vídeo a 60 fps, comparar frames originales `bones_eye_mod_pixels()` (hash, porcentaje de píxeles oscuros) con captura de ventana; desactivar temporalmente `PatBlt` en rama de prueba y/o presentar a DC de memoria.  
**Aceptar cuando:** no hay flashes de pantalla completa al caminar/girar ni durante resize y el framebuffer nunca queda parcialmente presentado.  
**No atribuir en firme** el fallo a GDI hasta separar negro de framebuffer del negro de presentación.

## BUG-006 — Menú principal/pausa no visible (P1 de integración / Skin + Bones host)
**Síntoma:** no hay menús. **Confirmado en código.**  
**Indicio:** `BONES/modules/skin/skin_ui2.c` implementa menú principal/pausa/inventario; `skin_modkit_impl.c` expone `bones_skin_ui_input/update/compose`. `BONES/src/app/win32_main.c` no llama estas funciones: solo dibuja framebuffer Eye. Escape únicamente libera captura.  
**Aceptar cuando:** menú principal, pausa y reanudación navegables con teclado, input enviado una sola vez por pulsación, pausa real sin pasos de física, click de ratón coherente. No presentar acciones no implementadas como funcionales.

## BUG-007 — HUD no visible (P1 de integración / Skin + Bones host)
**Síntoma:** faltan indicadores. **Confirmado en código:** HUD dibujado en Skin, pero no compuesto tras Eye en la aplicación Windows.  
**Aceptar cuando:** HUD se ve sin pisar profundidad 3D, escalado legible 320x200 y valores que Bones pueda proveer; NO inventar PV/PM/oro/inventario inexistentes. Se admite etiqueta «sin datos» hasta proveer estado canónico.

## FEAT-008 — Interacción contextual con E (P2 producto / Bones interacción + Skin UX + especialistas)
**Situación:** E no existe en `get_input` ni en el protocolo de interacción: es una funcionalidad pendiente, no regresión contra función previa.  
**Contrato propuesto:** al pulsar E en flanco de tecla, Bones resuelve objetivo desde centro de cámara, distancia limitada y oclusión; Skin muestra «E — Interactuar» y menú contextual; el propietario semántico del objetivo decide acción. Eye aporta objeto/cursor visual. No asignar comercio/economía sin decisión del director.  
**Aceptar cuando:** E funciona solo sobre objetivo válido cercano, no atraviesa paredes, no duplica acciones y no altera la simulación de otro cliente sin confirmación.

## FEAT-009 — Selector de modo individual y multijugador (P1 de experiencia / Skin + Mouth + Bones host)
**Situación:** menús no expuestos; UI actual tiene NUEVA PARTIDA, CONTINUAR, OPCIONES, SALIR pero ningún selector de modo. Mouth LAN 0.3.1 requiere configuración explícita; el EXE principal no inicia host ni cliente.  
**Mínimo viable:** Menú principal -> Un jugador / Multijugador experimental (Crear LAN / Unirse por IP). Opciones de multiplayer deshabilitadas o acompañadas de aviso si faltan actores, sockets o cliente 3D.  
**Aceptar cuando:** modo individual arranca sin UDP; modo LAN configura el host/cliente a petición, comunica fallos honestamente, y no reclama P2P Internet ni sesión final que aún no existe.

## FEAT-010 — Música ambiental y controles (P2 / Ear + Skin)
**Situación:** `BONES/modules/ear/modkit.c` ya sintetiza una partitura `EAR_STYLE_DUNGEON`, intenta abrir WinMM y bombea PCM en `after_tick`; falta comprobar salida audible en el equipo y proporcionar control usuario.  
**Aceptar cuando:** opción Música ON/OFF, volumen independiente, reproducción real de música y SFX de prueba; gestionar fallo de `waveOutOpen` y mostrar diagnóstico sin bloquear juego. No añadir otro motor musical.

## BUG-011 — El instalador no crea acceso directo de escritorio (P2 / empaquetado e instalador; Soul coordinación)
**Síntoma:** no aparece icono/EXE en escritorio. **Confirmado en el script proporcionado al usuario** `Instalar_Proyecto_144_Completo.ps1`: compila en `C:\144\BONES\build\Proyecto144_Bones_ModKit.exe` y opcionalmente lo ejecuta, pero no crea acceso directo.  
**Aceptar cuando:** primera instalación exitosa crea acceso directo de escritorio con ruta absoluta al EXE en la carpeta de instalación, icono y WorkingDirectory correcto; en reinstalación se actualiza sin duplicar iconos; nunca copiar EXE suelto sin sus dependencias.

## Orden recomendado
1. **Jugabilidad base:** BUG-002, BUG-003, BUG-004, BUG-001; test de posiciones y orientación de cámara.
2. **Estabilidad visual:** BUG-005; doble buffer/registro de frames, sin asumir causalidad.
3. **Acceso a funciones:** BUG-006, BUG-007, FEAT-008, FEAT-009.
4. **Usabilidad/acabado:** FEAT-010 y BUG-011.

## Datos de reproducción por recoger
- SHA de commit local (`git -C C:\144 rev-parse HEAD`), fecha y tamaño EXE.
- Captura o vídeo del salto lateral, del giro horizontal fallido y del flash negro.
- ¿Las flechas izquierda/derecha sí giran horizontalmente?
- ¿El salto quieto reproduce desplazamiento lateral o solo al caminar/correr?

**Estado general:** incidencias redactadas; no implican corrección, ejecución, aceptación técnica ni actualización del ejecutable.
