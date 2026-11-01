# SPDX-License-Identifier: GPL-2.0-or-later
# ftpoc on Linux: SG_IO on the scanner's /dev/sgN (ftio/ftio_posix.c).
# Windows: build.bat (ftio/ftio_win.c).
CC ?= cc
# the usage text is one long string (fine for gcc, past C99's minimum)
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-overlength-strings
LIBFT = ../../libft
DETAIL = ../detail
FTIO = ../../ftio
SANCHO = ../../sancho
SRC = ftpoc.c $(SANCHO)/sc_live.c $(FTIO)/ftio_posix.c $(FTIO)/sgio.c $(DETAIL)/ring.c $(DETAIL)/worker.c
INC = -I$(LIBFT) -I$(DETAIL) -I$(FTIO) -I$(SANCHO)

ftpoc: $(SRC) $(FTIO)/*.h $(SANCHO)/sc_live.h $(DETAIL)/ring.h $(DETAIL)/worker.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LIBFT)/libft.a -lm -pthread

# no hardware: the sg result mapping, and ftpoc's commands against a
# scripted fake transport (tests/test_ftpoc_posix.py drives the latter)
$(FTIO)/test/test_sgio: $(FTIO)/test/test_sgio.c $(FTIO)/sgio.c $(FTIO)/sgio.h
	$(CC) $(CFLAGS) -I$(FTIO) -I$(LIBFT)/test -o $@ $(FTIO)/test/test_sgio.c $(FTIO)/sgio.c

ftpoc_fake: ftpoc.c $(SANCHO)/sc_live.c $(FTIO)/test/ftio_fake.c \
		$(DETAIL)/ring.c $(DETAIL)/worker.c \
		$(FTIO)/*.h $(SANCHO)/sc_live.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) $(INC) -o $@ ftpoc.c $(SANCHO)/sc_live.c \
	    $(FTIO)/test/ftio_fake.c \
	    $(DETAIL)/ring.c $(DETAIL)/worker.c $(LIBFT)/libft.a -lm -pthread

test: $(FTIO)/test/test_sgio ftpoc_fake
	$(FTIO)/test/test_sgio

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftpoc ftpoc_fake $(FTIO)/test/test_sgio

FORCE:
.PHONY: test clean FORCE
