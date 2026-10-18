# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft
DETAIL = ../detail
SRC = ftdev.c detail.c util.c render_flags.c share.c stock.c print.c plan.c \
	$(DETAIL)/ring.c $(DETAIL)/worker.c

ftdev: $(SRC) *.h $(DETAIL)/ring.h $(DETAIL)/worker.h \
		$(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -I. -I$(DETAIL) -o $@ $(SRC) \
	    $(LIBFT)/libft.a -lm -pthread

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftdev

FORCE:
.PHONY: clean FORCE
