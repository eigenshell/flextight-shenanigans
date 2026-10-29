/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_tiff_ifd.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ft_endian.h"

/* the TIFF and DNG tags ft_add_ifd_common_tags writes */
enum {
  TAG_NEW_SUBFILE_TYPE = 254,
  TAG_IMAGE_WIDTH = 256,
  TAG_IMAGE_LENGTH = 257,
  TAG_BITS_PER_SAMPLE = 258,
  TAG_COMPRESSION = 259,
  TAG_PHOTOMETRIC = 262,
  TAG_IMAGE_DESCRIPTION = 270,
  TAG_MAKE = 271,
  TAG_MODEL = 272,
  TAG_ORIENTATION = 274,
  TAG_SAMPLES_PER_PIXEL = 277,
  TAG_X_RESOLUTION = 282,
  TAG_Y_RESOLUTION = 283,
  TAG_PLANAR_CONFIGURATION = 284,
  TAG_RESOLUTION_UNIT = 296,
  TAG_SOFTWARE = 305,
  TAG_DATE_TIME = 306,
  TAG_DNG_VERSION = 50706,
  TAG_DNG_BACKWARD_VERSION = 50707,
  TAG_UNIQUE_CAMERA_MODEL = 50708,
  TAG_WHITE_LEVEL = 50717,
  TAG_COLOR_MATRIX1 = 50721,
  TAG_AS_SHOT_NEUTRAL = 50728,
  TAG_CALIBRATION_ILLUMINANT1 = 50778
};

/* PhotometricInterpretation LinearRaw (DNG) */
#define PHOTOMETRIC_LINEAR_RAW 34892
/* CalibrationIlluminant1 D65 */
#define ILLUMINANT_D65 21
/* the TIFF header: "II", 42, the IFD's offset */
#define HEADER_BYTES 8
/* one IFD entry: tag, type, count, value or offset */
#define ENTRY_BYTES 12

/* bytes per value of a field type */
static size_t get_type_size(uint32_t type) {
  switch(type) {
    case FT_TIFF_SHORT:
      return 2;
    case FT_TIFF_LONG:
      return 4;
    case FT_TIFF_RATIONAL:
    case FT_TIFF_SRATIONAL:
    case FT_TIFF_DOUBLE:
      return 8;
    default:
      return 1;
  }
}

uint8_t* ft_add_ifd_entry(ft_ifd* ifd, uint32_t tag, uint32_t type,
                          uint32_t count) {
  if(ifd->entry_count == FT_IFD_MAX_TAGS || ifd->is_out_of_memory) {
    ifd->is_out_of_memory = true;
    return NULL;
  }
  ft_ifd_entry* entry = &ifd->entries[ifd->entry_count];
  entry->length = get_type_size(type) * count;
  /* calloc(0) may return NULL, so ask for at least 1 byte */
  entry->data = calloc(entry->length > 0 ? entry->length : 1, 1);
  if(entry->data == NULL) {
    ifd->is_out_of_memory = true;
    return NULL;
  }
  entry->tag = tag;
  entry->type = type;
  entry->count = count;
  entry->offset = 0;
  ifd->entry_count++;
  return entry->data;
}

void ft_add_ifd_short(ft_ifd* ifd, uint32_t tag, uint32_t value) {
  uint8_t* data = ft_add_ifd_entry(ifd, tag, FT_TIFF_SHORT, 1);
  if(data != NULL) ft_write_little_endian16(data, value);
}

void ft_add_ifd_long(ft_ifd* ifd, uint32_t tag, uint32_t value) {
  uint8_t* data = ft_add_ifd_entry(ifd, tag, FT_TIFF_LONG, 1);
  if(data != NULL) ft_write_little_endian32(data, value);
}

void ft_add_ifd_text(ft_ifd* ifd, uint32_t tag, const char* text) {
  size_t length = strlen(text);
  /* + 1 for the NUL, which calloc already set */
  uint8_t* data =
    ft_add_ifd_entry(ifd, tag, FT_TIFF_ASCII, (uint32_t)length + 1);
  if(data != NULL) memcpy(data, text, length);
}

void ft_add_ifd_bytes(ft_ifd* ifd, uint32_t tag, uint32_t type,
                      const uint8_t* bytes, uint32_t count) {
  uint8_t* data = ft_add_ifd_entry(ifd, tag, type, count);
  if(data != NULL) memcpy(data, bytes, count);
}

void ft_add_ifd_rationals(ft_ifd* ifd, uint32_t tag, uint32_t type,
                          const double* values, uint32_t count,
                          int32_t denominator) {
  uint8_t* data = ft_add_ifd_entry(ifd, tag, type, count);
  if(data == NULL) return;
  for(uint32_t index = 0; index < count; index++) {
    int64_t numerator = (int64_t)floor(values[index] * denominator + 0.5);
    /* a negative numerator keeps its two's complement bits: that is
     * how SRATIONAL stores it */
    ft_write_little_endian32(data + 8 * index, (uint32_t)numerator);
    ft_write_little_endian32(data + 8 * index + 4, (uint32_t)denominator);
  }
}

void ft_add_ifd_doubles(ft_ifd* ifd, uint32_t tag, const double* values,
                        uint32_t count) {
  uint8_t* data = ft_add_ifd_entry(ifd, tag, FT_TIFF_DOUBLE, count);
  if(data == NULL) return;
  for(uint32_t index = 0; index < count; index++) {
    /* copy the double's 8 bytes into an integer, then write them lowest
     * byte first: little endian on any host */
    uint64_t bits;
    memcpy(&bits, &values[index], 8);
    for(uint32_t byte = 0; byte < 8; byte++) {
      data[8 * index + byte] = (uint8_t)(bits >> (8 * byte));
    }
  }
}

ft_ifd_entry* ft_find_ifd_entry(ft_ifd* ifd, uint32_t tag) {
  for(uint32_t index = 0; index < ifd->entry_count; index++) {
    if(ifd->entries[index].tag == tag) return &ifd->entries[index];
  }
  return NULL;
}

/* the DNG tags: version 1.4, readable by 1.1, white level, colour */
static void add_dng_tags(ft_ifd* ifd, const ft_image* image) {
  static const uint8_t DNG_VERSION[4] = {1, 4, 0, 0};
  static const uint8_t DNG_BACKWARD_VERSION[4] = {1, 1, 0, 0};
  static const double NEUTRAL[3] = {1, 1, 1};
  ft_add_ifd_short(ifd, TAG_PHOTOMETRIC, PHOTOMETRIC_LINEAR_RAW);
  ft_add_ifd_bytes(ifd, TAG_DNG_VERSION, FT_TIFF_BYTE, DNG_VERSION, 4);
  ft_add_ifd_bytes(ifd, TAG_DNG_BACKWARD_VERSION, FT_TIFF_BYTE,
                   DNG_BACKWARD_VERSION, 4);
  char camera[80];
  snprintf(camera, sizeof camera, "%s %s", image->make, image->model);
  ft_add_ifd_text(ifd, TAG_UNIQUE_CAMERA_MODEL, camera);
  uint8_t* white_level =
    ft_add_ifd_entry(ifd, TAG_WHITE_LEVEL, FT_TIFF_LONG, 3);
  if(white_level != NULL) {
    ft_write_little_endian32(white_level, 65535);
    ft_write_little_endian32(white_level + 4, 65535);
    ft_write_little_endian32(white_level + 8, 65535);
  }
  ft_add_ifd_rationals(ifd, TAG_COLOR_MATRIX1, FT_TIFF_SRATIONAL,
                       image->xyz_to_rgb, 9, 10000000);
  ft_add_ifd_rationals(ifd, TAG_AS_SHOT_NEUTRAL, FT_TIFF_RATIONAL, NEUTRAL, 3,
                       1);
  ft_add_ifd_short(ifd, TAG_CALIBRATION_ILLUMINANT1, ILLUMINANT_D65);
}

void ft_add_ifd_common_tags(ft_ifd* ifd, const ft_image* image, uint32_t width,
                            uint32_t height, uint32_t samples_per_pixel,
                            bool is_dng) {
  /* "YYYY:MM:DD HH:MM:SS", the TIFF date format; empty when unknown */
  char stamp[20] = "";
  time_t now = time(NULL);
  struct tm* local = localtime(&now);
  if(local != NULL) {
    strftime(stamp, sizeof stamp, "%Y:%m:%d %H:%M:%S", local);
  }

  /* NewSubFileType 0: the main image */
  ft_add_ifd_long(ifd, TAG_NEW_SUBFILE_TYPE, 0);
  ft_add_ifd_long(ifd, TAG_IMAGE_WIDTH, width);
  ft_add_ifd_long(ifd, TAG_IMAGE_LENGTH, height);
  uint8_t* bits = ft_add_ifd_entry(ifd, TAG_BITS_PER_SAMPLE, FT_TIFF_SHORT,
                                   samples_per_pixel);
  for(uint32_t sample = 0; bits != NULL && sample < samples_per_pixel;
      sample++) {
    ft_write_little_endian16(bits + 2 * sample, 16);
  }
  /* 1 = no compression */
  ft_add_ifd_short(ifd, TAG_COMPRESSION, 1);
  if(image->description[0] != '\0') {
    ft_add_ifd_text(ifd, TAG_IMAGE_DESCRIPTION, image->description);
  }
  ft_add_ifd_text(ifd, TAG_MAKE, image->make);
  ft_add_ifd_text(ifd, TAG_MODEL, image->model);
  /* 1 = shown as stored */
  ft_add_ifd_short(ifd, TAG_ORIENTATION, 1);
  ft_add_ifd_short(ifd, TAG_SAMPLES_PER_PIXEL, samples_per_pixel);
  if(image->ppi > 0) {
    ft_add_ifd_rationals(ifd, TAG_X_RESOLUTION, FT_TIFF_RATIONAL, &image->ppi,
                         1, 1000);
    ft_add_ifd_rationals(ifd, TAG_Y_RESOLUTION, FT_TIFF_RATIONAL, &image->ppi,
                         1, 1000);
  }
  /* 1 = chunky: RGB together per pixel */
  ft_add_ifd_short(ifd, TAG_PLANAR_CONFIGURATION, 1);
  /* 2 = inches */
  if(image->ppi > 0) ft_add_ifd_short(ifd, TAG_RESOLUTION_UNIT, 2);
  ft_add_ifd_text(ifd, TAG_SOFTWARE, "flextight-shenanigans libft");
  if(stamp[0] != '\0') ft_add_ifd_text(ifd, TAG_DATE_TIME, stamp);
  if(is_dng) add_dng_tags(ifd, image);
}

/* qsort's comparison: ascending tag numbers */
static int compare_tags(const void* first, const void* second) {
  const ft_ifd_entry* a = first;
  const ft_ifd_entry* b = second;
  return (int)a->tag - (int)b->tag;
}

uint32_t ft_lay_out_ifd(ft_ifd* ifd) {
  qsort(ifd->entries, ifd->entry_count, sizeof ifd->entries[0], compare_tags);
  /* the header, the entry count, the entries, the next-IFD offset */
  uint32_t position = HEADER_BYTES + 2 + ENTRY_BYTES * ifd->entry_count + 4;
  for(uint32_t index = 0; index < ifd->entry_count; index++) {
    ft_ifd_entry* entry = &ifd->entries[index];
    if(entry->length > 4) {
      entry->offset = position;
      /* round up to an even length: TIFF values start on a word */
      position += (uint32_t)((entry->length + 1) & ~(size_t)1);
    } else {
      /* the value sits in the entry itself, after tag, type and count */
      entry->offset = HEADER_BYTES + 2 + ENTRY_BYTES * index + 8;
    }
  }
  return position;
}

int32_t ft_write_ifd(const ft_ifd* ifd, FILE* file) {
  uint8_t header[HEADER_BYTES];
  uint8_t entry_bytes[ENTRY_BYTES];
  bool has_failed = false;
  /* "II" = little endian, 42 = TIFF, the IFD right after the header */
  memcpy(header, "II*\0", 4);
  ft_write_little_endian32(header + 4, HEADER_BYTES);
  has_failed |= fwrite(header, 1, HEADER_BYTES, file) != HEADER_BYTES;
  ft_write_little_endian16(entry_bytes, ifd->entry_count);
  has_failed |= fwrite(entry_bytes, 1, 2, file) != 2;

  for(uint32_t index = 0; index < ifd->entry_count; index++) {
    const ft_ifd_entry* entry = &ifd->entries[index];
    memset(entry_bytes, 0, sizeof entry_bytes);
    ft_write_little_endian16(entry_bytes, entry->tag);
    ft_write_little_endian16(entry_bytes + 2, entry->type);
    ft_write_little_endian32(entry_bytes + 4, entry->count);
    if(entry->length <= 4) {
      memcpy(entry_bytes + 8, entry->data, entry->length);
    } else {
      ft_write_little_endian32(entry_bytes + 8, entry->offset);
    }
    has_failed |= fwrite(entry_bytes, 1, ENTRY_BYTES, file) != ENTRY_BYTES;
  }
  /* no next IFD */
  memset(entry_bytes, 0, 4);
  has_failed |= fwrite(entry_bytes, 1, 4, file) != 4;

  for(uint32_t index = 0; index < ifd->entry_count; index++) {
    const ft_ifd_entry* entry = &ifd->entries[index];
    if(entry->length <= 4) continue;
    has_failed |= fwrite(entry->data, 1, entry->length, file) != entry->length;
    /* pad an odd length to the word, as ft_lay_out_ifd counted */
    if((entry->length & 1) == 1) has_failed |= fputc(0, file) == EOF;
  }
  return has_failed ? -1 : 0;
}

void ft_free_ifd(ft_ifd* ifd) {
  for(uint32_t index = 0; index < ifd->entry_count; index++) {
    free(ifd->entries[index].data);
  }
  ifd->entry_count = 0;
}
