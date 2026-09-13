CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
INSTALL ?= install

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
LOCALBIN = $(HOME)/.local/bin

olly: olly.c
	$(CC) $(CFLAGS) olly.c -o olly

install: olly
	@if [ -n "$(DESTDIR)" ]; then \
		$(INSTALL) -d $(DESTDIR)$(BINDIR); \
		$(INSTALL) -m 0755 olly $(DESTDIR)$(BINDIR)/olly; \
		echo "Installed $(DESTDIR)$(BINDIR)/olly"; \
	elif mkdir -p "$(BINDIR)" 2>/dev/null && [ -w "$(BINDIR)" ]; then \
		$(INSTALL) -m 0755 olly "$(BINDIR)/olly"; \
		echo "Installed $(BINDIR)/olly"; \
	else \
		echo "*** $(BINDIR) is not writable; falling back to"; \
		echo "***   $(LOCALBIN)"; \
		mkdir -p "$(LOCALBIN)"; \
		$(INSTALL) -m 0755 olly "$(LOCALBIN)/olly"; \
		echo "Installed $(LOCALBIN)/olly"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/olly $(LOCALBIN)/olly

test: olly
	python3 tests/test_olly.py ./olly

test-oom: olly
	python3 tests/oom_sweep.py ./olly

clean:
	rm -f olly editor

.PHONY: clean install uninstall test test-oom