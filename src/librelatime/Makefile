CFLAGS = -Wall -Wextra -fPIC -shared -O2
LDFLAGS = -ldl
NAME := relatime
TARGET = lib$(NAME).so
SOURCE = $(NAME).c

.PHONY: all
all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

# Test program to demonstrate the interception
test_relatime: test_relatime.c
	$(CC) -o $@ $<

# Run a test
.PHONY: test
test: $(TARGET) test_relatime
	LD_PRELOAD=$(CURDIR)/$(TARGET) ./test_relatime

.PHONY: clean
clean:
	$(RM) $(TARGET) test_relatime tst*.txt

# This comment block must stay at the bottom of the file.
# Local Variables:
# mode: makefile-gmake
# fill-column: 80
# End:
#
# vim: filetype=make sw=2 tw=80 cc=+1 noet
