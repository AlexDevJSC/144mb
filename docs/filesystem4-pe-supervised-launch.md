# FileSystem4 / P2 — integración explícita del lanzador PE

Estado: **opt-in**, revisión pendiente. No intercepta automáticamente
toda ejecución de pe-isolated-run ni convierte Landlock en un sandbox
general de Windows. No cambia Núcleo3, ptrace ni los contratos de hilos.

## Uso

Primero debe existir el perfil real C: del lanzador:
$XDG_STATE_HOME/nt-native-linux/C: (o $HOME/.local/state/nt-native-linux/C:).
Ejecutar como usuario sin privilegios, desde una instalación con
fuse3, Landlock ABI >= 3 y Bubblewrap:

    mkdir -p "$HOME/nt-view"
    nt-fs-supervisor --lower "$HOME/.local/state/nt-native-linux/C:" \
      --view "$HOME/nt-view" --pe -- /ruta/absoluta/juego.exe

Cuando XDG_STATE_HOME está definido y es absoluto, usar esa raíz en
lugar del valor de ejemplo. La vista debe estar fuera del backing.

## Contrato y límites

- El supervisor exige que lower sea exactamente el mismo directorio
  físico (dev/ino) que el perfil C: del lanzador PE, propiedad del UID
  actual y con permisos privados. Rechaza profile C: final symlink,
  coincidencias falsas y ausencia del perfil.
- El binario host pe-isolated-run se resuelve sólo junto al supervisor
  instalado o junto a la build local; no ejecuta una ruta de launcher
  arbitraria aportada por el programa invitado. Requiere /usr/bin/bwrap.
- Preflight FUSE, revisión de montaje y probe Landlock siguen siendo
  obligatorios. El supervisor conserva el control del daemon FUSE,
  del lock de sesión y de los procesos que creó.
- Los clientes POSIX/Linux lanzados en modo ordinario siguen utilizando
  nt-dos83-landlock sobre la vista FUSE. El propio lanzador PE de confianza
  NO se coloca bajo ese Landlock: su runtime Win32 debe poder abrir
  el backing original y el registro OFD, y un Landlock que sólo permita
  escritura en la vista rompería dichas operaciones.
- El PE no se ejecuta directamente desde el supervisor: pe-isolated-run
  crea su contenedor Bubblewrap habitual, con sus permisos explícitos
  de /game y descriptores. Verifica una segunda vez que su C: real
  coincide con NT_FS_LOWER y falla antes del bind cuando no coincide.
  El indicador de entorno NT_FS_SUPERVISED no es autenticación por sí
  solo ni concede acceso adicional a ningún archivo.
- Un proceso Linux ajeno al modo confinado todavía puede abrir el
  backing si su UID/DAC/SELinux lo permite. La vista montada NO impone
  una política global ni hace de todas las operaciones POSIX transacciones
  NTFS. La asociación de un proceso PE externo con una sesión ya
  existente sigue siendo una operación de usuario explícita.

## Validación

Los tests sintéticos prueban el dispatch opt-in, permisos del perfil,
inodo exacto, negativa ante symlink y cierre supervisor ya existente.
La CI compila el lanzador modificado y ejecuta la regresión previa.

Pendiente en Fedora físico: montar FUSE real y ejecutar un PE AMD64
con Bubblewrap funcionando, al tiempo que un cliente Linux confinado
accede a la vista FUSE y respeta los mismos leases de archivo; probar
SELinux Enforcing, interrupciones, UID separado y paths sustituibles.
Ningún SKIP del aislamiento cuenta como aceptación final.
