# RPM experimental para Fedora KDE Plasma 45

## Instalación para tu usuario

Desde una copia del repositorio que incluya el instalador, ejecuta:

```sh
bash packaging/fedora/install.sh "$(git rev-parse HEAD)"
```

Usa un commit publicado del proyecto. El instalador descarga el repositorio
completo en `~/.local/share/nt-native-linux/source`, instala con DNF las
dependencias que falten y ejecuta `make all check check-pe-image` antes de
activar la versión. Se puede repetir con el mismo commit. La instalación
se activa en `~/.local/bin` con `pe-isolated-run`, `pe-import-audit`,
`nt-native-probe` y `nt-vulkan-probe`. La compilación y el lanzador se
ejecutan como usuario. El instalador verifica el SHA-256 del DXC nativo y
guarda sus bibliotecas y licencias en la versión instalada del runtime.

Ejecuta el instalador una vez por versión del runtime. Después, en una
sesión KDE Wayland, abre cada EXE compatible con el mismo lanzador.
El segundo argumento del instalador sigue disponible para abrir un EXE
al terminar la instalación.

```sh
"$HOME/.local/bin/pe-isolated-run" /ruta/al/programa.exe
```

Para dar al programa acceso de lectura y escritura a tus Documentos de
Linux, usa la ruta que configuraste en el escritorio.

```sh
"$HOME/.local/bin/pe-isolated-run" \
  --share "Documents=$(xdg-user-dir DOCUMENTS)" /ruta/al/programa.exe
```

Puedes usar la ruta absoluta si tu escritorio carece de `xdg-user-dir`.
El programa accede a esos mismos archivos mediante `C:\Users\guest\Documents`.
Consulta las [carpetas disponibles](../../docs/isolated-loader.md#carpetas-de-linux-compartidas).

Las aplicaciones comparten un disco C: persistente del usuario en
`$XDG_STATE_HOME/nt-native-linux/drive-c`, con
`$HOME/.local/state/nt-native-linux/drive-c` como ubicación predeterminada.
El lanzador proporciona las rutas de perfil y datos de aplicación para
guardar y recuperar preferencias entre sesiones.

## Construcción del RPM

El RPM instala el lanzador, el auditor y los comandos de diagnóstico. El
lanzador ejecuta PE externos mediante Bubblewrap. El spec limita el paquete
a x86-64 y ejecuta las pruebas nativas en `%check`. Para compilar DXIL con el
RPM, proporciona `libdxcompiler.so` de Linux y su biblioteca `libdxil.so`.

Desde un checkout Git, dentro de una VM desechable o un entorno de build Fedora:

```sh
sudo dnf install gcc binutils make redhat-rpm-config rpm-build git tar gzip
rpm_topdir="$PWD/build-rpm"
mkdir -p "$rpm_topdir/SOURCES"
git archive --format=tar.gz --prefix=nt-native-linux-0.1.0/ HEAD \
  > "$rpm_topdir/SOURCES/nt-native-linux-0.1.0.tar.gz"
rpmbuild --define "_topdir $rpm_topdir" -ba packaging/fedora/nt-native-linux.spec
rpm -qpl "$rpm_topdir"/RPMS/x86_64/nt-native-linux-0.1.0-*.rpm
```

El build usa el commit `HEAD`. Confirma el árbol que quieres empaquetar antes
de crear el archivo fuente. Para reproducir el entorno de Fedora, genera el
SRPM con el mismo spec y usa Mock con `-r fedora-45-x86_64`. La verificación
de instalación se hará en una VM con un RPM firmado y una clave aprobada para
esa VM.

Para probar el destino de instalación sin privilegios ni RPM:

```sh
make install DESTDIR="$PWD/build-stage" PREFIX=/usr
./build-stage/usr/bin/nt-native-probe
./build-stage/usr/bin/pe-isolated-run --help
```

La carga PE y el aislamiento siguen siendo experimentales. Se verificaron
una victoria y una derrota con Mines en Linux fuera de pantalla mediante
un arnés aislado. El usuario ha confirmado ambos finales en un portátil
Fedora KDE Wayland y ha comunicado fallos de menús. Las correcciones de
diálogos y menús tienen pruebas genéricas y comprobaciones adicionales del
Mines original en Linux fuera de pantalla. La revisión actual requiere
repetir los menús en el portátil. [Contrato del cargador](../../docs/isolated-loader.md).

La integración del escritorio tiene su propio hito. El paquete de laboratorio
puede instalarse sin registrar manejadores MIME, servicios, reglas de
`binfmt_misc` ni módulos de kernel. El paso a una imagen Fedora requiere
validación de VM y un kickstart mantenible.

Fuentes [Packaging Guidelines](https://docs.fedoraproject.org/en-US/packaging-guidelines/) ·
[Mock](https://rpm-software-management.github.io/mock/) ·
[Firma de RPM en Fedora 45](https://fedoraproject.org/wiki/Changes/Enforcing_signature_checking_by_default) ·
[Fedora Kickstarts](https://pagure.io/fedora-kickstarts/) ·
[Lorax](https://weldr.io/lorax/livemedia-creator.html) ·
[Fedora Remix](https://fedoraproject.org/wiki/Remix).
