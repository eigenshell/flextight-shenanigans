/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sgio.c — see sgio.h. The constants are Linux's (scsi/sg.h, the SCSI
 * midlayer's host and driver bytes), spelled out so this file builds and
 * tests anywhere. */
#include "sgio.h"

#define SG_DXFER_NONE (-1)
#define SG_DXFER_TO_DEV (-2)
#define SG_DXFER_FROM_DEV (-3)

#define DID_OK 0x00
#define DID_NO_CONNECT 0x01
#define DID_BUS_BUSY 0x02
#define DID_TIME_OUT 0x03
#define DID_BAD_TARGET 0x04
#define DID_ABORT 0x05
#define DID_PARITY 0x06
#define DID_ERROR 0x07
#define DID_RESET 0x08

#define DRIVER_TIMEOUT 0x06 /* low nibble of driver_status */
#define DRIVER_SENSE 0x08   /* sense data is valid: not an error */

int sgio_dir(unsigned char dir, unsigned len) {
  if (len == 0) return SG_DXFER_NONE;
  if (dir == 0x40) return SG_DXFER_FROM_DEV;
  if (dir == 0x80) return SG_DXFER_TO_DEV;
  return SG_DXFER_NONE;
}

int sgio_result(int status, int host, int driver, int resid, unsigned len,
                unsigned* got) {
  *got = 0;
  if (host == DID_TIME_OUT || (driver & 0x0f) == DRIVER_TIMEOUT)
    return SGIO_TIMEOUT;
  if ((status & 0xff) != 0 || host != DID_OK || (driver & ~DRIVER_SENSE))
    return SGIO_ERROR;
  if (resid < 0) resid = 0;
  *got = (unsigned)resid >= len ? 0 : len - (unsigned)resid;
  return 0;
}

unsigned sgio_out_len(const unsigned char* cdb, int cdb_len,
                      unsigned data_len) {
  if (cdb_len != 10 || (cdb[0] != 0x24 && cdb[0] != 0x3b)) return data_len;
  return ((unsigned)cdb[6] << 16) | ((unsigned)cdb[7] << 8) | cdb[8];
}

const char* sgio_host_name(int host) {
  switch (host) {
    case DID_OK:
      return "DID_OK";
    case DID_NO_CONNECT:
      return "DID_NO_CONNECT";
    case DID_BUS_BUSY:
      return "DID_BUS_BUSY";
    case DID_TIME_OUT:
      return "DID_TIME_OUT";
    case DID_BAD_TARGET:
      return "DID_BAD_TARGET";
    case DID_ABORT:
      return "DID_ABORT";
    case DID_PARITY:
      return "DID_PARITY";
    case DID_ERROR:
      return "DID_ERROR";
    case DID_RESET:
      return "DID_RESET";
    default:
      return "";
  }
}
