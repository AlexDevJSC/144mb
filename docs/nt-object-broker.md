# Broker NT de objetos entre procesos — base v1

El broker es un servicio Linux por usuario para **objetos NT con nombre** usados por varios procesos PE. No sustituye al gestor local de handles: los objetos anónimos y los objetos sin `NT_NATIVE_BROKER_SOCKET` siguen usando las tablas del runtime.

## Componentes

- `src/tools/nt_object_broker.c`: servidor de socket `AF_UNIX/SOCK_SEQPACKET` en carpeta privada. Autentica UID de los clientes mediante `SO_PEERCRED`, asigna nombre/tipo/estado y guarda una referencia por conexión viva.
- `src/runtime/object_broker.c`: cliente IPC, verificación de peer, recepción `SCM_RIGHTS`, comprobación de tamaño y sellos del `memfd`; conserva la conexión como lease mientras exista el objeto.
- `src/runtime/section_memory.c`: `Create/OpenFileMappingA/W` comparten el mismo `memfd` entre procesos independientes; las vistas siguen utilizando `mmap(MAP_SHARED)`, con memoria COW para `FILE_MAP_COPY`.
- `src/runtime/threads.c`: `Create/OpenEventW`, `Create/OpenSemaphoreW`, `Set/ResetEvent`, `ReleaseSemaphore` y `WaitForSingle/MultipleObjects` utilizan estado compartido con mutex robusto `PTHREAD_PROCESS_SHARED`. Las esperas de otro proceso se comprueban cada 10 ms en lugar de depender de la condición local.
- `src/tools/pe_isolated_run.c`: introduce en el sandbox **únicamente el socket autorizado**, no la carpeta anfitriona; la variable `NT_NATIVE_BROKER_SOCKET` apunta a la ruta visible en el sandbox.

## Inicio manual de la base

Desde la raíz del repositorio:

```bash
make
mkdir -p "$HOME/.local/state/nt-native-linux/broker"
chmod 0700 "$HOME/.local/state/nt-native-linux/broker"

export NT_NATIVE_BROKER_SOCKET="$HOME/.local/state/nt-native-linux/broker/objects.sock"
build/nt-object-broker --socket "$NT_NATIVE_BROKER_SOCKET"
```

En otra terminal del mismo usuario se exporta la **misma** ruta `NT_NATIVE_BROKER_SOCKET` antes de arrancar clientes con `pe-isolated-run`. Para programas PE independientes, ambos lanzamientos deben poder conectarse al mismo servidor: ejecutarlos con brokers diferentes crea espacios de nombres separados.

La variable configura el servicio a nivel del runtime y **no debe ser especificada programa a programa** cuando exista una sesión iniciada con esa variable. Esta primera base no instala aún un servicio automático de sesión ni inicia el broker de forma transparente; tampoco se ofrece reintento de conexión hacia otro broker cuando uno configurado se cae.

## Pruebas

```bash
make check-object-broker
make check
```

`tests/object_broker_smoke.sh` crea una carpeta temporal privada, inicia el servidor y ejecuta `tests/object_broker_test.c`. La prueba utiliza `fork + exec` para generar procesos Linux independientes, sin heredar los handles del test principal. Comprueba escritura de memoria compartida, señalización y consumo de eventos/semáforos, WaitAll, colisiones de nombres/tipos, múltiples clientes concurrentes, último cierre y limpieza de lease tras muerte del cliente. Verifica además que un socket configurado pero desaparecido no cambia silenciosamente a objetos locales.

## Corrección de recursos del cliente — 19-11-2026

La recepción `recvmsg(MSG_CMSG_CLOEXEC)` puede instalar descriptores
`SCM_RIGHTS` **antes** de que el cliente valide el encabezado y el
contenido de la respuesta. El cliente ahora recorre los mensajes auxiliares
devueltos por el kernel y cierra todos esos FDs ante cualquier error de
longitud, tipo, versión, truncamiento o derechos; el cierre por lease en
la ruta de éxito no cambia. La prueba
`make check-object-broker-malformed` crea un servidor SOCK_SEQPACKET
mismo UID que inyecta respuestas con tipo erróneo, payload corto y
`MSG_CTRUNC` con tres FDs, y compara el número de descriptores de
`/proc/self/fd` después de cada petición. Este target se incorpora a
`make check`.

Esta corrección no implementa herencia general de HANDLE, broker global,
duplicación remota, ACL Windows ni aislamiento entre procesos maliciosos
del mismo UID. **Componente 4: 96 % → 96 % (+0 puntos)**; corrige
un recurso de IPC sin ampliar los contratos Win32 soportados.

## Contrato autenticado por mensaje y derechos exactos — 19-11-2026

El broker de objetos NT **ya no acepta cualquier mensaje recibido a
través de un socket que alguna vez se abrió bajo el mismo UID**. El
servidor habilita `SO_PASSCRED` en el listener y en cada conexión
aceptada, y exige un único `SCM_CREDENTIALS` auténtico en cada
`SOCK_SEQPACKET`. PID, UID y GID efectivos del emisor deben coincidir
con `SO_PEERCRED` de la conexión original. Paquetes truncados, mensajes
auxiliares desconocidos y transferencias `SCM_RIGHTS` en peticiones se
rechazan, cerrando cualquier FD recibido. Esto impide que un proceso
hijo que recibió por `fork` un socket del padre herede su autoridad
para crear/abrir objetos NT sin nueva autenticación.

El broker también devuelve exactamente `req.rights`, no el máximo
`obj.rights` del creador, cuando la comprobación ya permitió la
apertura. El cliente rechaza una respuesta que añada derechos. La
prueba de integración del broker comprueba explícitamente una apertura
reducida a derechos 1 y un socket conectado por el padre cuyo único
paquete se envía desde un hijo distinto.

**Esto NO crea todavía tokens NT autenticados ni una DACL NT aplicada
a objetos**: autentica identidad Linux por sesión y por mensaje, y
aplica exclusivamente la máscara de acceso existente; varios programas
PE legítimos del mismo UID comparten un ámbito de confianza. No
aceptar descriptores de seguridad suministrados por el invitado como
autoridad sin un emisor confiable y un contrato NT completo. Tampoco
habilita duplicación entre PID ni acceso a recursos de otros UID.
Madurez componente 4: **96 % → 96 % (+0 puntos)** sin aceptación.

## Alcance y límites

Esta es la base operativa del **broker por usuario**, no un Object Manager Windows completo:

- Soporta secciones `memfd` sin archivo Windows y eventos/semáforos nombrados. No hay secciones de archivos ni mutex NT remoto todavía.
- Nombres de eventos/semáforos en el perfil ASCII local existente y secciones UTF-16 convertidas a UTF-8; no hay namespace `Global\`, directorios NT anidados, sesiones Windows ni ACL/SD Windows completas.
- Los handles siguen perteneciendo a cada proceso. `NtDuplicateObject` no duplica directamente un HANDLE entre procesos PE ajenos; cada proceso hace su propio `Open*` por nombre y recibe un descriptor del broker.
- El bloqueo evita pérdidas en modificación/consumo de estado entre procesos de este broker; la latencia aproximada de despertar por polling puede alcanzar 10 ms. No se promete latencia idéntica a Windows.
- El acceso a socket está reservado al usuario propietario; aplicaciones arbitrarias ejecutadas bajo el mismo UID comparten el ámbito básico de confianza. No abrir al resto de usuarios.
- Las pruebas de CI fuera de Bubblewrap no certifican por sí solas el sandbox de Fedora físico ni aplicaciones de terceros.

**Criterio de uso:** la base será apta para prototipo cuando se construya, pase las pruebas entre procesos y el RPM Fedora integre servidor y cliente. Las limitaciones documentadas son tareas posteriores y no obligan a adaptar cada aplicación.
