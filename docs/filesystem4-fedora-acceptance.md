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

    git checkout <commit-a-auditar>
    bash scripts/fedora-filesystem4-acceptance.sh --physical-confirmed --windows-reference /ruta/fs4-windows-reference.json

La revisión auditada incluye la ruta PE supervisada sobre un perfil C:
**descartable**; el modo histórico de PE supervisado y el nuevo servicio
UID no son equivalentes. El script ejecuta ambas comprobaciones por
separado y no utiliza ni sobrescribe el C: personal del operador.

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
4. P4: demostrar con un **segundo UID** que el backing físico no se puede
   abrir y que la vista FUSE rechaza al cliente no autorizado. Existe un
   servicio de UID dedicado, pero aún falta verificarlo en Fedora físico y
   distinguir una denegación DAC de una debida a la política SELinux.
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


## Actualización de aceptación: servicio UID y SELinux (18-11-2026)

Además de la suite legacy, fedora-filesystem4-acceptance.sh ahora ejecuta
scripts/fedora-fs4-service-acceptance.py contra el servicio UID REAL
preinstalado en Fedora y el montaje FUSE /run/ntnative-storage/view.
Se exige getenforce=Enforcing, cliente no-root, propietario de backing
ntnative-storage distinto, denegación de lectura directa, respuestas
autenticadas SCM_CREDENTIALS tanto en v1 como HELLO binario v3 y rechazo
de SCM_RIGHTS (transferencias de FDs).

En un directorio temporal de la vista comprueba lectura/escritura y rename,
más rechazos de chmod, utimens, setxattr, removexattr, hardlink, symlink y
lectura del marcador interno NT. Imprime etiquetas SELinux (ls -Zd) para
inspección manual, sin considerarlas demostración de una política confinada.

Requisitos previos: servicio UID instalado con FUSE montado, broker v3
habilitado por administrador mediante NT_STORAGE_EXPERIMENTAL_HANDLES=1,
usuario cliente no root. El script nunca configura DAC, mount ni SELinux.

Inspección humana obligatoria: contexto SELinux del servicio, AVC logs
(ausearch/journalctl), denegación por un segundo UID, PE reales concurrentes
y reinicio. Una denegación provocada por DAC no certifica SELinux.

Evidencia: esta sesión no dispone de Fedora físico. El entorno del asistente
es Debian 13 sin /dev/fuse ni /sys/fs/selinux/enforce. Validación física:
NO EJECUTADA. No desactivar SELinux para pasar el gate.
El entrypoint de aceptación de servicio requiere explícitamente el parámetro --physical-confirmed, rechaza CI y no retorna PASS al faltar SELinux, FUSE o el broker v3.


## Paso 4. Evidencia SELinux de administrador (18-11-2026)

Después de ejecutar la batería física bajo **un usuario normal**, el operador
recopila contextos y AVC como administrador, **sin desactivar SELinux ni alterar
DAC, policy, etiquetas, montajes o registros de auditoría**:

    sudo bash scripts/fedora-fs4-selinux-evidence.sh --physical-confirmed

El script exige Fedora x86-64, `getenforce=Enforcing` y
`/sys/fs/selinux/enforce=1`; rechaza GitHub Actions, CI y ejecución sin
confirmación expresa. Es de **solo lectura** sobre el sistema probado.

La evidencia se guarda con permisos 0600 bajo
`/var/log/ntnative-storage/fs4-selinux-XXXXXXXX.log`, accesible inicialmente
solo por root. Incluye contexto de procesos (scontext), etiquetas físicas
(tcontext), política esperada según matchpathcon, UID/modo DAC,
estado systemd, origen de la vista FUSE, estado de auditd y las entradas
AVC/USER_AVC del día. El volcado AVC puede contener **eventos de otros
programas**: revisar, limitar y anonimizar antes de compartirlo.

El responsable debe correlacionar hora, `pid`, `comm`, `scontext`,
`tcontext`, `tclass`, permisos solicitados y `permissive` con cada
prueba negativa. Una denegación por DAC no demuestra una política SELinux,
y **la ausencia de AVC tampoco demuestra confinamiento**. Si no se dispone
de `ausearch`, `auditctl`, `matchpathcon` o permisos de auditoría,
el informe marca la captura incompleta; no afirma aceptación.

La validación humana, segunda identidad UID, reinicio/corte eléctrico en
volumen descartable y comparación real Windows NTFS siguen pendientes hasta
disponer de sus registros. No sustituir esta evidencia por la CI de Fedora RPM,
que corre en contenedor y no certifica SELinux Enforcing físico.
