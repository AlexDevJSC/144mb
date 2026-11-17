# FileSystem4/P5 — Aceptación física Fedora y referencia Windows

**Estado: pruebas preparadas, NO EJECUTADAS en el equipo del propietario.**
Los resultados verdes de GitHub Actions NO son evidencia física de SELinux
Enforcing con usuarios, dispositivos ni permisos reales de la máquina.

## Paso 1. Referencia Windows NTFS real

En PowerShell de una máquina Windows con volumen temporal NTFS, desde
el checkout de la misma revisión:

    powershell -ExecutionPolicy Bypass -File scripts/windows-filesystem4-reference.ps1 -Output "$HOME\\fs4-windows-reference.json" -PhysicalWindowsConfirmed

El script crea un directorio temporal descartable y documenta:
insensibilidad a mayúsculas, preservación de bytes tras rename, hardlink,
atributo READONLY y alias 8.3 cuando NTFS lo proporcione. El script
comprueba realmente que el volumen de TEMP sea NTFS; si el alias 8.3 está
desactivado registra ausencia, no lo inventa.

La referencia no demuestra por sí sola que NT144 emule la semántica de
Windows. El propietario deberá comparar esos resultados con pruebas
equivalentes mediante PE en NT144 y anotar cada diferencia con la versión
de Windows, filesystem y código de error.

## Paso 2. Fedora físico con SELinux Enforcing

En la estación real Fedora x86-64, como usuario normal, con:
make, GCC, python3, xattrs, fuse3/fusermount3, bubblewrap, entorno
gráfico de pruebas si corresponde y los paquetes de compilación del
README de la revisión:

    git checkout feature/filesystem4-integrated-validation-20261117
    bash scripts/fedora-filesystem4-acceptance.sh --physical-confirmed --windows-reference /ruta/fs4-windows-reference.json

La rama integrada suma el modo PE de la PR #153 y la corrección de
supervisor/FUSE de la PR #150. En esta rama el script ejecuta también
una sesión PE aislada sobre un perfil C: **descartable**, sin utilizar
ni sobrescribir los datos personales de C: del usuario.

También se puede ejecutar sin --windows-reference para obtener un
informe parcial, pero el código de salida 3 indica INCOMPLETE.

Se genera evidencia en:

    ~/.local/state/nt-native-linux/fedora-acceptance/filesystem4-*.log

La herramienta:
- Rechaza root y CI, exige confirmación del operador y git limpio.
- Requiere SELinux Enforcing, /dev/fuse, fusermount3 y bubblewrap.
- Reutiliza suites Linux/PE/WAL/FUSE y los scripts físicos existentes.
- Rechaza SKIP de aislamiento o junction histórica: no falsea PASS.
- Valida que el JSON de Windows contiene el esquema NTFS esperado.
- Informa sólo de paso **automatizado**; nunca certifica 100 % NTFS.

## Paso 3. Pruebas manuales obligatorias no automatizadas

1. FileSystem P1: cortar el mediador/PE de forma abrupta en cada fase
   observable, reabrir otro proceso, examinar WAL, short names, inode
   dev/ino, xattrs y datos antes/después de reiniciar el PC.
2. P2: ejecutar PE real en Bubblewrap con supervisor, marcar archivos
   concretos, abrirlos desde clientes Linux vía FUSE y comprobar
   exclusiones NT sobre locks, truncate, rename y release.
3. P3: ejecutar mismo fixture de operaciones en Windows real y en NT144:
   comparar archivos, nombres cortos, atributos, errores, ACL y
   reparse; anotar divergencias sin descartarlas por falta de paridad.
4. P4: demostrar con un **UID distinto** denegación directa del backing
   bajo SELinux Enforcing; el prototipo actual **no tiene servicio UID**
   y esta prueba no puede aprobarse hasta que se implemente.
5. P5: simultaneidad de varios clientes PE/FUSE, caída de FUSE, cierre
   del supervisor, comprobación de limpieza de montajes y descriptores
   heredados; revisar AVC de SELinux y síntomas de corrupción.

**No realizar pruebas de corte eléctrico sobre datos importantes.**
Se requiere un volumen de prueba desechable y restaurable, y un protocolo
controlado de apagado/reinicio para evaluar durabilidad.

## Criterio de aceptación

Una CI verde demuestra regresión automatizada, no semántica NTFS completa.
El script Fedora no puede observar una computadora remota ni modificar
SELinux desde ChatGPT. Hasta ejecutar pruebas físicas/Windows y cerrar
incidencias, las PR #150, #152, #153, #155, #156 y P5 siguen siendo
desarrollos revisables, no un FileSystem 100 % aprobado.
