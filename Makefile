.PHONY: all
all: pmash

pmash: pmash.c
	$(CC) -g -o $@ -W -Wall $<

.PHONY: install
install: all
	cp -a pmash $$(type -fp pmash)

.PHONY: clean
clean:
	$(RM) pmash
