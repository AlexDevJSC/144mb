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
