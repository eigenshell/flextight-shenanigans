#ifndef SANCHO_H
#define SANCHO_H
#include <stdint.h>

#include "../ftio/ftio.h"

typedef struct scanner_info {
  io_scanner_handle device_handle;  // the scanners identifier
  BYTE inquiry_response[96];        // who is the scanner (imacon / loader)
  uint32_t inquiry_length;          // how long is the response
  uint32_t
    is_loader;  // 1 = bootloader running (needs firmware), 0 = firmware running
} scanner_info;

scanner_info sc_find_and_open_scanner(void);
void sc_close_scanner(scanner_info* info);

#endif
