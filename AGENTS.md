# Instrucciones para Codex y colaboradores

Trabaja sobre Linux x86-64. Lee `README.md`, `docs/architecture.md`,
`docs/reuse.md` y la tarea correspondiente en `docs/backlog.md` antes
de modificar código. Usa una rama corta por tarea.

## Contratos

- Define entradas, salidas, tamaños, alineación, ABI, códigos NTSTATUS,
  permisos y efectos parciales observables antes de implementar una API.
- Verifica los contratos con documentación pública y pruebas sintéticas
  en Linux. Registra la versión de la referencia, el comando y el resultado.
- Usa Linux estándar y conserva los caminos de error. Las primitivas de
  objetos y memoria deben tener límites explícitos de vida, propiedad y
  concurrencia. Conserva SELinux y el principio de mínimo privilegio.
- Mantén el código de Windows invitado separado de las interfaces del host.
  La ejecución de PE de laboratorio se limita a fixtures generados en el
  propio build. El soporte de PE externos exige diseño y revisión de
  aislamiento previos.
- Registra la licencia y el origen de cada dependencia. Usa SPDX para
  código propio, sin copiar implementaciones externas a los módulos MIT.
  Excluye código filtrado, DLL propietarias y bypasses de DRM o antitrampas.

## Verificación y entrega

- Ejecuta todas las pruebas en Linux x86-64, incluidos los PE de laboratorio.
  Usa `make -j2 check check-pe` con GCC y binutils. Con MinGW-w64
  disponible, ejecuta también `make check-pe-import`.
  Si no está disponible la herramienta PE, ejecuta `make check` y
  registra la limitación.
- Para cambios de parser o ABI, repite las pruebas con UBSan y agrega un
  caso mínimo que cubra el fallo. Para cambios observables, añade un
  fixture reproducible que se ejecute en Linux.
- Registra compilador, kernel, arquitectura, backend y comandos. Separa
  resultados locales de CI y de mediciones externas. Usa las indicaciones
  de `docs/validation.md` para comparar rendimiento.
- Presenta un PR con justificación, evidencia, revisión y alcance de
  licencia. Deja la fusión al mantenedor después de revisión y CI.
- Si se coordinan agentes, separa investigación, código y revisión.
  Cada resultado debe incluir evidencia que el integrador pueda
  verificar. Usa directorios de build distintos para pruebas simultáneas.

El punto de partida para la próxima contribución es `P0-01` en
[el backlog](docs/backlog.md). Usa los criterios de ese punto como
alcance del PR.
