# FileSystem3 — ACP, alias DOS 8.3, junctions y aceptación Fedora

Estado: implementación incremental en la rama `feature/filesystem3-acp-dos83-reparse-safety-20261116`. Esta nota distingue invariantes probadas de equivalencia NTFS, que **no está demostrada**.

## P1 — Entrada ANSI y rutas internas UTF-8

- Las API Win32 `A` de archivos y `nt_fopen` del CRT tratan sus bytes como el ACP del proceso, hoy CP1252, y convierten una sola vez a UTF-8. Incluso una secuencia que parezca UTF-8 válido sigue siendo ACP en una API `A`.
- Los argumentos de las API `W` se interpretan como UTF-16. El normalizador interno recibe UTF-8 válido; no admite bytes malformados ni consulta automáticamente un segundo nombre reinterpretado como CP1252.
- Se mantienen pruebas con `café`, rutas mixtas `A/W`, rutas distintas cuyos bytes pueden interpretarse de dos maneras y plegado de mayúsculas/minúsculas en Unicode.
- Este perfil **no** equivale a todas las páginas de códigos ANSI/DBCS ni reproduce todas las reglas NTFS de normalización.

## P2 — DOS 8.3

- Las identidades cortas pertenecen a cada entrada de directorio: clave formada con el directorio físico y el nombre de la entrada, persistida en el xattr del inodo sin crear archivos Windows auxiliares.
- Un hardlink en otro directorio recibe su alias independiente. Las asignaciones entre procesos de este runtime utilizan un bloqueo cooperativo por directorio.
- Antes de devolver un alias, se compara de nuevo `dev/ino` con la entrada física. Se rechazan cambios detectados durante la operación.
- **Mediación P2 añadida:** los renombrados Win32 dentro de un mismo grant preparan el alias del destino antes de `renameat2`, bloquean ambos directorios en orden canónico y confirman/revierten el xattr tras la operación. El alias se conserva si no colisiona; las rutas anteriores no deben resolver un archivo distinto.
- **Reconciliación Linux:** `nt-dos83-mediator --watch` sigue eventos inotify (parejas con cookie) y migra alias de forma eventual; `--scan` regenera alias ausentes. `--move` proporciona un camino mediado explícito para herramientas del anfitrión. Véase [protocolo y límites P2](dos83-mediation.md).
- **Límite:** los procesos Linux que omiten la mediación pueden renombrar antes del evento y desbordar la cola de inotify. No hay atomicidad universal ni journal durable frente a fallos de energía, ni paridad demostrada con reglas de NTFS.

## P3 — Junctions y formato persistente

- Formato de escritura actual: `user.ntnative.junction.v1`, cabecera `JUNC` + versión 1 + identidad de raíz y destino UTF-8 de hasta 1023 bytes, terminación NUL incluida.
- El lector admite el tamaño compacto y el registro histórico `PATH_MAX + 16` de esa misma clave/version; valida tamaño exacto, cabecera, terminación y raíz. No modifica el registro antiguo al leerlo.
- `FSCTL_SET/GET/DELETE_REPARSE_POINT` admite únicamente junction de directorio como mount-point reparse y destino físico bajo la **misma** raíz concedida. No ejecuta `mount`, no sigue symlinks host ni permite cadenas de junctions.
- Un registro corrupto no se sigue; la operación DELETE puede retirar ese marcador para recuperar la entrada.
- No se añaden los formatos incompatibles `JNC1`, `D832` o `D833`. Una futura modificación del esquema necesitará versión nueva y pruebas de migración antes de escribirla.
- Los tests con xattrs históricos >4 KiB emiten SKIP en sistemas que no puedan almacenarlos; no constituyen una validación positiva allí.
- Sigue pendiente paridad completa de NTFS (p. ej. otros tags, capacidades y rutas extendidas).

## P4 — Modelo de seguridad y regresiones

- `openat2` limita resolución bajo FDs de raíz concedidos con `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV`; el directorio final usado para metadatos queda fijado mediante FD.
- Pruebas de regresión: symlinks hostiles, denegación de raíz alternativa, renombrado externo de entradas que invalida alias, asignaciones 8.3 concurrentes en dos procesos, sustitución de directorio por symlink antes de SET, GET sobre xattr corrupto y limpieza por DELETE.
- La política de flocks sólo coordina componentes cooperantes. Las carreras contra escritores Linux externos posteriores a la última comprobación, SELinux real y los fallos de energía siguen requiriendo prueba específica; no hay una sandbox NTFS.

## P5 — Aceptación física: **no realizada**

Ejecutar en la estación física Fedora x86-64, no en CI:

```bash
git checkout main
bash scripts/fedora-filesystem-acceptance.sh --physical-confirmed
```

El script registra commit, kernel, distribución, tipo de FS del HOME, estado SELinux, pruebas Linux/PE y cualquier SKIP. Guarda un informe en `~/.local/state/nt-native-linux/fedora-acceptance/`. Termina con código 3 si falta una dependencia crítica o hay pruebas omitidas; código 0 sólo afirma que pasó **la parte automatizada**, nunca la aceptación manual.

Para dar por aceptada la prueba física, adjuntar al informe:
1. Modelo de sistema de archivos y soporte real de xattrs (especialmente lectura de registros históricos grandes en un FS que los admita).
2. Creación de nombres CP1252 mediante `A` y Unicode mediante `W`, y búsqueda con cambios de mayúsculas/minúsculas después de cerrar y reabrir el programa.
3. Aliases 8.3 después de reiniciar el equipo, hardlinks en directorios distintos, renombrados externos y dos procesos simultáneos; verificar que las rutas antiguas no apuntan a otro archivo.
4. Junction SET/GET/DELETE con destino autorizado y denegación de escape `D:\\` o UNC no concedido; comprobar qué ocurre si otro proceso Linux sustituye/renombra la entrada.
5. PE AMD64 reales de confianza, con puertos/red y carpetas concedidos expresamente; ejecutar pruebas del aislamiento Bubblewrap sin SKIP. Si el sistema deniega namespaces, no desactivar el aislamiento para obtener una supuesta victoria.
6. SELinux en modo Enforcing con caso denegado y permiso explícito, fallos de permisos, interrupción del proceso y recuperación tras cierre abrupto.
7. Registro de diferencias concretas frente a las mismas operaciones en Windows real (nombres, errores, contenido, atributos, handles y concurrencia).

Las pruebas verdes en Ubuntu o Fedora RPM **no** demuestran paridad NTFS ni autorizan a ejecutar PE de terceros no confiables sin aislamiento evaluado.
