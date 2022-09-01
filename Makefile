# Suppress those &@# "smart" (actually dumb!) quotes from GNU tools.
export LC_ALL := C

TARGETS := pmash

.PHONY: all
all: $(TARGETS)

%: %.c
	$(CC) -o $@ -Wall -Wextra $<

.PHONY: install
install: pmash := $(shell bash -c "type -fp pmash")
install: all
	$(if $(pmash),cp -a pmash $(pmash))

.PHONY: clean
clean: cleanups := $(wildcard $(TARGETS) *.o *.dSYM src/*.egg-info dist)
clean:
	$(if $(cleanups),$(RM) -r $(cleanups))

.PHONY: package
package: clean
	python3 -m build

.PHONY: test-upload
test-upload: package
	python3 -m twine upload --repository testpypi dist/*

.PHONY: upload
upload: package
	python3 -m twine upload dist/*

# vim: filetype=make shiftwidth=2 tw=80 cc=+1 noet
