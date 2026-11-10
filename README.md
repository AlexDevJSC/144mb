# nt-native-linux

Base experimental de una capa de compatibilidad NT y Win32 integrada en
Linux. El objetivo es ejecutar aplicaciones de Windows en Fedora KDE Plasma
manteniendo el kernel y los controladores de Linux. Cada implementación
reproduce un contrato observable mediante recursos del anfitrión. El objetivo
inicial es x86-64 con gráficos AMD mediante amdgpu y Mesa RADV.

## Estado de la base 0.1.0

- `NtQueryPerformanceCounter` en C, respaldado por `CLOCK_BOOTTIME`.
- Un programa de prueba PE32+ AMD64 que llama a esa implementación mediante
  el ABI Microsoft x64. El anfitrión ejecuta el código de un binario de prueba
  generado e incrustado durante el build.
- Un microbenchmark y un spec RPM para el comando ELF de diagnóstico.
- Quedan pendientes el cargador de aplicaciones PE, la resolución de
  importaciones, el modelo de procesos y objetos NT y la integración con KDE.

## Compilar y probar

En Linux x86-64 con GCC, GNU Make y GNU binutils:

```sh
make
make check
make check-pe
make benchmark
```

`make check-pe` requiere que `ld -V` liste `i386pep` y que `objdump -i`
liste `pe-x86-64`. El build genera `build/nt-counter.exe` y
`build/pe-host`. Ejecuta `build/pe-host` para repetir la prueba. El PE
espera una función entregada por el anfitrión y no tiene un punto de entrada
de aplicación Windows convencional. Todas las pruebas se ejecutan en Linux.

La prueba Linux y el comando de diagnóstico se compilan por separado del PE.
Puedes probar el destino del paquete sin privilegios con `make install
DESTDIR="$PWD/build-stage" PREFIX=/usr`.

## Navegación

| Ruta | Contenido |
| --- | --- |
| [include/ntnative](include/ntnative) y [src/ntdll](src/ntdll) | ABI interno y código de compatibilidad |
| [tests](tests) | Contratos, PE controlados y medición en Linux |
| [Arquitectura](docs/architecture.md) | Límites entre el kernel, el runtime y el escritorio |
| [Reutilización y licencias](docs/reuse.md) | Inventario, fuentes y dependencias |
| [Decisiones](docs/decisions.md) | Fundamento, alcance y condiciones de revisión |
| [Validación](docs/validation.md) | Pruebas realizadas y plan comparativo |
| [Backlog](docs/backlog.md) | Tareas ordenadas y definición del siguiente PR |
| [Fedora](packaging/fedora/README.md) | Compilación RPM y validación en VM |
| [AGENTS.md](AGENTS.md) | Instrucciones reproducibles para Codex |

El código y la documentación originales están bajo la [licencia MIT](LICENSE).
Las dependencias de la plataforma conservan sus licencias. La comparación con
Wine, Proton y ReactOS se documenta por separado del código que se ejecuta.
