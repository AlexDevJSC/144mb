# Fedora 45: implementación y cierre de bloqueos observados

Este documento describe cambios de código posteriores al
[informe original del 16-11-2026](fedora45-nt144qa-validation-2026-11-16.md).
No modifica el historial de aquella ejecución (27 PASS, 1 FALLO,
1 INCOMPLETO) ni afirma que se haya repetido en el Fedora físico.

## Secuencia aplicada

| Prioridad | Evidencia de campo | Implementación / comprobación nueva |
| --- | --- | --- |
| P0 | `KERNEL32.dll!lstrcmpW` ausente en PE hijo | `nt_lstrcmpW` y `nt_lstrcmpA` en NLS, exportadas en KERNEL32/KERNELBASE, pruebas de ACP/UTF-16 y fixture PE MinGW real `check-pe-lstrcmp` |
| P1 | Solo se detecta la importación ausente al ejecutar | `pe-import-audit --verify FILE1.exe FILE2.exe...` y `make check-pe-preflight`. Las importaciones conocidas se comprueban antes de Bubblewrap, y un PE negativo demuestra que se rechaza una API inexistente |
| P1/P2 | `libfluidsynth.so.3` ausente en el Fedora probado | RPM sigue requiriendo `fluidsynth-libs`; `make check-audio-required` impide etiquetar como PASS la ausencia de la librería; script Fedora permite instalarla **solo** con opción expresa |
| P1 | `SKIP` de boost positivo y prioridad multihilo | El runtime ya tenía algoritmos de transición y rollback; **no se inventa otra planificación**. Se incorpora ruta de aceptación opcional `--with-priority`, delegando `RLIMIT_NICE` a un proceso desechable y abandonando UID/GID privilegiados mediante `setpriv` **antes** de ejecutar los tests |
| P2 | Cuatro CPU permitidas, sin dos grupos reales | Se ejecuta el simulador de 80 CPU, pero se marca `HOST_LIMITED` la prueba física intergrupo. `--require-multigroup` falla explícitamente si faltan CPU permitidas en ambos grupos |
| P2 | `make check` sale 0 con `SKIP` internos | `scripts/analyze-fedora-qa.py` clasifica resultados y causas: API ausente, dependencia, privilegios, sandbox, topología; con `--strict`, 1 = fallo y 3 = incompleto |

### Limitación consciente del P0

La versión base de `lstrcmpW` utiliza `CompareStringW` con el perfil NLS
implementado hoy y transforma los resultados de comparación a negativo/cero/
positivo. `lstrcmpA` convierte usando el ACP explícito del runtime.
Esto desbloquea las llamadas comprobadas; **no constituye equivalencia de
colación lingüística Windows** en todas las configuraciones regionales.

### Limitación consciente del auditor

La comprobación incluye la tabla **directa** de importaciones PE32+ AMD64,
asociándolas al registro de exports nativos. No demuestra que una exportación
sea funcional ni cubre todos los escenarios de importaciones `delay-load`,
DLL dependientes o `LoadLibrary` en tiempo de ejecución. Se conservan las
validaciones estrictas del loader.

## Reproducción en una copia actualizada de Fedora x86_64

```bash
git rev-parse HEAD
make -j2 check-pe-preflight check-pe-lstrcmp
make -j2 check-audio-required
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  make -j2 check-pe-isolated

# Puerta de aceptación física, sin elevación implícita:
bash scripts/fedora-qa-environment.sh

# Instalar SOLO la dependencia ausente, si el usuario lo autoriza:
bash scripts/fedora-qa-environment.sh --install-audio

# Probar prioridad/boost con delegación temporal RLIMIT_NICE y
# drop de privilegios al usuario real, sin ejecutar PE como root:
bash scripts/fedora-qa-environment.sh --with-priority

# Exigir test físico multicgrupo (en el equipo original dará INCOMPLETO):
bash scripts/fedora-qa-environment.sh --require-multigroup

# Después de otra ejecución de la herramienta externa NT144 QA:
python3 scripts/analyze-fedora-qa.py /ruta/al/log.txt --strict
python3 scripts/analyze-fedora-qa.py /ruta/al/log.txt --json
```

La salida de la herramienta externa `nt144-qa` no está implementada ni
controlada por este repositorio. Su reporte puede seguir mostrando un bloque
incompleto si una prueba opcional tiene un `SKIP` de host. El analizador
no reescribe los resultados, solo muestra por qué están pendientes.

## Criterio de cierre

- Todos los imports directos de los fixtures aislados deben pasar el auditor.
- El fixture Windows `lstrcmp-fixture.exe` debe ejecutarse y devolver 0,
  no únicamente resolver una función desde C nativo.
- El PE padre debe alcanzar **realmente** el éxito del proceso hijo dentro
  de Bubblewrap, además de preservar los argumentos Unicode.
- `make check-audio-required` debe ejecutar FluidSynth sin `SKIP`.
- Las transiciones positivas de prioridad deben tener autorización real
  recuperable; nunca se clasifica el rechazo de privilegios como éxito.
- Una prueba física de múltiples grupos de CPU solo puede aprobarse en
  hardware con CPU permitidas de al menos dos grupos.
- El log nuevo debe incluir SHA, Fedora/kernel/SELinux, salidas y estados
  completos. CI Linux/Fedora RPM verde no sustituye aceptación física.

No se han modificado tests para aceptar respuestas falsas, ocultar skips
ni elevar privilegios de programas Windows desconocidos.


## NT144 QA — segunda ejecución, commit 96199dd5 (Fedora 45)

**Registro aportado por QA:** Fedora 45 x86-64, kernel
`7.3.3-300.fc45.x86_64`, SELinux `Enforcing`, cpuset permitido
`[0,1,2,3]`; 83 comprobaciones: 76 PASS, 6 FALLO y 1 INCOMPLETO.
El commit de origen es
`96199dd54f07140cd64ef27ba35d6c8ef001d0f4`.
Este resultado **no debe sobrescribirse** con la salida de una CI
posterior. El árbol `main` en el inicio de esta reparación coincidía
exactamente con ese SHA.

La PR de reparación NT144 QA aplica contratos distintos y los valida
independientemente, manteniendo SELinux, Bubblewrap y la separación
de derechos:

- `CreateEventW`: valida `SECURITY_ATTRIBUTES` x64 (24 bytes),
  `lpSecurityDescriptor == NULL`, y propaga `bInheritHandle` como
  atributo **por HANDLE** a la creación NT. La semántica y el estado
  del objeto no dependen de este bit. Con `bInheritHandles=TRUE`,
  `CreateProcessW` sigue rechazando eventos NT heredables que no
  pueden serializarse en el manifest de descriptores del hijo: no
  se transmite silenciosamente un HANDLE que el hijo no poseerá.
- `check-pe-preflight`: captura el código shell real con `$$?` y
  `$$status` en la receta GNU Make y exige el resultado negativo
  específico del auditor, tanto en código de salida como en nombre
  `KERNEL32.dll!MissingNtNativeApi`.
- MinGW GCC 16: los dos fixtures afectados se enlazan con un helper
  `memset` **limitado a tests PE freestanding**, sin CRT importada
  ni símbolos Win32 ficticios. Existe prueba de los bytes escritos
  y de los márgenes no modificados.
- DXGI/shaders: el texto del error del host añade un `0` espurio
  incluso a importaciones **nominales**; el formato por nombre se
  separa del formato ordinal. Un test inspecciona las cadenas reales
  `IMAGE_IMPORT_BY_NAME`, y las comprobaciones funcionales de los
  backends siguen siendo obligatorias.
- Menús y Raw Input: las pruebas SDL offscreen deben conservar
  el scroll de popups, el rechazo de ítems ya fuera del viewport
  y el suministro de `WM_INPUT` de acuerdo con `RIDEV_NOLEGACY`.

### Puertas P2 no completables con este cpuset

**Boost positivo de prioridad.** La subida reversible del `nice`
Linux requiere un `RLIMIT_NICE` y privilegios de recursos
autorizados para el proceso de prueba. La prueba ordinaria sin
esa concesión puede indicar `SKIP`; `--with-priority` usa una
delegación temporal con `prlimit` y `setpriv`, descarta UID/GID
privilegiados antes de ejecutar el test y no eleva el runtime ni
el PE general. Una transición de política Windows no probada
no se convierte en PASS por permitir un `nice` Linux.

**Prioridad multihilo positiva.** Hay que admitir efectivamente
las transiciones sobre todos los hilos gestionados, sin
`EPERM`, y demostrar rollback de cada TID y la prioridad
base tras errores. Un grupo de CPU pequeño no elimina la
necesidad de esa delegación reversible de recursos.

**Afinidad y processor groups físicos adicionales.** Un
cpuset con CPUs `[0,1,2,3]` cubre solo el grupo Windows 0.
El test sintético de 80 CPUs valida cálculos/índices, pero
**no** demuestra la transición efectiva entre al menos dos
grupos ni la programación física de CPU `>=64`. Debe
ejecutarse `--require-multigroup` sobre un anfitrión
con CPUs permitidas en ambos grupos, sin inventar topología
ni modificar las restricciones del usuario. Resultado en la
máquina actual: `INCOMPLETO / HOST_LIMITED`, no PASS.

Para la aceptación definitiva, ejecutar los objetivos de QA
en Fedora 45 físico con `SELinux=Enforcing`, guardar
logs completos y exigir explícitamente el mensaje
`PASS PE child launched inside the same Linux sandbox and returned its full exit code`.
Un `SKIP` por imposibilidad de crear namespaces en CI permanece
`INCOMPLETO`; no se desactiva Bubblewrap ni se altera el backend
FileSystem4. La fase 4 permanece abierta.
