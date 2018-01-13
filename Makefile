.PHONY: all
all: pmash

pmash: pmash.c
	$(CC) -g -o $@ -W -Wall $<

.PHONY: clean
clean:
	$(RM) pmash
