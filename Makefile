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

# Consider passing MDSH_HTTP_SERVER=<server> to make for this test
# to exercise HTTP cache flushing.
.PHONY: test_mdsh
test_mdsh: export MDSH_PATHS=foo*:bar
test_mdsh: mdsh
	@$(RM) foo* bar
	# Test $< path tracking ...
	./$< -c 'uname > foo'
	./$< -c 'touch foo foobar'
	./$< -c 'uname > foo; uname > bar'
	./$< -c 'grep -c . foo bar > /dev/null'
	./$< -c '$(RM) foo* bar'

	# Test $< NFS flushing ...
	MDSH_VERBOSE=1 MDSH_FLUSH_PATHS=. \
	  ./$< -c date

	# Test $< timing ...
	MDSH_TIMING=1 MDSH_XTRACE=1 ./$< -c 'uname; sleep 2'

.PHONY: clean
clean: cleanups := $(wildcard *.o $(TARGETS))
clean:
	$(if $(cleanups),$(RM) $(cleanups))

# vim: filetype=make shiftwidth=2 tw=80 cc=+1 noet
