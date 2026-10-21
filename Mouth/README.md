# Mouth — módulo P2P para Bones

Mouth es responsable de red y multijugador. Este directorio está reservado para el código fuente y las pruebas de Mouth.

## Estado de publicación
- Carpeta y guía creadas en GitHub.
- **Código fuente de Mouth v0.1 y v0.2: aún no transferido al repositorio.**
- Los paquetes originales están disponibles en la conversación de ChatGPT.
- No confundir presencia de esta carpeta con integración en Bones ni ejecución probada en Windows.

## Publicar los paquetes conservando las fuentes
Descargar a un mismo directorio `MOUTH_P2P_para_Bones_v0_1.zip` y `MOUTH_P2P_Bones_Visual_v0_2.zip`, clonar este repositorio y ejecutar `Mouth/importar_fuentes.ps1` desde la raíz de la copia local. Revisar `git status` y confirmar el push en GitHub. El script *no* hace commit ni push automáticamente.

Estructura prevista:
- `Mouth/history/v0_1/`: fuentes originales de la primera entrega.
- `Mouth/current/`: fuentes completas v0.2, manteniendo contratos, tests, scripts y documentación.
- `Mouth/README.md`: estado y guía.

La variante actual v0.2 añade la sincronización de referencias visuales, y sus archivos contienen la implementación previa, aunque se conserva v0.1 para trazabilidad.
