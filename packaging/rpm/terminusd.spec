Name:           terminusd
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
terminusd is a daemon that registers a delay inhibitor lock
with systemd-logind and executes a configured script or binary when the
PrepareForShutdown D-Bus signal is received. The lock is held until the
script exits, delaying the shutdown or reboot, or until the timeout of
InhibitDelayMaxSec is reached.

Typical use include running package manager updates, firmware updates
or any other tasks that must complete before the system powers off.

The script to execute, optional credentials and optional environment file
are all configured in /etc/terminusd.conf.

%prep
%autosetup

%build
make CFLAGS="%{optflags}"

%install
if [ -f daemon/terminusd.service ]; then
	SERVICE_FILE=daemon/terminusd.service
else
	SERVICE_FILE=terminusd.service
fi
if [ -f daemon/etc/terminusd.conf.example ]; then
	CONF_EXAMPLE=daemon/etc/terminusd.conf.example
else
	CONF_EXAMPLE=terminusd.conf.example
fi
if [ -f opt/terminusd/scripts/example-package-updates.sh ]; then
	EXAMPLE_UPDATE_SCRIPT=opt/terminusd/scripts/example-package-updates.sh
else
	EXAMPLE_UPDATE_SCRIPT=example-package-updates.sh
fi
if [ -f opt/terminusd/scripts/example-shutdown-notify.sh ]; then
	EXAMPLE_NOTIFY_SCRIPT=opt/terminusd/scripts/example-shutdown-notify.sh
else
	EXAMPLE_NOTIFY_SCRIPT=example-shutdown-notify.sh
fi
if [ -f opt/terminusd/scripts/example-persistent-shutdown-guard.sh ]; then
	EXAMPLE_PERSIST_GUARD_SCRIPT=opt/terminusd/scripts/example-persistent-shutdown-guard.sh
else
	EXAMPLE_PERSIST_GUARD_SCRIPT=example-persistent-shutdown-guard.sh
fi
if [ -f opt/terminusd/scripts/example-oneshot-shutdown-guard.sh ]; then
	EXAMPLE_ONESHOT_GUARD_SCRIPT=opt/terminusd/scripts/example-oneshot-shutdown-guard.sh
else
	EXAMPLE_ONESHOT_GUARD_SCRIPT=example-oneshot-shutdown-guard.sh
fi
if [ -f daemon/man/terminusd.8 ]; then
	MANPAGE_FILE=daemon/man/terminusd.8
else
	MANPAGE_FILE=terminusd.8
fi
if [ -f daemon/man/terminusctl.8 ]; then
	CTLMANPAGE_FILE=daemon/man/terminusctl.8
else
	CTLMANPAGE_FILE=terminusctl.8
fi

install -Dpm0755 terminusd \
	%{buildroot}%{_sbindir}/terminusd
install -Dpm0755 terminusctl \
	%{buildroot}%{_sbindir}/terminusctl
install -Dpm0644 "$SERVICE_FILE" \
	%{buildroot}%{_unitdir}/terminusd.service
install -Dpm0644 "$CONF_EXAMPLE" \
	%{buildroot}%{_sysconfdir}/terminusd.conf
install -d %{buildroot}%{_sysconfdir}/terminus.d
install -Dpm0644 daemon/etc/terminus.d/notifyusers.conf \
	%{buildroot}%{_sysconfdir}/terminus.d/notifyusers.conf
install -Dpm0644 daemon/etc/terminus.d/applyupdates.conf \
	%{buildroot}%{_sysconfdir}/terminus.d/applyupdates.conf
install -Dpm0755 "$EXAMPLE_NOTIFY_SCRIPT" \
	%{buildroot}/opt/terminusd/scripts/example-shutdown-notify.sh
install -Dpm0755 "$EXAMPLE_UPDATE_SCRIPT" \
	%{buildroot}/opt/terminusd/scripts/example-package-updates.sh
install -Dpm0755 "$EXAMPLE_PERSIST_GUARD_SCRIPT" \
	%{buildroot}/opt/terminusd/scripts/example-persistent-shutdown-guard.sh
install -Dpm0755 "$EXAMPLE_ONESHOT_GUARD_SCRIPT" \
	%{buildroot}/opt/terminusd/scripts/example-oneshot-shutdown-guard.sh
install -Dpm0644 "$MANPAGE_FILE" \
	%{buildroot}%{_mandir}/man8/terminusd.8
install -Dpm0644 "$CTLMANPAGE_FILE" \
	%{buildroot}%{_mandir}/man8/terminusctl.8

%post
%systemd_post terminusd.service
systemctl enable terminusd.service >/dev/null 2>&1 || :

%preun
%systemd_preun terminusd.service
if [ $1 -eq 0 ]; then
	systemctl disable --now terminusd.service >/dev/null 2>&1 || :
fi

%postun
%systemd_postun_with_restart terminusd.service
if [ $1 -eq 0 ]; then
	rm -f %{_sysconfdir}/terminus.d/notifyusers.conf
	rm -f %{_sysconfdir}/terminus.d/applyupdates.conf
	rm -f /opt/terminusd/scripts/example-shutdown-notify.sh
	rm -f /opt/terminusd/scripts/example-package-updates.sh
	rm -f /opt/terminusd/scripts/example-persistent-shutdown-guard.sh
	rm -f /opt/terminusd/scripts/example-oneshot-shutdown-guard.sh
	rmdir %{_sysconfdir}/terminus.d >/dev/null 2>&1 || :
	rmdir /opt/terminusd/scripts >/dev/null 2>&1 || :
	rmdir /opt/terminusd >/dev/null 2>&1 || :
fi

%files
%license LICENSE
%doc README.md
/opt/terminusd/scripts/example-shutdown-notify.sh
/opt/terminusd/scripts/example-package-updates.sh
/opt/terminusd/scripts/example-persistent-shutdown-guard.sh
/opt/terminusd/scripts/example-oneshot-shutdown-guard.sh
%{_sbindir}/terminusd
%{_sbindir}/terminusctl
%{_unitdir}/terminusd.service
%{_mandir}/man8/terminusd.8*
%{_mandir}/man8/terminusctl.8*
%config(noreplace) %{_sysconfdir}/terminusd.conf
%config %{_sysconfdir}/terminus.d/notifyusers.conf
%config %{_sysconfdir}/terminus.d/applyupdates.conf
%dir %{_sysconfdir}/terminus.d

%changelog
* Thu Mar 26 2026 Jonathan D. Hall <jon@jonathandavidhall.com> - 1.0.0-1
- Initial package
