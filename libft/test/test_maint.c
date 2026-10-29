/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft_maint.h"
#include "test/ftest.h"

int main(void) {
  unsigned char cdb[10], buf[FT_MAINT_HEADER_BYTES + FT_MAINT_MAX_DATA_BYTES];
  ft_err e;
  static const unsigned char want_io_cdb[10] = {0x3b, 0x01, 0,    0,    0,
                                                0,    0x00, 0x00, 0x0b, 0};
  static const unsigned char want_io_buf[11] = {0x01, 0x03, 0x44, 0x00, 0x00, 0,
                                                0,    0,    0,    0x05, 0x7f};
  static const unsigned char want_mem_cdb[10] = {0x3b, 0x01, 0,    0,    0,
                                                 0,    0x00, 0x00, 0x0e, 0};
  static const unsigned char want_mem_buf[14] = {
    0x01, 0x40, 0x00, 0x76, 0x88, 0, 0, 0, 0, 0x00, 0x10, 0x00, 0x00, 0x00};
  static const unsigned char want_peek_cdb[10] = {0x3c, 0x00, 0x40, 0x00, 0x76,
                                                  0x40, 0x00, 0x00, 0x7c, 0};
  /* FlexColor's own Faul Pixs read (session traces, ftpoc save_faul) */
  static const unsigned char want_faul_cdb[10] = {0x3c, 0x00, 0xe0, 0x00, 0x04,
                                                  0x00, 0x00, 0x01, 0x3e, 0};
  static const unsigned char want_flash_top_cdb[10] = {
    0x3c, 0x00, 0xf0, 0x00, 0xfc, 0x00, 0x00, 0x04, 0x00, 0};
  static const unsigned char want_loader_cdb[10] = {
    0x3c, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0};
  static const unsigned char want_peek_max_cdb[10] = {
    0x3c, 0x00, 0x30, 0x00, 0xfc, 0x00, 0x00, 0x04, 0x00, 0};
  const unsigned char dword16[4] = {0x10, 0x00, 0x00, 0x00};
  unsigned char big[FT_MAINT_MAX_DATA_BYTES + 1] = {0};

  /* I/O byte write to 0x344: pinned wire bytes */
  CHECK(ft_build_port_write(0x344, 0x7f, cdb, buf, sizeof buf, &e) == 11);
  CHECK(memcmp(cdb, want_io_cdb, 10) == 0);
  CHECK(memcmp(buf, want_io_buf, 11) == 0);
  CHECK(ft_build_port_write(0x342, 0x02, cdb, buf, sizeof buf, &e) == 11);

  /* forbidden ports */
  CHECK(ft_build_port_write(0x340, 0, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_build_port_write(0x300, 0, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_build_port_write(0x323, 0, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_build_port_write(0xf872, 0, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_build_port_write(0x346, 0, cdb, buf, sizeof buf, &e) == -1);
  CHECK(ft_build_port_write(0x343, 0, cdb, buf, sizeof buf, &e) == -1);

  /* memory write inside the timing table: pinned wire bytes */
  CHECK(ft_build_memory_write(0x4000, 0x7688, dword16, 4, cdb, buf, sizeof buf,
                              &e) == 14);
  CHECK(memcmp(cdb, want_mem_cdb, 10) == 0);
  CHECK(memcmp(buf, want_mem_buf, 14) == 0);

  /* table edges (last allowed byte 4000:76BB) */
  CHECK(ft_build_memory_write(0x4000, 0x7640, dword16, 4, cdb, buf, sizeof buf,
                              &e) == 14);
  CHECK(ft_build_memory_write(0x4000, 0x76b8, dword16, 4, cdb, buf, sizeof buf,
                              &e) == 14);
  CHECK(ft_build_memory_write(0x4000, 0x76b9, dword16, 4, cdb, buf, sizeof buf,
                              &e) == -1);
  CHECK(ft_build_memory_write(0x4000, 0x763f, dword16, 4, cdb, buf, sizeof buf,
                              &e) == -1);

  /* other segments, empty, oversized, small buffers */
  CHECK(ft_build_memory_write(0x3000, 0x7688, dword16, 4, cdb, buf, sizeof buf,
                              &e) == -1);
  CHECK(ft_build_memory_write(0x6108, 0x0000, dword16, 4, cdb, buf, sizeof buf,
                              &e) == -1);
  CHECK(ft_build_memory_write(0x4000, 0x7688, dword16, 0, cdb, buf, sizeof buf,
                              &e) == -1);
  CHECK(ft_build_memory_write(0x4000, 0x7640, big, sizeof big, cdb, buf,
                              sizeof buf, &e) == -1);
  CHECK(ft_build_memory_write(0x4000, 0x7688, dword16, 4, cdb, buf, 13, &e) ==
        -1);
  CHECK(ft_build_port_write(0x344, 0, cdb, buf, 10, &e) == -1);
  CHECK(strlen(e.message) > 0);

  /* every packet built is longer than the header: the firmware computes
   * len - 10 unchecked, so len < 11 would underflow (Phase A, A1) */
  CHECK(ft_build_memory_write(0x4000, 0x7640, dword16, 1, cdb, buf, sizeof buf,
                              &e) == 11);
  CHECK(cdb[8] == 11);

  /* peek (READ BUFFER = memory read): pinned wire bytes */
  CHECK(ft_build_memory_read(0x4000, 0x7640, 0x7c, cdb, &e) == 0);
  CHECK(memcmp(cdb, want_peek_cdb, 10) == 0);
  CHECK(ft_build_memory_read(0x3000, 0xfc00, FT_MAINT_MAX_PEEK_BYTES, cdb,
                             &e) == 0);
  CHECK(memcmp(cdb, want_peek_max_cdb, 10) == 0);

  /* peek refusals: other segments, length bounds, segment wrap */
  CHECK(ft_build_memory_read(0xe000, 0x0400, 0x13e, cdb, &e) == 0);
  CHECK(memcmp(cdb, want_faul_cdb, 10) == 0);
  CHECK(ft_build_memory_read(0xf000, 0xfc00, 0x400, cdb, &e) == 0);
  CHECK(memcmp(cdb, want_flash_top_cdb, 10) == 0);
  /* loader ROM (chip select 3): C000:0000..7FFF only */
  CHECK(ft_build_memory_read(0xc000, 0x0000, 0x400, cdb, &e) == 0);
  CHECK(memcmp(cdb, want_loader_cdb, 10) == 0);
  CHECK(ft_build_memory_read(0xc000, 0x7c00, 0x400, cdb, &e) == 0);
  CHECK(ft_build_memory_read(0xc000, 0x7c01, 0x400, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0xc000, 0x8000, 0x10, cdb, &e) == -1);
  /* the device windows (CS0 C8000.., CS1 D0000..) and the rest stay out */
  CHECK(ft_build_memory_read(0xc800, 0x0000, 0x10, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0xd000, 0x0000, 0x10, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0xd800, 0x0000, 0x10, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0xe800, 0x0000, 0x10, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0x6108, 0x0000, 0x10, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0x4000, 0x7640, 0, cdb, &e) == -1);
  CHECK(ft_build_memory_read(0x4000, 0x0000, FT_MAINT_MAX_PEEK_BYTES + 1, cdb,
                             &e) == -1);
  CHECK(ft_build_memory_read(0x4000, 0xfe00, 0x201, cdb, &e) == -1);

  /* live-mode writes (ft_build_live_write): pinned wire bytes for the
   * harmless test target 4000:8EEE */
  {
    static const unsigned char want_live_cdb[10] = {
      0x3b, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00};
    static const unsigned char want_live_buf[14] = {
      0x01, 0x40, 0x00, 0x8e, 0xee, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44};
    static const unsigned char want_seek_cdb[10] = {
      0x3b, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00};
    static const unsigned char want_seek_buf[12] = {
      0x01, 0x40, 0x00, 0x75, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01};
    const unsigned char marker[4] = {0x11, 0x22, 0x33, 0x44};
    const unsigned char seek_enable[2] = {0x01, 0x01};

    CHECK(ft_build_live_write(0x8eee, marker, 4, cdb, buf, sizeof buf, &e) ==
          14);
    CHECK(memcmp(cdb, want_live_cdb, 10) == 0);
    CHECK(memcmp(buf, want_live_buf, 14) == 0);
    CHECK(ft_build_live_write(0x7557, seek_enable, 2, cdb, buf, sizeof buf,
                              &e) == 12);
    CHECK(memcmp(cdb, want_seek_cdb, 10) == 0);
    CHECK(memcmp(buf, want_seek_buf, 12) == 0);

    /* the other allowed writes */
    CHECK(ft_build_live_write(0x7557, seek_enable, 1, cdb, buf, sizeof buf,
                              &e) == 11);
    CHECK(ft_build_live_write(0x8ed4, marker, 4, cdb, buf, sizeof buf, &e) ==
          14);
    CHECK(ft_build_live_write(0x8edc, marker, 4, cdb, buf, sizeof buf, &e) ==
          14);
    CHECK(ft_build_live_write(0x8ee4, marker, 4, cdb, buf, sizeof buf, &e) ==
          14);

    /* refused: neighbours, wrong lengths, live positions, the timing
     * table, offsets past the segment, small buffers */
    CHECK(ft_build_live_write(0x8eed, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8eef, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8eee, marker, 3, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8eee, marker, 5, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8eee, marker, 0, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x7557, marker, 3, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x7557, marker, 0, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x7558, marker, 1, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8ed0, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8ed8, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8ee0, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8ed4, marker, 2, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x7640, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x18eee, marker, 4, cdb, buf, sizeof buf, &e) ==
          -1);
    CHECK(ft_build_live_write(0x8eee, marker, 4, cdb, buf, 13, &e) == -1);
    CHECK(strlen(e.message) > 0);

    /* the timing-table writer was not widened to the live addresses */
    CHECK(ft_build_memory_write(0x4000, 0x8eee, marker, 4, cdb, buf, sizeof buf,
                                &e) == -1);
    CHECK(ft_build_memory_write(0x4000, 0x7557, marker, 1, cdb, buf, sizeof buf,
                                &e) == -1);
  }

  return TEST_DONE();
}
