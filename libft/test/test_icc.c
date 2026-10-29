/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_icc against Little CMS (tests only; libft does not link it). */
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft_icc.h"
#include "ft_output.h"
#include "test/ftest.h"

static unsigned long rd32(const unsigned char* p) {
  return (unsigned long)p[0] << 24 |
         (unsigned long)p[1] << 16 |
         (unsigned long)p[2] << 8 |
         p[3];
}

/* XYZ D65 -> Lab D50 (Bradford), as lcms reports PCS Lab */
static void lab_d50(const double xyz65[3], cmsCIELab* lab) {
  double a[9], v[3];
  cmsCIEXYZ x;
  int i;
  ft_get_bradford_matrix(a);
  for(i = 0; i < 3; i++)
    v[i] =
      a[3 * i] * xyz65[0] + a[3 * i + 1] * xyz65[1] + a[3 * i + 2] * xyz65[2];
  x.X = v[0];
  x.Y = v[1];
  x.Z = v[2];
  cmsXYZ2Lab(cmsD50_XYZ(), lab, &x);
}

int main(void) {
  static const size_t SIZES[4] = {2524, 2532, 492, 2540};
  static ft_output o;
  unsigned char p[FT_ICC_MAX_BYTES], q[FT_ICC_MAX_BYTES];
  double a[9];
  ft_err e;
  int sp, i, k;

  /* Bradford D65 -> D50 maps the white to D50 (ICC's 0.9642 1 0.8249) */
  ft_get_bradford_matrix(a);
  {
    double w[3] = {0.3127 / 0.3290, 1, (1 - 0.3127 - 0.3290) / 0.3290};
    double d[3] = {0.9642, 1.0, 0.8249};
    for(i = 0; i < 3; i++)
      CHECK_NEAR(a[3 * i] * w[0] + a[3 * i + 1] * w[1] + a[3 * i + 2] * w[2],
                 d[i], 1e-9);
  }
  CHECK(ft_build_icc_profile(FT_SPACE_SRGB, p, 100) == 0); /* too small */
  CHECK(ft_build_icc_profile((ft_space)9, p, sizeof p) == 0);

  for(sp = 0; sp < 4; sp++) {
    size_t n = ft_build_icc_profile((ft_space)sp, p, sizeof p);
    cmsHPROFILE prof, lab;
    cmsHTRANSFORM tr;
    char name[100];
    double worst = 0;
    CHECK(n > 128 && n <= FT_ICC_MAX_BYTES && n % 4 == 0);
    /* header: size, v2.1, mntr RGB -> XYZ, acsp, D50 */
    CHECK(rd32(p) == n && rd32(p + 8) == 0x02100000UL);
    CHECK(memcmp(p + 12, "mntrRGB XYZ ", 12) == 0);
    CHECK(memcmp(p + 36, "acsp", 4) == 0);
    CHECK(rd32(p + 68) == 0xF6D6 &&
          rd32(p + 72) == 0x10000 &&
          rd32(p + 76) == 0xD32D);
    CHECK(rd32(p + 128) == 9);
    /* the same bytes every time */
    CHECK(ft_build_icc_profile((ft_space)sp, q, sizeof q) == n &&
          memcmp(p, q, n) == 0);
    /* 240 (header, tag table) + desc + cprt 36 + 4 XYZ 80 + curv:
     * sRGB 108 + 2060 = 2524; Adobe RGB 120 + 16 = 492 */
    CHECK(n == SIZES[sp]);
    prof = cmsOpenProfileFromMem(p, (cmsUInt32Number)n);
    CHECK(prof != NULL);
    CHECK(cmsGetColorSpace(prof) == cmsSigRgbData);
    CHECK(cmsGetPCS(prof) == cmsSigXYZData);
    CHECK(cmsGetDeviceClass(prof) == cmsSigDisplayClass);
    CHECK(cmsIsMatrixShaper(prof));
    cmsGetProfileInfoASCII(prof, cmsInfoDescription, "en", "US", name,
                           sizeof name);
    CHECK(strstr(name, ft_describe_space((ft_space)sp)) != NULL);
    CHECK(strstr(name, "(flextight)") != NULL);

    /* our encoding, decoded by lcms through our profile, lands where
     * the colour is: dE2000 < 0.1 on in-gamut colours */
    CHECK(ft_compile_output((ft_space)sp, FT_GAMUT_CLIP, &o, &e) == 0);
    lab = cmsCreateLab4Profile(NULL);
    tr = cmsCreateTransform(prof, TYPE_RGB_16, lab, TYPE_Lab_DBL,
                            INTENT_RELATIVE_COLORIMETRIC, 0);
    CHECK(tr != NULL);
    srand(17 + sp);
    for(i = 0; i < 4000; i++) {
      float in[3], enc[3], t[3];
      unsigned short px[3];
      double xyz[3];
      cmsCIELab got, want;
      /* a random colour of the target: target RGB -> Rec.2020 */
      for(k = 0; k < 3; k++) t[k] = (float)(rand() / (RAND_MAX + 1.0));
      {
        double inv[9], m[9];
        for(k = 0; k < 9; k++) m[k] = o.rec2020_to_target[k];
        ft_invert_matrix3(m, inv);
        for(k = 0; k < 3; k++)
          in[k] = (float)(inv[3 * k] * t[0] +
                          inv[3 * k + 1] * t[1] +
                          inv[3 * k + 2] * t[2]);
      }
      ft_apply_output(&o, in, enc, 1);
      ft_quantise_output16(enc, px, 1);
      cmsDoTransform(tr, px, &got, 1);
      for(k = 0; k < 3; k++)
        xyz[k] = o.to_xyz[3 * k] * t[0] +
                 o.to_xyz[3 * k + 1] * t[1] +
                 o.to_xyz[3 * k + 2] * t[2];
      lab_d50(xyz, &want);
      if(cmsCIE2000DeltaE(&got, &want, 1, 1, 1) > worst)
        worst = cmsCIE2000DeltaE(&got, &want, 1, 1, 1);
    }
    CHECK(worst < 0.1);
    cmsDeleteTransform(tr);
    cmsCloseProfile(lab);
    cmsCloseProfile(prof);
  }

  /* our sRGB agrees with lcms's own sRGB */
  {
    size_t n = ft_build_icc_profile(FT_SPACE_SRGB, p, sizeof p);
    cmsHPROFILE ours = cmsOpenProfileFromMem(p, (cmsUInt32Number)n);
    cmsHPROFILE theirs = cmsCreate_sRGBProfile();
    cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
    cmsHTRANSFORM t1 = cmsCreateTransform(ours, TYPE_RGB_8, lab, TYPE_Lab_DBL,
                                          INTENT_RELATIVE_COLORIMETRIC, 0);
    cmsHTRANSFORM t2 = cmsCreateTransform(theirs, TYPE_RGB_8, lab, TYPE_Lab_DBL,
                                          INTENT_RELATIVE_COLORIMETRIC, 0);
    double worst = 0;
    for(i = 0; i < 4096; i++) {
      unsigned char px[3];
      cmsCIELab l1, l2;
      px[0] = (unsigned char)(i * 37);
      px[1] = (unsigned char)(i * 101 >> 2);
      px[2] = (unsigned char)(i * 13 >> 1);
      cmsDoTransform(t1, px, &l1, 1);
      cmsDoTransform(t2, px, &l2, 1);
      if(cmsCIE2000DeltaE(&l1, &l2, 1, 1, 1) > worst)
        worst = cmsCIE2000DeltaE(&l1, &l2, 1, 1, 1);
    }
    CHECK(worst < 0.1);
    cmsDeleteTransform(t1);
    cmsDeleteTransform(t2);
    cmsCloseProfile(ours);
    cmsCloseProfile(theirs);
    cmsCloseProfile(lab);
  }
  return TEST_DONE();
}
