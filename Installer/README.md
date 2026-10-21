# Instalador de Proyecto 1.44 para Windows

**Objetivo:** distribuir la compilación verificable del juego que está realmente incorporada en la rama main, sin confundir pull requests abiertos con versiones integradas.

Este instalador utiliza el EXE nativo generado por BONES/scripts/build_windows_msvc.bat (Bones ModKit ABI 1; cinco proveedores del registro estático). Instala en el perfil local del usuario, crea acceso directo en Inicio y permite desinstalar desde Windows. Se ofrece también ZIP portable sin necesidad de instalar.

El instalador NO incorpora automáticamente los PR pendientes, NO añade DLL ni modifica el límite de tamaño del ejecutable del videojuego. La interfaz multijugador/cliente gráfico continúan sin integrarse en main; no se presenta como una versión completa del RPG.

## Descargar una compilación lista
1. Abrir GitHub Actions: https://github.com/AlexDevJSC/144mb/actions/workflows/installer-main.yml
2. Seleccionar la ejecución correcta, comprobar que la rama sea main y que "Build and test integrated installer" termine en verde.
3. En la sección Artifacts, descargar Proyecto144_Windows_Main_<número>.
4. Dentro del ZIP de GitHub Actions hay un instalador Proyecto144_Instalador_<SHA>.exe, una variante Proyecto144_Portable_<SHA>.zip, VERSION.txt y MANIFEST.json.
5. Ejecutar el instalador. Windows puede avisar que el binario no está firmado digitalmente (este proyecto experimental aún no cuenta con firma de código). Verificar que el SHA del commit y archivo concuerdan con la documentación antes de ejecutarlo.

No descargues ejecutables de terceros. El ZIP portable contiene Proyecto144.exe y VERSION.txt. Se puede extraer en cualquier carpeta con permisos de escritura y abrir sin instalador. El instalador puede ser más grande que 1,44 MB: el presupuesto del juego se comprueba exclusivamente sobre Proyecto144.exe.

## Compilar localmente
Requiere Windows, MSVC x64 Native Tools Command Prompt, Python 3 de build y Inno Setup 6. Desde la raíz del repositorio:

    BONES\scripts\build_windows_msvc.bat
    powershell -NoProfile -ExecutionPolicy Bypass -File Installer\scripts\build_installer.ps1

El segundo paso verifica que el EXE exista, sea PE/MZ, mida como máximo 1.474.560 bytes, genere el manifest de SHA256 y cree instalador y portable. No descarga plugins o paquetes ejecutables de fuentes ajenas. El flujo CI instala Inno Setup únicamente en el runner de compilación si no se encuentra.

Para comprobar el instalador en una máquina de pruebas aislada:

    powershell -NoProfile -ExecutionPolicy Bypass -File Installer\scripts\test_installer.ps1 -Installer "Installer\dist\Proyecto144_Instalador_<SHA>.exe" -GameExe "BONES\build\Proyecto144_Bones_ModKit.exe"

El test instala en un directorio temporal, verifica byte por byte mediante SHA256 y ejecuta el desinstalador. No prueba el comportamiento de juego con ventanas visibles ni la conectividad con otro ordenador.

## Política de versiones
Solo se etiqueta «main» cuando los archivos se compilan desde ese commit. Bones 0.4.1/0.4.2, Eye 0.5.2, Skin 0.4/0.5, Brain 0.3.1 y Ear 0.4 han aparecido en PRs, no implica que sus cambios estén fusionados. La selección de módulos y merges pertenece a Bones/Soul y a sus responsables. **No auto-mergear ramas experimentales desde el instalador.**

La distribución autocontenida prioritaria sigue siendo Proyecto144.exe estático. El instalador usa Inno Setup para experiencia Windows pero no se necesita para ejecutar el juego portable.
