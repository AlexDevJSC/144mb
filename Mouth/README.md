# Mouth — red LAN para Bones ModKit 0.4.0

**Versión de integración:** Mouth 0.3.1. **Instalado en BONES/modules/mouth** como proveedor fuente enlazado de Bones ModKit ABI 1.  
**Pruebas verificadas:** [GitHub Actions Windows + Linux](https://github.com/AlexDevJSC/144mb/actions/runs/39594728594).  
**Estado:** compilación del juego de prueba oficial y de demo LAN confirmada; **aún no hay menú ni render de un segundo cliente en el ejecutable gráfico normal**.

## Instalar Mouth como módulo de Bones

El directorio ya está disponible en [BONES/modules/mouth](https://github.com/AlexDevJSC/144mb/tree/main/BONES/modules/mouth) y contiene los cuatro archivos que necesita el paquete fuente.

Con una copia de Bones 0.4.0 donde se mantienen las carpetas include, src, modules y scripts:

1. Extraer los archivos de Mouth dentro de modules/mouth, reemplazando exclusivamente su conector de prueba.
2. Abrir consola x64 Developer Command Prompt de Visual Studio.
3. Desde la carpeta BONES ejecutar scripts\build_windows_msvc.bat.
4. Obtener build\Proyecto144_Bones_ModKit.exe con Mouth enlazado.

El build oficial genera automáticamente el registro de módulos. Se ha corregido para compilar cada modkit.c con nombre de objeto distinto y enlazar ws2_32.lib, que necesita Mouth.

El paquete instalable se puede crear sin editar archivos fuente manualmente:

    python Mouth/scripts/sync_bones_module.py --check
    python Mouth/scripts/sync_bones_module.py --pack

La segunda orden genera Mouth/build/MOUTH_Bones_ModKit_0_4.zip (extraerlo DENTRO de BONES). Para reconstruir el módulo fuente desde Mouth: usar --apply y revisar cambios con Git.

## Multijugador LAN experimental

El host real de Mouth recibe entradas desde un cliente, las valida y llama BonesModServices.submit_move. Bones Foundation calcula la física. El cliente recibe posiciones confirmadas en paquetes UDP. Sin configuración LAN, el módulo queda registrado pero inactivo; por tanto no rompe la ejecución individual.

**El ejecutable normal de Bones no abre ninguna sesión por sí solo.** Para habilitar el modo host, la aplicación debe crear un segundo actor owner=2 y llamar a mouth_lan_configure_host ANTES de registrar los proveedores. La integración del menú «Crear/Unirse» con Skin es futura.

Demo LAN ya comprobada (loopback en Windows y Linux):

    Mouth\scripts\build_windows_msvc.bat
    Mouth\build\mouth_lan_demo.exe host 14444

En la consola del segundo PC:

    Mouth\build\mouth_lan_demo.exe join 192.168.1.10 14444

Usar la IP LAN real del host y permitir UDP 14444 únicamente en red privada. Controles: WASD, flechas izquierda/derecha, Shift y Espacio. Demo cliente: **consola**, sin segunda cámara 3D todavía. Ctrl+C cierra ambos procesos.

En Linux:

    make -C Mouth test
    make -C Mouth loopback
    make -C BONES modkit_test

## Archivos

- Mouth/include/mouth_lan.h y Mouth/src/mouth_lan.c: fuente original portable.
- Mouth/adapter/modkit.c: punto de entrada Bones, mantenido como fuente de referencia.
- BONES/modules/mouth/{modkit.c,mouth_lan.c,mouth_lan.h,README.txt}: paquete instalado.
- Mouth/scripts/sync_bones_module.py: sincronizar y empaquetar fuentes.
- Mouth/docs/INTEGRACION_BONES_0_4.txt: explicación exhaustiva de instalación, compatibilidad y pruebas.
- Mouth/tests/test_modkit_provider.c: descriptor ABI y modo offline/online.
- Mouth/tests/test_codec.c y test_loopback.py: wire format y dos procesos conectados.
- Mouth/docs/ESTADO_REAL.txt y REGISTRO_CAMBIOS.txt: estado actual y modificaciones.

## Medición real y limitaciones

GitHub Actions Windows/MSVC compiló el ejecutable oficial de pruebas Bones ModKit con Mouth de **192.512 bytes (último CI; antes 190.976)**, y la demo LAN de **189.440 bytes**. Eso **no valida el límite de 1,44 MB del juego terminado**.

No implementado: terceros/cuarto jugador, partida multijugador 3D final, lista de servidores, NAT, cifrado, autenticación criptográfica, interoperabilidad de bibliotecas Eye, persistencia concurrente, autoinstalación de mods/DLL o reconexión larga certificada. Las pruebas de red se han realizado entre dos procesos del mismo equipo (loopback), no entre ordenadores físicos.

Mouth 0.3.1 utiliza M144/1 para la demo LAN. Los ZIP históricos Mouth v0.1/v0.2 no están aún íntegramente publicados en GitHub. No se deben presentar como versiones wire compatibles.
