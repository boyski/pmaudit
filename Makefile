.PHONY: all
all: mdsh pmash

%: %.c
	$(CC) -g -o $@ -Wall -Wextra $<

.PHONY: install
install: all
	cp -a mdsh $$(type -fp mdsh)
	cp -a pmash $$(type -fp pmash)

.PHONY: clean
clean:
	$(RM) mdsh pmash
