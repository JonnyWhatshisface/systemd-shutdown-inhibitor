CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags libsystemd)
LDFLAGS = $(shell pkg-config --libs libsystemd)

TARGET   = terminusd
CTL      = terminusctl
SRCDIR   = daemon
SRC      = \
	$(SRCDIR)/inhibitor.c \
	$(SRCDIR)/utils.c \
	$(SRCDIR)/config.c \
	$(SRCDIR)/scripts.c \
	$(SRCDIR)/logind.c \
	$(SRCDIR)/test.c \
	$(SRCDIR)/guard.c \
	$(SRCDIR)/control.c
TEST_SRC = \
	$(SRCDIR)/utils.c \
	$(SRCDIR)/config.c \
	$(SRCDIR)/scripts.c \
	$(SRCDIR)/logind.c \
	$(SRCDIR)/test.c \
	$(SRCDIR)/guard.c
TESTDIR  = tests
TEST_BIN = $(TESTDIR)/test_inhibitor_config
TEST_MODE_BIN = $(TESTDIR)/test_inhibitor_test_mode
TEST_TERMINUSCTL_BIN = $(TESTDIR)/test_terminusctl_cli

PREFIX      = /usr
SBINDIR     = $(PREFIX)/sbin
SERVICEDIR  = /usr/lib/systemd/system
MANDIR      = $(PREFIX)/share/man
MAN8DIR     = $(MANDIR)/man8
CONFDIR     = /etc
CONFFILE    = terminusd.conf
DROPINDIR   = /etc/terminus.d
DROPIN_CONFIGS = \
	notifyusers.conf \
	applyupdates.conf
SERVICE_SRC = $(SRCDIR)/terminusd.service
SERVICENAME = terminusd.service
CONFEXAMPLE_SRC = $(SRCDIR)/etc/$(CONFFILE).example
MANPAGE_SRC = $(SRCDIR)/man/terminusd.8
MANPAGE_DST = terminusd.8
CTLMANPAGE_SRC = $(SRCDIR)/man/terminusctl.8
CTLMANPAGE_DST = terminusctl.8
EXAMPLESDIR = /opt/terminusd/scripts
EXAMPLE_SCRIPTS = \
	example-persistent-shutdown-guard.sh \
	example-oneshot-shutdown-guard.sh \
	example-package-updates.sh \
	example-shutdown-notify.sh

.PHONY: all clean install uninstall test test-build test-config test-test-mode test-terminusctl

all: $(TARGET)

all: $(CTL)

$(TARGET): $(SRC) $(SRCDIR)/inhibitor.h $(SRCDIR)/test.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(CTL): $(SRCDIR)/terminusctl.c $(SRCDIR)/inhibitor.h
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/terminusctl.c $(LDFLAGS)

test-build: $(TARGET) $(CTL) $(TEST_BIN) $(TEST_MODE_BIN) $(TEST_TERMINUSCTL_BIN)

$(TEST_BIN): $(TESTDIR)/test_inhibitor_config.c $(TEST_SRC) $(SRCDIR)/inhibitor.h $(SRCDIR)/test.h
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_inhibitor_config.c $(TEST_SRC) $(LDFLAGS)

$(TEST_MODE_BIN): $(TESTDIR)/test_inhibitor_test_mode.c $(TARGET)
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_inhibitor_test_mode.c $(LDFLAGS)

$(TEST_TERMINUSCTL_BIN): $(TESTDIR)/test_terminusctl_cli.c $(SRCDIR)/terminusctl.c $(SRCDIR)/inhibitor.h
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_terminusctl_cli.c $(LDFLAGS)

test: test-build
	./$(TEST_BIN)
	./$(TEST_MODE_BIN)
	./$(TEST_TERMINUSCTL_BIN)
	@echo "[PASS] All test suites passed"

test-config: test-build
	./$(TEST_BIN)

test-test-mode: test-build
	./$(TEST_MODE_BIN)

test-terminusctl: test-build
	./$(TEST_TERMINUSCTL_BIN)

install: $(TARGET) $(CTL)
	install -d $(DESTDIR)$(SBINDIR)
	install -d $(DESTDIR)$(SERVICEDIR)
	install -d $(DESTDIR)$(MAN8DIR)
	install -d $(DESTDIR)$(CONFDIR)
	install -d $(DESTDIR)$(DROPINDIR)
	install -d $(DESTDIR)$(EXAMPLESDIR)
	install -Dm755 $(TARGET) $(DESTDIR)$(SBINDIR)/$(TARGET)
	install -Dm755 $(CTL) $(DESTDIR)$(SBINDIR)/$(CTL)
	install -Dm644 $(SERVICE_SRC) \
		$(DESTDIR)$(SERVICEDIR)/$(SERVICENAME)
	install -Dm644 $(MANPAGE_SRC) \
		$(DESTDIR)$(MAN8DIR)/$(MANPAGE_DST)
	install -Dm644 $(CTLMANPAGE_SRC) \
		$(DESTDIR)$(MAN8DIR)/$(CTLMANPAGE_DST)
	@if [ ! -f $(DESTDIR)$(CONFDIR)/$(CONFFILE) ]; then \
		install -Dm644 $(CONFEXAMPLE_SRC) \
			$(DESTDIR)$(CONFDIR)/$(CONFFILE); \
		echo "Installed default config to $(DESTDIR)$(CONFDIR)/$(CONFFILE)"; \
	else \
		echo "Config $(DESTDIR)$(CONFDIR)/$(CONFFILE) already exists, not overwriting"; \
	fi
	@for d in $(DROPIN_CONFIGS); do \
		install -Dm644 $(SRCDIR)/etc/terminus.d/$$d $(DESTDIR)$(DROPINDIR)/$$d; \
		echo "Installed drop-in config to $(DESTDIR)$(DROPINDIR)/$$d"; \
	done
	@for s in $(EXAMPLE_SCRIPTS); do \
		if [ ! -f $(DESTDIR)$(EXAMPLESDIR)/$$s ]; then \
			install -Dm755 opt/terminusd/scripts/$$s $(DESTDIR)$(EXAMPLESDIR)/$$s; \
			echo "Installed example script to $(DESTDIR)$(EXAMPLESDIR)/$$s"; \
		else \
			echo "Script $(DESTDIR)$(EXAMPLESDIR)/$$s already exists, not overwriting"; \
		fi; \
	done
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		echo "Attempting to enable $(SERVICENAME)"; \
		systemctl daemon-reload >/dev/null 2>&1 || true; \
		systemctl enable $(SERVICENAME) >/dev/null 2>&1 || true; \
	fi

uninstall:
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		echo "Attempting to stop/disable $(SERVICENAME)"; \
		systemctl disable --now $(SERVICENAME) >/dev/null 2>&1 || true; \
	fi
	rm -f $(DESTDIR)$(SBINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SBINDIR)/$(CTL)
	rm -f $(DESTDIR)$(SERVICEDIR)/$(SERVICENAME)
	rm -f $(DESTDIR)$(MAN8DIR)/$(MANPAGE_DST)
	rm -f $(DESTDIR)$(MAN8DIR)/$(CTLMANPAGE_DST)
	rm -f $(DESTDIR)$(CONFDIR)/$(CONFFILE)
	@for d in $(DROPIN_CONFIGS); do \
		rm -f $(DESTDIR)$(DROPINDIR)/$$d; \
	done
	@for s in $(EXAMPLE_SCRIPTS); do \
		rm -f $(DESTDIR)$(EXAMPLESDIR)/$$s; \
	done
	@rmdir $(DESTDIR)$(DROPINDIR) >/dev/null 2>&1 || true
	@rmdir $(DESTDIR)$(EXAMPLESDIR) >/dev/null 2>&1 || true
	@rmdir $(DESTDIR)/opt/terminusd >/dev/null 2>&1 || true

clean:
	rm -f $(TARGET)
	rm -f $(CTL)
	rm -f $(TEST_BIN)
	rm -f $(TEST_MODE_BIN)
	rm -f $(TEST_TERMINUSCTL_BIN)

