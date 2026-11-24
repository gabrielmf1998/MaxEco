# Built by packaging/build.sh (rpmbuild -bb, Source0 = the release tarball).
# Release has no %%{?dist}, so one rpm serves Fedora, openSUSE and the rest.
%global debug_package %{nil}

Name:           maxeco
Version:        1.2.0
Release:        1
Summary:        Tray slider with three power levels (Normal, Eco, MaxEco) for the CPU, GPUs and fans
License:        MIT
URL:            https://github.com/gabrielmf1998/MaxEco
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64
BuildRequires:  gcc
BuildRequires:  make
Requires:       glibc >= 2.34
Requires:       systemd
Requires:       polkit

%description
A three step slider in the system tray. Normal runs everything as usual, Eco
cuts the power of the CPU and the graphics cards while games stay playable,
and MaxEco puts them at the lowest power the hardware allows. Going back to
Normal puts everything back exactly as it was.

The tray program is freestanding C (about 64 KB of memory, no CPU while
idle). A small root helper, started through systemd and allowed by a polkit
rule, changes the CPU frequency limits, the AMD and NVIDIA (through NVML)
graphics card limits and the Nuvoton motherboard fan curves.

%prep
%setup -q

%build
make

%check
tests/t_helper.sh build/maxeco-helper > /dev/null

%install
make install DESTDIR=%{buildroot} PREFIX=/usr
strip --strip-all %{buildroot}/usr/lib/maxeco/maxeco-helper
# %%license below puts the same file in the same place
rm -f %{buildroot}/usr/share/licenses/maxeco/LICENSE

%post
systemctl daemon-reload >/dev/null 2>&1 || :

%preun
# removal (not an upgrade): back to Normal while the helper still exists
if [ "$1" -eq 0 ]; then
    systemctl stop maxeco.service >/dev/null 2>&1 || :
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || :

%files
%license LICENSE
%doc README.md README.pt-BR.md
/usr/bin/maxeco
%dir /usr/lib/maxeco
/usr/lib/maxeco/maxeco-helper
/usr/lib/systemd/system/maxeco.service
/usr/lib/systemd/system/maxeco-level@.service
/usr/lib/systemd/system/maxeco-resume.service
/usr/lib/systemd/system/suspend.target.wants/maxeco-resume.service
/usr/lib/systemd/system/hibernate.target.wants/maxeco-resume.service
/usr/lib/systemd/system/hybrid-sleep.target.wants/maxeco-resume.service
/usr/lib/systemd/system/suspend-then-hibernate.target.wants/maxeco-resume.service
/usr/share/polkit-1/rules.d/50-maxeco.rules
/usr/share/applications/maxeco.desktop
/usr/share/icons/hicolor/scalable/apps/maxeco-on.svg
/usr/share/icons/hicolor/scalable/apps/maxeco-eco.svg
/usr/share/icons/hicolor/scalable/apps/maxeco-off.svg

%changelog
* Mon Nov 23 2026 gabrielmf1998 <110578985+gabrielmf1998@users.noreply.github.com> - 1.2.0-1
- Normal, Eco and MaxEco levels instead of on and off, mouse wheel on the icon
- 60 Hz screens are now an option, off by default
- graphics card fans are left as they were
