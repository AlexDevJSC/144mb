# RPM experimental para Fedora KDE Plasma 45

El paquete instala los comandos ELF `nt-native-probe` y `pe-isolated-run` y documentación. Para ejecutar PE ajenos, el segundo requiere Bubblewrap y no permite omitir el sandbox. El
comando permite verificar el backend del contador en una VM Fedora antes de
integrar un lanzador. El spec limita el paquete a x86-64 y ejecuta las pruebas
nativas en `%check`.

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

La carga PE y el aislamiento siguen siendo experimentales; véase [el contrato del cargador](../../docs/isolated-loader.md). El soporte de Mines todavía no está verificado.

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
