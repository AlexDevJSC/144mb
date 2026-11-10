# Validación

## Resultados locales

Entorno del 9 de noviembre de 2026. Ubuntu 24.04.3 LTS x86-64,
Linux 6.18.57, glibc 2.39, GCC 13.3 y GNU binutils 2.42.

| Comprobación | Resultado observado |
| --- | --- |
| `make -j2 BUILD_DIR=build-release check check-pe` | Pasaron 9 comprobaciones Linux, el comando de diagnóstico y 4 casos llamados desde código PE32+ AMD64. |
| UBSan en el host y la función | Pasaron `check` y `check-pe` sin diagnósticos. |
| ASan y UBSan en el host y la función | Pasaron `check` y `check-pe` con `ASAN_OPTIONS=detect_leaks=0`. LeakSanitizer no pudo leer `/proc` en este entorno. El PE freestanding queda sin instrumentar. |
| Revisión del parser | Con ASan y UBSan, se rechazaron 1.024 truncamientos y el solapamiento con cabeceras. Se probaron 262.144 variaciones de un byte sin ejecutar sus contenidos y sin diagnósticos de los sanitizadores. |
| Repetición y análisis estático | Pasaron 100 ejecuciones consecutivas del anfitrión PE. El build con `-fanalyzer` completó `check` y `check-pe`. |
| Inspección PE | `file` y `objdump` identificaron PE32+ AMD64. `objdump -r -j .text` confirmó que la función del fixture carece de relocaciones. Los builds de release y UBSan generaron el mismo PE byte por byte. |
| Instalación sin privilegios | `make install DESTDIR="$PWD/build-stage" PREFIX=/usr` y la ejecución desde ese destino funcionaron. |
| Definiciones de CI e incidencias | Los YAML se analizaron localmente. Los jobs necesitan una ejecución en GitHub. |
| `make BUILD_DIR=build-release benchmark` | Completó 7 muestras de 500.000 llamadas por ruta, además del calentamiento. |

En la ejecución de benchmark registrada, la mediana fue de **38,28
ns/llamada** para el reloj Linux con conversión y **44,39 ns/llamada**
para la función NT. Los rangos fueron 31,15–48,86 y 34,71–54,91
ns/llamada. El máximo RSS del proceso conjunto fue 1.280 KiB. Cada
muestra incluye el bucle de verificación. Estos datos describen esta
sesión, un reloj y dos rutas dentro del mismo proceso. Para comparar
huellas de backends hacen falta procesos separados y una carga de
aplicación representativa.

## Reproducir Linux

```sh
make -j2 check check-pe
make -j2 BUILD_DIR=build-ubsan \
  CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' check check-pe
make benchmark
```

La prueba usa el reloj real para verificar el orden y un reemplazo de
`clock_gettime` mediante `--wrap` para probar truncamiento a 100 ns,
el límite de 64 bits, desbordamiento y fallo del anfitrión. Los casos PE
usan el reloj real a través del puente Microsoft x64. Se comprueban
resultados y efectos sobre salidas cuando los define este prototipo.

## Importación PE por nombre

Con GCC, binutils y MinGW-w64 AMD64 disponibles en Linux x86-64:

```sh
make -j2 check check-pe check-pe-import
make -j2 BUILD_DIR=build-ubsan \
  CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' check check-pe check-pe-import
```

`objdump -p build/nt-counter-import.exe` muestra la importación de
`NtQueryPerformanceCounter` desde `ntdll.dll`. El PE comprueba frecuencia
positiva y estable, contador no decreciente y frecuencia opcional.
El resultado se comunica mediante el código de salida. El anfitrión
también verifica el rechazo de tablas truncadas, RVA fuera de rango,
ordinales, nombres inválidos e importaciones retardadas. Las variantes
inválidas solo llegan al parser.

GitHub Actions compila los PE de control con MinGW-w64 y los ejecuta
con nuestro cargador en Ubuntu. El trabajo de Fedora comprueba el RPM
y repite las pruebas del runtime.

## Cobertura de Mines

Las pruebas de los 30 equivalentes iniciales y las 74 incorporaciones
se ejecutan con `make check` en Linux. Las comprobaciones verifican
el registro y llaman a las funciones mediante el ABI Microsoft x64.

La auditoría de solo lectura leyó 197 importaciones directas, con 146
equivalentes registrados y 51 pendientes. Una entrada pendiente
identifica su símbolo por ordinal. Las pruebas y el inventario están
en [Compatibilidad de Mines](mines-compatibility.md).

## Cobertura pendiente

- Instalación y desinstalación del RPM en VM Fedora KDE.
- Suspensión y reanudación en máquina real. La elección de reloj deriva
  de su contrato documentado.
- `ntsync`, compositor y GPU. El entorno local carece de `/dev/ntsync`.
  Las pruebas gráficas de esta etapa usan superficies en memoria.
- Compatibilidad con aplicaciones externas y errores con punteros
  invitados no nulos. El anfitrión de prueba tiene un alcance acotado.

## Próximas mediciones

| Dimensión | Método |
| --- | --- |
| Comportamiento | Mantener en Linux un corpus de entradas, NTSTATUS, resultados y efectos esperados. Guardar revisión, distribución, kernel y arquitectura. |
| Estabilidad | Pruebas repetidas y concurrentes, timeout, caída, cierre de handles y recuperación de recursos. Registrar errores, cuelgues y diferencias. |
| Memoria | Ejecutar cada backend en un proceso y entorno separados. Capturar pico RSS y memoria proporcional de todos los procesos implicados con la misma carga. |
| Latencia y CPU | Mismo binario, máquina, estado de caché y configuración. Calentar, alternar orden, repetir, registrar p50 y p95, tiempo de CPU y dispersión. Separar trazas diagnósticas de las muestras de rendimiento. |
| Compatibilidad gráfica | Comparar apertura, render, entrada, cierre y pérdidas de dispositivo con versiones y capacidades Vulkan registradas. |
