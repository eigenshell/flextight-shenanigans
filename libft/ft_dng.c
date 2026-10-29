/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tag set and layout per the DNG 1.4 specification (Adobe, 2012) and
 * TIFF 6.0 (ft_tiff_ifd.h): little-endian, one IFD, tags ascending,
 * values over 4 bytes after the IFD (word-aligned), then the strips. */
#include "ft_dng.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_endian.h"
#include "ft_tiff_ifd.h"

/* the largest 16-bit sample */
#define SAMPLE_MAX 65535
/* XResolution is written as a rational over this */
#define RESOLUTION_DENOMINATOR 1000
/* the reader refuses more tags than this, or images wider or taller */
#define MAX_TAG_COUNT 200
#define MAX_IMAGE_SIDE 100000
/* the reader refuses a tag with more value bytes than this */
#define MAX_TAG_BYTES 0x10000000u
/* bytes per pixel: 3 samples of 16 bits */
#define PIXEL_BYTES 6
/* PhotometricInterpretation LinearRaw */
#define PHOTOMETRIC_LINEAR_RAW 34892

/* the tags the reader looks at */
enum {
  TAG_IMAGE_WIDTH = 256,
  TAG_IMAGE_LENGTH = 257,
  TAG_BITS_PER_SAMPLE = 258,
  TAG_COMPRESSION = 259,
  TAG_PHOTOMETRIC = 262,
  TAG_IMAGE_DESCRIPTION = 270,
  TAG_MAKE = 271,
  TAG_MODEL = 272,
  TAG_STRIP_OFFSETS = 273,
  TAG_SAMPLES_PER_PIXEL = 277,
  TAG_ROWS_PER_STRIP = 278,
  TAG_STRIP_BYTE_COUNTS = 279,
  TAG_X_RESOLUTION = 282,
  TAG_PLANAR_CONFIGURATION = 284,
  TAG_DNG_VERSION = 50706,
  TAG_COLOR_MATRIX1 = 50721,
  TAG_NOISE_PROFILE = 51041
};
static const uint32_t WANTED_TAGS[] = {TAG_IMAGE_WIDTH,
                                       TAG_IMAGE_LENGTH,
                                       TAG_BITS_PER_SAMPLE,
                                       TAG_COMPRESSION,
                                       TAG_PHOTOMETRIC,
                                       TAG_IMAGE_DESCRIPTION,
                                       TAG_MAKE,
                                       TAG_MODEL,
                                       TAG_STRIP_OFFSETS,
                                       TAG_SAMPLES_PER_PIXEL,
                                       TAG_ROWS_PER_STRIP,
                                       TAG_STRIP_BYTE_COUNTS,
                                       TAG_X_RESOLUTION,
                                       TAG_PLANAR_CONFIGURATION,
                                       TAG_DNG_VERSION,
                                       TAG_COLOR_MATRIX1,
                                       TAG_NOISE_PROFILE};
#define WANTED_TAG_COUNT (sizeof WANTED_TAGS / sizeof WANTED_TAGS[0])

bool ft_is_noise_profile_usable(const double* noise_profile) {
  /* pairs of S (slope, even index) and O (offset, odd index) */
  for(uint32_t index = 0; index < 6; index++) {
    double value = noise_profile[index];
    bool is_offset = index % 2 == 1;
    if(isfinite(value) == false || value < 0) return false;
    if(is_offset && value == 0) return false;
  }
  return true;
}

uint16_t ft_quantise_dng_sample(float value) {
  if(value <= 0) return 0;
  if(value >= 1) return SAMPLE_MAX;
  /* + 0.5 rounds to the nearest */
  return (uint16_t)(value * 65535.0f + 0.5f);
}

void ft_simulate_dng_storage(ft_image* image) {
  size_t value_count = 0;
  if(image->pixels != NULL) {
    value_count = (size_t)image->width * image->height * 3;
  }
  for(size_t index = 0; index < value_count; index++) {
    image->pixels[index] =
      (float)ft_quantise_dng_sample(image->pixels[index]) / 65535.0f;
  }
  /* XResolution: a rational over 1000 (ft_add_ifd_common_tags), left out
   * when ppi is 0. The numerator is stored in 32 bits, so keep only
   * those. */
  if(image->ppi > 0) {
    int64_t numerator =
      (int64_t)floor(image->ppi * RESOLUTION_DENOMINATOR + 0.5);
    image->ppi = (double)(uint32_t)numerator / RESOLUTION_DENOMINATOR;
  } else {
    image->ppi = 0;
  }
  if(image->has_noise_profile &&
     ft_is_noise_profile_usable(image->noise_profile) == false) {
    image->has_noise_profile = false;
  }
}

/* ------------------------------------------------------------- reader */

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

/* Where a tag's values are in the file. */
typedef struct tag_reference {
  /* the field type, FT_TIFF_* */
  uint32_t type;
  /* values; 0 = the file has no such tag */
  uint32_t count;
  /* the file offset of the value bytes */
  uint32_t offset;
} tag_reference;

/* the index of tag in WANTED_TAGS; -1 when the reader ignores it */
static int32_t find_wanted_index(uint32_t tag) {
  for(size_t index = 0; index < WANTED_TAG_COUNT; index++) {
    if(WANTED_TAGS[index] == tag) return (int32_t)index;
  }
  return -1;
}

/* the tag's reference; NULL when the file has no such tag */
static const tag_reference* find_tag(const tag_reference* tags, uint32_t tag) {
  int32_t index = find_wanted_index(tag);
  if(index < 0 || tags[index].count == 0) return NULL;
  return &tags[index];
}

/* Read value number index of a SHORT or LONG tag.
 * Returns false when there is no such tag or value, or another type. */
static bool read_tag_value(const uint8_t* bytes, const tag_reference* tag,
                           uint32_t index, uint32_t* value) {
  if(tag == NULL || index >= tag->count) return false;
  if(tag->type == FT_TIFF_SHORT) {
    *value = ft_read_little_endian16(bytes + tag->offset + 2 * index);
    return true;
  }
  if(tag->type == FT_TIFF_LONG) {
    *value = ft_read_little_endian32(bytes + tag->offset + 4 * index);
    return true;
  }
  return false;
}

/* is tag's first value present and equal to expected? */
static bool has_tag_value(const uint8_t* bytes, const tag_reference* tags,
                          uint32_t tag, uint32_t expected) {
  uint32_t value;
  return read_tag_value(bytes, find_tag(tags, tag), 0, &value) &&
         value == expected;
}

/* Read the IFD's entries for the wanted tags into tags[], checking every
 * value lies inside the file. Returns 0, or -1 with error filled. */
static int32_t read_ifd(const uint8_t* bytes, size_t length,
                        tag_reference* tags, ft_err* error) {
  uint32_t ifd_offset = ft_read_little_endian32(bytes + 4);
  if(ifd_offset < 8 || ifd_offset > length || length - ifd_offset < 2) {
    return ft_fail(error, "dng: bad IFD offset");
  }
  uint32_t entry_count = ft_read_little_endian16(bytes + ifd_offset);
  /* each entry is 12 bytes; 4 more for the next-IFD offset */
  if(entry_count > MAX_TAG_COUNT ||
     length - ifd_offset - 2 < 12 * (size_t)entry_count + 4) {
    return ft_fail(error, "dng: truncated IFD");
  }
  for(uint32_t index = 0; index < entry_count; index++) {
    const uint8_t* entry = bytes + ifd_offset + 2 + 12 * index;
    uint32_t tag = ft_read_little_endian16(entry);
    uint32_t type = ft_read_little_endian16(entry + 2);
    uint32_t count = ft_read_little_endian32(entry + 4);
    int32_t wanted = find_wanted_index(tag);
    if(wanted < 0 || count == 0) continue;
    size_t type_size = get_type_size(type);
    if(count > MAX_TAG_BYTES / type_size) {
      return ft_fail(error, "dng: tag %u too large", (unsigned)tag);
    }
    size_t value_bytes = type_size * count;
    tag_reference* reference = &tags[wanted];
    reference->type = type;
    reference->count = count;
    /* up to 4 bytes sit in the entry itself; more are elsewhere */
    if(value_bytes <= 4) {
      reference->offset = (uint32_t)(entry + 8 - bytes);
    } else {
      reference->offset = ft_read_little_endian32(entry + 8);
    }
    if(reference->offset > length || value_bytes > length - reference->offset) {
      return ft_fail(error, "dng: tag %u points past the end", (unsigned)tag);
    }
  }
  return 0;
}

/* Check the file is the subset this reader takes.
 * Returns 0, or -1 with error filled. */
static int32_t check_format(const uint8_t* bytes, const tag_reference* tags,
                            ft_err* error) {
  if(find_tag(tags, TAG_DNG_VERSION) == NULL) {
    return ft_fail(error, "dng: no DNGVersion: not a DNG");
  }
  if(has_tag_value(bytes, tags, TAG_PHOTOMETRIC, PHOTOMETRIC_LINEAR_RAW) ==
     false) {
    return ft_fail(error, "dng: not LinearRaw (only this writer's DNGs)");
  }
  /* compression 1 = none */
  if(has_tag_value(bytes, tags, TAG_COMPRESSION, 1) == false) {
    return ft_fail(error, "dng: compressed (only uncompressed is read)");
  }
  if(has_tag_value(bytes, tags, TAG_SAMPLES_PER_PIXEL, 3) == false) {
    return ft_fail(error, "dng: not 3 samples per pixel");
  }
  /* planar configuration 1 = chunky; missing means chunky too */
  if(find_tag(tags, TAG_PLANAR_CONFIGURATION) != NULL &&
     has_tag_value(bytes, tags, TAG_PLANAR_CONFIGURATION, 1) == false) {
    return ft_fail(error, "dng: not chunky (interleaved) RGB");
  }
  for(uint32_t sample = 0; sample < 3; sample++) {
    uint32_t bits;
    if(read_tag_value(bytes, find_tag(tags, TAG_BITS_PER_SAMPLE), sample,
                      &bits) == false ||
       bits != 16) {
      return ft_fail(error, "dng: not 16 bits per sample");
    }
  }
  return 0;
}

/* Read the NoiseProfile: 6 little-endian doubles. A bad one reads as
 * none (export then estimates). */
static void read_noise_profile(const uint8_t* bytes, const tag_reference* tags,
                               ft_image* image) {
  const tag_reference* noise = find_tag(tags, TAG_NOISE_PROFILE);
  if(noise == NULL || noise->type != FT_TIFF_DOUBLE || noise->count != 6) {
    return;
  }
  double values[6];
  for(uint32_t index = 0; index < 6; index++) {
    /* gather the 8 bytes, highest first, into an integer, then copy its
     * bits into the double */
    uint64_t bits = 0;
    for(int32_t byte = 7; byte >= 0; byte--) {
      bits = bits << 8 | bytes[noise->offset + 8 * index + (uint32_t)byte];
    }
    memcpy(&values[index], &bits, 8);
  }
  if(ft_is_noise_profile_usable(values)) {
    memcpy(image->noise_profile, values, sizeof values);
    image->has_noise_profile = true;
  }
}

/* Read ColorMatrix1: 9 signed rationals. */
static void read_colour_matrix(const uint8_t* bytes, const tag_reference* tags,
                               ft_image* image) {
  const tag_reference* matrix = find_tag(tags, TAG_COLOR_MATRIX1);
  if(matrix == NULL ||
     matrix->type != FT_TIFF_SRATIONAL ||
     matrix->count != 9) {
    return;
  }
  for(uint32_t index = 0; index < 9; index++) {
    const uint8_t* value = bytes + matrix->offset + 8 * index;
    /* SRATIONAL: two's complement, so the casts give the signed value */
    int32_t numerator = (int32_t)ft_read_little_endian32(value);
    int32_t denominator = (int32_t)ft_read_little_endian32(value + 4);
    if(denominator != 0) {
      image->xyz_to_rgb[index] = (double)numerator / denominator;
    }
  }
}

/* Copy a text tag into text (capacity bytes), cut to fit. */
static void read_text_tag(const uint8_t* bytes, const tag_reference* tags,
                          uint32_t tag, char* text, size_t capacity) {
  const tag_reference* reference = find_tag(tags, tag);
  if(reference == NULL || reference->type != FT_TIFF_ASCII) return;
  size_t length = reference->count < capacity ? reference->count : capacity - 1;
  memcpy(text, bytes + reference->offset, length);
  text[length] = '\0';
}

int32_t ft_parse_dng(const uint8_t* bytes, size_t length, ft_image* image,
                     ft_err* error) {
  tag_reference tags[WANTED_TAG_COUNT];
  memset(tags, 0, sizeof tags);
  if(length < 8 || memcmp(bytes, "II*\0", 4) != 0) {
    return ft_fail(error, "dng: not a little-endian TIFF");
  }
  if(read_ifd(bytes, length, tags, error) != 0) return -1;
  if(check_format(bytes, tags, error) != 0) return -1;

  uint32_t width;
  uint32_t height;
  if(read_tag_value(bytes, find_tag(tags, TAG_IMAGE_WIDTH), 0, &width) ==
       false ||
     read_tag_value(bytes, find_tag(tags, TAG_IMAGE_LENGTH), 0, &height) ==
       false ||
     width < 1 ||
     height < 1 ||
     width > MAX_IMAGE_SIDE ||
     height > MAX_IMAGE_SIDE) {
    return ft_fail(error, "dng: bad size");
  }
  size_t row_bytes = (size_t)width * PIXEL_BYTES;
  uint32_t rows_per_strip;
  if(read_tag_value(bytes, find_tag(tags, TAG_ROWS_PER_STRIP), 0,
                    &rows_per_strip) == false ||
     rows_per_strip < 1) {
    rows_per_strip = height;
  }
  uint32_t strip_count = (height + rows_per_strip - 1) / rows_per_strip;
  const tag_reference* offsets = find_tag(tags, TAG_STRIP_OFFSETS);
  const tag_reference* byte_counts = find_tag(tags, TAG_STRIP_BYTE_COUNTS);
  if(offsets == NULL ||
     byte_counts == NULL ||
     offsets->count != strip_count ||
     byte_counts->count != strip_count) {
    return ft_fail(error, "dng: strip tags do not match the size");
  }

  /* check every strip lies in the file before anything is allocated */
  for(uint32_t strip = 0; strip < strip_count; strip++) {
    uint32_t rows = strip + 1 < strip_count ? rows_per_strip
                                            : height - strip * rows_per_strip;
    uint32_t strip_offset;
    uint32_t strip_bytes;
    /* in double: rows x row_bytes may wrap on 32 bits */
    if(read_tag_value(bytes, offsets, strip, &strip_offset) == false ||
       read_tag_value(bytes, byte_counts, strip, &strip_bytes) == false ||
       (double)rows * row_bytes > (double)length ||
       strip_bytes != rows * row_bytes ||
       strip_offset > length ||
       strip_bytes > length - strip_offset) {
      return ft_fail(error, "dng: strip %u outside the file", (unsigned)strip);
    }
  }
  /* Strips may overlap, so they alone do not bound width x height: the
   * pixels must fit in the file (in double: no wrap on 32 bits). */
  if((double)width * height * PIXEL_BYTES > (double)length) {
    return ft_fail(error, "dng: %ux%u pixels do not fit in the file",
                   (unsigned)width, (unsigned)height);
  }
  if(ft_allocate_image(image, width, height, error) != 0) return -1;
  for(uint32_t strip = 0; strip < strip_count; strip++) {
    uint32_t rows = strip + 1 < strip_count ? rows_per_strip
                                            : height - strip * rows_per_strip;
    uint32_t strip_offset;
    read_tag_value(bytes, offsets, strip, &strip_offset);
    float* target = image->pixels + (size_t)strip * rows_per_strip * width * 3;
    size_t sample_count = (size_t)rows * width * 3;
    for(size_t sample = 0; sample < sample_count; sample++) {
      uint32_t value =
        ft_read_little_endian16(bytes + strip_offset + 2 * sample);
      target[sample] = (float)value / 65535.0f;
    }
  }

  const tag_reference* resolution = find_tag(tags, TAG_X_RESOLUTION);
  if(resolution != NULL && resolution->type == FT_TIFF_RATIONAL) {
    uint32_t numerator = ft_read_little_endian32(bytes + resolution->offset);
    uint32_t denominator =
      ft_read_little_endian32(bytes + resolution->offset + 4);
    if(denominator != 0) image->ppi = (double)numerator / denominator;
  }
  read_noise_profile(bytes, tags, image);
  read_colour_matrix(bytes, tags, image);
  read_text_tag(bytes, tags, TAG_IMAGE_DESCRIPTION, image->description,
                sizeof image->description);
  read_text_tag(bytes, tags, TAG_MAKE, image->make, sizeof image->make);
  read_text_tag(bytes, tags, TAG_MODEL, image->model, sizeof image->model);
  return 0;
}
