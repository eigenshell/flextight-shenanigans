# SPDX-License-Identifier: GPL-2.0-or-later
# ftpoc on Linux: SG_IO on the scanner's /dev/sgN (ftio_posix.c).
# Windows: build.bat (ftio_win.c).
CC ?= cc
# the usage text is one long string (fine for gcc, past C99's minimum)
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-overlength-strings
LIBFT = ../../libft
DETAIL = ../detail
SRC = ftpoc.c ftio_posix.c sgio.c $(DETAIL)/ring.c $(DETAIL)/worker.c
INC = -I$(LIBFT) -I$(DETAIL) -I.

ftpoc: $(SRC) *.h $(DETAIL)/ring.h $(DETAIL)/worker.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LIBFT)/libft.a -lm -pthread

# no hardware: the sg result mapping, and ftpoc's commands against a
# scripted fake transport (tests/test_ftpoc_posix.py drives the latter)
test/test_sgio: test/test_sgio.c sgio.c sgio.h
	$(CC) $(CFLAGS) -I. -I$(LIBFT)/test -o $@ test/test_sgio.c sgio.c

ftpoc_fake: ftpoc.c test/ftio_fake.c $(DETAIL)/ring.c $(DETAIL)/worker.c \
		*.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) $(INC) -o $@ ftpoc.c test/ftio_fake.c \
	    $(DETAIL)/ring.c $(DETAIL)/worker.c $(LIBFT)/libft.a -lm -pthread

test: test/test_sgio ftpoc_fake
	./test/test_sgio

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftpoc ftpoc_fake test/test_sgio

FORCE:
.PHONY: test clean FORCE
