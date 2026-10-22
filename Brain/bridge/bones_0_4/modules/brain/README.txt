BRAIN v0.3.0 — ADAPTADOR AUTOCONTENIDO PARA BONES 0.4.0 MODKIT ABI 1

INSTALACIÓN (ESTÁTICA / JUEGO NORMAL)
  Copiar estos archivos a BONES/modules/brain/:
    modkit.c
    brain.h
    brain_npc.c
  Mantener BONES/modules/brain/module.c, perteneciente al registro legado.
  Ejecutar desde BONES/: python3 scripts/gen_modkit_registry.py
    Linux: make modkit_test && make modkit_demo
    Windows: scripts\build_windows_msvc.bat (requiere MSVC y Python)
  El script actual de Bones SOLO compila modules/*/modkit.c; por ese motivo
  modkit.c incluye brain_npc.c en la misma unidad de traducción.
  NO compilar brain_npc.c por separado junto con modkit.c: símbolo duplicado.

INTERFAZ
  int32_t bones_mod_brain_query(uint32_t abi, BonesModDescriptor *out)
  ABI=1, role=BRAIN, implementation_version=3, state_schema_version=0.
  Permisos: READ_ACTORS | READ_TIME | SEND_COMMANDS.
  Sin llamadas a BonesEngine, Foundation, render, red, archivos ni UI.

CONDUCTA REAL
  El servicio actor_at identifica NPC owner=0, kind=2. Para estos NPC
  se invoca brain_npc_decide (lógica original de Brain v0.2).
  La profesión/vida/amenaza NO está disponible en BonesModActor: se utiliza
  una ficha demo con rol GUARD y 100% vida, sin percepción externa.
  Se emiten órdenes de patrulla durante 07:00–19:00 (del reloj demo),
  un movimiento lineal con giro periódico; física/colisiones son de Bones.
  El reloj demo inicia 08:00 y avanza un minuto por 60 ticks; esta escala
  NO es canónica y deberá sustituirse por el reloj lógico oficial.
  No se ordena mover jugadores, NPC owner>0 ni objetos del mundo.
  Nunca se envían órdenes con tick anterior ni issuer ajeno.
  Con comandos rechazados el módulo sigue activo; callback void no permite
  propagar error. Se contabiliza en memoria como diagnóstico sin UI.

RESTRICCIONES IMPORTANTES
  No hay pathfinding, objetivos espaciales, combate ni NPC con memoria.
  Economía, facciones, BVR1 y transacciones NO están conectados a ModKit.
  El snapshot BSF1 de Bones NO guarda estado privado de Brain.
  Por ello, state_schema_version=0, sin promesas de persistencia Brain.
  El comportamiento de patrulla es reproducible a partir de tick/ID,
  NO demuestra que clientes con versiones diferentes reproduzcan un mundo.
  Una sola instancia del proveedor Brain por proceso (rol exclusivo de Bones).
  Para DLL de desarrollo, compilar modkit.c con BRAIN_MOD_BUILD_SHARED:
  exporta bones_mod_query_v1, pero Bones Windows final NO auto-carga DLL.
  Soul debe revisar el cambio antes de declararlo integración estable.

v0.3.1 PREPARACIÓN REGRESIÓN PLAYTEST:
  - BrainModDiagnostics permite contar órdenes rechazadas y errores del host.
    Es API auxiliar de Brain, no una alteración de Bones ABI 1.
  - Detección de avance detenido: 8 ticks aceptados sin desplazamiento
    -> 14 ticks de giro in situ; no es navegación ni corrige colliders.
  - NO aplicar como verificación final hasta corrección Bones y revisión Soul.
  - brain_npc.c está INCLUIDO en modkit.c: NO añadirlo a compilación.
  - state_schema_version=0, no guardar memoria antiatasco en BSF1.
