# Mouth — multijugador P2P para Proyecto 1.44

**Versión actual publicada:** Mouth LAN v0.3.0 (prototipo para dos jugadores, UDP sobre IPv4).  
**Núcleo:** Bones Foundation v1 + Bones ModKit ABI 1.  
**Estado:** código y pruebas publicados; compilación y prueba LAN nativa **pendientes de verificar**.

## Qué hace

Un ordenador abre una sesión como **anfitrión autoritativo**. Su proceso instancia dos entidades de jugador mediante Bones Foundation; el jugador local controla la entidad 1, y el cliente remoto controla la entidad con owner=2. Mouth recibe controles UDP, elimina secuencias antiguas y utiliza BonesModServices.submit_move en el tick correcto; **las físicas se calculan en Bones**, no en Mouth.

El cliente envía controles y muestra por consola las posiciones de ambos actores confirmadas por el anfitrión. El ejemplo **todavía no dibuja la escena 3D en el cliente**: esta es una prueba vertical de red y física, no una versión jugable completa.

La sesión funciona sin servidor dedicado, sin dependencias del servicio externo ni intercambio de texturas. El límite de 1,44 MB deberá medirse con el ejecutable final del juego; el ejemplo todavía no cumple esa validación.

## Ejecutar en Windows

Descarga/clona el **repositorio completo** (necesita la carpeta hermana BONES/). Abre la consola x64 Developer Command Prompt para Visual Studio y ejecuta desde la raíz:

    Mouth\scripts\build_windows_msvc.bat

O, con MinGW-w64 GCC instalado en PATH:

    Mouth\scripts\build_windows_mingw.bat

En el PC anfitrión:

    Mouth\build\mouth_lan_demo.exe host 14444

Obtén su IPv4 LAN con ipconfig. En el PC cliente:

    Mouth\build\mouth_lan_demo.exe join 192.168.1.10 14444

Sustituye la IP de ejemplo por la IP real del PC anfitrión. Ambos ordenadores deben poder comunicarse por UDP puerto 14444; si Windows solicita permiso de cortafuegos, permitirlo **solo en red privada de confianza**. No publiques el puerto en Internet durante esta fase.

Con la consola de cada programa enfocada: **WASD** movimiento, flechas izquierda/derecha rotación, **Shift** sprint, **Espacio** salto. Se imprimen las posiciones del servidor. Ctrl+C cierra la consola.

## Compilar y probar en Linux

Desde la raíz del repositorio completo:

    make -C Mouth test
    make -C Mouth loopback
    make -C Mouth sanitize

La prueba loopback ejecuta anfitrión y cliente como dos procesos independientes sobre 127.0.0.1, comprueba la recepción de posiciones y verifica movimiento del actor remoto mediante las físicas reales de Bones. El cliente POSIX de demostración mueve al actor automáticamente para facilitar la prueba headless.

Estas órdenes son **instrucciones reproducibles**, no evidencia de una ejecución ya realizada.

## Rutas del código

- include/mouth_lan.h — contrato público LAN y codec.
- src/mouth_lan.c — socket UDP + protocolo + puente del host al ModKit.
- examples/lan_demo.c — ejecutables de consola para host y cliente.
- tests/test_codec.c, tests/test_loopback.py — regresión binaria y LAN.
- scripts/ y Makefile — compilación y tests.
- docs/PROTOCOLO_LAN_V1.txt — formatos y límites.
- docs/ESTADO_REAL.txt, docs/REGISTRO_CAMBIOS.txt — implementación real y cambios.

## Integración posterior con la aplicación Bones

El demo agrega el proveedor Mouth a ModKit con bones_mod_host_add(host, mouth_lan_query), conservando intactos los archivos del núcleo. En la aplicación principal Bones **ya existe un placeholder** llamado BONES/modules/mouth/modkit.c. Antes de incorporarlo en el registro generado oficial, Bones y Soul deberán reemplazar/delegar ese placeholder; no registrar dos proveedores del rol Mouth.

No se modifica BONES/include/bones/modkit.h ni la semántica de guardados de Skin. No existen todavía entrada de un tercer/cuarto usuario, estado persistente en red, cambios de inventario, reglas hash, sincronización visual avanzada, NAT traversal, cifrado, rollback ni migración de anfitrión. El session_id y el origen IP/puerto no son autenticación criptográfica.

## Versiones anteriores

Los paquetes originales Mouth v0.1 (red) y v0.2 (referencias visuales) se elaboraron previamente en ChatGPT, pero **sus fuentes completas aún no se han sincronizado con GitHub**. El antiguo importar_fuentes.ps1 permite importarlos como material histórico local si se descargan los ZIP. Mouth LAN v0.3 es un protocolo nuevo M144/1; **no es compatible en el cable** con los paquetes anteriores.

La validación de Windows, la medición real de bytes y la aprobación de Soul están pendientes.
