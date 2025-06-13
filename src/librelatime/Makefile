CFLAGS := -Wall -Wextra -fpic -shared -g -m64
SHLIBFLAGS := $(CFLAGS) -ldl

NAME := relatime
TARGET := lib$(NAME).so

.PHONY: all
all: $(TARGET)

$(TARGET): $(NAME).c
	$(CC) -o $@ $< $(SHLIBFLAGS)

test_relatime: test_relatime.c
	$(CC) -o $@ $<

SETUP := LD_PRELOAD=$(TARGET) LD_LIBRARY_PATH=$(CURDIR)

.PHONY: test
test: $(TARGET) test_relatime
	$(SETUP) ./test_relatime

.PHONY: clean
clean:
	$(RM) $(TARGET) test_relatime tst*.txt

# vim: filetype=make sw=2 tw=80 cc=+1 noet
