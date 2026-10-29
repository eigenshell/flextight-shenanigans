/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_output.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PI 3.14159265358979323846
/* the bisection steps when searching a gamut boundary */
#define BOUNDARY_STEPS 40
/* no OkLab chroma inside these spaces reaches this */
#define MAX_CHROMA 0.6
/* the ACES gamut compressor's power */
#define COMPRESS_POWER 1.2

/* A colour space's names and primaries. */
typedef struct space_info {
  /* the short name a user picks */
  const char* name;
  /* the full name */
  const char* description;
  /* CIE xy of red, green, blue and white */
  double xy[8];
} space_info;

static const space_info SPACES[] = {
  {.name = "srgb",
   .description = "sRGB",
   .xy = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290}},
  {.name = "p3",
   .description = "Display P3",
   .xy = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290}},
  {.name = "adobe",
   .description = "Adobe RGB (1998)",
   .xy = {0.640, 0.330, 0.210, 0.710, 0.150, 0.060, 0.3127, 0.3290}},
  {.name = "rec2020",
   .description = "Rec. ITU-R BT.2020",
   .xy = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290}},
};
#define SPACE_COUNT (sizeof SPACES / sizeof SPACES[0])

/* x - x is 0 for every finite x, and NaN for NaN and infinity */
static bool is_finite_float(float value) { return value - value == 0; }

int32_t ft_find_space(const char* name, ft_space* space) {
  for(size_t index = 0; name != NULL && index < SPACE_COUNT; index++) {
    if(strcmp(name, SPACES[index].name) == 0) {
      *space = (ft_space)index;
      return 0;
    }
  }
  return -1;
}

const char* ft_get_space_name(ft_space space) {
  /* the cast to unsigned also catches values below 0 */
  return (unsigned)space < SPACE_COUNT ? SPACES[space].name : "?";
}

const char* ft_describe_space(ft_space space) {
  return (unsigned)space < SPACE_COUNT ? SPACES[space].description : "?";
}

int32_t ft_find_gamut(const char* name, ft_gamut* gamut) {
  if(name != NULL && strcmp(name, "soft") == 0) {
    *gamut = FT_GAMUT_SOFT;
    return 0;
  }
  if(name != NULL && strcmp(name, "clip") == 0) {
    *gamut = FT_GAMUT_CLIP;
    return 0;
  }
  return -1;
}

void ft_get_space_primaries(ft_space space, double xy[8]) {
  /* an unknown space gives sRGB's */
  size_t index = (unsigned)space < SPACE_COUNT ? (size_t)space : 0;
  memcpy(xy, SPACES[index].xy, 8 * sizeof *xy);
}

int32_t ft_invert_matrix3(const double matrix[9], double inverse[9]) {
  const double* m = matrix;
  /* the determinant, by the first row */
  double determinant = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                       m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
  if(!(fabs(determinant) > 1e-12)) return -1;
  /* the adjugate (transposed cofactors) over the determinant */
  inverse[0] = (m[4] * m[8] - m[5] * m[7]) / determinant;
  inverse[1] = (m[2] * m[7] - m[1] * m[8]) / determinant;
  inverse[2] = (m[1] * m[5] - m[2] * m[4]) / determinant;
  inverse[3] = (m[5] * m[6] - m[3] * m[8]) / determinant;
  inverse[4] = (m[0] * m[8] - m[2] * m[6]) / determinant;
  inverse[5] = (m[2] * m[3] - m[0] * m[5]) / determinant;
  inverse[6] = (m[3] * m[7] - m[4] * m[6]) / determinant;
  inverse[7] = (m[1] * m[6] - m[0] * m[7]) / determinant;
  inverse[8] = (m[0] * m[4] - m[1] * m[3]) / determinant;
  return 0;
}

/* product = a b, 3x3 row-major */
static void multiply_matrices(const double a[9], const double b[9],
                              double product[9]) {
  for(uint32_t row = 0; row < 3; row++) {
    for(uint32_t column = 0; column < 3; column++) {
      product[3 * row + column] = a[3 * row] * b[column] +
                                  a[3 * row + 1] * b[3 + column] +
                                  a[3 * row + 2] * b[6 + column];
    }
  }
}

/* result = matrix vector */
static void apply_matrix(const double matrix[9], const double vector[3],
                         double result[3]) {
  result[0] =
    matrix[0] * vector[0] + matrix[1] * vector[1] + matrix[2] * vector[2];
  result[1] =
    matrix[3] * vector[0] + matrix[4] * vector[1] + matrix[5] * vector[2];
  result[2] =
    matrix[6] * vector[0] + matrix[7] * vector[1] + matrix[8] * vector[2];
}

void ft_compute_rgb_to_xyz(const double xy[8], double matrix[9]) {
  /* the columns: each primary's XYZ (Y = 1), then scaled so that
   * R + G + B = white */
  double primaries[9];
  for(uint32_t primary = 0; primary < 3; primary++) {
    double x = xy[2 * primary];
    double y = xy[2 * primary + 1];
    primaries[primary] = x / y;
    primaries[3 + primary] = 1;
    primaries[6 + primary] = (1 - x - y) / y;
  }
  double white[3] = {xy[6] / xy[7], 1, (1 - xy[6] - xy[7]) / xy[7]};
  double inverse[9];
  double scales[3];
  ft_invert_matrix3(primaries, inverse);
  apply_matrix(inverse, white, scales);
  for(uint32_t index = 0; index < 9; index++) {
    matrix[index] = primaries[index] * scales[index % 3];
  }
}

double ft_encode_space(ft_space space, double value) {
  if(!(value > 0)) return 0;
  /* exact: 1.055 - 0.055 is not 1 in double */
  if(value >= 1) return 1;
  if(space == FT_SPACE_ADOBE_RGB) return pow(value, 256.0 / 563.0);
  if(space == FT_SPACE_REC2020) {
    /* the BT.2020 / BT.709 curve */
    if(value < 0.018053968510807) return 4.5 * value;
    return 1.09929682680944 * pow(value, 0.45) - 0.09929682680944;
  }
  /* the sRGB curve, also Display P3's */
  if(value <= 0.0031308) return 12.92 * value;
  return 1.055 * pow(value, 1 / 2.4) - 0.055;
}

double ft_decode_space(ft_space space, double value) {
  if(!(value > 0)) return 0;
  if(value >= 1) return 1;
  if(space == FT_SPACE_ADOBE_RGB) return pow(value, 563.0 / 256.0);
  if(space == FT_SPACE_REC2020) {
    if(value < 0.081242858298635) return value / 4.5;
    return pow((value + 0.09929682680944) / 1.09929682680944, 1 / 0.45);
  }
  if(value <= 0.04045) return value / 12.92;
  return pow((value + 0.055) / 1.055, 2.4);
}

/* Ottosson's M1 (XYZ D65 -> LMS) with each row scaled (by 1.0001,
 * 0.99997, 0.99952) so that D65 from its xy (0.3127, 0.3290), the white
 * of every space here, is LMS 1, 1, 1: greys get a = b = 0 exactly (the
 * published M1 gives white a chroma of ~1e-4, which the gamut step near
 * L = 1 would tint). M2: LMS' -> Lab as published. */
static const double XYZ_TO_LMS[9] = {
  0.8190224426, 0.3619062605, -0.1288737860, 0.0329836559, 0.9292868603,
  0.0361446659, 0.0481771938, 0.2642395277,  0.6335478283,
};
static const double LMS_TO_LAB[9] = {
  0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951,  -2.4285922050,
  0.4505937099, 0.0259040371, 0.7827717662,  -0.8086757660,
};

/* the exact inverses of the two above (computed once, in rational
 * arithmetic, then rounded to double) */
static const double LMS_TO_XYZ[9] = {
  1.2268798683314697,    -0.55781499359524622, 0.28139105244024526,
  -0.040575747350531759, 1.1122868053781236,   -0.071711058134508787,
  -0.076372944091338227, -0.42149332243093923, 1.5869240173438854,
};
static const double LAB_TO_LMS[9] = {
  0.9999999984505199, 0.39633779217376786,   0.21580375806075877,
  1.0000000088817607, -0.10556134232365634,  -0.063854174771705893,
  1.0000000546724108, -0.089484182094965753, -1.2914855378640917,
};

void ft_convert_xyz_to_oklab(const double xyz[3], double lab[3]) {
  double lms[3];
  apply_matrix(XYZ_TO_LMS, xyz, lms);
  lms[0] = cbrt(lms[0]);
  lms[1] = cbrt(lms[1]);
  lms[2] = cbrt(lms[2]);
  apply_matrix(LMS_TO_LAB, lms, lab);
}

void ft_convert_oklab_to_xyz(const double lab[3], double xyz[3]) {
  double lms[3];
  apply_matrix(LAB_TO_LMS, lab, lms);
  lms[0] = lms[0] * lms[0] * lms[0];
  lms[1] = lms[1] * lms[1] * lms[1];
  lms[2] = lms[2] * lms[2] * lms[2];
  apply_matrix(LMS_TO_XYZ, lms, xyz);
}

/* The largest chroma at (lightness, hue) inside the gamut of from_xyz,
 * by bisection. The slack scales with the luminance (Y ~ L^3): an
 * absolute one would stretch the edge by tens of percent near black. */
static double find_boundary(const double from_xyz[9], double lightness,
                            double hue) {
  if(lightness <= 0 || lightness >= 1) return 0;
  double low = 0;
  double high = MAX_CHROMA;
  double hue_cos = cos(hue);
  double hue_sin = sin(hue);
  double tolerance = 1e-7 * lightness * lightness * lightness;
  if(tolerance < 1e-12) tolerance = 1e-12;
  for(uint32_t step = 0; step < BOUNDARY_STEPS; step++) {
    double chroma = 0.5 * (low + high);
    double lab[3] = {lightness, chroma * hue_cos, chroma * hue_sin};
    double xyz[3];
    double rgb[3];
    ft_convert_oklab_to_xyz(lab, xyz);
    apply_matrix(from_xyz, xyz, rgb);
    bool is_inside = true;
    for(uint32_t channel = 0; channel < 3; channel++) {
      if(!(rgb[channel] >= -tolerance && rgb[channel] <= 1 + tolerance)) {
        is_inside = false;
      }
    }
    if(is_inside) {
      low = chroma;
    } else {
      high = chroma;
    }
  }
  return low;
}

int32_t ft_compile_output(ft_space space, ft_gamut gamut, ft_output* output,
                          ft_err* error) {
  if((unsigned)space >= SPACE_COUNT) {
    return ft_fail(error, "output: unknown colour space %d", (int)space);
  }
  if(gamut != FT_GAMUT_SOFT && gamut != FT_GAMUT_CLIP) {
    return ft_fail(error, "output: unknown gamut mode %d", (int)gamut);
  }
  memset(output, 0, sizeof *output);
  output->space = space;
  output->gamut = gamut;

  double xy[8];
  double target_to_xyz[9];
  double xyz_to_target[9];
  double rec2020_to_xyz[9];
  double xyz_to_rec2020[9];
  double rec2020_to_target[9];
  ft_get_space_primaries(space, xy);
  ft_compute_rgb_to_xyz(xy, target_to_xyz);
  ft_invert_matrix3(target_to_xyz, xyz_to_target);
  ft_get_space_primaries(FT_SPACE_REC2020, xy);
  ft_compute_rgb_to_xyz(xy, rec2020_to_xyz);
  ft_invert_matrix3(rec2020_to_xyz, xyz_to_rec2020);
  multiply_matrices(xyz_to_target, rec2020_to_xyz, rec2020_to_target);
  for(uint32_t index = 0; index < 9; index++) {
    output->rec2020_to_target[index] = (float)rec2020_to_target[index];
    output->to_xyz[index] = target_to_xyz[index];
    output->from_xyz[index] = xyz_to_target[index];
  }
  if(gamut == FT_GAMUT_SOFT) {
    for(uint32_t step = 0; step <= FT_OUTPUT_LIGHTNESS_STEPS; step++) {
      for(uint32_t hue_step = 0; hue_step <= FT_OUTPUT_HUE_STEPS; hue_step++) {
        double lightness = (double)step / FT_OUTPUT_LIGHTNESS_STEPS;
        double hue = 2 * PI * hue_step / FT_OUTPUT_HUE_STEPS;
        output->target_boundary[step][hue_step] =
          (float)find_boundary(xyz_to_target, lightness, hue);
        output->source_boundary[step][hue_step] =
          (float)find_boundary(xyz_to_rec2020, lightness, hue);
      }
    }
  }
  for(uint32_t index = 0; index < FT_OUTPUT_CURVE_SIZE; index++) {
    /* the table is indexed by sqrt(value): entry index holds value x^2 */
    double x = (double)index / (FT_OUTPUT_CURVE_SIZE - 1);
    output->curve[index] = (float)ft_encode_space(space, x * x);
  }
  return 0;
}

/* Bilinear lookup in a boundary table; hue in radians, any range. */
static double look_up_boundary(
  const float table[FT_OUTPUT_LIGHTNESS_STEPS + 1][FT_OUTPUT_HUE_STEPS + 1],
  double lightness, double hue) {
  double x = lightness * FT_OUTPUT_LIGHTNESS_STEPS;
  double y = hue / (2 * PI) * FT_OUTPUT_HUE_STEPS;
  /* x - x is 0 only for a finite x */
  if(x - x != 0 || y - y != 0) return 0;
  /* wrap the hue into 0..steps */
  y -= floor(y / FT_OUTPUT_HUE_STEPS) * FT_OUTPUT_HUE_STEPS;
  if(x < 0) x = 0;
  if(x > FT_OUTPUT_LIGHTNESS_STEPS) x = FT_OUTPUT_LIGHTNESS_STEPS;
  int32_t row = (int32_t)x;
  int32_t column = (int32_t)y;
  if(row >= FT_OUTPUT_LIGHTNESS_STEPS) row = FT_OUTPUT_LIGHTNESS_STEPS - 1;
  if(column >= FT_OUTPUT_HUE_STEPS) column = FT_OUTPUT_HUE_STEPS - 1;
  double row_weight = x - row;
  double column_weight = y - column;
  return (1 - row_weight) * ((1 - column_weight) * table[row][column] +
                             column_weight * table[row][column + 1]) +
         row_weight * ((1 - column_weight) * table[row + 1][column] +
                       column_weight * table[row + 1][column + 1]);
}

/* Compress chroma above the knee so that source maps onto target (the
 * ACES gamut compressor's power curve, p = 1.2): slope 1 at the knee,
 * continuous, strictly increasing, below target for every chroma. */
static double compress_chroma(double chroma, double knee, double source,
                              double target) {
  double above_knee = chroma - knee;
  double source_room = source - knee;
  double target_room = target - knee;
  if(above_knee <= 0) return chroma;
  if(target_room <= 0) return target;
  /* the source fits: only clamp */
  if(source_room <= target_room) return chroma < target ? chroma : target;
  double scale =
    source_room /
    pow(pow(source_room / target_room, COMPRESS_POWER) - 1, 1 / COMPRESS_POWER);
  return knee + above_knee / pow(1 + pow(above_knee / scale, COMPRESS_POWER),
                                 1 / COMPRESS_POWER);
}

/* each channel clamped to 0..1 */
static void clamp_rgb(float rgb[3]) {
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(!(rgb[channel] > 0)) {
      rgb[channel] = 0;
    } else if(!(rgb[channel] < 1)) {
      rgb[channel] = 1;
    }
  }
}

void ft_map_output_gamut(const ft_output* output, float rgb[3]) {
  /* NaN or infinity in: black */
  if(is_finite_float(rgb[0]) == false ||
     is_finite_float(rgb[1]) == false ||
     is_finite_float(rgb[2]) == false) {
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;
    return;
  }
  if(output->gamut == FT_GAMUT_CLIP || output->space == FT_SPACE_REC2020) {
    clamp_rgb(rgb);
    return;
  }
  double linear[3] = {rgb[0], rgb[1], rgb[2]};
  double xyz[3];
  double lab[3];
  apply_matrix(output->to_xyz, linear, xyz);
  ft_convert_xyz_to_oklab(xyz, lab);
  double chroma = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
  double hue = atan2(lab[2], lab[1]);
  /* a huge float overflows the cube root's range: also black */
  if(chroma - chroma != 0 || hue - hue != 0 || lab[0] - lab[0] != 0) {
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 0;
    return;
  }
  double lightness = lab[0] > 0 ? (lab[0] < 1 ? lab[0] : 1) : 0;
  double target_chroma =
    look_up_boundary(output->target_boundary, lightness, hue);
  double knee = FT_OUTPUT_KNEE * target_chroma;
  /* untouched when it fits */
  if(chroma <= knee || chroma <= FT_OUTPUT_GREY_CHROMA) {
    clamp_rgb(rgb);
    return;
  }
  double source_chroma =
    look_up_boundary(output->source_boundary, lightness, hue);
  /* beyond Rec.2020: compress from there */
  if(source_chroma < chroma) source_chroma = chroma;
  chroma = compress_chroma(chroma, knee, source_chroma, target_chroma);
  lab[0] = lightness;
  lab[1] = chroma * cos(hue);
  lab[2] = chroma * sin(hue);
  ft_convert_oklab_to_xyz(lab, xyz);
  apply_matrix(output->from_xyz, xyz, linear);
  rgb[0] = (float)linear[0];
  rgb[1] = (float)linear[1];
  rgb[2] = (float)linear[2];
  /* the table interpolation can land a hair outside */
  clamp_rgb(rgb);
}

/* the transfer curve through the table, linear between entries */
static float encode_value(const ft_output* output, float value) {
  if(!(value > 0)) return output->curve[0];
  if(value >= 1) return output->curve[FT_OUTPUT_CURVE_SIZE - 1];
  float position = sqrtf(value) * (FT_OUTPUT_CURVE_SIZE - 1);
  int32_t index = (int32_t)position;
  if(index >= FT_OUTPUT_CURVE_SIZE - 1) {
    return output->curve[FT_OUTPUT_CURVE_SIZE - 1];
  }
  float fraction = position - (float)index;
  return output->curve[index] +
         fraction * (output->curve[index + 1] - output->curve[index]);
}

void ft_apply_output(const ft_output* output, const float* input,
                     float* encoded, size_t pixel_count) {
  const float* m = output->rec2020_to_target;
  for(size_t pixel = 0; pixel < pixel_count; pixel++) {
    float red = input[3 * pixel];
    float green = input[3 * pixel + 1];
    float blue = input[3 * pixel + 2];
    /* NaN or infinity anywhere: black */
    if(is_finite_float(red) == false ||
       is_finite_float(green) == false ||
       is_finite_float(blue) == false) {
      red = 0;
      green = 0;
      blue = 0;
    }
    float target[3];
    target[0] = m[0] * red + m[1] * green + m[2] * blue;
    target[1] = m[3] * red + m[4] * green + m[5] * blue;
    target[2] = m[6] * red + m[7] * green + m[8] * blue;
    ft_map_output_gamut(output, target);
    for(uint32_t channel = 0; channel < 3; channel++) {
      encoded[3 * pixel + channel] = encode_value(output, target[channel]);
    }
  }
}

/* lowbias32 (Chris Wellons): a well-mixed 32-bit hash. uint32_t
 * arithmetic wraps at 2^32, which the hash relies on. */
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

void ft_quantise_output8(const float* encoded, uint8_t* bytes,
                         size_t pixel_count, size_t first_pixel) {
  for(size_t index = 0; index < pixel_count * 3; index++) {
    float value = encoded[index];
    if(!(value > 0)) {
      bytes[index] = 0;
      continue;
    }
    if(value >= 1) {
      bytes[index] = 255;
      continue;
    }
    /* TPDF dither: the difference of two uniform values in 0..1 gives a
     * triangle-shaped noise of +-1 LSB. The sample's number seeds both,
     * so tiles and threads give the same bytes. */
    uint32_t sample = (uint32_t)(first_pixel * 3 + index);
    double first = hash32(2 * sample) / 4294967296.0;
    double second = hash32(2 * sample + 1) / 4294967296.0;
    double quantised = floor(value * 255.0 + 0.5 + (first - second));
    if(quantised < 0) quantised = 0;
    if(quantised > 255) quantised = 255;
    bytes[index] = (uint8_t)quantised;
  }
}

void ft_quantise_output16(const float* encoded, uint16_t* values,
                          size_t pixel_count) {
  for(size_t index = 0; index < pixel_count * 3; index++) {
    float value = encoded[index];
    if(!(value > 0)) {
      values[index] = 0;
    } else if(value >= 1) {
      values[index] = 65535;
    } else {
      /* + 0.5 rounds to the nearest */
      values[index] = (uint16_t)(value * 65535.0f + 0.5f);
    }
  }
}
