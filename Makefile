# Suppress &@# "smart" (actually dumb!) quotes from GNU tools.
export LC_ALL := C

TARGETS := mdsh pmash

.PHONY: all
all: $(TARGETS)

%: %.c
	$(CC) -g -o $@ -Wall -Wextra $<

.PHONY: install
install: mdsh := $(shell bash -c "type -fp mdsh")
install: pmash := $(shell bash -c "type -fp pmash")
install: all
	$(if $(mdsh),cp -a mdsh $(mdsh))
	$(if $(pmash),cp -a pmash $(pmash))

.PHONY: test_mdsh
test_mdsh: export MDSH_PATHS=foo*:bar
test_mdsh: mdsh
	@$(RM) foo* bar
	./$< -c 'uname > foo'
	./$< -c 'touch foo foobar'
	./$< -c 'uname > foo; uname > bar'
	sleep 1; ./$< -c 'grep -c . foo bar > /dev/null'
	./$< -c '$(RM) foo* bar'

.PHONY: clean
clean: cleanups := $(wildcard *.o $(TARGETS))
clean:
	$(if $(cleanups),$(RM) $(cleanups))
