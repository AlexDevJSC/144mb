# Registro de discrepancias — Soul

Revisión inicial: **21/10/2026**. Cada entrada distingue hecho comprobado, riesgo y trabajo pendiente. El objetivo no es atribuir errores de código que todavía no se han demostrado.

## S-001 — BONES 0.3.0: publicación parcial
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** `BONES/README.md` indica 0.3.0, coincidente con `PROYECTO_144_Bones_Foundation_v0_3_0.zip`; `BONES/ESTADO_SUBIDA.md` declara explícitamente que faltan headers, foundation.c, más fuentes, adaptador Eye, scripts, tests y otros archivos.
- **Conclusión:** no existe discrepancia nominal de versión, **sí de completitud**. No declarar el árbol compilable ni equivalente al ZIP.
- **Acción:** completar subida y ejecutar compilación/tests; conservar evidencia de hash de commit y resultado.

## S-002 — EYE 0.5.1 no publicada en su propia carpeta
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** entrega localizable `Eye_0_5_1_Tejados_Corregidos.zip`, documentada como EYE 0.5.1; la rama principal no contiene un árbol autónomo de código EYE. El conector `BONES/modules/eye/module.c` no sustituye las fuentes de Eye.
- **Conclusión:** versión contra código no verificable; entrega aparentemente aún no publicada.
- **Acción:** publicar fuentes de EYE 0.5.1 sin alterar versiones antiguas y comprobar contratos con BONES.

## S-003 — BRAIN, EAR, SKIN y MOUTH: entregas localizadas, código especializado ausente de GitHub
- **Gravedad:** ALTA · **Estado:** ABIERTA.
- **Evidencia:** ZIP identificados por metadatos como BRAIN 0.2, EAR 0.2, SKIN 0.2 y MOUTH 0.2; solo aparecen conectores mínimos dentro de `BONES/modules/`.
- **Conclusión:** no se puede corroborar igualdad de versiones, fuentes ni pruebas.
- **Acción:** subir cada módulo especializado en ruta propia, con versión y manifest de entrega.

## S-004 — Responsabilidad de persistencia potencialmente contradictoria
- **Gravedad:** MEDIA · **Estado:** PENDIENTE DE ACLARACIÓN.
- **Evidencia:** `BONES/README.md` dice que SKIN implementa «el sistema de guardados asignado por dirección»; el documento `Usos de los chats.txt`, protocolo v1.0, declara que persistencia y guardados no tienen responsabilidad definitiva asignada.
- **Conclusión:** la asignación debe contrastarse con cualquier decisión posterior del director. No atribuir el subsistema a SKIN ni revertir código sin confirmación.

## Convenciones
- **DESFASE:** la versión declarada difiere y ambas son comparables.
- **NO PUBLICADO:** existe una entrega identificada, pero no su código correspondiente en GitHub.
- **INCOMPLETO:** la versión nominal coincide, pero faltan archivos de esa entrega.
- **NO VERIFICABLE:** faltan fuentes, metadatos confiables o acceso para contrastar contenido.
- **CERRADO:** existe prueba reproducible y se registra commit/versión correctora.

En revisiones futuras, actualizar entradas existentes en lugar de duplicarlas y registrar nuevas solo cuando haya evidencia nueva.
