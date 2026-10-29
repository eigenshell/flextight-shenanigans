/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Baseline JPEG encoder (ITU T.81), 8-bit YCbCr 4:4:4, one interleaved
 * scan, optimised Huffman tables. Each step does what libjpeg-turbo does
 * with jpeg_set_defaults + jpeg_set_quality(q, TRUE) + optimize_coding
 * and the default integer DCT, so the output bytes match it:
 *   - edges: the last column and row repeat out to a multiple of 8;
 *   - colour: the JFIF matrix in 16-bit fixed point, Cb/Cr rounded with
 *     0.5 - epsilon;
 *   - DCT: the Loeffler-Ligtenberg-Moschytz integer transform with 13
 *     constant bits and 2 pass-1 bits (output scaled by 8);
 *   - quantisation: rounded division by 8 * the table entry;
 *   - Huffman: frequencies from a first pass, code lengths by T.81 K.2
 *     (ties go to the higher symbol, a reserved all-ones code), lengths
 *     over 16 folded as in K.3;
 *   - markers: SOI, JFIF APP0, ICC APP2s, COM, DQT 0, DQT 1, SOF0,
 *     DHT DC0 AC0 DC1 AC1, SOS, data, EOI.
 * The DCT is redone in the second pass instead of keeping the
 * coefficients: memory stays one block row. */
#include "ft_jpeg.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the largest image side baseline JPEG allows here */
#define MAX_SIDE 65500
/* the most profile bytes one APP2 marker carries, and the most markers */
#define ICC_CHUNK_BYTES 65519
#define ICC_MAX_CHUNKS 255
/* the output buffer starts this big and doubles when full */
#define FIRST_CAPACITY_BYTES 4096

/* the JPEG markers we write */
enum {
  MARKER_SOI = 0xD8,
  MARKER_EOI = 0xD9,
  MARKER_APP0 = 0xE0,
  MARKER_APP2 = 0xE2,
  MARKER_COM = 0xFE,
  MARKER_DQT = 0xDB,
  MARKER_SOF0 = 0xC0,
  MARKER_DHT = 0xC4,
  MARKER_SOS = 0xDA
};

/* the colour components, in the order they are coded */
enum { COMPONENT_Y, COMPONENT_CB, COMPONENT_CR };
/* the two kinds of Huffman table */
enum { TABLE_DC, TABLE_AC };
/* the AC symbols for "16 zeros" and "the rest of the block is zero" */
#define SYMBOL_ZERO_RUN 0xF0
#define SYMBOL_END_OF_BLOCK 0x00

/* zigzag position -> natural (row-major) index */
static const uint8_t ZIGZAG[64] = {
  0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* T.81 Annex K quantisation tables, natural order: luminance, then
 * chrominance */
static const uint8_t BASE_QUANTISATION[2][64] = {
  {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
   14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
   18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
   49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
  {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
   24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};

/* The JPEG being written, and its bit writer. */
typedef struct output_buffer {
  uint8_t* bytes;
  size_t length;
  size_t capacity;
  /* a realloc failed: everything after is dropped */
  bool is_out_of_memory;
  /* bits not yet written out: the low pending_bit_count bits */
  uint32_t pending_bits;
  uint32_t pending_bit_count;
} output_buffer;

/* One Huffman table: the lengths and values as written, and the codes
 * derived from them. */
typedef struct huffman_table {
  /* counts[1..16]: how many codes have that many bits */
  uint8_t counts[17];
  /* the symbols, shortest code first */
  uint8_t values[256];
  /* each symbol's code and its length in bits; length 0 = no code */
  uint16_t codes[256];
  uint8_t code_lengths[256];
} huffman_table;

/* The encoder's state. */
typedef struct encoder {
  /* the quantisation tables, natural order: luminance, chrominance */
  int32_t quantisation[2][64];
  /* symbol counts from the first pass: [DC or AC][table][symbol] */
  int64_t frequencies[2][2][257];
  /* the tables built from them: [DC or AC][table] */
  huffman_table tables[2][2];
} encoder;

static void append_bytes(output_buffer* out, const void* bytes, size_t length) {
  if(out->is_out_of_memory) return;
  if(out->length + length > out->capacity) {
    size_t capacity = out->capacity > 0 ? out->capacity : FIRST_CAPACITY_BYTES;
    while(capacity < out->length + length) capacity *= 2;
    uint8_t* bigger = realloc(out->bytes, capacity);
    if(bigger == NULL) {
      out->is_out_of_memory = true;
      return;
    }
    out->bytes = bigger;
    out->capacity = capacity;
  }
  memcpy(out->bytes + out->length, bytes, length);
  out->length += length;
}

static void append_byte(output_buffer* out, uint32_t value) {
  uint8_t byte = (uint8_t)value;
  append_bytes(out, &byte, 1);
}

/* a 16-bit number, big endian */
static void append_word(output_buffer* out, uint32_t value) {
  append_byte(out, value >> 8);
  append_byte(out, value & 255);
}

/* Append bit_count bits of value to the entropy-coded data, most
 * significant first. A 0 byte follows every 0xFF byte (byte stuffing),
 * so the data never looks like a marker. */
static void append_bits(output_buffer* out, uint32_t value,
                        uint32_t bit_count) {
  /* keep only bit_count bits of value, then add them below the pending
   * ones */
  out->pending_bits =
    (out->pending_bits << bit_count) | (value & ((1u << bit_count) - 1));
  out->pending_bit_count += bit_count;
  while(out->pending_bit_count >= 8) {
    uint32_t byte = (out->pending_bits >> (out->pending_bit_count - 8)) & 255;
    out->pending_bit_count -= 8;
    append_byte(out, byte);
    if(byte == 255) append_byte(out, 0);
  }
  out->pending_bits &= (1u << out->pending_bit_count) - 1;
}

/* the 8-bit quantisation tables for a quality, as jpeg_set_quality */
static void build_quantisation_tables(encoder* state, int32_t quality) {
  int32_t scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for(uint32_t table = 0; table < 2; table++) {
    for(uint32_t index = 0; index < 64; index++) {
      int64_t value =
        ((int64_t)BASE_QUANTISATION[table][index] * scale + 50) / 100;
      if(value < 1) value = 1;
      if(value > 255) value = 255;
      state->quantisation[table][index] = (int32_t)value;
    }
  }
}

/* The forward DCT (Loeffler-Ligtenberg-Moschytz, as libjpeg-turbo's
 * jfdctint), in place, on level-shifted samples. The constants are
 * fixed point with CONSTANT_BITS fraction bits; pass 1 keeps
 * PASS1_BITS extra bits. */
#define CONSTANT_BITS 13
#define PASS1_BITS 2
/* divide by 2^bits, rounding: add half, then shift right */
#define DESCALE(value, bits) \
  (((value) + (INT64_C(1) << ((bits) - 1))) >> (bits))
static void transform_block(int64_t* data) {
  /* pass 0 works on rows, pass 1 on columns */
  for(uint32_t pass = 0; pass < 2; pass++) {
    for(uint32_t line = 0; line < 8; line++) {
      int64_t* p = pass == 1 ? data + line : data + line * 8;
      /* the distance between neighbours: 1 in a row, 8 in a column */
      uint32_t stride = pass == 1 ? 8 : 1;
      uint32_t shift =
        pass == 1 ? CONSTANT_BITS + PASS1_BITS : CONSTANT_BITS - PASS1_BITS;
      int64_t t0 = p[0] + p[7 * stride];
      int64_t t7 = p[0] - p[7 * stride];
      int64_t t1 = p[stride] + p[6 * stride];
      int64_t t6 = p[stride] - p[6 * stride];
      int64_t t2 = p[2 * stride] + p[5 * stride];
      int64_t t5 = p[2 * stride] - p[5 * stride];
      int64_t t3 = p[3 * stride] + p[4 * stride];
      int64_t t4 = p[3 * stride] - p[4 * stride];
      int64_t t10 = t0 + t3;
      int64_t t13 = t0 - t3;
      int64_t t11 = t1 + t2;
      int64_t t12 = t1 - t2;
      if(pass == 1) {
        p[0] = DESCALE(t10 + t11, PASS1_BITS);
        p[4 * stride] = DESCALE(t10 - t11, PASS1_BITS);
      } else {
        p[0] = (t10 + t11) * (1 << PASS1_BITS);
        p[4 * stride] = (t10 - t11) * (1 << PASS1_BITS);
      }
      /* the even part; each constant is its value x 2^13 */
      /* 0.541196100 */
      int64_t z1 = (t12 + t13) * 4433;
      /* 0.765366865 */
      p[2 * stride] = DESCALE(z1 + t13 * 6270, shift);
      /* 1.847759065 */
      p[6 * stride] = DESCALE(z1 - t12 * 15137, shift);
      /* the odd part */
      z1 = t4 + t7;
      int64_t z2 = t5 + t6;
      int64_t z3 = t4 + t6;
      int64_t z4 = t5 + t7;
      /* 1.175875602 */
      int64_t z5 = (z3 + z4) * 9633;
      /* 0.298631336, 2.053119869, 3.072711026, 1.501321110 */
      t4 *= 2446;
      t5 *= 16819;
      t6 *= 25172;
      t7 *= 12299;
      /* -0.899976223, -2.562915447, -1.961570560, -0.390180644 */
      z1 *= -7373;
      z2 *= -20995;
      z3 *= -16069;
      z4 *= -3196;
      z3 += z5;
      z4 += z5;
      p[7 * stride] = DESCALE(t4 + z1 + z3, shift);
      p[5 * stride] = DESCALE(t5 + z2 + z4, shift);
      p[3 * stride] = DESCALE(t6 + z2 + z3, shift);
      p[stride] = DESCALE(t7 + z1 + z4, shift);
    }
  }
}

/* One block of a component at block column block_x of the block row
 * whose 8 rows (edge-repeated) are rows[]: colour conversion, DCT and
 * quantisation. coefficients receives the result in natural order. */
static void encode_block(const encoder* state, const uint8_t* const rows[8],
                         uint32_t width, uint32_t block_x, uint32_t component,
                         int32_t coefficients[64]) {
  int64_t data[64];
  const int32_t* quantisation =
    state->quantisation[component == COMPONENT_Y ? 0 : 1];
  for(uint32_t y = 0; y < 8; y++) {
    for(uint32_t x = 0; x < 8; x++) {
      /* past the right edge, repeat the last column */
      uint32_t column = block_x * 8 + x;
      if(column >= width) column = width - 1;
      const uint8_t* pixel = rows[y] + column * 3;
      int32_t red = pixel[0];
      int32_t green = pixel[1];
      int32_t blue = pixel[2];
      /* The JFIF matrix with 16 fraction bits: FIX(0.29900) = 19595 ...
       * Cb and Cr: + 128, and 0.5 - epsilon so 255 stays 255. */
      int32_t value;
      switch(component) {
        case COMPONENT_Y:
          value = (19595 * red + 38470 * green + 7471 * blue + 32768) >> 16;
          break;
        case COMPONENT_CB:
          value = (-11059 * red -
                   21709 * green +
                   32768 * blue +
                   (128 << 16) +
                   32767) >>
                  16;
          break;
        default:
          value =
            (32768 * red - 27439 * green - 5329 * blue + (128 << 16) + 32767) >>
            16;
          break;
      }
      /* level shift: 0..255 -> -128..127 */
      data[y * 8 + x] = value - 128;
    }
  }
  transform_block(data);
  for(uint32_t index = 0; index < 64; index++) {
    /* the DCT's output is scaled by 8: divide by 8 x the table entry,
     * rounding half away from zero */
    int64_t divisor = (int64_t)quantisation[index] * 8;
    int64_t value = data[index];
    if(value < 0) {
      coefficients[index] = (int32_t)(-((-value + divisor / 2) / divisor));
    } else {
      coefficients[index] = (int32_t)((value + divisor / 2) / divisor);
    }
  }
}

/* the bits needed for |value|: 0 for 0, 1 for 1, 2 for 2..3, ... */
static uint32_t count_bits(int32_t value) {
  uint32_t bit_count = 0;
  if(value < 0) value = -value;
  while(value != 0) {
    bit_count++;
    value >>= 1;
  }
  return bit_count;
}

/* the bits written after a symbol: the value itself, or value - 1 for a
 * negative one (its one's complement in bit_count bits) */
static uint32_t get_extra_bits(int32_t value) {
  return (uint32_t)(value < 0 ? value - 1 : value);
}

/* Code one block's symbols: count them (out NULL, first pass) or write
 * them. previous_dc is the component's last DC value, updated. */
static void code_block(encoder* state, output_buffer* out,
                       const int32_t coefficients[64], uint32_t table,
                       int32_t* previous_dc) {
  const huffman_table* dc_table = &state->tables[TABLE_DC][table];
  const huffman_table* ac_table = &state->tables[TABLE_AC][table];
  bool is_counting = out == NULL;

  /* the DC coefficient is coded as the difference from the last block */
  int32_t difference = coefficients[0] - *previous_dc;
  *previous_dc = coefficients[0];
  uint32_t bit_count = count_bits(difference);
  if(is_counting) {
    state->frequencies[TABLE_DC][table][bit_count]++;
  } else {
    append_bits(out, dc_table->codes[bit_count],
                dc_table->code_lengths[bit_count]);
    if(bit_count > 0) append_bits(out, get_extra_bits(difference), bit_count);
  }

  /* the AC coefficients in zigzag order: runs of zeros and values */
  uint32_t zero_run = 0;
  for(uint32_t position = 1; position < 64; position++) {
    int32_t value = coefficients[ZIGZAG[position]];
    if(value == 0) {
      zero_run++;
      continue;
    }
    while(zero_run > 15) {
      if(is_counting) {
        state->frequencies[TABLE_AC][table][SYMBOL_ZERO_RUN]++;
      } else {
        append_bits(out, ac_table->codes[SYMBOL_ZERO_RUN],
                    ac_table->code_lengths[SYMBOL_ZERO_RUN]);
      }
      zero_run -= 16;
    }
    bit_count = count_bits(value);
    /* the symbol: the zero run in the high 4 bits, the size below */
    uint32_t symbol = (zero_run << 4) + bit_count;
    if(is_counting) {
      state->frequencies[TABLE_AC][table][symbol]++;
    } else {
      append_bits(out, ac_table->codes[symbol], ac_table->code_lengths[symbol]);
      append_bits(out, get_extra_bits(value), bit_count);
    }
    zero_run = 0;
  }
  if(zero_run > 0) {
    if(is_counting) {
      state->frequencies[TABLE_AC][table][SYMBOL_END_OF_BLOCK]++;
    } else {
      append_bits(out, ac_table->codes[SYMBOL_END_OF_BLOCK],
                  ac_table->code_lengths[SYMBOL_END_OF_BLOCK]);
    }
  }
}

/* Build an optimal Huffman table from symbol frequencies: T.81 K.2 code
 * lengths, K.3 limit to 16 bits, then the canonical codes (C.2). */
static void build_huffman_table(const int64_t* symbol_frequencies,
                                huffman_table* table) {
  /* the "never" frequency K.2 uses to find the smallest */
  const int64_t NO_FREQUENCY = 1000000000;
  int64_t frequencies[257];
  /* code_size[i]: the code length of symbol i; others[i]: the next
   * symbol in i's branch of the tree, -1 = none */
  int32_t code_size[257];
  int32_t others[257];
  /* length_counts[n]: symbols with an n-bit code */
  int32_t length_counts[33];
  memcpy(frequencies, symbol_frequencies, sizeof frequencies);
  memset(code_size, 0, sizeof code_size);
  memset(length_counts, 0, sizeof length_counts);
  for(uint32_t index = 0; index < 257; index++) others[index] = -1;
  /* a reserved symbol, so no code is all ones */
  frequencies[256] = 1;

  /* K.2: join the two least frequent branches until one is left. Ties
   * go to the higher symbol (the "<=" below). */
  while(true) {
    int32_t first = -1;
    int32_t second = -1;
    int64_t smallest = NO_FREQUENCY;
    for(int32_t symbol = 0; symbol <= 256; symbol++) {
      if(frequencies[symbol] != 0 && frequencies[symbol] <= smallest) {
        smallest = frequencies[symbol];
        first = symbol;
      }
    }
    smallest = NO_FREQUENCY;
    for(int32_t symbol = 0; symbol <= 256; symbol++) {
      if(frequencies[symbol] != 0 &&
         frequencies[symbol] <= smallest &&
         symbol != first) {
        smallest = frequencies[symbol];
        second = symbol;
      }
    }
    if(second < 0) break;
    frequencies[first] += frequencies[second];
    frequencies[second] = 0;
    /* every symbol in both branches gets one bit longer */
    code_size[first]++;
    while(others[first] >= 0) {
      first = others[first];
      code_size[first]++;
    }
    others[first] = second;
    code_size[second]++;
    while(others[second] >= 0) {
      second = others[second];
      code_size[second]++;
    }
  }
  for(uint32_t symbol = 0; symbol <= 256; symbol++) {
    if(code_size[symbol] != 0) {
      length_counts[code_size[symbol] > 32 ? 32 : code_size[symbol]]++;
    }
  }
  /* K.3: fold lengths over 16 bits back into the allowed range */
  int32_t length = 32;
  for(; length > 16; length--) {
    while(length_counts[length] > 0) {
      int32_t shorter = length - 2;
      while(length_counts[shorter] == 0) shorter--;
      length_counts[length] -= 2;
      length_counts[length - 1]++;
      length_counts[shorter + 1] += 2;
      length_counts[shorter]--;
    }
  }
  /* drop the reserved symbol's code: the longest one */
  while(length_counts[length] == 0) length--;
  length_counts[length]--;

  memset(table, 0, sizeof *table);
  for(uint32_t bits = 1; bits <= 16; bits++) {
    table->counts[bits] = (uint8_t)length_counts[bits];
  }
  uint32_t position = 0;
  for(int32_t bits = 1; bits <= 32; bits++) {
    for(uint32_t symbol = 0; symbol <= 255; symbol++) {
      if(code_size[symbol] == bits) {
        table->values[position] = (uint8_t)symbol;
        position++;
      }
    }
  }
  /* canonical codes in the order of values[]: count up, and shift left
   * when the length grows */
  uint32_t code = 0;
  position = 0;
  for(uint32_t bits = 1; bits <= 16; bits++) {
    for(uint32_t index = 0; index < table->counts[bits]; index++) {
      uint8_t symbol = table->values[position];
      table->codes[symbol] = (uint16_t)code;
      table->code_lengths[symbol] = (uint8_t)bits;
      code++;
      position++;
    }
    code <<= 1;
  }
}

/* a marker and its segment length */
static void append_marker(output_buffer* out, uint32_t marker,
                          uint32_t length) {
  append_byte(out, 0xFF);
  append_byte(out, marker);
  append_word(out, length);
}

static void append_jfif(output_buffer* out, double ppi) {
  static const uint8_t JFIF[5] = {'J', 'F', 'I', 'F', 0};
  append_marker(out, MARKER_APP0, 16);
  append_bytes(out, JFIF, 5);
  /* JFIF 1.01 */
  append_byte(out, 1);
  append_byte(out, 1);
  if(ppi > 0) {
    int64_t density = (int64_t)(ppi + 0.5);
    if(density < 1) density = 1;
    if(density > 65535) density = 65535;
    /* 1 = dots per inch */
    append_byte(out, 1);
    append_word(out, (uint32_t)density);
    append_word(out, (uint32_t)density);
  } else {
    /* 0 = no unit: an aspect ratio of 1:1 */
    append_byte(out, 0);
    append_word(out, 1);
    append_word(out, 1);
  }
  /* no thumbnail */
  append_byte(out, 0);
  append_byte(out, 0);
}

/* the ICC profile in numbered APP2 chunks */
static void append_icc_profile(output_buffer* out, const uint8_t* profile,
                               size_t profile_length) {
  static const uint8_t ICC_NAME[12] = "ICC_PROFILE";
  size_t chunk_count = (profile_length + ICC_CHUNK_BYTES - 1) / ICC_CHUNK_BYTES;
  size_t offset = 0;
  for(uint32_t chunk = 1; offset < profile_length; chunk++) {
    size_t length = profile_length - offset;
    if(length > ICC_CHUNK_BYTES) length = ICC_CHUNK_BYTES;
    /* safe: length is at most ICC_CHUNK_BYTES */
    append_marker(out, MARKER_APP2, (uint32_t)(length + 16));
    append_bytes(out, ICC_NAME, 12);
    append_byte(out, chunk);
    append_byte(out, (uint32_t)chunk_count);
    append_bytes(out, profile + offset, length);
    offset += length;
  }
}

static void append_headers(const encoder* state, output_buffer* out,
                           uint32_t width, uint32_t height, double ppi,
                           const uint8_t* icc_profile,
                           size_t icc_profile_length, const char* comment) {
  append_byte(out, 0xFF);
  append_byte(out, MARKER_SOI);
  append_jfif(out, ppi);
  if(icc_profile != NULL && icc_profile_length > 0) {
    append_icc_profile(out, icc_profile, icc_profile_length);
  }
  if(comment != NULL && comment[0] != '\0') {
    size_t length = strlen(comment);
    /* safe: the caller checked FT_JPEG_COMMENT_MAX_BYTES */
    append_marker(out, MARKER_COM, (uint32_t)(length + 2));
    append_bytes(out, comment, length);
  }
  /* the quantisation tables, in zigzag order */
  for(uint32_t table = 0; table < 2; table++) {
    append_marker(out, MARKER_DQT, 67);
    append_byte(out, table);
    for(uint32_t index = 0; index < 64; index++) {
      append_byte(out, (uint32_t)state->quantisation[table][ZIGZAG[index]]);
    }
  }
  /* SOF0: 8 bits, the size, 3 components, no subsampling (0x11), Y on
   * table 0 and Cb, Cr on table 1 */
  append_marker(out, MARKER_SOF0, 17);
  append_byte(out, 8);
  append_word(out, height);
  append_word(out, width);
  append_byte(out, 3);
  for(uint32_t component = 0; component < 3; component++) {
    append_byte(out, component + 1);
    append_byte(out, 0x11);
    append_byte(out, component == COMPONENT_Y ? 0 : 1);
  }
  /* the Huffman tables: DC 0, AC 0, DC 1, AC 1 */
  for(uint32_t table = 0; table < 2; table++) {
    for(uint32_t kind = 0; kind < 2; kind++) {
      const huffman_table* huffman = &state->tables[kind][table];
      uint32_t value_count = 0;
      for(uint32_t bits = 1; bits <= 16; bits++) {
        value_count += huffman->counts[bits];
      }
      append_marker(out, MARKER_DHT, 19 + value_count);
      append_byte(out, (kind << 4) | table);
      append_bytes(out, huffman->counts + 1, 16);
      append_bytes(out, huffman->values, value_count);
    }
  }
  /* SOS: 3 components (Y on tables 0/0, Cb and Cr on 1/1), spectral
   * selection 0..63, no successive approximation */
  append_marker(out, MARKER_SOS, 12);
  append_byte(out, 3);
  for(uint32_t component = 0; component < 3; component++) {
    append_byte(out, component + 1);
    append_byte(out, component == COMPONENT_Y ? 0x00 : 0x11);
  }
  append_byte(out, 0);
  append_byte(out, 63);
  append_byte(out, 0);
}

/* Every MCU (Y, Cb, Cr blocks) in raster order: count (out NULL) or
 * write. */
static void encode_scan(encoder* state, output_buffer* out,
                        const uint8_t* pixels, uint32_t width,
                        uint32_t height) {
  uint32_t blocks_across = (width + 7) / 8;
  uint32_t blocks_down = (height + 7) / 8;
  int32_t previous_dc[3] = {0, 0, 0};
  int32_t coefficients[64];
  const uint8_t* rows[8];
  for(uint32_t block_y = 0; block_y < blocks_down; block_y++) {
    /* the 8 rows of this block row; past the bottom, repeat the last */
    for(uint32_t y = 0; y < 8; y++) {
      uint32_t row = block_y * 8 + y;
      if(row >= height) row = height - 1;
      rows[y] = pixels + (size_t)row * width * 3;
    }
    for(uint32_t block_x = 0; block_x < blocks_across; block_x++) {
      for(uint32_t component = 0; component < 3; component++) {
        encode_block(state, rows, width, block_x, component, coefficients);
        uint32_t table = component == COMPONENT_Y ? 0 : 1;
        code_block(state, out, coefficients, table, &previous_dc[component]);
      }
    }
  }
}

int32_t ft_encode_jpeg(const uint8_t* pixels, uint32_t width, uint32_t height,
                       int32_t quality, double ppi, const uint8_t* icc_profile,
                       size_t icc_profile_length, const char* comment,
                       uint8_t** jpeg, size_t* jpeg_length, ft_err* error) {
  *jpeg = NULL;
  *jpeg_length = 0;
  if(pixels == NULL ||
     width < 1 ||
     height < 1 ||
     width > MAX_SIDE ||
     height > MAX_SIDE) {
    return ft_fail(error, "jpeg: bad size %ux%u", (unsigned)width,
                   (unsigned)height);
  }
  if(quality < 1 || quality > 100) {
    return ft_fail(error, "jpeg: quality %d (1..100)", (int)quality);
  }
  if(icc_profile != NULL &&
     icc_profile_length > (size_t)ICC_CHUNK_BYTES * ICC_MAX_CHUNKS) {
    return ft_fail(error, "jpeg: ICC profile too large");
  }
  if(comment != NULL && strlen(comment) > FT_JPEG_COMMENT_MAX_BYTES) {
    return ft_fail(error, "jpeg: comment too long");
  }
  encoder* state = calloc(1, sizeof *state);
  if(state == NULL) return ft_fail(error, "jpeg: out of memory");

  build_quantisation_tables(state, quality);
  /* first pass: count the symbols, to build optimal tables */
  encode_scan(state, NULL, pixels, width, height);
  for(uint32_t kind = 0; kind < 2; kind++) {
    for(uint32_t table = 0; table < 2; table++) {
      build_huffman_table(state->frequencies[kind][table],
                          &state->tables[kind][table]);
    }
  }
  /* second pass: write; the DCT is redone instead of keeping the
   * coefficients, so memory stays one block row */
  output_buffer out;
  memset(&out, 0, sizeof out);
  append_headers(state, &out, width, height, ppi, icc_profile,
                 icc_profile_length, comment);
  encode_scan(state, &out, pixels, width, height);
  /* pad the last byte with ones */
  if(out.pending_bit_count > 0) {
    append_bits(&out, 0x7F, 8 - out.pending_bit_count);
  }
  append_byte(&out, 0xFF);
  append_byte(&out, MARKER_EOI);
  free(state);
  if(out.is_out_of_memory) {
    free(out.bytes);
    return ft_fail(error, "jpeg: out of memory");
  }
  *jpeg = out.bytes;
  *jpeg_length = out.length;
  return 0;
}
