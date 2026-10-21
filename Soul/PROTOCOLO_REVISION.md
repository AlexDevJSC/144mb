# Protocolo de comprobación de código — Soul v1

1. **Descubrir:** revisar las últimas entregas localizables de BONES, EYE, BRAIN, EAR, SKIN y MOUTH en los archivos/chats del proyecto a los que haya acceso, ordenando por fecha y leyendo también el changelog/manifest si es accesible. No asumir que se han consultado todos los mensajes de todos los chats.
2. **Auditar GitHub:** leer `AlexDevJSC/144mb` rama `main`, árbol de cada desarrollador y manifiestos de versión; distinguir conectores alojados por Bones de módulos especializados completos.
3. **Comparar:** cotejar nombre, versión interna cuando se pueda leer, ruta, archivos esperados, hashes de commit y pruebas. Si solo se conoce el nombre del ZIP, escribir `versión nominal pendiente de verificación interna`.
4. **Clasificar:** versión coincidente; desactualizada; versión adelantada; no publicada; incompleta; formato/contrato incompatible; información insuficiente. Reportar cambios de dependencias y versiones de API cuando aparezcan.
5. **Registrar:** actualizar `Soul/ESTADO_VERSIONES.md` y `Soul/DISCREPANCIAS.md`. Reutilizar identificadores S-### para problemas persistentes, no generar ruido ni reescribir fechas si nada cambió.
6. **No intervenir:** no modificar código de BONES/EYE/BRAIN/EAR/SKIN/MOUTH automáticamente, no fusionar ramas y no declarar pruebas superadas sin ejecución reproducible.
7. **Ejecución recurrente:** si una tarea programada dispone de acceso para leer archivos del proyecto y GitHub, comprobar diferencias nuevas y actualizar la carpeta Soul cuando esté autorizado. Si no tiene acceso a chats/archivos o permisos de escritura, indicar claramente qué comparación no fue posible y **no inventar resultados**.
8. **Notificación:** avisar al director solo por discrepancias nuevas, cambios materiales de estado o bloqueos nuevos. No reiterar las mismas alertas cada día.

El criterio de autoridad para asignación de responsabilidades está en `Usos de los chats.txt`; se aplica con las decisiones posteriores explícitas del director.
