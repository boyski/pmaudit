CFLAGS := -Wall -Wextra -fPIC -shared -O2
NAME := relatime
TARGET := lib$(NAME).so

.PHONY: all
all: $(TARGET)

$(TARGET): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $< -ldl

test_relatime: test_relatime.c
	$(CC) -o $@ $<

.PHONY: test
test: $(TARGET) test_relatime
	LD_PRELOAD=$(CURDIR)/$(TARGET) ./test_relatime

.PHONY: clean
clean:
	$(RM) $(TARGET) test_relatime tst*.txt

# vim: filetype=make sw=2 tw=80 cc=+1 noet
