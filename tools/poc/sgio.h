/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgio.h — the pure half of ftio_posix.c: how an SG_IO request is built
 * and how its completion maps to ftpoc's codes. No I/O, so the mapping is
 * tested without a scanner (test_sgio.c).
 */
#ifndef SGIO_H
#define SGIO_H

/* sg_io_hdr.dxfer_direction for ftpoc's SRB direction byte (0x40 in,
 * 0x80 out, 0 none) and payload length: no payload is always "none". */
int sgio_dir(unsigned char dir, unsigned len);

#define SGIO_TIMEOUT (-2)
#define SGIO_ERROR   (-3)

/* A completed request (status, host_status, driver_status, resid of the
 * returned sg_io_hdr; len = dxfer_len): 0 and *got = bytes moved (a
 * resid underrun is NOT an error: the scanner sends empty and partial
 * reads while the host is ahead of the CCD), SGIO_TIMEOUT, or SGIO_ERROR
 * (CHECK CONDITION, a host/driver failure: DID_NO_CONNECT right after a
 * microcode upload, ...). */
int sgio_result(int status, int host, int driver, int resid, unsigned len,
                unsigned *got);

/* The data-out length the microcode itself asks for, whatever the host's
 * buffer: SET WINDOW (0x24, 3000:22e3) and WRITE BUFFER (0x3b,
 * 3000:2475) share one data-out path (3000:247d) that takes the DMA
 * count from CDB bytes 6-8, big-endian 24-bit (FUN_1efa(cdb, 8, 2)).
 * FlexColor's SetWindow CDB 24 00 00 00 00 00 00 54 00 00 so asks for
 * 0x005400 = 21504 bytes, not the 84 of the descriptor: the Windows
 * miniport pads the rest silently, Linux's aic7xxx calls it a data
 * overrun (DID_ERROR). 10-byte CDBs only; anything else: data_len. */
unsigned sgio_out_len(const unsigned char *cdb, int cdb_len,
                      unsigned data_len);

/* "DID_NO_CONNECT" for host_status 1 etc. ("" when unknown) */
const char *sgio_host_name(int host);

#endif
