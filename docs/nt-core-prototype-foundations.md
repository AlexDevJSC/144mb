# Base integrada del runtime NT/Win32 — perfil prototipo

## Propósito

Primero conectar las infraestructuras reutilizables; después validar programas
concretos y optimizar. Este documento enumera lo que tiene **código** y
**contratos mínimos comprobables** en lugar de atribuir compatibilidad general
por número de exportaciones. Es independiente del proyecto de videojuego
PROYECTO 1.44 / BONES.

## Componentes del prototipo

| Núcleo | Base disponible | Próximo enlace genérico a desarrollar |
| --- | --- | --- |
| 1. PE / DLL | Cargador PE32+ x64, importaciones, relocaciones, DLL locales, DllMain y TLS inicial | Registro dinámico de TLS en todos los hilos, búsqueda Win32 y descarga concurrente |
| 2. PEB / TEB | PEB/TEB parciales, TLS/FLS, listas de módulos locales | Incorporar secciones y módulos cargados tardíamente al estado del proceso |
| 3. Objetos NT | Handles de eventos y semáforos locales; secciones anónimas/nominadas locales con referencias | Broker general entre procesos, mutexes y namespace con derechos |
| 4. Procesos / hilos | Creación de hilos/hijos acotada, espera, APC normales, prioridades/afinidad representables | Contextos y suspensión de hilos arbitrarios, herencia interproceso de objetos NT |
| 5. Memoria | Reservas privadas, protecciones y consultas; **secciones memfd** con vistas compartidas/COW, MEM_MAPPED y puente NtMapViewOfSection | Mapeos basados en archivos Windows, mapeos en procesos externos, PAGE_GUARD |
| 6. Excepciones | Desenrollado x64 parcial, RaiseException y VEH software | SIGSEGV/SIGILL/SIGFPE a excepciones invitadas y continuación segura |
| 7. NTDLL / Win32 | Registro de APIs de KERNEL32/KERNELBASE/NTDLL; mismas secciones por ambos ABI | Cobertura de API generales y contratos de Windows que aún no existen |

## Implementación transversal de esta entrega

El módulo `section_memory.c` utiliza `memfd_create` + `mmap` para secciones
de memoria sin fichero, lo que permite compartir bytes entre dos vistas y
un proceso nacido por `fork`. Aporta dos caminos hacia el mismo objeto:

- KERNEL32: `CreateFileMappingW`, `OpenFileMappingW`, `MapViewOfFile`,
  `FlushViewOfFile`, `UnmapViewOfFile`.
- NTDLL: `NtCreateSection`, `NtMapViewOfSection`,
  `NtUnmapViewOfSection`; también nombres `Zw*` mediante el registro común.

Los handles participan en `CloseHandle`, `DuplicateHandle`,
`GetHandleInformation`, `SetHandleInformation` y
`GetProcessHandleCount`. Las vistas sobreviven al cierre de todos los
handles hasta que se desmonta la última vista. `VirtualQuery` las informa
como `MEM_MAPPED`. No hay páginas ejecutables ni rutas host ajenas al sandbox.

### Alcance conscientemente limitado

- `CreateFileMappingW` sólo admite el handle de paging file
  (`INVALID_HANDLE_VALUE`), nombre opcional local, tamaño positivo de hasta
  2 GiB y `PAGE_READONLY` / `PAGE_READWRITE`. No implementa secciones de
  un archivo PE/Windows existente.
- `NtCreateSection` sólo crea secciones sin fichero ni
  `OBJECT_ATTRIBUTES`, con `SEC_COMMIT` o flags cero;
  `NtMapViewOfSection` sólo mapea en el proceso actual, sin direcciones
  impuestas ni políticas avanzadas. El LASTERROR Win32 se preserva.
- Los nombres no cruzan procesos, aunque Linux sí comparte físicamente las
  páginas entre vistas de un mismo `memfd` y descendientes que las hereden.
  Un broker de objetos y el paso autenticado de FDs siguen pendientes.
- `VirtualProtect` sobre estas vistas, `MapViewOfFileEx`, nombres con
  rutas de namespace NT y el mapeo de archivos ordinarios no están en esta
  entrega. La simulación de `CreateFileMappingW` no significa NTFS completa.

## Evidencia mínima

```bash
make check
# Ejecuta: build/section-memory-test
# El test incluye resolver imports, error codes, COW, memfd compartido con
# fork, tipos VirtualQuery, referencias al cerrar y ABI Nt/Zw + Win32.

make BUILD_DIR=build-ubsan \
     CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' check
```

Las CI comprueban el entorno Linux y el empaquetado RPM Fedora. No validan
por sí solas aplicaciones desconocidas ni Fedora KDE físico. Un componente
sin implementación adecuada no se marca «finalizado» por pasar una prueba
de humo.

## Secuencia siguiente sugerida

1. Extender esta misma base a secciones de **archivo** y mapeos coherentes
   con los handles del sistema de archivos.
2. Añadir un **broker de objetos y recursos** entre procesos creados por el
   runtime (secciones/eventos/semáforos), con protocolo y derechos comunes.
3. Usar esas estructuras para la carga dinámica de DLL y estado PEB/TEB
   mientras ya existen hilos.
4. Completar el puente de señales/excepciones y el manejo de páginas
   protegidas; verificarlo con PE sintéticos.
5. Cuando estén las bases conectadas, priorizar las pruebas reales en Fedora
   y los fallos específicos de aplicaciones.

No se establece un porcentaje universal de compatibilidad a partir de esta
entrega; cada parte sigue teniendo límites verificables.
