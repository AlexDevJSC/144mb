# Remediación basada en NT144 QA de Fedora 45 (16-11-2026)

Fuente prioritaria: [tesis de campo](fedora45-nt144qa-validation-2026-11-16.md).
Los números **27 PASS / 1 FAIL / 1 INCOMPLETO** son los del informe
histórico; no se reescriben ni se consideran resultados de este cambio.
El checkout de aquella ejecución no estaba identificado por SHA.

## Implementaciones en el orden aprobado

| Orden | Estado del cambio | Archivo / prueba y criterio real |
| --- | --- | --- |
| 1. `lstrcmpW` | Implementación ordinal, deliberadamente sin afirmar colación lingüística Windows | `src/runtime/nls.c`, `nls.h`, export `KERNEL32/KERNELBASE` en `exports.c`. NUL-terminated UTF-16, ABI Microsoft x64, signo negativo/0/positivo |
| 2. Cobertura de la API | Añadida prueba real PE32+ enlazada con `-lkernel32` y prueba host ABI | `make check-pe-lstrcmp`, `tests/nls_lstrcmp_test.c`, `tests/pe/lstrcmp_fixture.c`. Los símbolos deben entrar por IAT; se prueban prefijos, vacías, Unicode, signo y LastError |
| 3. Hijo PE Bubblewrap | Desbloqueada la primera importación conocida; **la aceptación física sigue sin ejecutar desde esta rama** | `make check-pe-isolated`: exige el retorno real del hijo y los argumentos, sin desactivar sandbox. Si surge un siguiente fallo, recopilarlo y repararlo |
| 4. Omisiones invisibles | Nuevo clasificador estricto de tres resultados y evidencia por literal | `scripts/qa_classify.py`, unit tests, `scripts/fedora-check-stage4.sh`: `SKIP` o falta de PASS obligatorio → INCOMPLETE (3), error → FAIL (1) |
| 5. Nice, multihilo y boost | CI ya dispone de pruebas positivas; ahora Fedora físico permite reproducirlas **solo mediante opt-in** | `NT144_QA_POSITIVE_NICE=1` ejecuta `sudo -n prlimit` **únicamente** en dos pruebas desechables, nunca en PE ni procesos host ajenos; se requiere éxito real 0→4→0 y boost reversible |
| 7. FluidSynth | Perfil completo exige la SONAME correcta, sin considerar ausencia como éxito | `scripts/fedora-qa-preflight.sh` verifica `dlopen(libfluidsynth.so.3)`; `make check-native-audio-required` comprueba API/síntesis sin un SoundFont externo |
| 8. Landlock/FUSE | Pruebas de denegación corregidas para no redirigir a `/dev/null` **dentro** del confinamiento | `tests/dos83_fuse_smoke.sh`: los intentos se ejecutan y su stderr se captura fuera; una denegación del propio `/dev/null` es fallo de la prueba, no PASS de seguridad |
| 9. Evidencia y preflight | Logs persistentes, metadata de SHA/kernel/SELinux/cpus y clasificación JSON | `scripts/fedora-check-stage4.sh` guarda `commands.log`, `metadata.txt`, `classification.json`; install opcional de paquetes vía `--install` |
| 6. Multigrupo físico | Gate preparado, sin fabricar resultado | `scripts/fedora-stage4-multigroup-acceptance.sh` mide CPUs **permitidas** y grupos invitados compactos. Necesita Fedora KDE físico con al menos 65 CPUs accesibles |

**No se ha implementado ni certificado una comparación lingüística completa
de `lstrcmpW` con Windows.** Las colaciones particulares requieren un
fixture diferencial Windows y probablemente un backend NLS más amplio
(componente arquitectónico de APIs Win32); una igualdad ordinal no es paridad.

## Reproducción en Fedora 45

Desde el repositorio actualizado (trabajar con commit identificado):

```bash
git rev-parse HEAD
bash scripts/fedora-qa-preflight.sh
# Instala dependencias solo por acción explícita del operador:
bash scripts/fedora-qa-preflight.sh --install

make -j2 check-pe-lstrcmp
make -j2 check-pe-isolated
python3 -m unittest discover -s tests -p 'test_qa_classify.py'

# Opt-in ÚNICAMENTE para pruebas nativas de prioridad con nice restaurable.
# Si no hay sudo sin contraseña o RLIMIT_NICE reversible, el estado
# correcto es INCOMPLETE; no ejecutar Windows PE como root.
NT144_QA_POSITIVE_NICE=1 bash scripts/fedora-check-stage4.sh
```

El validador conserva artefactos en
`${XDG_STATE_HOME:-$HOME/.local/state}/nt144-qa/stage4/run-*/`.
Puede especificarse `NT144_QA_OUTPUT_DIR` para elegir otro destino.
No subir logs con rutas privadas, tokens o detalles de host sin revisarlos.

Para hardware adecuado:

```bash
NT144_QA_POSITIVE_NICE=1 \
  bash scripts/fedora-stage4-multigroup-acceptance.sh
```

**Criterios para actualizar la tesis histórica:** obtener el SHA de la
nueva prueba física, corroborar la línea del hijo PE en su mismo sandbox,
revisar la clasificación JSON completa y repetir la herramienta externa
`nt144-qa --profile completo`. La CI Linux y el RPM Fedora en
contenedor **no** sustituyen estas mediciones físicas. Un QA sin FAIL
pero con SKIP tampoco es una aceptación completa.
