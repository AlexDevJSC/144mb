# MOUTH — Propuesta de descubrimiento de mundos LAN (solo diseño)

Estado: NO APROBADO / NO IMPLEMENTADO. Fecha: 21/10/2026.
Versiones existentes: Bones 0.4.0, Mouth 0.4.0, ModKit ABI 1, wire M144/1.
Destinatarios de aprobación: Bones y Soul. Consultar a Skin, Eye y responsables de persistencia.

## Problema
Mouth 0.4.0 exige world_id, seed y generator_version iguales en ambos participantes ANTES de mouth_session_join. IP y puerto por sí solos no permiten cargar un mundo procedural desconocido, y tampoco se verifica el hash completo de reglas, recetas Eye y estado de guardados. Es importante preservar la autoridad de Bones y el límite de 1,44 MB.

## Alternativa A: invitación con datos del mundo (sin alterar M144/1)
1. Bones obtiene world_id, seed y generator_version de su mundo real.
2. Skin muestra estos valores en un código de invitación versionado y, por separado, IP:puerto. El formato del código requiere acuerdo previo.
3. El cliente introduce código e IP:puerto, valida el generador local y crea MouthLanConfig para mouth_session_join.
4. Mouth usa handshake y sesión actuales: sin protocolo nuevo y sin modificar física.
5. Si el cliente no tiene mundo o valores correctos se muestra el límite; no utilizar semillas ficticias ni la demo seed.
6. El código NO es autenticación. No se hace merge de guardados ni se permite unión a una rama divergente sin aprobación del sistema de persistencia.

Es un procedimiento para sesiones con mundo precompartido, NO un descubrimiento por IP y puerto solamente.

## Alternativa B: descubrimiento previo de metadatos, futuro M144/2
NO IMPLEMENTAR hasta aprobación de Bones+Soul y acuerdo con Eye/Skin/Brain.

Secuencia propuesta:
1. Skin recoge IP:puerto. Cliente envía WORLD_QUERY versionado, sin actor ni autoridad de gameplay.
2. Host responde WORLD_METADATA acotado: versiones wire/motor/reglas, world_id, seed, generator_version; fingerprint real de recetas y librería Eye; roles/módulos requeridos; capacidad máxima (2 en esta fase); rama/checkpoint cuando corresponda.
3. Cliente comprueba compatibilidad de generador, reglas y recetas. Igual material_id no implica igual imagen. En mismatch muestra error y NO inicia simulación.
4. Bones/Brain/Skin validan la política de branch/checkpoint. Mouth no fusiona partidas, no adjudica hechos ni altera snapshots persistentes.
5. Cliente solicita JOIN_REQUEST vinculado a esos metadatos y a una petición específica. Host comprueba capacidad, versión y reglas, revalida mundo/checkpoint si cambian, y autoriza o rechaza.
6. Bones asigna/spawnea el actor remoto y su owner real; solo tras aceptación Mouth enlaza la sesión. El cliente NO proporciona issuer o actor autorizado.
7. Arranca INPUT + SNAPSHOT del contrato autorizado; nunca se restaura el estado físico desde datos de cliente.

Opciones de compatibilidad: M144/1 permanece para mundos preconocidos; M144/2 es un protocolo aparte que se negocia explícitamente. Un cliente de versión desconocida se rechaza con causa, sin downgrade silencioso ni reinterpretar paquetes.

Reglas obligatorias de diseño:
- Formato binario endian canónico, longitudes y límites estrictos, opcodes/tamaños por definir; no hay tramas nuevas autorizadas aún.
- Poca amplificación en UDP preautorización: tamaño de respuesta y frecuencia limitada, rate-limit por endpoint, datos expuestos únicamente en LAN de confianza.
- Correlación consulta-respuesta, nonce y límites de reintentos: útiles contra confusiones/replays triviales, NO equivalen a seguridad criptográfica.
- Fingerprints de algoritmos/recetas Eye y ruleset, no sólo identificadores de material.
- El mundo debe permanecer estable entre la consulta y aceptación; si cambia, se revalida o cancela.
- Guardados: Skin/Brain/Bones deciden divergencias; no realizar conciliación en Mouth.
- Errores futuros propuestos (NO existen en API 0.4.0): WORLD_UNAVAILABLE, GENERATOR_MISMATCH, RECIPE_MISMATCH, RULESET_MISMATCH, SAVE_CHECKPOINT_CONFLICT, SERVER_FULL, JOIN_DENIED, UNSUPPORTED_WIRE, TIMEOUT.
- Distribución final en único EXE estático; no usar DLL ni servidor dedicado como precondición.

## Comparación
A: mantiene M144/1 y API 0.4, requiere código e IP:puerto compartidos; menor riesgo y más fricción al usuario.
B: permitiría unirse con IP:puerto si la negociación y persistencia se resuelven; requiere cambio explícito de protocolo, nuevo test de amenazas y revisión de presupuesto de bytes.

## Recomendación para el siguiente hito
Usar opción A sólo para playtests de mundo conocido y acordado. Someter B a aprobación de Bones y Soul, revisando primero las huellas Eye, política de checkpoint con Skin/Brain y asignación de owner con Bones.
NO programar B silenciosamente ni modificar M144/1.

## Matriz de aceptación futura (PENDIENTE, NO EJECUTADA)
- Descubrimiento sin creación de actor antes de la validación.
- Versión wire distinta rechazada sin degradación.
- Material ID igual pero generador Eye/hash diferente se rechaza.
- Replays de JOIN no crean dos actores; INPUT nunca manda issuer arbitrario.
- Server lleno, dos peticiones simultáneas, pérdida/reordenación y limitación de CPU/tráfico UDP.
- Checkpoint incompatible rechazado sin merge automático.
- Host cambia de mundo a mitad de negociación: cancelar/revalidar.
- Dos PCs Windows LAN, 60 Hz Bones, visualización de ambos actores.
- EXE combinado ≤ 1.474.560 B, medido tras incluir todos los módulos.

## Decisiones solicitadas
Bones: origen canónico world_id/seed/checkpoint; momento de spawn tras validar JOIN.
Soul: aprobar diseño A provisional o autorizar fase de diseño detallada de B; versionado y matriz mínima de QA.
Eye: composición de huella determinista de recetas y recursos.
Skin/Brain: política de entrada en rama/partida existente, sin resolver conflictos desde Mouth.
