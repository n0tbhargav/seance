Name:           seance
Version:        0.1.0
Release:        1
Summary:        GTK3 terminal built on the Ghostty terminal core
License:        MIT
URL:            https://github.com/ghostty-org/ghostty
AutoReqProv:    no
%define debug_package %{nil}
%define __os_install_post %{nil}
Requires:       libgtk-3-0 Mesa-libEGL1 Mesa-dri fontconfig
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
Seance is a GTK3 terminal for SLES 15 SP4+ built on the Ghostty terminal core (libghostty),
rendered in software (Mesa llvmpipe) so no GPU is required.

%install
mkdir -p %{buildroot}/opt/seance %{buildroot}/usr/bin
cp -a %{_sourcedir}/opt/seance/. %{buildroot}/opt/seance/
ln -sf /opt/seance/bin/seance %{buildroot}/usr/bin/seance
ln -sf /opt/seance/bin/seancectl %{buildroot}/usr/bin/seancectl
mkdir -p %{buildroot}/usr/share/applications
cp %{_sourcedir}/usr/share/applications/seance.desktop %{buildroot}/usr/share/applications/
# normalize modes (stage tree may come from a bind mount with odd perms)
chmod 0755 %{buildroot}/opt/seance/bin/seance %{buildroot}/opt/seance/lib/libghostty.so
find %{buildroot}/opt/seance/share -type d -exec chmod 0755 {} +
find %{buildroot}/opt/seance/share -type f -exec chmod 0644 {} +

%files
%defattr(-,root,root,-)
/opt/seance
/usr/bin/seance
/usr/bin/seancectl
/usr/share/applications/seance.desktop
