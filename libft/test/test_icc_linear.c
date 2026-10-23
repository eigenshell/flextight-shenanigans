/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The linear scanner-RGB profile of the 2b negative TIFFs, against Little
 * CMS (tests only; libft does not link it). */
#include "ft_icc.h"
#include "ft_image.h"
#include "ft_output.h"
#include "test/ftest.h"
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rd32(const unsigned char *p) {
  return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
         (unsigned long)p[2] << 8 | p[3];
}

int main(void) {
  static const char DESC[] = "scanner RGB, linear (flextight)";
  unsigned char p[FT_ICC_MAX];
  double inv[9], a[9];
  ft_image im;
  size_t n;
  int i, k;

  ft_image_init(&im); /* xyz_to_rgb: sRGB until profiled */
  /* 240 header and tag table + desc 12 + 32 + 78 (-> 124) + cprt 36 +
   * four XYZ 80 + an empty curv 12 */
  n = ft_icc_build_linear(im.xyz_to_rgb, DESC, p, sizeof p);
  CHECK(n == 492);
  CHECK(rd32(p) == n && memcmp(p + 12, "mntrRGB XYZ ", 12) == 0);
  CHECK(ft_icc_build_linear(im.xyz_to_rgb, DESC, p, 491) == 0);
  {
    static const double sing[9] = {1, 2, 3, 2, 4, 6, 0, 0, 1};
    CHECK(ft_icc_build_linear(sing, DESC, p, sizeof p) == 0);
  }
  { /* a long description is cut at 63 characters */
    char longd[200];
    cmsHPROFILE pr;
    char name[100];
    memset(longd, 'x', sizeof longd - 1);
    longd[sizeof longd - 1] = 0;
    n = ft_icc_build_linear(im.xyz_to_rgb, longd, p, sizeof p);
    pr = cmsOpenProfileFromMem(p, (cmsUInt32Number)n);
    CHECK(pr != NULL);
    cmsGetProfileInfoASCII(
        pr, cmsInfoDescription, "en", "US", name, sizeof name
    );
    CHECK(strlen(name) == 63);
    cmsCloseProfile(pr);
  }

  n = ft_icc_build_linear(im.xyz_to_rgb, DESC, p, sizeof p);
  {
    cmsHPROFILE prof = cmsOpenProfileFromMem(p, (cmsUInt32Number)n);
    cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
    cmsHTRANSFORM tr;
    cmsToneCurve *trc;
    char name[100];
    double worst = 0;
    CHECK(prof != NULL);
    CHECK(cmsIsMatrixShaper(prof));
    CHECK(cmsGetDeviceClass(prof) == cmsSigDisplayClass);
    trc = cmsReadTag(prof, cmsSigRedTRCTag);
    CHECK(trc != NULL && cmsIsToneCurveLinear(trc));
    cmsGetProfileInfoASCII(
        prof, cmsInfoDescription, "en", "US", name, sizeof name
    );
    CHECK(strcmp(name, DESC) == 0);
    /* linear 16-bit RGB decoded by lcms through the profile lands on
     * XYZ = xyz_to_rgb^-1 RGB, adapted to D50: dE2000 < 0.1 */
    CHECK(ft_mat3_inv(im.xyz_to_rgb, inv) == 0);
    ft_icc_bradford(a);
    tr = cmsCreateTransform(
        prof, TYPE_RGB_16, lab, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, 0
    );
    CHECK(tr != NULL);
    srand(5);
    for(i = 0; i < 4000; i++) {
      unsigned short px[3];
      double rgb[3], xyz[3], d50[3];
      cmsCIEXYZ x;
      cmsCIELab got, want;
      for(k = 0; k < 3; k++) {
        px[k] = (unsigned short)(rand() % 65536);
        rgb[k] = px[k] / 65535.0;
      }
      for(k = 0; k < 3; k++)
        xyz[k] = inv[3 * k] * rgb[0] + inv[3 * k + 1] * rgb[1] +
                 inv[3 * k + 2] * rgb[2];
      for(k = 0; k < 3; k++)
        d50[k] =
            a[3 * k] * xyz[0] + a[3 * k + 1] * xyz[1] + a[3 * k + 2] * xyz[2];
      x.X = d50[0];
      x.Y = d50[1];
      x.Z = d50[2];
      cmsXYZ2Lab(cmsD50_XYZ(), &want, &x);
      cmsDoTransform(tr, px, &got, 1);
      if(cmsCIE2000DeltaE(&got, &want, 1, 1, 1) > worst)
        worst = cmsCIE2000DeltaE(&got, &want, 1, 1, 1);
    }
    CHECK(worst < 0.1);
    /* and it is not sRGB: linear mid-grey is far lighter than sRGB's */
    {
      unsigned short g[3] = {13107, 13107, 13107}; /* 0.2 */
      cmsCIELab l;
      cmsDoTransform(tr, g, &l, 1);
      CHECK_NEAR(l.L, 51.8, 0.5); /* Y 0.2 -> L* 51.8 */
    }
    cmsDeleteTransform(tr);
    cmsCloseProfile(lab);
    cmsCloseProfile(prof);
  }
  return TEST_DONE();
}
