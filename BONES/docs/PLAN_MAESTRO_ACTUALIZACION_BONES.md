# Proyecto 1.44 — Anexo de estado Bones, 20/10/2026

**Referencia primaria:** `PLAN_MAESTRO_ORIGINAL_v1.md`, conservado literalmente. Este anexo documenta ejecución técnica, no modifica las decisiones de diseño pendientes del maestro. La fase actual es un prototipo de físicas y contratos de módulos construido sobre la prueba visual previa de Eye.

## Hechos implementados

- **Sección 13 / Software en C:** nuevo núcleo `src/bones/*.c` independiente de render/Win32; cabecera pública `include/bones/bones.h`; interfaz con módulos y callbacks del renderizador.
- **Sección 13.3 / Bucle:** paso físico fijo de 60 Hz, input normalizado, actualización y salida visual; tiempo externo con acumulador QPC en la app Win32 (fuente codificada, ejecución Windows sin verificar).
- **Sección 6 / Gráficos:** adaptador a Eye v0.1, que mantiene texturas procedurales, quads triangulados y sprites; geometría provisional de sala y columnas. El core **no está acoplado** a la librería gráfica.
- **Sección 5 / Mundo:** claves de sectores con semilla+coordenada+versión+dominio; solo sala estática, sin streaming/generación de mundo amplio.
- **Sección 8 / Acciones:** movimiento con aceleración, carrera y salto, contacto suelo plano, colisión cilindro aproximado contra AABB estáticas y bloqueo en NPC (sin IA).
- **Sección 17 / Pruebas:** pruebas físicas, de claves, registro de módulos y comparación de hashes gráficos en un entorno Linux, incluidos sanitizadores.

## Lo que NO se ha validado ni debe anunciarse como logrado

Windows nativo, tamaño de EXE final <= 1.474.560 bytes, simulación de NPC, RPG, multiusuario, mundo virtualmente infinito, guardado/fusión, determinismo entre arquitecturas, capacidad real de terreno con pendiente. Direct3D 11 sigue siendo una **propuesta por evaluar**, no la API final. Los modos de muerte, clases y las demás decisiones pendientes del maestro permanecen pendientes.

## Coordinación con Eye

Eye controla presentación/arte; Bones controla estado/física. La integración se realiza mediante `BonesRenderBackend` (callbacks `begin`, `draw_quad`, `draw_sprite`, `end`). Cuando Eye reemplace su rasterizador, solo deberá cambiar su adaptador. El código original de Eye no se ha modificado.

## Puerta de ingeniería siguiente

Compilar y ejecutar en Windows, probar entrada y colisiones reales, medir FPS y tamaño final del EXE, resolver incidencias sin modificar en silencio objetivos de distribución. Ampliar el modelo de terreno/colisión únicamente tras esta validación y nuevas pruebas.

## Ampliación 0.2 — Host de motores complementarios (20/10/2026)

En línea con el **apartado 13** del maestro, Bones dispone ahora de un
registro estático de cinco subsistemas identificados como Eye, Ear, Brain,
Skin y Mouth. El documento maestro no define oficialmente las funciones de
los últimos cuatro; se mantienen propuestas de responsabilidad, descritas
en `CONTRATO_SUBSISTEMAS_v1.txt`, sin asumir que esas partes estén acabadas.

**Realizado:** orden de inicialización basado en dependencias, detección de
ciclos y de versiones incorrectas, callbacks de pasos de simulación, escena y
eventos, parada en orden inverso y rollback de inicialización. Eye v0.1 se
conecta al slot Eye y continúa funcionando. La simulación y colisiones no
requieren Eye, Ear, Brain, Skin ni Mouth. Las pruebas de integración simulan
los demás motores, no los implementan.

**Tamaño y compatibilidad:** el núcleo sigue sin Win32 ni dependencias de
render, y la API pública se incrementa a versión 2 por ampliación de la
estructura del motor. Los eventos volátiles actuales no son la persistencia
semántica prevista en los apartados 11 y 12. No se ha medido el PE distribuible
de Windows, por lo que sigue pendiente el requisito prioritario de 1,44 MB.

**Siguiente puerta de decisión:** validar build Windows, acordar contratos
funcionales con responsables reales de cada subsistema y formalizar eventos/
comandos autoritativos para NPC e inventario antes de conectar lógica RPG.


## Anexo de avance Bones v0.2.1 — Geometria compartida (20-10-2026)

No altera el documento maestro original. Se ha anadido una casa visual de
10 quads, con dos frontones triangulares de quads degenerados, dos faldones
inclinados, paredes, suelo y puerta puramente visual. El renderizador Eye
existente recibe los mismos ID 0 piedra, 1 suelo y 2 madera; ninguna forma
exige textura exclusiva.

Se ha creado un puente voluntario para que Eye aporte tiles ARGB 64x64
al adaptador, sin acoplar BonesScene a EyeRecipe. El codigo real EYE1/EYE2
no esta presente; su conexion queda pendiente. El núcleo C11, su cabecera
publica y sus simulaciones NO se han alterado.

Ver `CONTRATO_GEOMETRIA_MATERIALES_EYE_v0_2_1.txt` y pruebas nuevas.
La API publica sigue en 2. No hay prueba de PE Windows o certificacion de
1.474.560 bytes para el juego final.

## Anexo Bones 0.3.0 — Fundación modular (21/10/2026)

Se implementa una capa optativa `BonesFoundation` en C11 que envuelve
`BonesEngine` sin modificar `include/bones/bones.h` (API 2 intacta).
Añade entidades con identificadores de rama, acciones validadas por tick y
propietario, controlador físico reutilizado para múltiples actores,
instantáneas BSF1 de estados básicos y colisionadores, y registro de módulos
por carpetas. La escena se completa con sprites provisionales de entidades.
El registro se resuelve durante compilación y no introduce DLL de ejecución.

**Esto NO resuelve aún** P2P, guardados a disco, fusión de ramas, scripts de IA,
interfaz Skin ni la integración de Eye 0.5. El prototipo sigue teniendo límites
reducidos de actores, colisionadores y material. La API aditiva es una propuesta
que Soul debe revisar antes de fijarla como contrato público estable.

## Bones 0.4.0 — propuesta ModKit ABI 1 (21/10/2026)

Se ha implementado una capa de módulos insertables, enlazables de forma
estática para el EXE final y opcionalmente como DLL/.so durante desarrollo.
Las estructuras públicas históricas de Bones API2 no han cambiado. La
independencia de los especialistas se consigue con 
`include/bones/modkit.h` y un host de servicios de capacidad limitada,
con pruebas de permisos, ciclo de vida, render real de Eye, órdenes y
snapshots de Foundation. El contrato nuevo se considera **propuesto** hasta
revisión de Soul. **No hay validación del ejecutable Windows ni del techo
1,44MB**, y los otros motores no están implementados en esta entrega.
