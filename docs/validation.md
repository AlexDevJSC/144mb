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
ordinales en los perfiles del contador, nombres inválidos e importaciones
retardadas. El perfil de escritorio admite `COMCTL32.dll #17` y rechaza
un ordinal distinto, bits reservados activos y una IAT inconsistente.
Las variantes inválidas solo llegan al parser.

GitHub Actions compila los PE de control con MinGW-w64 y los ejecuta
con nuestro cargador en Ubuntu. El trabajo de Fedora comprueba el RPM
y repite las pruebas del runtime.

## Cobertura de Mines

Las pruebas de los 30 equivalentes iniciales y las 74 incorporaciones
se ejecutan con `make check` en Linux. Las comprobaciones verifican
el registro y llaman a las funciones mediante el ABI Microsoft x64.

La auditoría de solo lectura encontró equivalentes para las 197 importaciones
directas del archivo de referencia. `make check-desktop` prueba los perfiles
de diálogo, recursos, captura y selección de archivos con SDL en Linux.
Las pruebas y sus límites están en [Compatibilidad de Mines](mines-compatibility.md).

## Reutilización entre ejecutables

Auditoría de los archivos aportados por el usuario, revisada el 11 de
noviembre de 2026 en Linux x86-64. La tabla registra arquitectura,
importaciones directas y pruebas realizadas. Un equivalente registrado
requiere además comprobar su comportamiento para la aplicación concreta.

| Ejecutable | Arquitectura | Importaciones con equivalente | Resultado comprobado |
| --- | --- | --- | --- |
| `mines.exe` | AMD64 | 197 de 197 | Victoria y derrota en Linux, confirmadas también por el usuario en Fedora. Guardado y recuperación de preferencias comprobados fuera de pantalla. |
| PE de prueba del perfil | AMD64 | 5 de 5 | Dos procesos independientes conservan datos usando las funciones de entorno y archivos del runtime. |
| `Squadron121_DX12.exe` | AMD64 | 69 de 324 | Auditoría de solo lectura con 255 importaciones pendientes, incluidas funciones de sistema, ventanas, bibliotecas externas y gráficos. |
| Instalador AssaultCube 1.3.0.2 | x86, PE32 | Auditor AMD64 fuera de alcance | Se inspeccionó la arquitectura del instalador. El contenido instalado requiere una comprobación separada. |
| `Maniaplanet_Setup.exe` | x86, PE32 | Auditor AMD64 fuera de alcance | Se inspeccionó la arquitectura del instalador. |

En Squadron121 quedan 121 entradas de `KERNEL32`, 47 de `USER32`, 22 de
`libfluidsynth-3`, 18 de `sl.interposer` y 15 de `WS2_32`, además de
otras bibliotecas. La auditoría usa el registro compartido de equivalentes
y cuantifica la reutilización existente. Las pruebas funcionales y la
integración de dependencias siguen teniendo sus propios criterios de cierre.
El archivo auditado tiene SHA-256
`cfc4bbeb6948490f72ca63b031c9339c7eb68a9ef40f0b54b78da58cd8355d01`.

Se reproduce con `build/pe-import-audit --unresolved /ruta/Squadron121_DX12.exe`.
El auditor devuelve estado 1 al encontrar importaciones pendientes.

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
