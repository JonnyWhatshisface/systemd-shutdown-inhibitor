Name:           terminusd
Version:        1.0.0
Release:        1%{?dist}
Summary:        Systemd shutdown inhibitor daemon for complete shutdown control

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
if [ -f systemd/terminusd.service ]; then
	SERVICE_FILE=systemd/terminusd.service
else
	SERVICE_FILE=terminusd.service
fi
if [ -f config/terminusd.conf.example ]; then
	CONF_EXAMPLE=config/terminusd.conf.example
else
	CONF_EXAMPLE=terminusd.conf.example
fi
if [ -f examples/scripts/example-package-updates.sh ]; then
	EXAMPLE_UPDATE_SCRIPT=examples/scripts/example-package-updates.sh
else
	EXAMPLE_UPDATE_SCRIPT=example-package-updates.sh
fi
if [ -f examples/scripts/example-shutdown-notify.sh ]; then
	EXAMPLE_NOTIFY_SCRIPT=examples/scripts/example-shutdown-notify.sh
else
	EXAMPLE_NOTIFY_SCRIPT=example-shutdown-notify.sh
fi
if [ -f examples/scripts/example-persistent-shutdown-guard.sh ]; then
	EXAMPLE_PERSIST_GUARD_SCRIPT=examples/scripts/example-persistent-shutdown-guard.sh
else
	EXAMPLE_PERSIST_GUARD_SCRIPT=example-persistent-shutdown-guard.sh
fi
if [ -f examples/scripts/example-oneshot-shutdown-guard.sh ]; then
	EXAMPLE_ONESHOT_GUARD_SCRIPT=examples/scripts/example-oneshot-shutdown-guard.sh
else
	EXAMPLE_ONESHOT_GUARD_SCRIPT=example-oneshot-shutdown-guard.sh
fi
if [ -f examples/scripts/example-terminate-user-sessions.sh ]; then
	EXAMPLE_TERMINATE_SESSIONS_SCRIPT=examples/scripts/example-terminate-user-sessions.sh
else
	EXAMPLE_TERMINATE_SESSIONS_SCRIPT=example-terminate-user-sessions.sh
fi
if [ -f man/terminusd.8 ]; then
	MANPAGE_FILE=man/terminusd.8
else
	MANPAGE_FILE=terminusd.8
fi
if [ -f man/terminusctl.8 ]; then
	CTLMANPAGE_FILE=man/terminusctl.8
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
install -Dpm0644 config/dropins/notifyusers.conf \
	%{buildroot}%{_sysconfdir}/terminus.d/notifyusers.conf
install -Dpm0644 config/dropins/applyupdates.conf \
	%{buildroot}%{_sysconfdir}/terminus.d/applyupdates.conf
install -Dpm0644 config/dropins/terminateusersessions.conf \
	%{buildroot}%{_sysconfdir}/terminus.d/terminateusersessions.conf
install -Dpm0755 "$EXAMPLE_NOTIFY_SCRIPT" \
	%{buildroot}%{_libexecdir}/terminusd/examples/example-shutdown-notify.sh
install -Dpm0755 "$EXAMPLE_UPDATE_SCRIPT" \
	%{buildroot}%{_libexecdir}/terminusd/examples/example-package-updates.sh
install -Dpm0755 "$EXAMPLE_PERSIST_GUARD_SCRIPT" \
	%{buildroot}%{_libexecdir}/terminusd/examples/example-persistent-shutdown-guard.sh
install -Dpm0755 "$EXAMPLE_ONESHOT_GUARD_SCRIPT" \
	%{buildroot}%{_libexecdir}/terminusd/examples/example-oneshot-shutdown-guard.sh
install -Dpm0755 "$EXAMPLE_TERMINATE_SESSIONS_SCRIPT" \
	%{buildroot}%{_libexecdir}/terminusd/examples/example-terminate-user-sessions.sh
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
	rm -f %{_sysconfdir}/terminus.d/terminateusersessions.conf
	rm -f %{_libexecdir}/terminusd/examples/example-shutdown-notify.sh
	rm -f %{_libexecdir}/terminusd/examples/example-package-updates.sh
	rm -f %{_libexecdir}/terminusd/examples/example-persistent-shutdown-guard.sh
	rm -f %{_libexecdir}/terminusd/examples/example-oneshot-shutdown-guard.sh
	rm -f %{_libexecdir}/terminusd/examples/example-terminate-user-sessions.sh
	rmdir %{_sysconfdir}/terminus.d >/dev/null 2>&1 || :
	rmdir %{_libexecdir}/terminusd/examples >/dev/null 2>&1 || :
	rmdir %{_libexecdir}/terminusd >/dev/null 2>&1 || :
fi

%files
%license LICENSE
%doc README.md
%{_libexecdir}/terminusd/examples/example-shutdown-notify.sh
%{_libexecdir}/terminusd/examples/example-package-updates.sh
%{_libexecdir}/terminusd/examples/example-persistent-shutdown-guard.sh
%{_libexecdir}/terminusd/examples/example-oneshot-shutdown-guard.sh
%{_libexecdir}/terminusd/examples/example-terminate-user-sessions.sh
%{_sbindir}/terminusd
%{_sbindir}/terminusctl
%{_unitdir}/terminusd.service
%{_mandir}/man8/terminusd.8*
%{_mandir}/man8/terminusctl.8*
%dir %{_libexecdir}/terminusd
%dir %{_libexecdir}/terminusd/examples
%config(noreplace) %{_sysconfdir}/terminusd.conf
%config %{_sysconfdir}/terminus.d/notifyusers.conf
%config %{_sysconfdir}/terminus.d/applyupdates.conf
%config %{_sysconfdir}/terminus.d/terminateusersessions.conf
%dir %{_sysconfdir}/terminus.d

%changelog
* Thu Mar 26 2026 Jonathan D. Hall <jon@jonathandavidhall.com> - 1.0.0-1
- Initial package
