# shard — minimal distributed task runner
# Single binary, no dependencies beyond a C compiler and libc.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -Isrc
LDFLAGS ?=
PREFIX  ?= /usr/local

BIN     = shard
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
DEP     = $(OBJ:.o=.d)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

# The dashboard lives in web/dashboard.html and is embedded into the binary.
# The generated header is committed, so a plain `make` never regenerates it.
src/ui_page.h: web/dashboard.html tools/embed.sh
	./tools/embed.sh web/dashboard.html shard_ui_page > $@

src/ui.o: src/ui_page.h

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

test: $(BIN)
	./tests/run.sh

# Build the end-user documentation site with atomik-ssg. The source lives in
# docs-src/; the built site is synced to docs/ because GitHub Pages serves a
# project site from the /docs folder (atomik-ssg refuses to build across ..,
# so we build in place and copy).
docs:
	cd docs-src && atomik-ssg build
	rm -rf docs
	cp -r docs-src/public docs
	touch docs/.nojekyll

docs-serve:
	cd docs-src && atomik-ssg serve

clean:
	rm -f $(OBJ) $(DEP) $(BIN)

.PHONY: all install uninstall test docs docs-serve clean
