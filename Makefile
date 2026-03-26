CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags libsystemd)
LDFLAGS = $(shell pkg-config --libs libsystemd)

TARGET   = system-update-inhibitor
SRCDIR   = daemon
SRC      = $(SRCDIR)/inhibitor.c
TESTDIR  = tests
TEST_BIN = $(TESTDIR)/test_inhibitor_config

PREFIX      = /usr
SBINDIR     = $(PREFIX)/sbin
LOCALBINDIR = /usr/local/sbin
SERVICEDIR  = /usr/lib/systemd/system
MANDIR      = $(PREFIX)/share/man
MAN8DIR     = $(MANDIR)/man8
CONFDIR     = /etc
CONFFILE    = system-update-inhibitor.conf
SERVICE_SRC = $(SRCDIR)/system-update-inhibitor.service
SERVICENAME = system-update-inhibitor.service
MANPAGE_SRC = $(SRCDIR)/system-update-inhibitor.8
MANPAGE_DST = system-update-inhibitor.8
EXAMPLE_SCRIPT     = examples/example-dnf-update.sh
EXAMPLE_SCRIPT_DST = update-on-shutdown.sh

.PHONY: all clean install uninstall test test-build

all: $(TARGET)

$(TARGET): $(SRC) $(SRCDIR)/inhibitor.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

test-build: $(TEST_BIN)

$(TEST_BIN): $(TESTDIR)/test_inhibitor_config.c $(SRC) $(SRCDIR)/inhibitor.h
	$(CC) $(CFLAGS) -o $@ $(TESTDIR)/test_inhibitor_config.c $(LDFLAGS)

test: test-build
	./$(TEST_BIN)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(SBINDIR)/$(TARGET)
	install -Dm644 $(SERVICE_SRC) \
		$(DESTDIR)$(SERVICEDIR)/$(SERVICENAME)
	install -Dm644 $(MANPAGE_SRC) \
		$(DESTDIR)$(MAN8DIR)/$(MANPAGE_DST)
	@if [ ! -f $(DESTDIR)$(CONFDIR)/$(CONFFILE) ]; then \
		install -Dm644 $(SRCDIR)/$(CONFFILE).example \
			$(DESTDIR)$(CONFDIR)/$(CONFFILE); \
		echo "Installed default config to $(DESTDIR)$(CONFDIR)/$(CONFFILE)"; \
	else \
		echo "Config $(DESTDIR)$(CONFDIR)/$(CONFFILE) already exists, not overwriting"; \
	fi
	@if [ ! -f $(DESTDIR)$(LOCALBINDIR)/$(EXAMPLE_SCRIPT_DST) ]; then \
		install -Dm755 $(EXAMPLE_SCRIPT) \
			$(DESTDIR)$(LOCALBINDIR)/$(EXAMPLE_SCRIPT_DST); \
		echo "Installed example script to $(DESTDIR)$(LOCALBINDIR)/$(EXAMPLE_SCRIPT_DST)"; \
	else \
		echo "Script $(DESTDIR)$(LOCALBINDIR)/$(EXAMPLE_SCRIPT_DST) already exists, not overwriting"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SERVICEDIR)/$(SERVICENAME)
	rm -f $(DESTDIR)$(MAN8DIR)/$(MANPAGE_DST)
	rm -f $(DESTDIR)$(CONFDIR)/$(CONFFILE)
	rm -f $(DESTDIR)$(LOCALBINDIR)/$(EXAMPLE_SCRIPT_DST)

clean:
	rm -f $(TARGET)
	rm -f $(TEST_BIN)

