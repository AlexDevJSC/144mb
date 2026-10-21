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

## BUG-002 — El ratón no gira horizontalmente (P1 / Bones input Windows; Eye revisión)
**Resultado de prueba del jugador (21/10):** flechas izquierda/derecha **SÍ** giran correctamente; mover ratón en X no produce giro horizontal percibido, pero ratón Y funciona. **Defecto observado, causa no instrumentada.**
**Evidencia de código:** `BONES/src/app/win32_main.c::get_input` lee `GetCursorPos` y ejecuta `SetCursorPos` en cada tick fijo; `BONES/src/bones/physics.c` suma `mouse_dx * .0023f` al yaw; `BONES/third_party/eye/sw_renderer.c::world_to_view` utiliza el yaw. Las flechas comparten ese canal de orientación, así que un error puramente en el renderizado del yaw es menos probable.
**Hipótesis prioritaria:** adquisición/captura de desplazamiento X en Windows (recentrado por tick, foco, escalado DPI o eventos perdidos). No afirmar una causa exacta sin registrar `mouse_dx` y yaw.
**Corrección en dos fases (responsable Bones):**
1. Instrumentar durante 5-10 segundos: `mouse_dx, mouse_dy`, yaw/pitch antes/después, `captured`, foco de ventana y tecla/estado del frame. Si `mouse_dx=0` durante movimiento X real, está fallando la adquisición. Si `mouse_dx!=0` y yaw varía, investigar otra etapa.
2. Si se confirma adquisición inestable, implementar `WM_INPUT` / Raw Input con deltas relativos X/Y acumulados al procesar mensajes, consume-once por tick, sincronización con foco y descarte de eventos en activación/captura/liberación; no usar `SetCursorPos` como fuente de deltas. Conservar las flechas de prueba.
**Aceptación:** giro horizontal y vertical del ratón bidireccional, continuo, sin deriva/recentrados; teclado sigue funcionando; no cambia velocidad según FPS/ticks ni tras Alt+Tab.

## BUG-003 — WASD no se corresponde con la orientación percibida (P1 / Bones física + app; Eye cámara)
**Síntoma:** controles de movimiento erráticos. **Por verificar como defecto**, pues el código sí mapea W/S forward/back y A/D strafe y la base trigonométrica coincide con la transformación de Eye.  
**Rutas:** `BONES/src/app/win32_main.c::get_input`, `BONES/src/bones/physics.c::bones_physics_step_actor`, `BONES/third_party/eye/sw_renderer.c::world_to_view`.  
**Repro:** en espacio abierto, con yaw 0, 90, 180 y 270 grados, mantener W, S, A y D individualmente; grabar cambios x/z y vector de cámara. Repetir con ratón no capturado y capturado.  
**Esperado:** W hacia donde mira cámara, S hacia atrás, A izquierda y D derecha; nunca invertir ni modificar dirección por cambiar pitch.  
**Aceptar cuando:** todos los cuadrantes correctos; sin acumulación indebida de input ni cambios de dirección al pulsar combinaciones.

## BUG-004 — El salto desplaza la cámara lateralmente desde reposo (P1 / Bones física/input + Eye diagnóstico)
**Resultado de prueba del jugador (21/10):** Space **SÍ** causa desplazamiento lateral aparente incluso cuando no se mantiene WASD y se ha detenido el personaje. **Observación confirmada por jugador; desplazamiento físico vs visual por determinar.**
**Evidencia de código:** `BONES/src/bones/physics.c::bones_physics_step_actor` modifica la velocidad vertical al saltar, pero no añade directamente X/Z; `horizontal_move` puede preservar movimiento residual durante desaceleración. La cámara se construye a partir de `player.feet` + altura ocular. Si realmente X/Z, yaw y pitch permanecen constantes, una desviación visual procede de render/proyección/oclusión y no de la física lateral.
**Causa raíz:** ABIERTA. La coincidencia con fallos de ratón sugiere aislar primero entrada/foco; NO concluir que sean el mismo bug.
**Prueba mínima A:** detenerse varios segundos; liberar captura con Esc sin tocar ratón; pulsar solo Space y observar. Repetir con captura activada. Si cambia entre ambos casos, investigar deltas de ratón/host.
**Prueba instrumentada B:** guardar por tick `W/A/S/D, jump, mouse_dx/dy, player.feet.x/y/z, velocity.x/y/z, yaw, pitch` antes y después de un salto. Ejecutar también test headless con `BonesInput` que solo tenga `jump=1`, con velocidad X/Z inicial exactamente cero; comprobar `dx=dz=0` dentro de tolerancia.
**Interpretación:** (a) X/Z cambian -> entrada/residuo/colisión/física; (b) X/Z constantes pero yaw/pitch cambia -> lectura de ratón/teclado; (c) X/Z y yaw/pitch constantes -> Eye/proyección/visual del salto. Separar movimiento en mundo de desplazamiento aparente de objetos por parallax al elevar cámara.
**Aceptación:** desde reposo salto solo vertical, sin cambios X/Z ni orientación no solicitada; desde movimiento conserva inercia conforme física estable. **No** anular movimiento horizontal legítimo en el aire.

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
- Resultado de saltar con el ratón liberado mediante Esc tras 2 segundos de reposo, comparado con ratón capturado.
- Trazas de deltas X/Y del ratón y posiciones X/Y/Z + yaw/pitch antes y después de salto.
- SHA de commit local (`git -C C:\144 rev-parse HEAD`), fecha y tamaño EXE.
- Captura o vídeo del salto lateral, del giro horizontal fallido y del flash negro.
- [RESPONDIDO 21/10] Las flechas izquierda/derecha SÍ giran horizontalmente.
- [RESPONDIDO 21/10] Space provoca desviación lateral percibida incluso desde reposo sin WASD.

**Estado general:** incidencias redactadas; no implican corrección, ejecución, aceptación técnica ni actualización del ejecutable.
