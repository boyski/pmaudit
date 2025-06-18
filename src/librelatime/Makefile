CFLAGS := -Wall -Wextra -fpic -shared -g -m64
SHLIBFLAGS := $(CFLAGS) -ldl

NAME := relatime
RELATIME := lib$(NAME).so

.PHONY: all
all: $(RELATIME)

$(RELATIME): $(NAME).c
	$(CC) -o $@ $< $(SHLIBFLAGS)

test_relatime: test_relatime.c
	$(CC) -g -o $@ $<

.PHONY: test
test: test_relatime $(RELATIME)
	LD_PRELOAD=$(CURDIR)/$(RELATIME) ./$<

.PHONY: clean
clean:
	$(RM) $(RELATIME) test_relatime tst*.txt

# vim: filetype=make sw=2 tw=80 cc=+1 noet
