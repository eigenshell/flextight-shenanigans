/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sgio.c: SG_IO directions and how completions map to ftpoc's codes. The
 * literals are Linux's (scsi/sg.h; the midlayer's host bytes), pinned
 * here, not taken from sgio.c. */
#include "ftest.h"
#include "sgio.h"
#include <string.h>

int main(void) {
  unsigned got = 99;

  /* directions: in, out, none; no payload is always none (scanstop) */
  CHECK(sgio_dir(0x40, 2) == -3);  /* SG_DXFER_FROM_DEV */
  CHECK(sgio_dir(0x80, 84) == -2); /* SG_DXFER_TO_DEV */
  CHECK(sgio_dir(0x00, 0) == -1);  /* SG_DXFER_NONE */
  CHECK(sgio_dir(0x40, 0) == -1);
  CHECK(sgio_dir(0x80, 0) == -1);

  /* full read */
  CHECK(sgio_result(0, 0, 0, 0, 3996, &got) == 0 && got == 3996);
  /* empty read: an underrun of everything, normal while the host is
   * ahead of the CCD */
  got = 99;
  CHECK(sgio_result(0, 0, 0, 3996, 3996, &got) == 0 && got == 0);
  /* partial read */
  CHECK(sgio_result(0, 0, 0, 1000, 3996, &got) == 0 && got == 2996);
  /* a resid past the length or negative cannot make bytes up */
  CHECK(sgio_result(0, 0, 0, 5000, 3996, &got) == 0 && got == 0);
  CHECK(sgio_result(0, 0, 0, -4, 3996, &got) == 0 && got == 3996);
  /* no data phase */
  CHECK(sgio_result(0, 0, 0, 0, 0, &got) == 0 && got == 0);
  /* DRIVER_SENSE (0x08) with GOOD status: still a completion */
  CHECK(sgio_result(0, 0, 0x08, 0, 84, &got) == 0 && got == 84);

  /* timeouts: DID_TIME_OUT (3), DRIVER_TIMEOUT (6) */
  CHECK(sgio_result(0, 0x03, 0, 0, 2, &got) == SGIO_TIMEOUT && got == 0);
  CHECK(sgio_result(0, 0, 0x06, 0, 2, &got) == SGIO_TIMEOUT);

  /* right after the microcode trailer: DID_NO_CONNECT (1), zeroed
   * sense, for ~3 s (stage 1, src/flextight/upload.py) */
  got = 99;
  CHECK(sgio_result(0, 0x01, 0, 0, 2, &got) == SGIO_ERROR && got == 0);
  CHECK(strcmp(sgio_host_name(0x01), "DID_NO_CONNECT") == 0);
  /* CHECK CONDITION (status 0x02) with sense */
  CHECK(sgio_result(0x02, 0, 0x08, 0, 2, &got) == SGIO_ERROR);
  /* BUSY (0x08), DID_BUS_BUSY (2), DRIVER_ERROR (4) */
  CHECK(sgio_result(0x08, 0, 0, 0, 2, &got) == SGIO_ERROR);
  CHECK(sgio_result(0, 0x02, 0, 0, 2, &got) == SGIO_ERROR);
  CHECK(sgio_result(0, 0, 0x04, 0, 2, &got) == SGIO_ERROR);
  CHECK(strcmp(sgio_host_name(0x42), "") == 0);

  /* data-out lengths the microcode asks for (3000:247d: CDB bytes 6-8,
   * BE 24-bit), on FlexColor's captured CDBs */
  {
    /* SetWindow, every session: 84-byte descriptor, 0x005400 asked */
    static const unsigned char win[10] = {0x24, 0, 0, 0, 0, 0, 0, 0x54, 0, 0};
    /* microcode chunk (tests/test_upload.py CHUNK_CDB): 10-byte
     * header + 4096 code = 4106; the trailer: 10 */
    static const unsigned char chunk[10] = {0x3b, 0x01, 0,    0,    0,
                                            0,    0,    0x10, 0x0a, 0};
    static const unsigned char trailer[10] = {0x3b, 0x01, 0,    0,    0,
                                              0,    0,    0x00, 0x0a, 0};
    /* ScannerReady, INQUIRY: data-in, never padded */
    static const unsigned char poll[6] = {0xdf, 0, 0, 0, 0x02, 0};
    CHECK(sgio_out_len(win, 10, 84) == 21504);
    CHECK(sgio_out_len(chunk, 10, 4106) == 4106);
    CHECK(sgio_out_len(trailer, 10, 10) == 10);
    CHECK(sgio_out_len(poll, 6, 2) == 2);
  }
  return TEST_DONE();
}
