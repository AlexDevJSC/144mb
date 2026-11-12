Name:           nt-native-linux
Version:        0.1.0
Release:        1%{?dist}
Summary:        Experimental NT and PE32+ compatibility runtime for Linux
License:        MIT
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64
Requires:       bubblewrap
Requires:       fluidsynth-libs

BuildRequires:  binutils
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(SDL2_image)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(libffi)
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(libvkd3d)
BuildRequires:  pkgconfig(libvkd3d-utils)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  fluidsynth-libs
BuildRequires:  dejavu-sans-fonts

%description
Experimental NT/Win32 compatibility runtime with a timing probe and a
PE32+ loader launched through Bubblewrap. PE application support is
incomplete and must not be represented as general Windows compatibility.

%prep
%autosetup

%build
%set_build_flags
%make_build

%install
%make_install PREFIX=%{_prefix} BINDIR=%{_bindir}

%check
%make_build check check-pe-image

%files
%license LICENSE
%doc README.md docs/architecture.md docs/validation.md docs/isolated-loader.md docs/vulkan-d3d12.md docs/squadron-compatibility.md docs/kernel-user.md
%{_bindir}/nt-native-probe
%{_bindir}/pe-isolated-run
%{_bindir}/nt-vulkan-probe
%{_bindir}/pe-import-audit

%changelog
* Mon Nov 09 2026 nt-native-linux contributors - 0.1.0-1
- Package the experimental native counter probe.
