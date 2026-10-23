/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_upload.h"
#include "test/ftest.h"
#include <string.h>

int main(void) {
  static unsigned char code[FT_UPLOAD_CHUNK], buf[FT_UPLOAD_CHUNK + 10];
  unsigned char cdb[10];
  ft_err e;
  int i;
  /* CDBs as FlexColor sent them (SRBs in the session 2 and 3 traces) */
  static const unsigned char want_chunk_cdb[10] = {0x3b, 0x01, 0,    0,    0,
                                                   0,    0x00, 0x10, 0x0a, 0};
  static const unsigned char want_trail_cdb[10] = {0x3b, 0x01, 0,    0,    0,
                                                   0,    0x00, 0x00, 0x0a, 0};
  static const unsigned char want_hdr0[10] = {0x01, 0x30, 0, 0, 0,
                                              0,    0,    0, 0, 0};
  static const unsigned char want_hdr15[10] = {0x01, 0x3f, 0, 0, 0,
                                               0,    0,    0, 0, 0};
  static const unsigned char want_trail[10] = {0x00, 0x30, 0x01, 0x00, 0x0e,
                                               0,    0,    0,    0,    0};

  for(i = 0; i < FT_UPLOAD_CHUNK; i++)
    code[i] = (unsigned char)(i * 7 + 3);

  CHECK(ft_upload_chunk(0, code, cdb, buf, sizeof buf, &e) == 4106);
  CHECK(memcmp(cdb, want_chunk_cdb, 10) == 0);
  CHECK(memcmp(buf, want_hdr0, 10) == 0);
  CHECK(memcmp(buf + 10, code, FT_UPLOAD_CHUNK) == 0);

  CHECK(ft_upload_chunk(15, code, cdb, buf, sizeof buf, &e) == 4106);
  CHECK(memcmp(cdb, want_chunk_cdb, 10) == 0);
  CHECK(memcmp(buf, want_hdr15, 10) == 0);

  CHECK(ft_upload_chunk(16, code, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_upload_chunk(-1, code, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_upload_chunk(0, code, cdb, buf, 4105, &e) == -1);
  CHECK(strlen(e.msg) > 0);

  CHECK(ft_upload_trailer(cdb, buf, sizeof buf, &e) == 10);
  CHECK(memcmp(cdb, want_trail_cdb, 10) == 0);
  CHECK(memcmp(buf, want_trail, 10) == 0);
  CHECK(ft_upload_trailer(cdb, buf, 9, &e) == -1);

  /* loader identity: INQUIRY product field at byte 16 */
  memset(buf, ' ', 36);
  memcpy(buf + 8, "Imacon  SCSI Loader     L302", 28);
  CHECK(ft_upload_is_loader(buf, 36) == 1);
  memcpy(buf + 16, "FleXTight II    M333", 20);
  CHECK(ft_upload_is_loader(buf, 36) == 0);
  CHECK(ft_upload_is_loader(buf, 20) == 0);

  /* the microcode file: --microcode alone, else beside the exe, then
   * the current directory */
  {
    static char p[2][FT_UPLOAD_PATH], lng[FT_UPLOAD_PATH + 8];
    CHECK(strcmp(FT_UPLOAD_FILE, "MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths("D:\\fw\\m.3xx", "C:\\web\\ftpoc.exe", p) == 1);
    CHECK(strcmp(p[0], "D:\\fw\\m.3xx") == 0);
    CHECK(ft_upload_paths("", "C:\\web\\ftpoc.exe", p) == 0);
    CHECK(ft_upload_paths("C:\\web\\ftpoc.exe", NULL, p) == 1);
    CHECK(
        ft_upload_paths(NULL, "c:\\flextight_shenanigans\\web\\ftpoc.exe", p) ==
        2
    );
    CHECK(strcmp(p[0], "c:\\flextight_shenanigans\\web\\MICROCOD.3XX") == 0);
    CHECK(strcmp(p[1], "MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths(NULL, "/opt/ft/bin/ftpoc", p) == 2);
    CHECK(strcmp(p[0], "/opt/ft/bin/MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths(NULL, "C:\\a/b\\ftpoc.exe", p) == 2);
    CHECK(strcmp(p[0], "C:\\a/b\\MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths(NULL, "ftpoc.exe", p) == 1);
    CHECK(strcmp(p[0], "MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths(NULL, NULL, p) == 1);
    CHECK(strcmp(p[0], "MICROCOD.3XX") == 0);
    CHECK(ft_upload_paths(NULL, "", p) == 1);
    /* too long: left out, never another place in its stead */
    memset(lng, 'a', sizeof lng - 1);
    lng[sizeof lng - 1] = 0;
    CHECK(ft_upload_paths(lng, NULL, p) == 0);
    lng[FT_UPLOAD_PATH - 1] = 0; /* 519 chars: fits */
    CHECK(ft_upload_paths(lng, NULL, p) == 1);
    lng[FT_UPLOAD_PATH - 13] = '\\'; /* dir + 12 = 520: no */
    CHECK(ft_upload_paths(NULL, lng, p) == 1);
    CHECK(strcmp(p[0], "MICROCOD.3XX") == 0);
    lng[FT_UPLOAD_PATH - 13] = 'a';
    lng[FT_UPLOAD_PATH - 14] = '\\'; /* dir + 12 = 519: fits */
    CHECK(ft_upload_paths(NULL, lng, p) == 2);
    CHECK(strlen(p[0]) == FT_UPLOAD_PATH - 1);
  }

  return TEST_DONE();
}
