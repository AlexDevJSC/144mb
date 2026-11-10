Name:           nt-native-linux
Version:        0.1.0
Release:        1%{?dist}
Summary:        Experimental NT timing probe for Linux
License:        MIT
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  binutils
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(libffi)
BuildRequires:  dejavu-sans-fonts

%description
An experimental implementation of the NtQueryPerformanceCounter timing
contract using Linux CLOCK_BOOTTIME, packaged as a diagnostic command.

%prep
%autosetup

%build
%set_build_flags
%make_build

%install
%make_install PREFIX=%{_prefix} BINDIR=%{_bindir}

%check
%make_build check

%files
%license LICENSE
%doc README.md docs/architecture.md docs/validation.md
%{_bindir}/nt-native-probe

%changelog
* Mon Nov 09 2026 nt-native-linux contributors - 0.1.0-1
- Package the experimental native counter probe.
