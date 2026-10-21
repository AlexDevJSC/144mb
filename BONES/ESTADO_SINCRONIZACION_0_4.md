# BONES 0.4.0 — estado de sincronización

**NO SINCRONIZADO COMPLETAMENTE.**

El repositorio contiene archivos parciales de v0.3.0 y aún carece de la mayoría de archivos del ZIP de v0.4.0. El paquete original verificado localmente contiene **80 entradas**, incluyendo documentación, fuentes, scripts, pruebas y una captura.

En la rama `bones/sync-v0.4.0` se ha añadido `BONES/scripts/sync_from_zip.py`. Desde un clon local y con el ZIP `PROYECTO_144_Bones_ModKit_v0_4_0.zip`:

```sh
python BONES/scripts/sync_from_zip.py /ruta/PROYECTO_144_Bones_ModKit_v0_4_0.zip
python BONES/scripts/sync_from_zip.py /ruta/PROYECTO_144_Bones_ModKit_v0_4_0.zip --apply
git status
git add BONES/
git commit -m "feat(BONES): sync modular engine v0.4.0"
git push origin bones/sync-v0.4.0
```

Revisar diferencias antes de fusionar; no se deben reemplazar archivos ajenos a BONES. El script **no borra archivos anteriores automáticamente**.

El ModKit ABI 1 es experimental y requiere revisión de Soul. La validación Windows y la cota final de 1.474.560 bytes siguen pendientes.

Solo publicar la sincronización como finalizada tras verificar presencia de todos los archivos y las pruebas del proyecto.
