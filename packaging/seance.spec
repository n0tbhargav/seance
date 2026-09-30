Name:           seance
Version:        0.1.1
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
# icons: expose the bundled ones to the system icon theme via symlinks
for d in 48x48 64x64 128x128 256x256; do
  mkdir -p %{buildroot}/usr/share/icons/hicolor/$d/apps
  ln -sf /opt/seance/share/icons/hicolor/$d/apps/seance.png %{buildroot}/usr/share/icons/hicolor/$d/apps/seance.png
done
mkdir -p %{buildroot}/usr/share/icons/hicolor/scalable/apps
ln -sf /opt/seance/share/icons/hicolor/scalable/apps/seance.svg %{buildroot}/usr/share/icons/hicolor/scalable/apps/seance.svg
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
/usr/share/icons/hicolor/*/apps/seance.*

%post
gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || :
update-desktop-database -q >/dev/null 2>&1 || :

%postun
gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || :
update-desktop-database -q >/dev/null 2>&1 || :
