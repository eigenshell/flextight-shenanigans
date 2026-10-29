/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Layout per ICC.1:2001-04 (version 2.4 of the format; the profiles say
 * 2.1, the lowest version with every type used here). Every number in a
 * profile is big endian. */
#include "ft_icc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_endian.h"

/* entries of a sampled transfer curve */
#define CURVE_ENTRIES 1024
/* the header's size, and the tag table's start */
#define HEADER_BYTES 128
/* the tags: desc, cprt, wtpt, rXYZ, gXYZ, bXYZ, rTRC, gTRC, bTRC */
#define TAG_COUNT 9
/* bytes per tag table entry: signature, offset, size */
#define TAG_ENTRY_BYTES 12
/* the curve argument of build_profile for "linear" */
#define LINEAR_CURVE (-1)
/* the longest description the profiles carry, without the NUL */
#define MAX_DESCRIPTION 63
/* the room for the space's name in a description */
#define MAX_SPACE_NAME 40

/* ICC's D50 as stored in s15Fixed16 (the header's illuminant) */
static const double D50[3] = {0.9642, 1.0, 0.8249};

/* the tags, in table order */
enum {
  TAG_DESCRIPTION,
  TAG_COPYRIGHT,
  TAG_WHITE_POINT,
  TAG_RED_COLORANT,
  TAG_GREEN_COLORANT,
  TAG_BLUE_COLORANT,
  TAG_RED_CURVE,
  TAG_GREEN_CURVE,
  TAG_BLUE_CURVE
};
static const char* const TAG_SIGNATURES[TAG_COUNT] = {
  "desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"};

/* s15Fixed16: a signed number with 16 fraction bits, big endian */
static void write_s15_fixed16(uint8_t* out, double value) {
  int64_t fixed = (int64_t)floor(value * 65536.0 + 0.5);
  /* a negative value keeps its two's complement bits */
  ft_write_big_endian32(out, 0, (uint32_t)fixed);
}

void ft_get_bradford_matrix(double matrix[9]) {
  static const double BRADFORD[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                                     0.0367, 0.0389, -0.0685, 1.0296};
  double xy[8];
  ft_get_space_primaries(FT_SPACE_SRGB, xy);
  /* D65's XYZ from its xy */
  double white[3] = {xy[6] / xy[7], 1, (1 - xy[6] - xy[7]) / xy[7]};
  /* both whites in the Bradford cone space */
  double source[3];
  double target[3];
  for(uint32_t row = 0; row < 3; row++) {
    source[row] = BRADFORD[3 * row] * white[0] +
                  BRADFORD[3 * row + 1] * white[1] +
                  BRADFORD[3 * row + 2] * white[2];
    target[row] = BRADFORD[3 * row] * D50[0] +
                  BRADFORD[3 * row + 1] * D50[1] +
                  BRADFORD[3 * row + 2] * D50[2];
  }
  double inverse[9];
  ft_invert_matrix3(BRADFORD, inverse);
  /* scaled = diag(target / source) x BRADFORD */
  double scaled[9];
  for(uint32_t row = 0; row < 3; row++) {
    for(uint32_t column = 0; column < 3; column++) {
      scaled[3 * row + column] =
        target[row] / source[row] * BRADFORD[3 * row + column];
    }
  }
  /* matrix = inverse x scaled */
  for(uint32_t row = 0; row < 3; row++) {
    for(uint32_t column = 0; column < 3; column++) {
      matrix[3 * row + column] = 0;
      for(uint32_t index = 0; index < 3; index++) {
        matrix[3 * row + column] +=
          inverse[3 * row + index] * scaled[3 * index + column];
      }
    }
  }
}

/* The tag writers below write a tag's data at out and return its size
 * (unpadded). */

/* textDescriptionType: ASCII only, no Unicode, no Mac script */
static size_t write_description(uint8_t* out, const char* text) {
  /* with the NUL */
  size_t length = strlen(text) + 1;
  memcpy(out, "desc", 4);
  memset(out + 4, 0, 4);
  ft_write_big_endian32(out, 8, (uint32_t)length);
  memcpy(out + 12, text, length);
  /* Unicode language and count, ScriptCode code, count and 67 bytes */
  memset(out + 12 + length, 0, 4 + 4 + 2 + 1 + 67);
  return 12 + length + 78;
}

static size_t write_text(uint8_t* out, const char* text) {
  size_t length = strlen(text) + 1;
  memcpy(out, "text", 4);
  memset(out + 4, 0, 4);
  memcpy(out + 8, text, length);
  return 8 + length;
}

static size_t write_xyz(uint8_t* out, const double xyz[3]) {
  memcpy(out, "XYZ ", 4);
  memset(out + 4, 0, 4);
  write_s15_fixed16(out + 8, xyz[0]);
  write_s15_fixed16(out + 12, xyz[1]);
  write_s15_fixed16(out + 16, xyz[2]);
  return 20;
}

/* curve: an ft_space, or LINEAR_CURVE (no entries: the identity) */
static size_t write_curve(uint8_t* out, int32_t curve) {
  memcpy(out, "curv", 4);
  memset(out + 4, 0, 4);
  if(curve == LINEAR_CURVE) {
    ft_write_big_endian32(out, 8, 0);
    return 12;
  }
  if(curve == FT_SPACE_ADOBE_RGB) {
    /* one entry: a gamma as u8Fixed8, 2.19921875 = 563/256 */
    ft_write_big_endian32(out, 8, 1);
    ft_write_big_endian16(out, 12, 0x0233);
    return 14;
  }
  ft_write_big_endian32(out, 8, CURVE_ENTRIES);
  for(uint32_t index = 0; index < CURVE_ENTRIES; index++) {
    double value =
      ft_decode_space((ft_space)curve, (double)index / (CURVE_ENTRIES - 1));
    ft_write_big_endian16(out, 12 + 2 * index,
                          (uint32_t)floor(value * 65535 + 0.5));
  }
  return 12 + 2 * CURVE_ENTRIES;
}

/* Build the profile for an RGB -> XYZ D65 matrix with a transfer curve
 * (an ft_space, or LINEAR_CURVE), described as description. Returns its
 * size, or 0 when it does not fit capacity. */
static size_t build_profile(const double rgb_to_xyz[9], int32_t curve,
                            const char* description, uint8_t* buffer,
                            size_t capacity) {
  uint8_t profile[FT_ICC_MAX_BYTES];
  if(capacity < 1) return 0;

  /* the colorants: each column of rgb_to_xyz, adapted to D50 */
  double bradford[9];
  double colorants[3][3];
  ft_get_bradford_matrix(bradford);
  for(uint32_t colorant = 0; colorant < 3; colorant++) {
    for(uint32_t row = 0; row < 3; row++) {
      colorants[colorant][row] = 0;
      for(uint32_t index = 0; index < 3; index++) {
        colorants[colorant][row] +=
          bradford[3 * row + index] * rgb_to_xyz[3 * index + colorant];
      }
    }
  }

  memset(profile, 0, sizeof profile);
  /* the tag data starts after the header and the tag table (a count,
   * then one entry per tag) */
  size_t position = HEADER_BYTES + 4 + TAG_ENTRY_BYTES * TAG_COUNT;
  size_t offsets[TAG_COUNT];
  size_t sizes[TAG_COUNT];
  for(uint32_t tag = 0; tag < TAG_COUNT; tag++) {
    uint8_t* out = profile + position;
    offsets[tag] = position;
    switch(tag) {
      case TAG_DESCRIPTION:
        sizes[tag] = write_description(out, description);
        break;
      case TAG_COPYRIGHT:
        sizes[tag] = write_text(out, "No copyright, use freely");
        break;
      case TAG_WHITE_POINT:
        sizes[tag] = write_xyz(out, D50);
        break;
      case TAG_RED_COLORANT:
      case TAG_GREEN_COLORANT:
      case TAG_BLUE_COLORANT:
        sizes[tag] = write_xyz(out, colorants[tag - TAG_RED_COLORANT]);
        break;
      case TAG_RED_CURVE:
        sizes[tag] = write_curve(out, curve);
        break;
      default:
        /* green and blue share red's curve: same offset, no new data */
        offsets[tag] = offsets[TAG_RED_CURVE];
        sizes[tag] = sizes[TAG_RED_CURVE];
        continue;
    }
    /* the next tag starts on a 4-byte boundary */
    position += (sizes[tag] + 3) & ~(size_t)3;
  }

  /* the header */
  ft_write_big_endian32(profile, 0, (uint32_t)position);
  /* version 2.1 */
  ft_write_big_endian32(profile, 8, 0x02100000u);
  memcpy(profile + 12, "mntr", 4);
  memcpy(profile + 16, "RGB ", 4);
  memcpy(profile + 20, "XYZ ", 4);
  /* a fixed date, so every run gives the same bytes: 2026-01-01 */
  ft_write_big_endian16(profile, 24, 2026);
  ft_write_big_endian16(profile, 26, 1);
  ft_write_big_endian16(profile, 28, 1);
  memcpy(profile + 36, "acsp", 4);
  /* rendering intent 0: perceptual */
  ft_write_big_endian32(profile, 64, 0);
  write_s15_fixed16(profile + 68, D50[0]);
  write_s15_fixed16(profile + 72, D50[1]);
  write_s15_fixed16(profile + 76, D50[2]);
  /* the tag table */
  ft_write_big_endian32(profile, HEADER_BYTES, TAG_COUNT);
  for(uint32_t tag = 0; tag < TAG_COUNT; tag++) {
    uint32_t entry = HEADER_BYTES + 4 + TAG_ENTRY_BYTES * tag;
    memcpy(profile + entry, TAG_SIGNATURES[tag], 4);
    ft_write_big_endian32(profile, entry + 4, (uint32_t)offsets[tag]);
    ft_write_big_endian32(profile, entry + 8, (uint32_t)sizes[tag]);
  }
  if(position > capacity) return 0;
  memcpy(buffer, profile, position);
  return position;
}

size_t ft_build_icc_profile(ft_space space, uint8_t* buffer, size_t capacity) {
  /* the cast to unsigned also catches values below 0 */
  if((unsigned)space > FT_SPACE_REC2020) return 0;
  double xy[8];
  double rgb_to_xyz[9];
  ft_get_space_primaries(space, xy);
  ft_compute_rgb_to_xyz(xy, rgb_to_xyz);
  /* "<space> (flextight)", the space's name cut to MAX_SPACE_NAME */
  char description[MAX_DESCRIPTION + 1];
  const char* suffix = " (flextight)";
  const char* name = ft_describe_space(space);
  size_t length = 0;
  while(name[length] != '\0' && length < MAX_SPACE_NAME) {
    description[length] = name[length];
    length++;
  }
  for(size_t index = 0; suffix[index] != '\0'; index++) {
    description[length] = suffix[index];
    length++;
  }
  description[length] = '\0';
  return build_profile(rgb_to_xyz, (int32_t)space, description, buffer,
                       capacity);
}

size_t ft_build_linear_icc_profile(const double xyz_to_rgb[9],
                                   const char* description, uint8_t* buffer,
                                   size_t capacity) {
  double rgb_to_xyz[9];
  if(ft_invert_matrix3(xyz_to_rgb, rgb_to_xyz) != 0) return 0;
  char cut[MAX_DESCRIPTION + 1];
  size_t length = 0;
  while(description[length] != '\0' && length < MAX_DESCRIPTION) {
    cut[length] = description[length];
    length++;
  }
  cut[length] = '\0';
  return build_profile(rgb_to_xyz, LINEAR_CURVE, cut, buffer, capacity);
}
