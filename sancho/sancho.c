#include "sancho.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../libft/ft_upload.h"

typedef enum {
  ANSWER_NONE,       /* no reply (yet) */
  ANSWER_NOT_IMACON, /* some other device */
  ANSWER_FIRMWARE,   /* Imacon, firmware running */
  ANSWER_LOADER      /* Imacon, boot loader */
} inquiry_answer;

static inquiry_answer classify_answer(int response,
                                      const BYTE* response_buffer) {
  if(response < 0) {
    return ANSWER_NONE;
  } else if(memcmp(response_buffer + 8, "Imacon", 6) != 0) {
    return ANSWER_NOT_IMACON;
  } else if(ft_is_loader(response_buffer, (size_t)response)) {
    return ANSWER_LOADER;
  } else {
    return ANSWER_FIRMWARE;
  }
}

scanner_info sc_find_and_open_scanner(void) {
  static const BYTE inquiry_command[6] = {0x12, 0,    0,
                                          0,    0x60, 0};  // Who are you?
  scanner_info result;  // a place to hold the result of the inquiry
  memset(&result, 0, sizeof result);
  result.device_handle = NO_SCANNER;  // set default to indicate no scanner
                                      // found

  scanner_info
    loader_found;  // in case we find a bootloader instead of a scanner
  memset(&loader_found, 0, sizeof loader_found);
  loader_found.device_handle = NO_SCANNER;

  for(uint32_t i = 0; i < io_candidate_count(); i++) {
    io_scanner_handle candidate = io_open_candidate(i);

    if(candidate == NO_SCANNER) {
      continue;
    }
    bool keep_candidate = false;
    bool done_asking = false;

    for(uint32_t attempt = 0; attempt < 8 && done_asking == false; attempt++) {
      BYTE response_buffer[96];
      int response;
      memset(response_buffer, 0, sizeof response_buffer);
      response =
        io_send_command_to(candidate, inquiry_command, 6, SCSI_FROM_DEVICE,
                           response_buffer, sizeof response_buffer, 3000);

      switch(classify_answer(response, response_buffer)) {
        case ANSWER_NONE:
          break;
        case ANSWER_NOT_IMACON:
          done_asking = true;
          break;
        case ANSWER_FIRMWARE:
          sc_close_scanner(&loader_found);
          result.device_handle = candidate;
          memcpy(result.inquiry_response, response_buffer,
                 sizeof result.inquiry_response);
          result.inquiry_length = (uint32_t)response;
          result.is_loader = 0;
          g_scanner = candidate;
          return result;
        case ANSWER_LOADER:
          if(loader_found.device_handle == NO_SCANNER) {
            loader_found.device_handle = candidate;
            memcpy(loader_found.inquiry_response, response_buffer,
                   sizeof loader_found.inquiry_response);
            loader_found.inquiry_length = (uint32_t)response;
            loader_found.is_loader = 1;
            keep_candidate = true;
          }
          done_asking = true;
          break;
      }
    }

    if(keep_candidate == false) {
      scanner_close(candidate);
    }
  }
  g_scanner = loader_found.device_handle;
  return loader_found;
}

void sc_close_scanner(scanner_info* info) {
  if(info->device_handle == NO_SCANNER) {
    return;
  } else {
    scanner_close(info->device_handle);
    info->device_handle = NO_SCANNER;
  }
}
