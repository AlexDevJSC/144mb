# FileSystem4 / FileSystem6 — auditoría acotada del QA de Fedora 45

Revisión de origen: `96199dd54f07140cd64ef27ba35d6c8ef001d0f4`.
La comparación GitHub `96199dd...main` al iniciar este trabajo fue
`identical` (0 ahead, 0 behind): los cambios de esta rama parten de
esa revisión sin arrastrar modificaciones de otro componente.

## Procedencia

El responsable de NT144 QA comunica para Fedora 45 (kernel
`7.3.3-300.fc45.x86_64`, SELinux `Enforcing`, `/dev/fuse` disponible,
cuatro CPU): 76 PASS, 6 FALLO, 1 INCOMPLETO. Ningún FALLO atribuido
a los objetivos específicos de FileSystem4. Es **evidencia reportada por
el usuario**, no una ejecución reproducida en este entorno ni una
certificación de NTFS.

De `fs-handle-broker.log` se han comunicado `ResourceWarning: unclosed file
<_io.BufferedReader ...>` alrededor de las líneas 304/332 del
`tests/fs4_handle_broker_test.py` previo. El caso terminaba
`Ran 9 tests\nOK`. El archivo bruto de esa ejecución de QA no forma
parte de la evidencia descargada del repositorio en esta revisión.

El informe `fs-ntfs-diff.log` comunica
`PASS NT144 live Win32 W-API differential probe (hardlink unimplemented)`:
el PASS es del subconjunto ensayado, **no** de hardlinks.

## Hallazgo reproducible por lectura de código

`Broker.__init__` abría `stdout=subprocess.PIPE` y
`stderr=subprocess.PIPE`. El cierre normal hacía solamente
`client.close()` y `process.wait()`, leyendo `stderr` **solo si
se incumplía** el `assert`. En las pruebas de muerte deliberada se
cerraba el socket sin cerrar ambos pipes. En el fallo de HELLO se
invocaba `stderr.read()` antes de terminar al proceso, con riesgo
de espera indeterminada. Estos son recursos del **harness Python**,
no un diagnóstico de fuga en el proceso C.

La ruta del broker C `src/tools/nt_storage_handle_broker.c` tiene una
tabla acotada de 32 capacidades por sesión; `serve()` libera
`nt_FindClose`/`nt_CloseHandle` de las entradas restantes tras terminar
el bucle por cierre de peer, nonce incorrecto, timeout o límites. El
proceso cierra el socket antes de salir. `SIGKILL` provoca cierre
de descriptores por el kernel y se prueba con recuperación desde
otro proceso. **No se ha reproducido ni demostrado fuga interna del
servicio**: no se cambia este código.

## Cambios realizados

- El constructor del harness libera socket servidor/cliente, termina
  el worker y espera drenando ambos pipes en cualquier fallo de arranque.
  No lee `stderr` de un proceso aún vivo.
- `Broker.close(expected_returncode=..., timeout=...)` cierra el canal,
  llama `communicate` (drena y cierra *ambos* streams), recoge la salida
  diagnóstica y valida el código de salida. Un worker atascado termina
  en `SIGKILL`, se recoge el hijo y se informa error.
- Los tests `SIGKILL` y los seis puntos de crash mantienen la muerte
  **real**, comprueban `-SIGKILL` y llaman al mismo cierre determinista;
  el nuevo worker conserva las comprobaciones de datos y OFD lease.
- Se cierra el primer worker si falla crear el segundo en las tres
  pruebas de concurrencia.
- Nuevo test de cierre normal, test de HELLO fallido con contabilidad
  `/proc/self/fd`, y test de timeout bajo `SIGSTOP` con comprobación
  de reap y cierre de pipes.
- `check-fs4-handle-broker` ejecuta Python con
  `-W error::ResourceWarning`; **no se añade un filtro global que
  silencie avisos**. Las aserciones explícitas detectan streams no
  cerrados incluso cuando la advertencia en `__del__` sería diferida.

## Contratos FileSystem4 conservados

- No se cambian el IPC binario v3, límites, nonce, capacidades, ni la
  política de credenciales.
- No se cambian `FUSE`, `Landlock`, SELinux, `openat2`, el backing UID
  ni las restricciones de rutas DOS 8.3/UNC/carpetas.
- No se modifican WAL, journal, `delete-pending`, `rename`, xattrs,
  reparse, ACL ni políticas de sincronización/lease.
- El rechazo de hardlinks desde FUSE UID se conserva; véase
  `docs/filesystem4-hardlinks-contract-blocker.md`.

## Matriz de validación y límites

Los siguientes son **objetivos existentes** y deben ejecutarse sobre
el checkout exacto de esta PR:

```sh
make -j2 check-fs4-handle-broker
make -j2 check-fs4-ntfs-diff
make -j2 check-fs4-reparse-wal
make -j2 check-fs4-storage-service
make -j2 check-dos83-fuse
make -j2 check-fs4-fuse-staging-sigkill
make -j2 check-fs4-readonly-session
make -j2 check-fs4-service-supervisor
make -j2 check-fs4-acceptance-gates
make -j2 check-dos83-wal
make -j2 check
```

**Entorno de autoría de esta PR:** Debian 13 (no Fedora físico), sin
`/dev/fuse`, sin SELinux Enforcing ni acceso a los artefactos brutos
de NT144 QA. El transporte GitHub permite modificar y consultar
CI, pero el entorno local carece de checkout y de acceso directo a
GitHub para ejecutar `make` contra este árbol. Las pruebas
del Makefile deben verificarse por el job GitHub Actions específico,
registrando por separado el éxito de FileSystem4 y eventuales fallos
de Core4/Win32 ajenos. No considerar falta de ejecución como PASS.

Aprobación física requiere repetir en Fedora con SELinux Enforcing,
inspeccionar AVC/etiquetas/UID real, pruebas negativas de segundo UID,
FUSE real, procesos concurrentes, reinicio y comparar Windows NTFS.
`SIGKILL` no equivale a corte eléctrico; una prueba de alimentación
debe usar volumen desechable y supervisión del operador.

**Afirmación permitida de no regresión:** el cambio es solo en harness
y documentación (además del target Python específico); cualquier
afirmación de `sin regresiones detectadas` exige consultar la CI
resultante de esta rama y no extrapolar sus PASS a pruebas no ejecutadas.

No se realizan cambios en procesos, APC, gráficos, USER32,
`CreateEventW`, afinidad/planificador, enlazador PE o auditor de
importaciones; tales defectos, si surgen, corresponden al Núcleo u
otros responsables.
