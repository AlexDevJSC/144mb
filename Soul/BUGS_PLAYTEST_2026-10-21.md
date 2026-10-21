# SOUL — Incidencias del playtest integrado de Proyecto 1.44
Fecha: 21/10/2026 · Origen de síntomas: prueba de usuario en Windows sobre el ejecutable generado por `Instalar_Proyecto_144_Completo.ps1`.
Base de inspección: `AlexDevJSC/144mb`, rama `main`, árbol Git `d959aa927a40711942ae0a35fa88ecc740bcc062`.

## Política de evidencia y alcance
- **Síntoma observado**: comunicado por el usuario; Soul no ha reproducido todavía la ejecución gráfica sobre ese equipo Windows.
- **Causa confirmada por inspección**: el código actual demuestra una ausencia o una ruta defectuosa concreta.
- **Hipótesis pendiente**: razonable, pero NO atribuir como causa definitiva antes de logs/reproducción.
- **Omisión de integración**: existe parte de la funcionalidad en el especialista, pero el host no la llama.
- No modificar la arquitectura pública ModKit ABI 1 ni fuentes ajenas sin revisión. No implementar lógica de juego dentro de Skin ni del renderizador Eye.
- Gravedad orientada a **playtest**, no a explotación/seguridad.

## B-001 | Colisiones ausentes en paredes interiores/casa de demostración
**Gravedad:** CRÍTICA jugable · **Propietario:** Bones · **Colaboradores:** Eye (congruencia visual) · **Estado:** CAUSA CONFIRMADA EN CÓDIGO.
**Observado:** se atraviesan paredes interiores/visibles de la casa.
**Evidencia:** `BONES/src/demo/test_house.c` implementa `bones_test_house_module()` con solo `on_scene`; `bones_house_append_scene()` agrega caras visibles mediante `bones_scene_add_quad` sin ningún `bones_world_add_collider`. `BONES/src/bones/world.c` construye colisionadores de la sala antigua, no las 10 caras de la casa. `BONES/src/bones/physics.c:blocked()` solo consulta `world.colliders`.
**Repro:** acercarse a fachada/pared de la casa y mantener W o A/D; observar cruce de geometría.
**Esperado:** superficies sólidas registradas en mundo físico detienen al cilindro del jugador y a NPC, excepto aberturas funcionales.
**Corrección propuesta:** la casa debe tener una descripción física independiente (AABB de pared divididas por huecos, IDs estables y límites/capacidad); no inferir colisión directamente del quad dibujado. No bloquear una puerta interactiva con una AABB indivisible.
**Test de aceptación:** caminar/correr/saltar hacia cada pared desde ambos lados sin penetración; pasar únicamente por puertas abiertas; colisión reproducible después de recargar y con entidades.

## B-002 | Ratón vertical funciona, horizontal no gira correctamente
**Gravedad:** ALTA · **Propietario:** Bones (capa Win32/input/cámara) · **Colaborador:** Eye (verificación presentación) · **Estado:** SÍNTOMA, CAUSA NO CONFIRMADA.
**Evidencia:** `BONES/src/app/win32_main.c:get_input` lee `GetCursorPos` y asigna `mouse_dx/mouse_dy`, reposicionando con `SetCursorPos`; `BONES/src/bones/physics.c` sí aplica `mouse_dx` al `yaw`; `BONES/modules/eye/modkit.c` recibe `camera_yaw`. No falta la fórmula yaw en código.
**Repro:** clic para capturar; mover ratón exclusivamente X ±, después Y ±; repetir con flechas izquierda/derecha.
**Esperado:** giro horizontal estable, continuo y de signo correcto; giro vertical independiente.
**Diagnóstico obligatorio:** registrar por tick delta X/Y del cursor, yaw antes/después y camera_yaw entregado al frame; aislar recenter/foco, DPI, WM_CAPTURECHANGED y múltiples ticks. Valorar WM_INPUT/Raw Input si el delta actual se pierde; NO cambiar signos por intuición.
**Test:** recorrer 360º a ambos lados sin cambiar posición de pies; mismos resultados básicos con flechas; capturar/liberar/recapturar sin bloqueos.

## B-003 | WASD no conserva semántica relativa a cámara
**Gravedad:** ALTA · **Propietario:** Bones · **Estado:** SÍNTOMA; REPRODUCCIÓN PENDIENTE.
**Evidencia:** `get_input` mapea `W/S/A/D` a forward/backward/left/right; `physics.c` convierte intención en X/Z por senos/cosenos de `yaw`. A primera inspección las bases de movimiento y de cámara son algebraicamente coherentes: no hay inversión inequívoca demostrada.
**Esperado:** W avanzar según vector de cámara horizontal, S retroceder, A/D hacer strafe perpendicular sin girar; WASD rotan su orientación únicamente con yaw. Diagonales normalizadas.
**Diagnóstico:** probar cada tecla desde yaw 0°, +90°, 180°, -90°, con ratón inmóvil, comparar desplazamiento con forward/right de cámara e imprimir yaw, feet X/Z y velocidades. Repetir sin y con pared cercana para separar colisión de orientación.
**Aceptación:** producto escalar del desplazamiento con el vector esperado positivo para W y D, negativo para S y A, aproximadamente nulo con dirección perpendicular; sin dependencia extraña de FPS.

## B-004 | Cámara se desplaza lateralmente al saltar
**Gravedad:** ALTA · **Propietario:** Bones · **Colaborador:** Eye · **Estado:** SÍNTOMA, CAUSA PENDIENTE.
**Evidencia:** en `BONES/src/bones/physics.c` el salto cambia `velocity.y` y `grounded`; no aplica impulso lateral explícito. La escena de Bones usa `player.feet + eye_height`.
**Diagnóstico:** saltar desde reposo absoluto 10 veces y luego en movimiento; registrar `feet.x/z`, `velocity.x/z`, `yaw/pitch`, cámara enviada a Eye. Separar inercia horizontal esperada de desplazamiento inducido por Space o artefacto visual (paralaje/clipping).
**Aceptación:** Space estando quieto no cambia ni X/Z ni yaw (tolerancia numérica acotada); salto conserva movimiento horizontal previamente existente sin impulsos espurios.

## B-005 | Destello/pantalla negra al desplazarse
**Gravedad:** ALTA · **Propietarios:** Bones (presentación GDI) + Eye (rasterización/clipping) · **Estado:** SÍNTOMA; HIPÓTESIS FUERTE EN PRESENTACIÓN.
**Evidencia:** `BONES/src/app/win32_main.c:present` ejecuta `PatBlt(...,BLACKNESS)` en la ventana **cada fotograma** antes de `StretchDIBits`; `WM_PAINT` solo BeginPaint/EndPaint. `BONES/third_party/eye/sw_renderer.c:p144_renderer_begin` limpia framebuffer cada fotograma, y el clip cercano del render necesita contraste por pruebas.
**Hipótesis:** se observa la limpieza negra GDI entre dos operaciones o una escena vacía/clip temporal. No puede atribuirse con certeza sin vídeo/capturas del framebuffer.
**Diagnóstico:** registrar timestamp/FPS y hash o conteo de píxeles del framebuffer por frame; comparar captura del framebuffer antes de GDI frente a vídeo de ventana. Si framebuffer conserva imagen pero ventana parpadea, investigar buffer doble/offscreen y WM_PAINT. Si framebuffer ya viene vacío, analizar clip, escena y depth. No eliminar tests visuales sin identificar capa.
**Aceptación:** 5 min de recorrido continuo sin fotogramas negros no justificados, al girar/saltar/redimensionar.

## B-006 | No hay acción Interactuar por E
**Tipo:** FUNCIÓN PENDIENTE/INTEGRACIÓN · **Gravedad:** MEDIA · **Responsables:** Bones (detección/autoridad y eventos de intención), Skin (prompts/ventanas), Brain (respuesta NPC), Eye (resaltado opcional).
**Evidencia:** `win32_main.c:get_input` no mapea E; `BonesInput` y demo no exponen sistema de objetivo/raycast interactivo. La puerta de `test_house.c` se describe como superficie visible no funcional.
**Contrato propuesto:** E = intentar interactuar al flanco de pulsación con el objetivo válido más próximo a retícula en rango corto (p. ej. 2 m, parámetro de diseño a aprobar). Bones determina objetivo e identidad/visibilidad, Skin representa acción, dueño lógico valida la consecuencia, Mouth valida autoridad online. No convertir cada tecla E en un menú genérico sin objeto seleccionado.
**Aceptación:** mostrar '[E] Interactuar' cuando haya objetivo; pulsar una vez ejecuta exactamente una intención; fuera de rango/no visible no opera; las puertas no se abren solo con quads decorativos.

## B-007 | Falta selección de partida individual y LAN
**Tipo:** OMISIÓN DE INTEGRACIÓN · **Gravedad:** MEDIA · **Responsables:** Skin (menús y navegación), Mouth (sesión LAN), Bones (arranque/cambio de modo).
**Evidencia:** Skin ya define pantallas y acciones UI2 en `BONES/modules/skin/skin_ui2.h`, pero `BONES/src/app/win32_main.c` no llama a `bones_skin_ui_input`, `bones_skin_ui_update` ni `bones_skin_ui_compose`. Mouth `BONES/modules/mouth/modkit.c` permanece inerte salvo configurar host LAN explícitamente ANTES de registrar módulos. Demo 3D Win32 no crea actor remoto ni abre sesión.
**Contrato propuesto:** menú inicial 'Individual / Crear partida LAN / Unirse a LAN / Salir'. El estado LAN debe mostrar host/puerto/IP y errores, sin simular conexión; una opción sin soporte aún debe figurar deshabilitada y explicada.
**Aceptación:** Individual inicia demo sin sockets; Crear partida habilita puerto y actor remoto; Unirse valida respuesta de host; volver al menú cierra recursos limpiamente; 2 PC LAN privada verificados antes de llamar a multijugador jugable.

## B-008 | Falta HUD
**Tipo:** OMISIÓN DE COMPOSICIÓN · **Gravedad:** MEDIA · **Responsables:** Skin (HUD), Bones/App (datos y composición), Eye (frame de fondo).
**Evidencia:** `skin_modkit_impl.c` ofrece `bones_skin_ui_compose` y `Skin2View`; el Win32 host únicamente pasa el framebuffer de Eye a GDI. No existe integración de composición de Skin; estado de Bones aún no contiene HP/maná/oro canónicos.
**Corrección propuesta:** app crea/copia framebuffer propio 320×200 (o interfaz de composición segura), mezcla Skin **después** del mundo y antes de GDI, envía datos disponibles reales, y conecta navegación. No modificar buffer `const` interno ni inventar estadísticas.
**Aceptación:** HUD dibujado sobre escena actual sin alterar Z-buffer; datos verídicos o 'N/D' si no existen; controles de menú no interfieren con juego.

## B-009 | No aparece acceso directo en Escritorio tras instalar
**Tipo:** OMISIÓN DEL INSTALADOR · **Gravedad:** BAJA · **Responsable:** Soul (instalador) · **Estado:** CAUSA CONFIRMADA.
**Evidencia:** `Instalar_Proyecto_144_Completo.ps1` crea/clona `C:\144`, compila `BONES/build/Proyecto144_Bones_ModKit.exe`, opcionalmente ejecuta, pero **no crea .lnk ni copia ejecutable a escritorio**.
**Corrección propuesta:** crear acceso directo `.lnk` del usuario a ruta estable con WorkingDirectory correcta, tras compilación exitosa (y poder regenerarlo después de actualizaciones); respetar Escritorio redirigido a OneDrive y las cuentas elevadas. Preferir acceso directo a copiar EXE obsoleto.
**Aceptación:** doble clic en acceso directo ejecuta el build vigente y funciona con rutas correctas; desinstalar/actualizar no deja enlace roto sin informar.

## Plan de corrección por fases
A. **Bloqueo jugabilidad:** B-001, B-002, B-003, B-004, B-005. Bones y Eye aportan pruebas reproducibles por separado; resolver primero orientación/input y colisiones antes de test de interacción.
B. **Usabilidad:** B-008, B-006 (HUD + tecla E); contrato de autoridad Bones/Skin/Brain con pruebas.
C. **Modos de juego:** B-007, limitado al alcance LAN comprobable; no vender conectividad online general.
D. **Distribución:** B-009, shortcut por instalador sin tocar motor.

## Check-list del próximo playtest
1. Registrar commit: `git -C C:\144 rev-parse HEAD`, compilador, resolución, FPS.
2. Ratón: ¿flechas izquierda/derecha sí giran? ¿los deltas X se registran? ¿el yaw del jugador cambia?
3. WASD: sentido a yaw 0º y 90º, quieto y en movimiento, y después junto a paredes.
4. Espacio: desde parado ¿X/Z cambia? ¿solo la percepción de cámara cambia?
5. Parpadeo: registrar 10 segundos con móvil o captura y anotar si se ve negro puro o color de limpieza.
6. Verificar qué muro se atraviesa (casa demo frente a paredes originales).
7. Menú/HUD/interacción: no clasificarlos como 'regresión' antes de haber sido conectados en Win32.
8. Ejecutar tests de Bones/Eye/Skin/Mouth que correspondan; no cerrar incidencia por 'compila'.
