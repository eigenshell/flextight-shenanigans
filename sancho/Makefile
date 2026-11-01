# SPDX-License-Identifier: GPL-2.0-or-later
# Sancho, the scanner bridge: talks to the scanner through ftio and
# hands results back to its caller. Builds libsancho.a; a program that
# uses it also links ftio (ftio/ftio_posix.c + ftio/sgio.c) and libft.
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../libft
FTIO = ../ftio
INC = -I$(FTIO) -I$(LIBFT)

libsancho.a: sancho.o sc_live.o
	$(AR) rcs $@ sancho.o sc_live.o

sancho.o: sancho.c sancho.h $(FTIO)/ftio.h $(LIBFT)/ft_upload.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ sancho.c

sc_live.o: sc_live.c sc_live.h $(FTIO)/ftio.h $(LIBFT)/ft_maint.h $(LIBFT)/ft.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ sc_live.c

clean:
	rm -f sancho.o sc_live.o libsancho.a

.PHONY: clean
