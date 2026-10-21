# Anexo Brain al Plan Maestro v1.0

**Fecha:** 20 de octubre de 2026. **Responsable de subsistema:** Brain.

Este anexo documenta avances del subsistema de IA y simulación del PROYECTO
1.44. **No sustituye ni modifica el documento maestro original**: las
cuestiones abiertas de su §2.2 y §22 siguen pendientes de decisión.

## Correspondencia con el plan maestro

- §4: narrativa emergente como objetivo. En v0.1 hay estados y eventos
  causales mínimos; no se ha construido un sistema de historias.
- §5: coordenadas de sector de 64 bits y semillas estables presentes en el
  modelo, pero Brain no genera terreno ni asentamientos: corresponde al
  motor de mundo/Bones.
- §9.1: decisión de tres niveles compatible: NPC cercano evaluado por
  intención, asentamiento con stocks agregados, región latente preparada por
  avance agregado. No hay aún modelado dinámico de interacción regional.
- §9.2: reloj lógico configurable y rutinas de comerciantes/guardias/aldeanos;
  el calendario exacto no está decidido.
- §9.3: producción, consumo, reservas, precio, compra idempotente en memoria;
  integración transaccional con SAVE todavía pendiente.
- §9.4: primeras relaciones entre facciones y causa mediante evento semántico.
- §9.5: misiones procedurales NO implementadas.
- §10: `is_host` en operaciones mutables como defensa local de interfaz, sin
  networking ni verificación de que el proceso sea realmente el anfitrión.
- §11–12: WorldId y EventId branch+serial implementados en datos; NO hay
  serializer, ancestro común, persistencia durable ni merge PBW.
- §13: módulos `sim/` lógicamente separados, C11 portable; integración con
  otras partes mediante API pública, sin código real de Bones disponible.
- §14: se mantiene objetivo de 1.474.560 bytes de **todo** el .exe. No hay
  medida de integración y no se declara conformidad.
- §15 fase 3 / §19 SIM-001: el reloj lógico y comerciante básico tienen
  pruebas locales. SAVE-001/002, NET-*, MERGE-* permanecen sin validar.

## Resultados reproducidos en esta entrega

`make test`, `make demo`, `make bones-demo` pasan en Linux GCC C11. El núcleo
pasó ASan/UBSan. Con la secuencia de pruebas actual, `sizeof(BrainWorld)` es
23.488 bytes y el hash de ejemplo del test es `62cd298edfd0d634`.
Son resultados **del entorno de pruebas**, no garantías entre compiladores.

## Restricciones abiertas para integración

Bones todavía no ha aportado un contrato de navegación, anclas, físicas ni
percepción; el consumidor entregado es un stub. El ledger de RAM (256 eventos)
y `BrainWallet` no equivalen a un sistema de persistencia transaccional: no
usarlos en guardados reales ni promocionar la fusión PBW como implementada.
