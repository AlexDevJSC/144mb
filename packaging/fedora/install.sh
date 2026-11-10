#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

usage() {
    printf 'Uso: bash install.sh COMMIT [ruta/a/mines.exe]\n'
    printf 'Descarga el repositorio, prueba e instala el runtime en tu usuario.\n'
}

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

if [[ ${1-} == --help || ${1-} == -h ]]; then
    usage
    exit 0
fi
if (( $# < 1 || $# > 2 )) || [[ ! ${1-} =~ ^[0-9a-f]{40}$ ]]; then
    usage >&2
    exit 2
fi

revision=$1
game=
if (( $# == 2 )); then
    [[ -f $2 && -r $2 ]] || fail "No puedo leer el ejecutable: $2"
    game=$(realpath -e -- "$2")
fi
(( EUID != 0 )) || fail 'Ejecuta el instalador con tu usuario habitual.'
. /etc/os-release
[[ ${ID-} == fedora && $(uname -m) == x86_64 ]] ||
    fail 'Este instalador requiere Fedora x86-64.'

dependencies=(git gcc binutils make pkgconf-pkg-config bubblewrap dejavu-sans-fonts
              /usr/bin/flock 'pkgconfig(sdl2)' 'pkgconfig(cairo)' 'pkgconfig(libffi)')
missing=()
for dependency in "${dependencies[@]}"; do
    if ! rpm -q --whatprovides "$dependency" >/dev/null 2>&1; then
        missing+=("$dependency")
    fi
done
if (( ${#missing[@]} > 0 )); then
    sudo dnf -y install "${missing[@]}"
fi

origin=https://github.com/AlexDevJSC/144mb.git
root="$HOME/.local/share/nt-native-linux"
bin="$HOME/.local/bin"
checkout="$root/source"
version="$root/versions/$revision"
commands=(nt-native-probe pe-isolated-run)

(
    mkdir -p -- "$root" "$bin"
    exec 9>"$root/install.lock"
    flock 9
    work=$(mktemp -d "$root/.install.XXXXXX")
    trap 'rm -rf -- "$work"' EXIT

    for name in "${commands[@]}"; do
        path="$bin/$name"
        if [[ -e $path || -L $path ]]; then
            [[ -L $path && $(readlink -- "$path") == "$root/current/bin/$name" ]] ||
                fail "Ya existe $path y pertenece a otra instalación."
        fi
    done

    source="$checkout"
    if [[ -e $checkout || -L $checkout ]]; then
        [[ ! -L $checkout && -d $checkout/.git &&
           $(git -C "$checkout" config --local --get remote.origin.url) == "$origin" ]] ||
            fail "La carpeta $checkout ya está ocupada."
        [[ -z $(git -C "$checkout" status --porcelain) ]] ||
            fail "Hay cambios locales en $checkout. Guárdalos antes de reinstalar."
    else
        source="$work/source"
        git clone --no-checkout "$origin" "$source"
    fi

    if ! git -C "$source" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$source" fetch origin "$revision"
    fi
    git -C "$source" switch --detach "$revision"
    if [[ $source != "$checkout" ]]; then
        mv -T -- "$source" "$checkout"
    fi

    candidate="$version"
    if [[ ! -e $version && ! -L $version ]]; then
        build="build-fedora-$revision"
        make -C "$checkout" -j"$(nproc)" BUILD_DIR="$build" all check check-pe-image
        candidate="$work/payload"
        make -C "$checkout" BUILD_DIR="$build" DESTDIR="$candidate" PREFIX=/ BINDIR=/bin install
    fi
    "$candidate/bin/nt-native-probe"
    "$candidate/bin/pe-isolated-run" --help >/dev/null 2>&1
    if [[ $candidate != "$version" ]]; then
        mkdir -p -- "$root/versions"
        mv -T -- "$candidate" "$version"
    fi

    for name in "${commands[@]}"; do
        path="$bin/$name"
        if [[ ! -L $path ]]; then
            ln -sT -- "$root/current/bin/$name" "$path"
        fi
    done
    ln -s -- "$version" "$work/current"
    mv -Tf -- "$work/current" "$root/current"
)

printf 'Instalación lista (%s).\n' "$revision"
printf 'Código fuente en %s\n' "$checkout"
printf 'Lanzador en %s\n' "$bin/pe-isolated-run"

if [[ -n $game ]]; then
    exec "$version/bin/pe-isolated-run" "$game"
fi
