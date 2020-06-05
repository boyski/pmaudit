# Suppress &@# "smart" (actually dumb!) quotes from GNU tools.
export LC_ALL := C

TARGETS := pmash

.PHONY: all
all: $(TARGETS)

%: %.c
	$(CC) -g -o $@ -Wall -Wextra $<

.PHONY: install
install: pmash := $(shell bash -c "type -fp pmash")
install: all
	$(if $(pmash),cp -a pmash $(pmash))

.PHONY: clean
clean: cleanups := $(wildcard *.o $(TARGETS))
clean:
	$(if $(cleanups),$(RM) $(cleanups))

# vim: filetype=make shiftwidth=2 tw=80 cc=+1 noet
