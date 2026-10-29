/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sc_live.h"

#include <stdint.h>
#include <string.h>

#include "ft_maint.h"
#include "ftio.h"

/* INQUIRY: "who are you?", asking for 96 bytes */
#define INQUIRY_LENGTH 96
/* where the microcode puts its scan state in the INQUIRY answer */
#define INQUIRY_SCAN_STATE_OFFSET 0x35
#define COMMAND_TIMEOUT_MS 3000
/* the live addresses are all in the scanner's data segment */
#define LIVE_SEGMENT 0x4000

int32_t sc_live_read_scan_state(void) {
  static const uint8_t inquiry_command[6] = {0x12, 0, 0, 0, INQUIRY_LENGTH,
                                             0};
  uint8_t inquiry_response[INQUIRY_LENGTH];
  memset(inquiry_response, 0, sizeof inquiry_response);

  int response = scsi_send_command(inquiry_command, sizeof inquiry_command,
                                   SCSI_FROM_DEVICE, inquiry_response,
                                   sizeof inquiry_response,
                                   COMMAND_TIMEOUT_MS);
  /* A negative response is an error code. A short answer (the loader's,
   * for instance) does not reach byte 0x35 at all. */
  if(response <= INQUIRY_SCAN_STATE_OFFSET) {
    return SC_LIVE_STATE_UNKNOWN;
  }
  return inquiry_response[INQUIRY_SCAN_STATE_OFFSET];
}

sc_live_result sc_live_poke_and_verify(uint32_t offset, const uint8_t* data,
                                       uint32_t length, uint8_t* read_back,
                                       int32_t* scsi_code, ft_err* error,
                                       uint32_t settle_ms) {
  uint8_t write_command[10];
  /* header + at most 4 data bytes; leave room to spare */
  uint8_t write_packet[FT_MAINT_HEADER_BYTES + 16];
  *scsi_code = 0;

  /* Build the write first: refused means nothing goes on the wire. */
  int32_t packet_length = ft_build_live_write(offset, data, length,
                                             write_command, write_packet,
                                             sizeof write_packet, error);
  if(packet_length < 0) {
    return SC_LIVE_REFUSED;
  }

  /* safe: packet_length is positive inside this branch */
  int response = scsi_send_command(write_command, sizeof write_command,
                                   SCSI_TO_DEVICE, write_packet,
                                   (uint32_t)packet_length,
                                   COMMAND_TIMEOUT_MS);
  if(response < 0) {
    *scsi_code = response;
    return SC_LIVE_SEND_FAILED;
  }

  /* Stay off the bus so the scanner's main loop runs the write before
   * the next command (our read) replaces it. */
  sleep_ms(settle_ms);

  /* Read the same bytes back (READ BUFFER 0x3c = memory read). The
   * whitelist above already limits offset/length, so this cannot fail. */
  uint8_t peek_command[10];
  if(ft_build_memory_read(LIVE_SEGMENT, offset, length, peek_command, error) < 0) {
    return SC_LIVE_REFUSED;
  }
  memset(read_back, 0, length);
  response = scsi_send_command(peek_command, sizeof peek_command,
                               SCSI_FROM_DEVICE, read_back, length,
                               COMMAND_TIMEOUT_MS);
  if(response < 0) {
    *scsi_code = response;
    return SC_LIVE_PEEK_FAILED;
  }

  /* A short read counts as a mismatch: the missing bytes stay 0. */
  if(memcmp(read_back, data, length) != 0) {
    return SC_LIVE_MISMATCH;
  }
  return SC_LIVE_OK;
}
