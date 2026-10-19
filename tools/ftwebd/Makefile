# SPDX-License-Identifier: GPL-2.0-or-later
# ftwebd on Linux (tests, later the Pi). Windows: build.bat.
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft
SRC = ftwebd.c http.c task.c batch.c proc_posix.c

ftwebd: $(SRC) *.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -o $@ $(SRC) $(LIBFT)/libft.a -lm -pthread

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftwebd

FORCE:
.PHONY: clean FORCE
