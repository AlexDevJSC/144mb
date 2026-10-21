# PROYECTO 1.44
## Documento maestro de diseño y desarrollo — Versión 1.0

**RPG 3D procedural de fantasía medieval para Windows, desarrollado en C, con mundo simulado, cooperativo P2P y partidas guardadas fusionables.**

**Estado:** especificación inicial de producto y arquitectura; todavía no existe una implementación validada.  
**Fecha:** 20 de octubre de 2026.  
**Naturaleza del documento:** guía de desarrollo viva, no promesa de viabilidad ni inventario de funcionalidades completadas.

---

# 1. Resumen ejecutivo

PROYECTO 1.44 es un videojuego de rol en primera persona inspirado en *The Elder Scrolls: Arena* y *Daggerfall*. El jugador explora un mundo continuo y virtualmente ilimitado de fantasía medieval clásica; combate en tiempo real, sube de nivel, pertenece a una clase definida y participa en un ecosistema de aldeas, comerciantes, facciones y personajes con rutinas. La narrativa surge principalmente de las reglas del mundo y de las decisiones del jugador, no de una campaña lineal.

El juego debe ser **completamente jugable en solitario** y admitir **cooperativo PvE de dos a cuatro personas** mediante conexión directa IP/puerto. Cada copia del mundo podrá progresar sin conexión. Cuando dos jugadores vuelvan a reunirse, el sistema intentará fusionar sus versiones de partida a partir de un ancestro común, conservando cambios compatibles y presentando al anfitrión los conflictos que necesiten intervención.

El desafío distintivo es el formato: **objetivo prioritario, un único ejecutable nativo de Windows de hasta 1.474.560 bytes**, autocontenido en código y recursos de arranque, sin instalador y sin cachés persistentes obligatorias. Los archivos de guardado son una excepción explícita: almacenan el progreso creado por los jugadores. **Plan de respaldo:** paquete de distribución dentro del mismo límite, permitiendo recursos o cachés generados a posteriori.

El motor será propio, en C, con Win32 y una API gráfica nativa propuesta (Direct3D 11); almacenará la geometría de escenarios como quads y la enviará al renderizador como triángulos. Las texturas se sintetizarán a partir de recetas compactas inspiradas en las técnicas de *.kkrieger*; personajes y enemigos tendrán sprites 2D orientados hacia la cámara al estilo Daggerfall. Mundo, asentamientos y mazmorras utilizarán semillas y generadores deterministas.

**Criterio de éxito inicial:** una demo técnica que pruebe generación reproducible, renderizado, simulación mínima, P2P de dos jugadores y fusión verificable de dos guardados, sin superar el límite de distribución elegido.

# 2. Alcance y registro de decisiones

## 2.1 Decisiones confirmadas por el diseñador

| Área | Decisión |
|---|---|
| Género | Aventura RPG clásica, exploración y acción equilibradas |
| Referentes | Arena y Daggerfall |
| Ambientación | Fantasía medieval clásica |
| Narración | Emergente, producida por sistemas |
| Combate | Tiempo real y en primera persona |
| Progresión | Experiencia, niveles y clases cerradas |
| Magia | Sistema tradicional de hechizos |
| Equipo | Armaduras por piezas y distintos tipos de armas |
| Actividades | Sigilo, robo, comercio, economía, artesanía, recolección y viviendas |
| Mundo | Abierto, continuo y virtualmente infinito |
| Asentamientos | Plantillas combinadas proceduralmente |
| Mazmorras | Laberintos grandes y generación procedural con objetivos variables |
| NPC | Economía, rutinas, relaciones y mundo simulado |
| Misiones | Procedurales sencillas |
| Simulación ambiental | Clima, día/noche, NPC con rutinas; dinámica de facciones y economía como objetivo de diseño previo |
| Multijugador | Cooperativo PvE, 2–4 jugadores |
| Solitario | Juego íntegro sin conexión |
| Conexión | Dirección IP y puerto |
| Persistencia | Guardados compartidos, transferibles y fusionables; anfitrión resuelve conflictos |
| Imagen | Daggerfall clásico, 3D poligonal y enemigos/NPC en sprites 2D |
| Sonido | Minimalista por la restricción de tamaño |
| Prioridad de producto | Simulación de NPC y mundo, sin sacrificar la solidez multijugador |
| Primer entregable | Demo técnica del motor |
| Lenguaje/plataforma | C, Windows, ejecutable sin instalador |

## 2.2 Decisiones por confirmar; no deben tratarse como cerradas

- **Muerte:** el diseñador considera varias soluciones razonables, incluida recuperación del cadáver, pérdida de recursos y permadeath. El documento propone modos configurables, **sin fijar un único sistema canónico**.
- **Número y definición de clases:** cuatro clases de prototipo (Guerrero, Mago, Pícaro, Clérigo) son sugerencias de implementación, no un catálogo definitivo.
- **Escala temporal:** relación entre tiempo de juego y tiempo real, alcance de eventos históricos y reconciliación de calendarios divergentes.
- **Fidelidad de simulación:** detalle concreto de NPC, economía, relaciones y facciones en el lanzamiento.
- **Grado de interior continuo:** mundo exterior continuo confirmado; transiciones técnicas discretas para interiores siguen abiertas si aportan estabilidad.
- **API gráfica definitiva:** se propone Direct3D 11; debe compararse con un renderizador/software compacto durante el hito de investigación.
- **Tratamiento de cambios incompatibles de dos líneas de tiempo:** deben definirse reglas caso por caso y probarse con prototipos.

## 2.3 Jerarquía de prioridades

1. Ejecutable autocontenido dentro de 1,44 MB (objetivo A), y juego funcional en Windows.
2. Simulación reproducible del mundo y persistencia correcta.
3. Cooperativo P2P sólido, con límites de red comunicados honestamente.
4. Experiencia RPG de exploración y combate que resulte divertida.
5. Profundidad de economía, NPC, facciones y variedad de contenido.
6. Optimización estética, efectos adicionales y expansión de contenido.

Si las restricciones entran en conflicto, no se eliminarán silenciosamente funciones esenciales: se abrirá una decisión técnica documentada y se medirá el coste de cada opción.

# 3. Contrato de distribución: 1,44 MB

## 3.1 Objetivo A, prioritario

- **Un único `.exe` de hasta 1.474.560 bytes**, medidos en el archivo final distribuido.
- Sin instalador, motor externo ni bibliotecas redistribuibles obligatorias.
- Contiene código, shaders precompilados necesarios, datos de arranque, recetas y recursos mínimos.
- El juego puede generar escenas, texturas y mallas **en memoria**; no necesita mantener una caché gráfica persistente entre sesiones.
- Los **guardados del usuario** sí se escriben a disco y pueden superar 1,44 MB. No forman parte del tamaño de distribución.
- Debe funcionar después de copiar el ejecutable a una máquina Windows compatible, sin acceso a Internet para la modalidad individual.

El tamaño nominal de un disquete de alta densidad es 1.474.560 bytes; el espacio utilizable real de un volumen FAT12 puede ser algo menor. Si el objetivo es **caber físicamente en un disquete real**, se validará ese requisito por separado con una imagen de disquete y no solo con una comparación de bytes.

## 3.2 Objetivo B, respaldo

El paquete original de instalación/copia sigue limitado a 1,44 MB, pero el ejecutable puede generar y mantener recursos persistentes o cachés fuera del paquete después de iniciarse. Solo se activará como alternativa ante una restricción demostrada o mediante decisión explícita.

## 3.3 Lo que el límite NO mide

- RAM y VRAM consumidas durante la ejecución.
- Tamaño de capturas de pantalla y partidas guardadas.
- Datos transmitidos por red o copias que un jugador comparta voluntariamente.
- Archivos de compilación y herramientas de desarrollo que nunca se distribuyen.

## 3.4 Política de medición

El proceso de compilación de publicación generará una métrica reproducible del tamaño final y fallará si se supera el techo seleccionado. **No se asumirán ahorros por quads, compresión de ejecutable o generación procedural sin comparativas reales.** La protección del ejecutable, antivirus y advertencias de Windows se tratarán como aspectos de distribución, no como requisitos a desactivar.

# 4. Experiencia de juego y pilares

## 4.1 Fantasía del jugador

«Soy un aventurero en un reino fantástico que parece existir independientemente de mí. Puedo viajar, combatir, ganar prestigio, comerciar o provocar cambios; mis amigos pueden intervenir en ese mismo mundo, separarse y volver a unir sus historias».

## 4.2 Bucle principal

1. **Explorar:** localizar aldeas, ruinas, caminos, comerciantes y acontecimientos.
2. **Elegir:** aceptar misiones, practicar una profesión, robar, comerciar o combatir.
3. **Actuar:** realizar acciones mediante combate en primera persona y sistemas RPG.
4. **Cambiar:** actualizar economía, relaciones, seguridad, NPC, facciones y estados únicos.
5. **Progresar:** adquirir experiencia, equipo y capacidades.
6. **Persistir y compartir:** guardar el mundo, jugar offline o reunirse en P2P y fusionar cambios compatibles.

## 4.3 Principios de diseño

- **Reglas antes que excepciones:** preferir interacciones generales que produzcan situaciones emergentes.
- **Determinismo donde es necesario:** mismo estado inicial, mismas entradas lógicas y mismo orden de eventos deben producir el mismo mundo base.
- **El jugador entiende la causalidad:** una escasez, una guerra o un precio alto debe tener una explicación rastreable.
- **Juego individual íntegro:** el contenido no debe exigir un segundo jugador.
- **P2P sin promesas irreales:** la conexión directa puede exigir configuración de red; no hay conectividad universal garantizada.
- **Tamaño medido constantemente:** cada sistema entra al proyecto con un presupuesto comprobable.

# 5. Estructura del mundo

## 5.1 Espacio virtualmente infinito

El espacio se divide en **sectores identificados por coordenadas enteras de 64 bits**, semilla y versión del generador. Esto ofrece una extensión práctica enorme, aunque matemáticamente finita por límites de representación. La escena activa utilizará un origen local desplazable (*floating origin*) para no perder precisión gráfica lejos del punto inicial.

Cada sector tiene capas: relieve, bioma, hidrología simplificada, caminos, asentamientos, edificios, mazmorras y entidades relevantes. La generación debe ser estable entre sesiones y clientes que compartan versión y semilla.

## 5.2 Generación por regiones

1. Calcular claves estables a partir de `world_seed`, coordenadas, versión y dominio de generación.
2. Definir relieve y bioma en baja resolución, con continuidad en bordes de sector.
3. Determinar puntos de interés, asentamientos y rutas viables.
4. Crear la geometría local visible a partir de primitivas y quads.
5. Instanciar personajes y recursos solo si la zona está activa.
6. Aplicar sobre el estado generado las diferencias persistentes del guardado.

**Norma:** no usar un único PRNG global dependiente del orden de carga para todo el mundo; la generación de un sector debe ser independiente del orden en el que se visitan otros sectores.

## 5.3 Pueblos y ciudades

Combinación de **plantillas arquitectónicas + gramáticas procedurales**: tipologías de casas, calles, plazas, mercados, murallas, posadas y servicios. La estructura macro de un pueblo y sus identificadores deben permanecer estables. Se generan variantes visuales de forma económica, evitando que cada edificio necesite un modelo almacenado por separado.

Para evitar ciudades visualmente vacías, incluir reglas de densidad, jerarquía de vías, servicios mínimos, distribución por actividades y puntos singulares. El motor no debe generar ciudades imposibles de recorrer ni puertas inaccesibles.

## 5.4 Mazmorras

Mezcla elegida por el diseñador: **grandes laberintos** y **objetivos procedurales cambiantes**. Se proponen generadores que ensamblen habitaciones, corredores, niveles y conexiones verticales desde módulos reutilizables; cada resultado se valida por conectividad, ruta hasta el objetivo, rutas de salida y ubicación de puertas, enemigos y tesoros.

Las mazmorras pueden contener zonas especiales realizadas manualmente como plantillas, sin abandonar su disposición procedural. Una misma semilla permite que varios jugadores vean el mismo laberinto.

## 5.5 Visibilidad y streaming

- Activar únicamente sectores cercanos; retirar los inactivos de la escena gráfica.
- Agrupar superficies por material y textura para reducir llamadas de dibujo.
- No dibujar caras interiores ocultas de edificios cerrados.
- En interiores, estudiar portales/salas y *frustum culling*.
- Mantener estados agregados de regiones no visibles, no todas sus entidades individualizadas.

# 6. Motor gráfico inspirado en Saturn y Daggerfall

## 6.1 Representación geométrica

Paredes, suelos, techos, tejados y objetos sencillos se describen internamente como cuadriláteros, a menudo deducidos directamente de reglas geométricas. La GPU recibe triángulos: **cada quad plano se triangula en memoria**, con UV, normales y orden de vértices coherentes. No se presupone que cuatro vértices sean más rápidos que seis índices; la ventaja buscada es simplificar datos y algoritmos de autoría/generación.

Las superficies no planas o complejas se subdividen, porque un quad deformado no admite una interpolación geométrica única universal. El motor utilizará proyección 3D y texturizado correctos; no reproducirá las deformaciones de la Saturn salvo como un efecto visual deliberado.

## 6.2 API y ejecutable

**Propuesta inicial:** Win32 para ventana/entrada, Direct3D 11 para gráficos, API de audio del sistema, Winsock2 para red. Compilar shaders fuera del ejecutable de publicación e incrustar los bytecodes requeridos. Evitar dependencias de desarrollo que tengan que instalarse en el PC del jugador. El motor no podrá suponer que disponer de Direct3D garantiza un dispositivo gráfico con todas las funciones opcionales; debe definir requisitos mínimos y fallar con mensaje claro.

## 6.3 Sprites 2D en un entorno 3D

NPC y enemigos son *billboards* verticales con animaciones por fotogramas y orientaciones discretas. Las versiones de prototipo empezarán con uno o pocos ángulos y estados (reposo, caminar, atacar, caer). La paleta y el atlas se comparten entre grupos de criaturas; las variaciones de color pueden sintetizarse en memoria.

El sistema de colisiones de entidades será independiente del sprite visible. La rotación visual no debe cambiar el volumen de impacto ni la física del personaje.

## 6.4 Estética

Resolución interna reducida opcional, paletas restringidas y texturas pequeñas. El objetivo es **estética Daggerfall legible**, no replicar todos los defectos de Saturn. Dibujar a baja resolución no obliga a degradar controles ni legibilidad de la interfaz en monitores modernos.

# 7. Texturas y materiales procedurales

## 7.1 Principio

Inspiración conceptual en *.kkrieger*: almacenar recetas y operadores reutilizables en lugar de numerosas imágenes finales. Un material puede combinar patrón base, ruido determinista, máscara, deformación, cuantización de color y desgaste.

## 7.2 Pipeline de material

1. Identificar familia de material: piedra, madera, ladrillo, tierra, metal, tela o vegetación.
2. Leer una receta compacta versionada, parámetros y semilla.
3. Evaluar operadores deterministas en CPU o GPU durante la carga de una región.
4. Convertir a atlas/imagen final en RAM/VRAM; reutilizar mientras sea visible.
5. Descartar del caché **en memoria** al liberar la región. No exigir una caché persistente en disco.

## 7.3 Estructura orientativa de receta

```text
MaterialRecipe {
  family: STONE,
  palette: GREY_MOSS,
  scale: 2,
  pattern_id: 3,
  noise_seed: 0x26AC,
  wear: 5,
  moss: 2
}
```

Es una especificación conceptual, no un formato binario final. Para el ejecutable restringido podría codificarse en pocos bytes por variante, compartiendo generadores y paletas.

## 7.4 Decisiones de rendimiento y compresión

No se sintetiza la misma textura cada fotograma. La generación puede ser diferida, priorizada y cacheada en memoria. Antes de implementar un operador, comparar **coste del código + recetas** con el coste real de almacenar unas pocas texturas comprimidas; la generación procedural no garantiza siempre una reducción de tamaño.

## 7.5 Sonido

Audio minimalista: pequeñas formas de onda, envolventes y secuencias para efectos básicos; música ambiental sencilla si cabe. No se almacenan grabaciones extensas. La calidad audible se evaluará en altavoces comunes y con niveles de volumen seguros; el silencio ambiental puede ser una herramienta artística legítima.

# 8. Sistemas de rol

## 8.1 Personaje y clases

El jugador crea personaje dentro de una **clase cerrada**, obtiene experiencia, sube niveles y equipa armas/armaduras por piezas. Las primeras cuatro clases sugeridas para validar la arquitectura son Guerrero, Mago, Pícaro y Clérigo. No se adopta aún una lista definitiva ni una progresión numérica sin pruebas de equilibrio.

**Estructura mínima de datos:** identidad de personaje, clase, nivel, experiencia, atributos, salud, maná, competencias, inventario, equipo y reputaciones. Las acciones que modifican la propiedad de objetos requieren eventos persistentes inequívocos.

## 8.2 Combate

Tiempo real en primera persona: movimiento, orientación, ataques con distintas categorías de arma, bloqueo/esquiva simplificados, lanzamiento de hechizos y enemigo con estados de percepción y ataque. El anfitrión valida impactos y gastos de recursos en multijugador. Primero se diseña un combate simple y fiable; después se amplían movimientos, efectos y IA.

## 8.3 Magia

Magia tradicional: hechizos categorizados en daño, defensa, curación, control y utilidad. Los efectos deben reutilizar operadores generales (modificar estado, aplicar daño, duración, área, condición) para contener el tamaño del ejecutable.

## 8.4 Equipamiento, botín y objetos únicos

Armaduras por piezas y tipos de armas. Cada instancia que pueda cambiar de dueño se identifica de manera estable, evitando duplicaciones durante fusión de partidas. Los recursos fungibles (monedas, provisiones) utilizan operaciones rastreables de débito/crédito, no solo sobrescrituras de valores.

## 8.5 Muerte, cadáver y riesgo

**Requisito confirmado:** interesa que un cadáver pueda retener el equipo. **Por cerrar:** modo de penalización y permadeath. Propuesta de modos: normal (reaparición y recuperación de cadáver), difícil (pérdida adicional), extremo (muerte permanente del personaje). La primera demo técnica no necesita implementar todos los modos; sí debe reservar eventos de muerte y posesión de equipo.

## 8.6 Actividades no bélicas

Sigilo, robo, comercio, recolección, artesanía y viviendas pertenecen al diseño objetivo. Para controlar alcance se implementarán después de contar con NPC, propiedad de objetos y guardados estables. El comercio será el primer sistema no bélico; permitirá ejercitar simulación, transacciones y fusión.

# 9. Simulación de NPC, facciones y economía

## 9.1 Tres niveles de detalle

- **Nivel cercano (individual):** NPC activos con posición, horario, inventario de interés, percepción y comportamiento.
- **Nivel de asentamiento (agregado):** población, empleo, alimentos, producción, riqueza, seguridad, precios y relaciones.
- **Nivel regional (latente):** reglas y estados compactos, materializados cuando la región se visita o interviene en un acontecimiento relevante.

No se simularán cada segundo todos los NPC de un mundo virtualmente infinito: sería computacionalmente inviable.

## 9.2 Día/noche y rutinas

Habrá un reloj del mundo independiente del reloj real. Los NPC cercanos siguen horarios de trabajo, descanso y comercio; el clima afecta visibilidad o desplazamiento si se justifica. Cuando una región distante vuelve a activarse, su estado se actualiza mediante pasos agregados deterministas, no reproduciendo millones de fotogramas pasados.

## 9.3 Economía mínima viable

Cada asentamiento produce y consume categorías agregadas: alimento, materias primas y bienes. Fórmulas sencillas modifican reservas y precio local. Transacciones importantes crean eventos persistentes. La economía tendrá controles para evitar crecimiento negativo absurdo, precios infinitos y bucles de explotación triviales.

## 9.4 Facciones y relaciones

Una facción posee afiliaciones, reputación, influencia y relaciones con otras facciones. Las decisiones del jugador y acontecimientos agregados cambian esos valores. Las guerras, bloqueos o cambios de control son contenido avanzado, **no requisitos del primer prototipo**. Debe existir siempre un motivo legible que explique el cambio.

## 9.5 Misiones procedurales sencillas

Contratos a partir de **solicitante + necesidad + destino + acción + recompensa + condiciones**. Ejemplos: entregar provisiones, despejar una mina, recuperar un objeto único, escoltar, investigar una desaparición. Las misiones se validan para no referirse a NPC inaccesibles, objetos ya consumidos o destinos imposibles. Los resultados se guardan como eventos semánticos, no como texto de misión únicamente.

# 10. Multijugador P2P

## 10.1 Modelo de autoridad

De uno a cuatro jugadores; el **anfitrión autoritativo** ejecuta la simulación que modifica el estado compartido. Los clientes calculan el terreno estático localmente y muestran predicción/interpolación de movimiento. El anfitrión valida daño, inventario, recursos, activación de misiones y acontecimientos de mundo.

P2P significa aquí **tráfico directo entre el anfitrión y sus amigos**; no significa que todos los participantes sean autoridades iguales sobre la simulación.

## 10.2 Transporte

Winsock2 y UDP. Habrá canales lógicos: actualizaciones no fiables y frecuentes para posiciones; mensajes secuenciados y reintentables para acciones importantes; transferencias fragmentadas y verificadas para instantáneas o cambios de guardado. Es una propuesta de protocolo: todavía se deben medir congestión, latencia y tamaño de implementación.

## 10.3 Protocolo de entrada

1. El anfitrión abre un mundo y escucha en puerto UDP configurable.
2. El cliente introduce IP y puerto.
3. Intercambian versión de protocolo, hash del ejecutable o protocolo de reglas, `world_id`, semilla y versión de generación.
4. Verifican si las versiones de guardado comparten ancestro y si requieren fusión previa.
5. El anfitrión fija el estado válido y transmite diferencias o una instantánea verificada.
6. El cliente entra en la simulación y recibe actualizaciones de estado.

Si las versiones de reglas o del generador no son compatibles, **no se intentará una sincronización silenciosa**.

## 10.4 NAT, cortafuegos y seguridad

Conectar IP/puerto puede exigir configurar reenvío de puertos y autorizar el ejecutable en el cortafuegos. No se garantiza conectividad entre todas las redes domésticas sin servidores auxiliares. Se podría investigar NAT traversal en el futuro; la versión mínima se limita a LAN y conexiones directas donde el anfitrión sea alcanzable.

Los paquetes se validan con tamaño máximo, versión, secuencia y rangos permitidos. Se restringen mensajes inválidos, reenvíos repetidos y solicitudes que alteren estados de otros jugadores. Sin una infraestructura de autenticación adicional no se promete seguridad fuerte ni resistencia completa frente a trampas.

## 10.5 Reglas de sesión

Los guardados solo se consideran confirmados cuando la transacción correspondiente ha sido aceptada por el anfitrión. Ante desconexión, el cliente recupera el último estado confirmado; las acciones sin confirmar no se dan por realizadas. La migración automática de anfitrión queda fuera de la primera versión.

# 11. Persistencia y fusión de mundos (PBW)

## 11.1 Objetivo

Permitir que dos jugadores continúen **el mismo mundo** desde un guardado común, progresen por separado y, al reunirse, fusionen lo compatible sin un servidor persistente. Esta es una característica diferencial y uno de los mayores riesgos técnicos.

## 11.2 Modelo de datos

- **World ID:** identidad estable del mundo, distinta de su semilla.
- **Generation version:** versión de reglas que crea regiones, NPC y elementos de base.
- **Snapshot/base:** instantánea del último ancestro común entre las ramas.
- **Branch ID:** identidad de cada historia divergente.
- **Event ID:** identificador único por rama y secuencia.
- **Semantic events:** compras, apertura de cofres, cambios de facción, muertes, resolución de misiones, etc.
- **Materialized state:** estado actual derivado del mundo inicial más eventos y compactaciones.

La fusión no debe equiparar «dos guardados con la misma semilla» con «dos versiones del mismo mundo». La historia compartida y el `world_id` son imprescindibles.

## 11.3 Algoritmo de fusión propuesto

1. Validar formatos, mundo de origen y ancestro común.
2. Calcular diferencias de las ramas A y B respecto al ancestro.
3. Clasificar cambios por entidad/recurso, tipo de operación y dependencia causal.
4. Fusionar automáticamente cambios independientes y operaciones con reglas conmutativas verificables.
5. Simular o validar invariantes: propiedad única, cantidades no negativas, misiones coherentes, NPC vivos/muertos, estado de facciones.
6. Crear una cola de conflictos semánticos; mostrar consecuencias comprensibles al anfitrión.
7. Resolver dependencias posteriores que resulten incompatibles con la elección.
8. Crear **una nueva instantánea fusionada**, conservando ramas originales como respaldo hasta confirmar integridad.
9. Distribuir el hash/identidad del resultado antes de iniciar el juego compartido.

No se intentará fusionar por mera comparación de marcas de tiempo, ni se usará *last-write-wins* como política universal.

## 11.4 Tabla de reglas de fusión iniciales

| Cambio de dos ramas | Regla preliminar |
|---|---|
| Abrir cofres distintos | Combinar si independientes |
| Descubrir lugares distintos | Unión de descubrimientos válidos |
| Dos compras independientes en tiendas distintas | Combinar si fondos, stock y propiedad permiten ambas |
| Comprar la misma pieza única en dos ramas | Conflicto de propiedad; nunca duplicar |
| Matar y salvar al mismo NPC | Conflicto narrativo explícito |
| Dos versiones incompatibles de una misión | Elegir rama o reconciliar mediante regla específica |
| Cambiar reservas del mismo pueblo | No sumar estados finales: conciliar transacciones y calendario |
| Dos ramas con progresión de tiempo distinta | Política temporal expresa; nunca sumar duraciones sin análisis |
| Divergencia de inventario de un mismo personaje | Verificar propiedad y origen de cada objeto; conflicto si hay doble gasto |

## 11.5 El anfitrión como árbitro, con límites

El anfitrión decide conflictos no resolubles automáticamente y recibe una explicación de efectos secundarios. No puede «fusionar» una muerte y supervivencia contradictorias simplemente editando un booleano: deben invalidarse o revisarse los acontecimientos dependientes.

La interfaz incluirá: entidad afectada, versión A, versión B, motivo, consecuencias conocidas, opción de conservar A/B, aplazar y cancelar fusión. Las decisiones quedan registradas como acontecimientos de reconciliación auditables.

## 11.6 Restricciones explícitas

- Las fusiones automáticas **no son garantizadas** aunque ambos jugadores actúen de buena fe.
- Mantener economías y calendarios divergentes compatibles requiere políticas específicas.
- Una fusión puede ser imposible sin perder o reinterpretar parte del progreso de alguna rama.
- La primera versión permitirá fusionar solo clases de acontecimientos probadas; lo desconocido bloqueará la operación con un error claro.
- Se mantiene siempre una copia recuperable del último guardado válido.

# 12. Archivos de guardado, versiones y estabilidad

## 12.1 Formato binario propuesto

Cabecera con `magic`, versión del formato, versión de generación, `world_id`, `branch_id`, semilla, fecha lógica, tamaño total, índice de bloques y verificación de integridad. Bloques separados para estado agregado, entidades únicas, personajes, inventarios, descubrimientos, eventos y metadatos de ancestros.

La representación exacta debe ser portable entre compilaciones: **no serializar directamente estructuras C con padding, punteros u orden de bytes implícito**. Usar enteros de tamaño fijo, orden de bytes documentado y límites explícitos.

## 12.2 Identidad de objetos y transacciones

Identificadores estables derivados de mundo/sector/generador para entidades base, y de rama/secuencia para entidades creadas durante el juego. Cada transferencia relevante de objetos tiene origen, destino y event ID, para impedir duplicación por repetición de paquetes o por merge ingenuo.

## 12.3 Guardado seguro

Escritura a archivo temporal, validación y sustitución del original mediante mecanismo adecuado del sistema. Ante corrupción o fallo de energía, ofrecer recuperación del último estado completo. Un checksum detecta corrupción accidental; no constituye prueba criptográfica de autoría.

## 12.4 Compatibilidad entre versiones

La versión del ejecutable no puede cambiar sin más la geometría de zonas ya persistidas. La política inicial será **fusión solamente entre versiones compatibles**; las migraciones del formato se implementarán de forma explícita y probada. Compatibilidades antiguas se limitarán si mantener muchos generadores elevaría demasiado el tamaño del programa.

# 13. Diseño de software en C

## 13.1 Módulos propuestos

```text
src/
  app/        arranque Win32, bucle principal, entrada
  core/       memoria, ids, PRNG, tiempo, serialización
  render/     dispositivo D3D11, quads, sprites, atlas, cámara
  world/      sectores, biomas, poblaciones, edificios, mazmorras
  texture/    operadores, recetas y síntesis de materiales
  sim/        reloj, NPC, economía, facciones, acontecimientos
  rpg/        personajes, combate, hechizos, objetos, inventario
  net/        UDP, protocolo, orden, fiabilidad selectiva
  save/       snapshots, journal de eventos, diff, merge
  ui/         HUD, menús, diálogo de conflictos
  audio/      síntesis y mezclador mínimo
assets_src/    fuentes no distribuidas: recetas, tablas, sprites de autor
shaders_src/   HLSL de desarrollo; salida compilada embebida
scripts/       compilación, empaquetado, pruebas y medición
spec/          formatos binarios, protocolo y reglas de fusión
```

La estructura es conceptual: para producir un ejecutable pequeño podrían combinarse módulos en unidades de compilación y aplicar optimizaciones de enlace, sin perder separación lógica en el código fuente.

## 13.2 Contratos básicos (pseudocódigo C, no compilado)

```c
typedef struct { uint64_t hi, lo; } WorldId;
typedef struct { int64_t x, y; } SectorCoord;
typedef struct { uint64_t branch, serial; } EventId;

bool world_generate_sector(WorldId world, uint64_t seed,
                           uint32_t generator_version,
                           SectorCoord where, SectorData *out);

bool host_apply_action(WorldState *world,
                       PlayerId actor, const Action *request,
                       Event *committed_event);

MergeResult save_merge_three_way(const Save *common_base,
                                 const Save *a, const Save *b,
                                 ConflictList *out_conflicts);
```

La semilla por sí sola no determina un resultado si cambian algoritmos, punto flotante, orden de eventos o versiones. El prototipo deberá incluir pruebas de igualdad de *hashes* del estado generado entre máquinas.

## 13.3 Bucle del programa

Entrada y mensajes de red → simulación de paso fijo → confirmación de eventos → actualización de la región activa → renderizado interpolado → audio/HUD. Las rutinas de generación largas deben amortizarse para no bloquear el movimiento. El anfitrión no usará el tiempo de render como reloj de simulación.

## 13.4 Herramientas de desarrollo (fuera del límite)

Se permite utilizar compiladores, depuradores, analizadores de rendimiento, generadores de shaders y scripts Python en el proceso de creación: **solo el artefacto distribuido** está limitado a 1,44 MB. Un editor de materiales o herramienta visual de misiones puede existir aparte, pero su contenido exportado tiene que respetar el presupuesto del juego.

# 14. Presupuesto de tamaño y rendimiento

## 14.1 Cuotas iniciales de ingeniería (NO tamaños medidos)

| Componente | Techo orientativo |
|---|---:|
| Ventana Win32 y renderizador D3D11 | 210 KiB |
| Generación de mundo y mazmorras | 180 KiB |
| Síntesis de materiales y recetas | 120 KiB |
| RPG, combate y objetos | 170 KiB |
| Simulación de NPC, economía y misiones | 190 KiB |
| Guardados, versiones y fusión | 180 KiB |
| UDP, protocolo y sincronización | 120 KiB |
| Sprites, datos artísticos y animación | 145 KiB |
| Interfaz, entrada adicional y sonido | 65 KiB |
| Reserva de integración | 60 KiB |
| **Total** | **1.440 KiB** |

Son **cuotas de planificación negociables**, no previsiones de compilación. Que sumen 1.440 KiB no demuestra que los componentes quepan. Se medirán bytes reales y se redistribuirá espacio en cada hito. La reserva de 60 KiB es estrecha, por lo que el riesgo de sobrepasar el límite es alto.

## 14.2 Objetivos de rendimiento por validar

- Movimiento fluido en una región de prototipo con decenas de entidades, primero en hardware de referencia medido.
- Simulación con paso fijo independiente de FPS, sin saltos perceptibles al generar sectores.
- P2P de dos clientes en LAN con reconciliación correcta bajo latencia y pérdida de paquetes simulada.
- Generación y guardado de sectores reproducibles con hashes idénticos.

No fijar requisitos de RAM, GPU o FPS universales antes de instrumentar el prototipo en hardware concreto.

# 15. Roadmap de producción y puertas de decisión

## Fase 0 — Investigación y esqueleto de publicación

**Resultado:** compilar un `.exe` en C para Windows sin dependencias redistribuibles obligatorias; ventana, entrada, salida limpia, informe de bytes y pruebas de ejecución en entorno limpio.

**Puerta:** ¿arranca sin instalación? ¿es medible el tamaño y reproducible la compilación? Si no, no avanzar.

## Fase 1 — Renderizador mínimo

**Resultado:** cámara en primera persona; una habitación de seis quads convertidos a triángulos; una textura procedural; un billboard animado; movimiento y colisión básica.

**Puerta:** representación correcta de quads, texturas, cámara y sprite; tamaño auditado.

## Fase 2 — Mundo determinista

**Resultado:** sectores conectados sin discontinuidades visibles, terreno y un asentamiento por plantillas, una mazmorra accesible, regeneración idéntica desde semilla.

**Puerta:** mismos hashes en dos ejecuciones y cargas de sectores independientes del orden.

## Fase 3 — Estado y simulación

**Resultado:** reloj del mundo; un comerciante con horario, stock y precio; una operación de compra; persistencia de una puerta/cofre y una entidad única.

**Puerta:** guardar, cerrar, abrir y recuperar exactamente el estado; validación de propiedad de objetos.

## Fase 4 — P2P básico

**Resultado:** dos instancias conectadas en LAN por IP/puerto; movimiento visible, una interacción compartida, reconexión básica y eventos importantes confirmados por anfitrión.

**Puerta:** pérdida/duplicación de paquetes no duplica objetos ni aplica dos veces una transacción.

## Fase 5 — Fusión de partidas

**Resultado:** dos ramas que parten del mismo guardado; una modificación independiente por rama; merge automático; un conflicto deliberado; interfaz de resolución y respaldo de guardado.

**Puerta:** se preservan las invariantes del mundo y ninguna fusión destruye la última partida válida.

## Fase 6 — Vertical slice RPG

**Resultado:** una clase o pequeña selección, combate, un hechizo, una mazmorra con enemigo, misión procedural básica, recompensas y progresión. Primero jugable en solitario; validar cooperativo después.

**Puerta:** un ciclo completo exploración → acción → progreso → guardado → reunión P2P es divertido y técnicamente reproducible.

## Fase 7 — Ampliación controlada

Expandir biomas, facciones, artesanía, viviendas, magia y economía **solo si** el binario permanece bajo el objetivo o se acepta formalmente el plan B.

# 16. Primera demo técnica: especificación cerrada de aceptación

La demo mostrará una región generada desde una semilla con pequeña aldea, camino y mazmorra; un comerciante, un cofre, dos jugadores y un elemento persistente. No promete campaña, exploración infinita exhaustiva ni una economía profunda.

**Escenario de prueba A — Determinismo:** dos ejecutables compatibles generan las mismas coordenadas, edificio y mazmorra desde la misma semilla; los hashes de datos canónicos coinciden.

**Escenario B — Estado persistente:** se abre un cofre, se guarda, se cierra y se carga; el cofre sigue abierto y conserva su identificador.

**Escenario C — P2P:** dos jugadores conectados por IP/puerto se ven mover y la recogida de un objeto único aparece una sola vez en ambos.

**Escenario D — Merge automático:** tras dividirse un guardado, A abre un cofre y B realiza una compra independiente; ambos cambios sobreviven al merge.

**Escenario E — Merge conflictivo:** A y B adquieren por separado el mismo objeto único o resuelven una misma decisión de forma contradictoria; la fusión detecta conflicto, pide intervención y jamás duplica el objeto.

**Escenario F — Integridad:** interrumpir la escritura del guardado no debe destruir el último guardado íntegro.

**Escenario G — Tamaño:** el ejecutable final medido está por debajo del techo del modo de publicación elegido; si no lo está, la demo se etiqueta como funcional pero **no conforme al objetivo de tamaño**.

# 17. Estrategia de pruebas

## 17.1 Unitarias y deterministas

- Hashes de sectores generados con varias semillas y coordenadas positivas/negativas.
- Continuidad de relieve en bordes y existencia de camino/entrada a mazmorra.
- Aleatoriedad estable pese a cambiar el orden de carga.
- Economía con reservas limitadas y transacciones sin saldo negativo cuando corresponda.
- Serialización/deserialización con tipos de tamaño fijo y datos corruptos.

## 17.2 Propiedades de fusión

- Fusión de ramas idénticas = mismo estado sin duplicaciones.
- Fusión de cambios independientes = unión válida.
- Eventos repetidos o reordenados fuera de condiciones seguras = rechazados o tratados sin doble aplicación.
- Un objeto único tiene un único propietario válido.
- Resolución de conflicto no deja misiones imposibles sin declararlo.
- Merge fallido no sobreescribe los guardados de entrada.

## 17.3 Red y robustez

- Retardo, jitter, pérdida y duplicación de datagramas.
- Desconexión durante comercio, muerte, recogida y guardado.
- Versiones incompatibles de protocolo y estado.
- Paquetes con longitudes maliciosas, IDs inválidos y fragmentación incompleta.
- Conexión LAN y conexión por Internet con el puerto expuesto, dejando constancia de NAT incompatible.

## 17.4 Regresión de tamaño

Registrar bytes por compilación y probar la variante de publicación. Cualquier hito que supere presupuesto debe señalar el módulo responsable, valorar alternativas y conservar una compilación anterior recuperable.

# 18. Riesgos, mitigaciones y criterios de parada

| Riesgo | Impacto | Mitigación / decisión |
|---|---|---|
| El EXE supera 1,44 MB | Crítico | Medición continua, reducción de dependencias, plan B documentado |
| Procedural pero repetitivo | Alto | Plantillas singulares, reglas de composición, tests de variedad |
| Mundo enorme con simulación costosa | Crítico | Tres niveles de simulación; solo estado relevante |
| Incompatibilidad de semillas/versiones | Alto | Generador versionado, IDs estables y hashes canónicos |
| Merge crea objetos duplicados | Crítico | Propiedad única, eventos idempotentes y pruebas adversarias |
| Merge produce contradicciones narrativas | Crítico | Conflictos explícitos y revisión de dependencias |
| P2P falla tras NAT | Alto | IP/puerto, guía de red; no prometer universalidad |
| Caída del anfitrión | Alto | Confirmaciones y guardados íntegros; sin migración automática en MVP |
| Desborde de alcance RPG | Crítico | Demo técnica y vertical slice antes de ampliar contenido |
| Estética demasiado pobre | Medio | Pruebas visuales tempranas con materiales y sprites artísticos |

**Criterio de parada de una función:** si su implementación compromete integridad del guardado o identidad única de objetos, no debe incorporarse al prototipo público hasta tener una corrección verificada.

# 19. Primer backlog accionable

| ID | Historia de ingeniería | Prueba de terminado |
|---|---|---|
| ENG-001 | Crear proyecto C y compilación Windows x64 | EXE arranca sin instalador |
| ENG-002 | Medidor automatizado de bytes de release | Build falla si se rebasa límite |
| GFX-001 | Abrir ventana y presentar un fotograma | Frame estable sin librerías externas |
| GFX-002 | Cámara FPS y entradas | Movimiento y ratón funcionales |
| GFX-003 | Quad lógico convertido a 2 triángulos | Pared correcta con UV |
| MAT-001 | Generador procedural de piedra | Misma semilla produce misma textura |
| GFX-004 | Billboard de NPC y colisión | Sprite correcto sin afectar hitbox |
| WRD-001 | PRNG por dominio/sector | Independencia del orden de carga |
| WRD-002 | Terreno con bordes continuos | Test de vecinos pasa |
| WRD-003 | Pueblo por plantillas | Mismo trazado por semilla |
| WRD-004 | Mazmorra conectada | Entrada, objetivo y salida alcanzables |
| SIM-001 | Reloj lógico y comerciante | Horario reproducible |
| SAVE-001 | Guardado binario versionado | Round-trip exacto y verificado |
| SAVE-002 | Identificadores y eventos únicos | Idempotencia de aplicación |
| NET-001 | Host/cliente UDP por IP/puerto | Dos instancias LAN conectadas |
| NET-002 | Replicar un objeto único | Sin duplicación con paquetes repetidos |
| MERGE-001 | Identificar ancestro común | Rechazar mundos ajenos |
| MERGE-002 | Fusionar eventos independientes | Propiedades verificadas |
| MERGE-003 | Resolver conflicto de objeto único | UI y resultado coherente |
| QA-001 | Ejecutar suite de pruebas de integración | Informe repetible y fallos bloqueantes |

**Dependencias principales:** ENG-001→GFX-001→GFX-003; WRD-001→WRD-002/003/004; SAVE-001/002→NET-002→MERGE-001/002/003. No comenzar las fases de economía profunda sin demostrar integridad de SAVE-002.

# 20. Organización de trabajo y documentación

Mantener el proyecto en control de versiones con código, pruebas, herramientas, especificación de guardado, protocolo de red, **registro de decisiones arquitectónicas (ADR)** y changelog de compatibilidad. Cada cambio que afecte a la generación del mundo o a los guardados obliga a incrementar la versión correspondiente y a añadir pruebas.

**Definition of Done de una funcionalidad:** especificación breve, implementación C, prueba automatizable o manual reproducible, documentación de formato/protocolo si aplica, medición de tamaño, verificación de regresiones críticas y decisión de inclusión.

**Cadencia propuesta:** hitos de demostración técnica por módulos, sin prometer fechas ni presupuestos de tiempo hasta medir la velocidad real de desarrollo.

# 21. Elementos fuera de alcance deliberadamente

- Servidores dedicados y mundos públicos persistentes cuando nadie juega.
- Matchmaking online, cuentas, microtransacciones y servidores centrales de autenticación.
- MMORPG masivo o cooperación para más de cuatro personas en la primera versión.
- Simulación individual de toda la población de un mundo infinito.
- Narrativa principal escrita de gran extensión y cientos de misiones a mano.
- Migración automática del anfitrión, antitrampas competitivo y conectividad NAT universal.
- Motor multiplataforma desde el primer día.

**Otra idea distinta de conversaciones previas:** *PROTOCOL: PANIC*, el Friendslop 3D de puzles contrarreloj con habitaciones interconectadas, es **un proyecto separado**. Comparte principios de cooperación y sistemas emergentes, pero no debe incluirse como subsistema de este RPG ni consumir su presupuesto de 1,44 MB.

# 22. Cuestiones pendientes de decisión

1. ¿Cuál es el formato definitivo de clases y progresión inicial? Las cuatro clases citadas son propuestas.
2. ¿Qué modos de muerte se ofrecerán y cómo interactúan cadáver, equipo y fusión?
3. ¿Qué duración de día, calendario y política de reconciliación de tiempos divergentes tendrá el juego?
4. ¿Hasta qué punto se preservarán las rutinas individuales de NPC importantes al descargar un sector?
5. ¿Se permiten transiciones breves a interiores o todos los espacios serán visibles de forma físicamente continua?
6. ¿Cuál es el mínimo hardware Windows objetivo que justifica la elección de D3D11?
7. ¿Qué parte de la fusión de inventarios, economías y misiones puede automatizarse sin pérdida de consistencia?
8. ¿Qué límites de tamaño se impondrán a los guardados y al historial de ramas?
9. ¿El modo extremo de muerte estará activado por personaje, por mundo o por sesión?
10. ¿Cómo se distribuirá un ejecutable no firmado sin confundir al jugador respecto a los avisos de Windows?

Estas preguntas **no bloquean iniciar** los primeros hitos gráficos y de persistencia, siempre que sus contratos se mantengan suficientemente generales.

# 23. Próximas acciones recomendadas

**Primera tarea real:** compilar una aplicación Win32 mínima en C, mostrar una habitación 3D formada por quads, generar una textura de piedra a partir de una semilla y comprobar el tamaño del ejecutable. Este primer artefacto demostrará la base gráfica y permitirá elegir con datos si Direct3D 11 es viable para el límite.

**En paralelo, sobre papel:** cerrar el modelo de `world_id`, `branch_id`, `entity_id`, `event_id` y el formato de guardado v1. La fusión debe diseñarse antes de que comercio, botín o misiones empiecen a crear estados difíciles de reconciliar.

**Segundo experimento decisivo:** dos guardados derivados del mismo estado con cambios independientes y contradictorios, fusionados mediante una herramienta de prueba sin motor gráfico. Si esto falla, el proyecto conserva su RPG procedural, pero no debe anunciar guardados fusionables como característica probada.

**Regla final:** el proyecto debe demostrar primero que es pequeño, determinista y consistente. Después debe demostrar que es divertido. Solo entonces merece escalar su mundo y su simulación.

---

# Anexo A. Glosario esencial

- **Quad:** superficie lógica de cuatro vértices, triangulada para renderizado.
- **Billboard:** plano con sprite que se orienta a la cámara.
- **Semilla:** valor determinista que inicializa un generador de contenido.
- **Sector:** unidad espacial de generación/carga de mundo.
- **Anfitrión autoritativo:** participante cuyo proceso valida el estado común de la sesión.
- **Snapshot:** instantánea verificable de un estado.
- **Evento semántico:** operación con significado de juego, como comprar, morir o completar misión.
- **Ancestro común:** estado compartido del que partieron dos ramas de guardado.
- **Fusión de tres vías:** comparación del ancestro común con cada rama para reconciliar diferencias.
- **Invariante:** propiedad que siempre debe cumplirse, como que un objeto único no pertenezca a dos jugadores simultáneamente.
- **Determinismo:** mismos datos y reglas producen resultados equivalentes verificables.
- **Plan B:** paquete de distribución de 1,44 MB que permite cachés generadas persistentes.

# Anexo B. Estado de avance al emitir este documento

| Elemento | Estado |
|---|---|
| Visión y alcance conceptual | Definidos a nivel de diseño |
| Decisiones del cuestionario | Recogidas; excepciones identificadas |
| Arquitectura C/Windows | Propuesta, sin prototipo comprobado |
| Renderizador y síntesis de texturas | Propuestos, no implementados |
| Simulación, economía y misiones | Especificadas conceptualmente |
| Multijugador P2P | Protocolo propuesto, sin pruebas |
| Guardados fusionables | Algoritmo de alto nivel, sin pruebas |
| Límite real de 1,44 MB | Objetivo pendiente de validación |
| Juego completo | No desarrollado |

*Fin del documento maestro v1.0.*
