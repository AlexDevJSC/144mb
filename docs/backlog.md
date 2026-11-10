# Backlog

La prioridad P0 cubre la publicación y validación inicial y la primera
importación real. Las tareas posteriores dependen del resultado de P0.

| ID | Prioridad | Tarea | Criterio verificable |
| --- | --- | --- | --- |
| PUB-01 | P0 | Publicar la base en el repositorio privado autorizado. | Verificar el nombre y el propietario, habilitar Issues, cargar la base y obtener el resultado de los jobs de CI en Linux. Crear la incidencia P0-01 sin fusionar el trabajo posterior. |
| P0-01 | P0 | Resolver una importación PE AMD64 real a nuestro backend. | Crear un PE de consola con MinGW-w64 que importe `ntdll.dll!NtQueryPerformanceCounter`. Resolver esa IAT por nombre con la función propia y ejecutar el fixture controlado en Linux. |
| P0-02 | P0 | Construir y validar el RPM en Fedora KDE 45. | Compilar en Mock, registrar el payload, ejecutar `%check` y validar instalación, sonda y desinstalación en VM. |
| P1-01 | P1 | Frontera de memoria invitada y fallos. | Validar en Linux los contratos documentados para páginas no accesibles, salidas que cruzan página, orden de escrituras y aliasing. Añadir guardas y pruebas aisladas. |
| P1-02 | P1 | Eventos NT y espera múltiple. | Validar manual y auto-reset, timeout sin consumir objetos en espera múltiple, cierre concurrente y ausencia de `/dev/ntsync`. Medir ambos backends con la misma carga. |
| P1-03 | P1 | Handles y ciclo de vida del proceso. | Implementar derechos, herencia, duplicación, cierre y terminación. Probar carreras y reutilización de identificadores. |
| P1-04 | P1 | Segunda función de reloj y primeros contratos de archivos. | Probar `NtQuerySystemTime` y especificar un subconjunto `NtCreateFile` con una tabla de nombres, modos y errores observados. |
| P2-01 | P2 | Lanzador de KDE y backend provisional. | Asociar PE en VM, mantener el camino ELF normal, crear estado por aplicación y mostrar qué backend ejecutó cada programa. Probar apertura, instalación y desinstalación con fixtures. |
| P2-02 | P2 | Primera ventana y presentación AMD. | Demostrar ventana Win32 con foco y escala en Wayland. Verificar prueba D3D11 con DXVK y una prueba D3D12 con VKD3D-Proton, con versiones y extensiones registradas. |
| P3-01 | P3 | Imagen Fedora KDE reproducible. | Compilar con kickstart y Lorax, verificar arranque en VM, actualizar y desinstalar los componentes mediante RPM. |

## Encargo para Codex P0-01

Crea la rama `feature/pe-ntdll-import` desde la base revisada. Trabaja
en `tests/pe/` y en un módulo acotado de carga. Conserva el fixture
actual como referencia ABI. Usa MinGW-w64 para un PE estándar y registra
la tabla de importación con `objdump -p`. El binario debe comunicar un
resultado comprobable en el anfitrión de prueba Linux.

En Linux, valida límites de los RVA, tabla de descriptores, nombres
terminados en NUL y entradas IAT. Define un perfil con una importación
admitida y rechaza ordinales, importaciones retardadas, DLL desconocidas
y tablas fuera de rango. Conserva W^X y entrada limitada a fixtures.
Incluye pruebas de importación inválida y resultado del contador.
Ejecuta `make check check-pe` y repite las pruebas con UBSan en Linux.
Adjunta al PR los resultados, el análisis de seguridad y las referencias
del formato. Deja la ejecución arbitraria, TLS y relocaciones generales
para tareas separadas.
