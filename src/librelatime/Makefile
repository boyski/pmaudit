CFLAGS := -Wall -Wextra -fPIC -shared -O2
NAME := relatime
TARGET := lib$(NAME).so

.PHONY: all
all: $(TARGET)

$(TARGET): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $< -ldl

test_relatime: test_relatime.c
	$(CC) -o $@ $<

SETUP := LD_PRELOAD=$(TARGET) LD_LIBRARY_PATH=$(CURDIR)

.PHONY: test
test: $(TARGET) test_relatime
	$(SETUP) ./test_relatime
#	$(SETUP) /usr/bin/gcc -c hello.c

.PHONY: clean
clean:
	$(RM) $(TARGET) test_relatime tst*.txt

# vim: filetype=make sw=2 tw=80 cc=+1 noet
