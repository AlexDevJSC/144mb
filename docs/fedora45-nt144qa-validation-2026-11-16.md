# Informe de campo NT144 QA — Fedora 45 — 16-11-2026

> **Estado:** evidencia de pruebas aportada por un usuario de Fedora 45 y análisis técnico del repositorio; **no** es certificación de compatibilidad universal con Windows, ni cierre de ningún componente. **Pendiente de revisión e integración**. Esta documentación no implementa ni repara API.
>
> **Para otros proyectos y agentes:** leer también [criterios del núcleo](nt-core-readiness.md), [orden vinculante](nt-core-development-order.md), [fase 4](nt-stage4-process-threads.md), [arquitectura](architecture.md) y [AGENTS.md](../AGENTS.md). No extraer porcentajes de madurez ni declarar paridad a partir de este informe.

## 1. Procedencia y límites de la evidencia

- **Fecha de ejecución reportada:** 2026-11-16; **sistema:** Fedora 45 x86-64, según la terminal del usuario.
- **Herramienta externa:** NT144 QA, perfil `completo`, 29 comprobaciones (host real, runtime real y fixtures sintéticos). El código de esta herramienta y sus fuentes completas **no están adjuntos a este informe**; no confundir sus categorías con una certificación independiente.
- **Repositorio probado:** copia de `AlexDevJSC/144mb` clonada con `nt144-qa --repo "$HOME/.local/share/nt144-qa/repo" --clone --profile completo`. El log confirma el clon, **pero no registra el SHA HEAD de aquel checkout**; no atribuir el resultado a un commit exacto.
- **Referencia de análisis del código:** rama `main` inspeccionada el 2026-11-16, HEAD `04d9a0c56fc238272a2ee21aba8be5d9a98e9ee6`. Este SHA documenta la **inspección**, no identifica necesariamente el clon probado en la máquina del usuario.
- **Entorno observable:** cuatro CPU lógicas permitidas (índices 0–3), un grupo CPU en el test; no se verificó hardware de >64 CPU. No consta la versión exacta del kernel, `uname -a`, tipo de instalación física/VM, estado SELinux, ni una prueba diferencial simultánea contra Windows real.
- **Logs de origen:** tres salidas de terminal proporcionadas por el usuario en la conversación, no publicadas como archivos brutos en GitHub. Informes JSON/Markdown generados localmente en `~/.local/state/nt144-qa/runs/`, identificadores de ejecución `20261116-213251-3854`, `20261116-213724-4408` y `20261116-214353-5277`. **Esos informes no están almacenados en el repositorio**.

### Evolución observada

| Ejecución | Fuente del checkout | PASS | FALLO | INCOMPLETO | Interpretación |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | Runtime instalado, dependencias parciales | 5 | 0 | 24 | Pruebas no lanzadas por falta de `libcrypto`, `fuse3`, compilador MinGW-w64 |
| 2 | Runtime instalado, dependencias ya presentes | 8 | 1 | 20 | Objetivos Makefile ausentes en ese checkout; el fallo sintético de 80 CPU era `No hay ninguna regla...` |
| 3 | Clon nuevo del repositorio | **27** | **1** | **1** | Mayor cobertura; fallo de importación PE reproducido en `check-pe-isolated`; `make check` sale 0 pero el QA clasifica `INCOMPLETO` por `SKIP` |

Los tres números son **casos de NT144 QA**, no una cuenta de pruebas unitarias ni porcentajes de madurez. La ausencia de objetivo en la ejecución 2 **no** demuestra un fallo de compatibilidad NT/Win32.

Dependencias adicionales que el usuario instaló antes de la segunda ejecución:

```sh
sudo dnf install -y openssl-devel fuse3-devel mingw64-gcc
pkg-config --modversion libcrypto fuse3
x86_64-w64-mingw32-gcc --version
```

Versión mostrada por el usuario: libcrypto 4.0.3, FUSE3 3.18.3, MinGW GCC 16.2.1.

## 2. Matriz de evidencia de la tercera ejecución

| Componente | Evidencia favorable (alcance delimitado) | Sin demostrar / bloqueos |
| --- | --- | --- |
| **1. Cargador PE y DLL** | `check-pe-image`, `check-pe-dll-graph` y ejecución de fixtures PE32+ AMD64 por el host; carga dinámica/estática, TLS, exportaciones/forwarders en casos controlados | `check-pe-isolated` **FALLO** por `KERNEL32.dll!lstrcmpW` no resuelta antes de completar el fixture de procesos hijos |
| **2. PEB/TEB** | `check-pe-native-core`, `check-pe-teb-tls`, registros GS/TEB/PEB y TLS de fixture | PEB/TEB universal, interacciones arbitrarias de aplicaciones no demostradas |
| **3. Objetos NT** | `check-object-broker`, `check-pe-core-objects`, `check-pe-named-events`; IPC Linux y broker autenticado entre procesos independientes | ACL, tokens y objetos NT globales completos pendientes según `nt-core-readiness.md` |
| **4. Procesos e hilos** | `pidfd`, afinidad real de hilos, `check-stage4-quantum`, `check-pe-suspended-thread`, `check-pe-process-affinity`, `check-pe-thread-groups`, prioridad/APC y fixture sintético 80 CPU | **No** está demostrado el proceso hijo PE dentro del mismo Bubblewrap; ni quantum NT equivalente, multicgrupo físico, boost positivo con privilegios o capacidades generales de hilos externos |
| **5. Memoria virtual** | `memfd`/`mmap`, `check-pe-memory`, secciones compartidas, COW y mapeo controlado | Mapeo NT general/remoto y protecciones completas no demostradas |
| **6. SEH/VEH** | `check-pe-seh`, `check-pe-veh` y contratos de unwind en la suite nativa | Traducción general de señales Linux y semántica completa de excepciones Windows no demostradas |
| **7. NTDLL/Win32** | `check-pe-kernel-user`, `check-pe-import`, `check-pe-nt-io`, `check-pe-platform` | Falta resolver `lstrcmpW` en KERNEL32/KERNELBASE; pasar fixtures de importación de un par de funciones no prueba cobertura global |

Hallazgos particulares:

- **Planificador:** `check-stage4-quantum` registra `FAIRNESS_RATIO=0.97974`, pero explícitamente `NT_QUANTUM_VERIFIED=NO` y `SCHEDULER_EQUIVALENCE=NOT_DEMONSTRATED`. Es observación Linux, no emulación fiel del quantum NT.
- **80 CPU:** `PASS sparse synthetic 80-CPU compact guest groups (NOT physical hardware)`. No sustituye prueba sobre una máquina multigrupo real.
- **`make check`:** código de salida **0**, pero el QA informa `INCOMPLETO`; se leen `SKIP positive boost`, `SKIP positive multithread priority transition`, `SKIP process group inheritance` y `SKIP native FluidSynth ABI: libfluidsynth.so.3 unavailable`. No presentarlos como pruebas positivas.
- **FUSE/Landlock:** `check-dos83-fuse` termina `PASS`, incluye montaje FUSE y denegaciones de escritura por Landlock/Bubblewrap. Se imprimen varios `sh: ... /dev/null: Permiso denegado` dentro del test restringido: **auditar** esos errores de redirección, sin concluir automáticamente que el confinamiento es inseguro o que el host tiene permisos rotos. El log reporta `PASS`, no certificación de seguridad formal.
- **Sandbox:** en la prueba 8 sí pasan el rechazo del worker sin sandbox, el PE de escritorio, la persistencia de preferencias y la carpeta Linux compartida. **No** aparece la línea de éxito exigida para el PE hijo; no declarar superada la aceptación física del componente 4.

## 3. Defecto reproducible: importación `KERNEL32.dll!lstrcmpW`

### Traza original de la tercera ejecución

```text
[8/29] [REAL_RUNTIME] Componente 1: Ejecutar PE controlados en Bubblewrap real (incluye hijos)
$ make -j2 BUILD_DIR=build-nt144-qa check-pe-isolated
...
PASS PE preferences persist across two isolated processes
PASS PE known folder resolves to the original Linux file
Unsupported guest import KERNEL32.dll!lstrcmpW
PE loader: Unresolved or malformed PE import table
Guest exited with code 21
make: *** [Makefile:643: check-pe-isolated] Error 21
RESULTADO: FALLO (salida 2, 6.9s)
```

El texto `Guest exited with code 21` es la salida de la cadena de ejecución del fixture, **no una prueba de que el proceso hijo haya completado su lógica**. El fallo demostrado es **resolución de importación ausente**; quedan por descubrir potenciales defectos adicionales después de arreglar esa primera barrera.

### Inspección independiente del código del repositorio

- `src/runtime/exports.c`: `kernel32_exports` incluye `CompareStringW` pero **no** `lstrcmpW`; `kernelbase.dll` reutiliza la tabla `kernel32_exports`.
- `tests/pe/child_process_fixture.c`: la rama `NT_CHILD_FIXTURE` llama a `lstrcmpW` para validar entorno Unicode: `Grüße`, `árbol`, `niño`; es la importación que el linker MinGW coloca en el PE hijo.
- `src/runtime/nls.c`: existe `nt_CompareStringW`. **No asumir equivalencia completa**: la semántica de comparación lingüística de Windows, el resultado de `CompareStringW` (1/2/3) y el de `lstrcmpW` (negativo/cero/positivo) no son intercambiables sin adaptación y verificación.

### Especificación de corrección propuesta (NO implementada)

1. Implementar `lstrcmpW` nativa en un módulo apropiado del runtime, usando **`NT_MS_ABI`** (convención x64 de Windows), cadenas `LPCWSTR` **UTF-16 de 16 bits**, terminadas en NUL y **comparación sensible a mayúsculas**. Documentar la comparación según las reglas de Windows/locale aplicables; no asumir que `wchar_t` de Linux (habitualmente 32 bits) es `WCHAR`.
2. Devolver **<0, 0 o >0** según el orden de las cadenas. Si se reutiliza `CompareStringW`, transformar sus códigos de retorno y tratar correctamente error `0`; comprobar la equivalencia lingüística frente a Windows antes de afirmar paridad. Una solución ordinal mínima puede desbloquear el fixture, pero **no valida** toda la compatibilidad de la API.
3. Exportar el nombre exacto `"lstrcmpW"` en el registro `kernel32_exports` para KERNEL32 y su alias KERNELBASE, manteniendo la resolución del host en un único sitio. Considerar `lstrcmpA` **solo si hay implementación y pruebas independientes**; no declarar cobertura no medida.
4. Añadir una prueba de importación PE32+ AMD64 que enlace realmente con `KERNEL32.dll!lstrcmpW` (no únicamente una llamada directa C), verifique cadenas iguales y diferentes, prefijos, cadenas vacías, Unicode (`Grüße`, `árbol`, `niño`) y el signo del orden de retorno. Añadir comprobaciones de ABI x64 y conservación de errores documentada, y comparación diferencial sobre Windows para colación regional.
5. Reejecutar `check-pe-isolated`: exigir la línea `PASS PE child launched inside the same Linux sandbox and returned its full exit code`, y, si corresponde, `PASS PE launcher preserves Windows arguments, escaping and Unicode`. Eliminar la primera barrera **no garantiza** que estos escenarios posteriores pasen.
6. Repetir `make check`, fixtures relevantes, sanitizadores cuando afecte al parser o ABI, y `scripts/fedora-check-stage4.sh` sobre Fedora con comprobación estricta de **no SKIP**. Publicar el log nuevo, SHA exacto, cambios y CI antes de afirmar cierre.

**Prioridad arquitectónica:** afecta al componente **7 (API NTDLL/Win32)**, pero **bloquea la validación del componente 4 (procesos e hilos)**. Según [orden vinculante](nt-core-development-order.md), se permite intervenir **solo en la interfaz mínima transversal** necesaria para eliminar el bloqueo, sin cambiar la prioridad principal 4 → 1 → 5 → 2 → 3 → 6 → 7.

## 4. Protocolo de reproducción y aceptación para otros proyectos

En Fedora 45 x86-64 con dependencias del repositorio instaladas, desde el clon de `main`:

```sh
git rev-parse HEAD
uname -a
cat /etc/fedora-release
getenforce  # si SELinux está disponible
pkg-config --modversion libcrypto fuse3
x86_64-w64-mingw32-gcc --version

# Reproducción focalizada; NO usar sudo para ejecutar PE:
make -j2 BUILD_DIR=build-nt144-qa check-pe-isolated

# Verificación extensa tras una corrección:
make -j2 BUILD_DIR=build-nt144-qa check
make -j2 BUILD_DIR=build-nt144-qa check-pe-import check-pe-kernel-user
bash scripts/fedora-check-stage4.sh
```

**Criterios de aceptación de la incidencia:**

- `lstrcmpW` resuelta por IAT mediante el ABI correcto, con pruebas funcionales y de error del alcance declarado.
- El PE hijo y los argumentos pasan dentro del **mismo sandbox**, sin saltarse Bubblewrap y sin usar `sudo`.
- El script Fedora de fase 4 pasa en las condiciones de host que exige, sin convertir `SKIP` en `PASS`.
- Evidencia nueva reproducible: commit, Fedora/kernel, privilegios/SELinux, CPU/grupos reales, command line, log y CI vinculados.
- **No** cambiar porcentajes de madurez ni declarar compatibilidad general hasta evidencias independientes adicionales.

## 5. Conclusión / tesis de trabajo para integradores

La tercera ejecución demuestra que **el subconjunto ensayado** de NT/Win32 consigue correr múltiples contratos reales y fixtures PE AMD64 en Fedora 45 (27/29 casos del harness). La única falla efectiva en ese perfil es, por ahora, una **importación ausente de `lstrcmpW`** en el escenario PE-hijo bajo Bubblewrap. La suite nativa restante no declara fallo, aunque contiene omisiones explícitas (`SKIP`). Por tanto:

1. No confundir disponibilidad de recursos Linux (`pidfd`, `memfd`, `AF_UNIX`) con equivalencia de semántica NT.
2. No confundir pruebas sintéticas, como 80 CPU, con validación física multigrupo.
3. No confundir importaciones resueltas con contratos de Windows completos ni compatibilidad de aplicaciones arbitrarias.
4. Tratar `lstrcmpW` como **impedimento comprobado para avanzar las pruebas de procesos hijos** y como una corrección transversal mínima, sujeta a regresión.
5. Conservar evidencia falsable y diferenciar tres estados: **PASS efectivo**, **FALLO real**, **INCOMPLETO/SKIP**.

La tesis es **diagnóstica y refutable**: a medida que se eliminen barreras del cargador, los siguientes fallos deben documentarse en lugar de suponer el cierre de la fase 4.
