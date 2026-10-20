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

/* "DID_NO_CONNECT" for host_status 1 etc. ("" when unknown) */
const char *sgio_host_name(int host);

#endif
