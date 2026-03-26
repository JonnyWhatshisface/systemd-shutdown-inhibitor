Name:           system-update-inhibitor
Version:        1.0.0
Release:        1%{?dist}
Summary:        Systemd shutdown inhibitor daemon for running tasks before shutdown

License:        GPL-3.0-or-later
URL:            https://github.com/JonnyWhatshisface/systemd-shutdown-inhibitor
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  systemd-rpm-macros

%{?systemd_requires}

%description
system-update-inhibitor is a daemon that registers a delay inhibitor lock
with systemd-logind and executes a configured script or binary when the
PrepareForShutdown D-Bus signal is received. The lock is held until the
script exits, delaying the shutdown or reboot, or until the timeout of
InhibitDelayMaxSec is reached.

Typical use cases include running package manager updates, firmware updates
or any other tasks that must complete before the system powers off.

The script to execute, optional credentials and optional environment file
are all configured in /etc/system-update-inhibitor.conf.

%prep
%autosetup

%build
if [ -f daemon/inhibitor.c ]; then
	SRC_C=daemon/inhibitor.c
else
	SRC_C=inhibitor.c
fi
%{__cc} %{optflags} $(pkg-config --cflags libsystemd) \
	-o system-update-inhibitor "$SRC_C" \
	$(pkg-config --libs libsystemd)

%install
if [ -f daemon/system-update-inhibitor.service ]; then
	SERVICE_FILE=daemon/system-update-inhibitor.service
else
	SERVICE_FILE=system-update-inhibitor.service
fi
if [ -f daemon/system-update-inhibitor.conf.example ]; then
	CONF_EXAMPLE=daemon/system-update-inhibitor.conf.example
else
	CONF_EXAMPLE=system-update-inhibitor.conf.example
fi
if [ -f examples/example-dnf-update.sh ]; then
	EXAMPLE_SCRIPT=examples/example-dnf-update.sh
else
	EXAMPLE_SCRIPT=example-dnf-update.sh
fi
if [ -f daemon/system-update-inhibitor.8 ]; then
	MANPAGE_FILE=daemon/system-update-inhibitor.8
else
	MANPAGE_FILE=system-update-inhibitor.8
fi

install -Dpm0755 system-update-inhibitor \
	%{buildroot}%{_sbindir}/system-update-inhibitor
install -Dpm0644 "$SERVICE_FILE" \
	%{buildroot}%{_unitdir}/%{name}.service
install -Dpm0644 "$CONF_EXAMPLE" \
	%{buildroot}%{_sysconfdir}/system-update-inhibitor.conf
install -Dpm0755 "$EXAMPLE_SCRIPT" \
	%{buildroot}%{_docdir}/%{name}/examples/update-on-shutdown.sh
install -Dpm0644 "$MANPAGE_FILE" \
	%{buildroot}%{_mandir}/man8/system-update-inhibitor.8

%post
%systemd_post %{name}.service

%preun
%systemd_preun %{name}.service

%postun
%systemd_postun_with_restart %{name}.service

%files
%license LICENSE
%doc README.md
%dir %{_docdir}/%{name}/examples
%attr(0755,root,root) %{_docdir}/%{name}/examples/update-on-shutdown.sh
%{_sbindir}/system-update-inhibitor
%{_unitdir}/%{name}.service
%{_mandir}/man8/system-update-inhibitor.8*
%config(noreplace) %{_sysconfdir}/system-update-inhibitor.conf

%changelog
* Thu Mar 26 2026 Jonathan D. Hall <jon@jonathandavidhall.com> - 1.0.0-1
- Initial package
