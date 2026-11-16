# P2.3 — Mediación previa obligatoria para alias DOS 8.3

## Qué problema resuelve

El mecanismo P2.1/P2.2 ya coordina a los procesos del runtime y puede reconciliar renombrados Linux **después** de un evento `inotify`. Eso no asegura que una aplicación Linux arbitraria espere al mediador antes de modificar la carpeta.

En este perfil P2.3 se ofrece una **vista FUSE3** de la carpeta concedida: las operaciones de nombres de los clientes de la vista pasan por callbacks del kernel hacia `nt-dos83-fuse`, que delega las renombradas/borrados a las API Win32 del runtime y su protocolo de alias. El proceso del runtime mantiene como C: la carpeta física **inferior**, y los programas Linux acceden sólo a la vista **superior**.

Esto no necesita un volumen NTFS ni sustituye Btrfs/ext4: Linux puede montar NTFS con `ntfs3`, pero poder leer NTFS no proporciona mediación de rutas Win32 sobre un árbol Linux cualquiera.

## Perímetro de seguridad: condición necesaria

Para afirmar que **todas** las escrituras están mediadas, todo cliente capaz de modificar las entradas debe tener acceso de escritura **exclusivamente** a la vista FUSE. No puede conservar FDs antiguos ni otros aliases o montajes escribibles que den acceso al directorio inferior. Opciones de aislamiento:

- **Sesión confinada preferida (probada en CI):** `nt-dos83-landlock` exige Landlock ABI >=3 (REFER y TRUNCATE), verifica el tipo/subtipo y el origen del montaje FUSE, y permite acciones de escritura o cambio de nombres sólo bajo la vista. Cierra los descriptores heredados >2 y rechaza stdin/stdout/stderr respaldados por archivos regulares o directorios, ya que Landlock no revoca FDs abiertos antes de imponer la política. No necesita privilegios ni Bubblewrap; las restricciones se heredan por los descendientes del proceso.
- **Alternativa adicional:** `scripts/dos83-fuse-confined.sh` usa Bubblewrap para montar el árbol anfitrión en modo sólo lectura y volver a montar la vista FUSE como único acceso escribible. Su verificación **falla cerrada** si no hay una vista FUSE válida, Bubblewrap o una separación de carpetas. En runners que prohíben namespaces de usuario se marca SKIP, no PASS.
- **Servicio con identidad separada:** administrar el árbol inferior mediante un UID dedicado al mediador y al runtime autorizado; los clientes de otros UIDs acceden sólo a FUSE con política `allow_other`, `default_permissions` y reglas explícitas de SELinux. No está automatizado y **no debe** asumirse seguro por el mero hecho de crear la vista.

Si un programa Linux continúa pudiendo hacer `renameat2`, `unlinkat` o crear entradas directamente en la carpeta inferior, **P2.3 NO está garantizado**, aunque `inotify` esté activado.

## Uso experimental en Fedora

Requiere `fuse3`, `fuse3-devel` durante compilación, `/dev/fuse`, `fusermount3` y permisos para montar FUSE. La compilación del binario es opcional si `pkg-config fuse3` no está presente.

```bash
make
mkdir -p "$HOME/p2-view"
build/nt-dos83-fuse "$HOME/p2-backing" "$HOME/p2-view" -f
```

En otra terminal, tras confirmar que el montaje aparece como `fuse.nt-dos83`:

```bash
build/nt-dos83-landlock --view "$HOME/p2-view" -- /bin/sh

# Alternativa, sólo si Bubblewrap puede crear namespaces:
scripts/dos83-fuse-confined.sh --lower "$HOME/p2-backing" \
    --view "$HOME/p2-view" -- /bin/sh
```

Las llamadas a `mv`, `touch`, `rm`, `mkdir`, `ln` y las escrituras de archivos ordinarios en `p2-view` se dirigen a FUSE. El mediador no permite escrituras de clientes a `user.ntnative.*`; bloquea symlinks como intermediarios y cruces de mounts inferiores mediante `openat2`.

**No usar sobre datos importantes.** El prototipo no es una implementación POSIX completa: los symlinks host se rechazan, el modo de reemplazar directorios y los locks pueden diferir de Linux nativo, y no se han probado todos los metadatos extendidos, ACL, `renameat2` flags y primitivas de I/O. No se monta automáticamente al arrancar ni se ejecuta con privilegios elevados por defecto.

## Pruebas y condiciones de aceptación

```bash
make check-dos83-fuse
make check

# En un Fedora físico, con SELinux Enforcing y /dev/fuse:
bash scripts/fedora-p2-3-acceptance.sh
```

La prueba ejecuta siempre una autocomprobación del backend, de prohibición de escape y de rechazo de una carpeta ordinaria presentada como si fuera FUSE. La variante física `NT_DOS83_REQUIRE_PHYSICAL=1` falla con código 3 ante cualquier SKIP de FUSE o Landlock, en lugar de informar PASS ficticio. Si FUSE está disponible, monta un directorio temporal y prueba renombrado 8.3, creación, symlinks hostiles y denegación de modificación de xattrs reservados. Con Landlock ABI >=3 se prueba en un proceso Linux real que el acceso directo al backing no puede **crear, truncar, renombrar, eliminar ni crear hardlinks cruzados**, que el acceso a la vista sí puede escribir y que un descriptor heredado no elude la política. Bubblewrap queda como alternativa de aislamiento de namespaces, no requisito para probar Landlock.

**La CI verde con un SKIP de FUSE montado no certifica P2.3 físico.** Para cerrar P2.3 en Fedora físico todavía es necesario ejecutar las pruebas montadas sin SKIP, evaluar SELinux Enforcing, probar múltiples clientes concurrentes, reinicios, sustitución de backing, expiración de handles, accesos desde otro UID, hardlinks, fallos de energía y una matriz diferencial contra NTFS real.

## Garantías y límites

- **Operaciones en vista FUSE, mientras el servicio está vivo:** el kernel envía las operaciones de nombres antes de llevarlas al backing. Los renombrados pasan por el broker Win32, con persistencia 8.3 según la política del runtime.
- **Operaciones directas en backing:** el wrapper Landlock restringe las llamadas de ese proceso y sus descendientes sobre la jerarquía inferior; un programa del anfitrión iniciado **fuera** de esa política todavía puede modificarla. Un UID de servicio independiente con permisos inferiores cerrados sería necesario para una prohibición sistémica frente a todos los procesos no privilegiados.
- **Landlock limita escritura/nombres, no todas las mutaciones:** algunas operaciones de metadatos (como chmod/chown) no están gobernadas por los derechos de FS incluidos. Los programas capaces de alterar metadatos de backing requieren además control DAC/SELinux o la alternativa de aislamiento completo con mount namespace.
- **Cortes del servicio y crash-consistency:** las solicitudes de vista fallan al caer el servidor FUSE, pero preparación de xattr + renombrado no es una transacción atómica de disco. No está probada la recuperación exacta de NTFS tras cortes de energía.
- **inotify:** sigue disponible para los procesos no confinados, pero no se considera una garantía fuerte de P2.3.

La solución propuesta es un **perímetro FUSE obligatorio y fail-closed**, no convertir el controlador `ntfs3` en un nuevo runtime Win32. La validez del nivel 3 sólo se atribuye al perímetro efectivamente aislado y a las operaciones soportadas y probadas.
