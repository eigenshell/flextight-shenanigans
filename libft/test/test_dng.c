/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "test/ftest.h"

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long m;
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    m = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)m);
    CHECK(b && fread(b, 1, (size_t)m, f) == (size_t)m);
    fclose(f);
    *n = (size_t)m;
    return b;
}

/* offset of tag's 12-byte IFD entry in a buffer from ft_dng_write */
static size_t entry_of(const unsigned char *b, unsigned tag)
{
    size_t ifd = b[4] | b[5] << 8 | (size_t)b[6] << 16 | (size_t)b[7] << 24;
    unsigned cnt = b[ifd] | b[ifd + 1] << 8, i;
    for (i = 0; i < cnt; i++) {
        size_t p = ifd + 2 + 12 * i;
        if ((unsigned)(b[p] | b[p + 1] << 8) == tag)
            return p;
    }
    return 0;
}

static void put16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

/* a file whose h one-row strips all point at the same row: every strip is
   valid alone, but w x h pixels cannot be in the file */
static unsigned char *overlap_file(unsigned long w, unsigned long h,
                                   size_t *n)
{
    unsigned long ne = 11, ifd = 8, offs = ifd + 2 + 12 * ne + 4;
    unsigned long lens = offs + 4 * h, bps = lens + 4 * h, pix = bps + 8;
    unsigned char *b, *p;
    unsigned long i;
    *n = pix + w * 6;
    b = calloc(*n, 1);
    CHECK(b != NULL);
    memcpy(b, "II*\0", 4);
    put32(b + 4, ifd);
    put16(b + ifd, ne);
    p = b + ifd + 2;
#define ENT(tag, type, cnt, val) do { put16(p, tag); put16(p + 2, type); \
    put32(p + 4, cnt); put32(p + 8, val); p += 12; } while (0)
    ENT(256, 4, 1, w);
    ENT(257, 4, 1, h);
    ENT(258, 3, 3, bps);
    ENT(259, 3, 1, 1);
    ENT(262, 3, 1, 34892);
    ENT(273, 4, h, offs);
    ENT(277, 3, 1, 3);
    ENT(278, 4, 1, 1);
    ENT(279, 4, h, lens);
    ENT(284, 3, 1, 1);
    ENT(50706, 1, 4, 0x00040101);
#undef ENT
    put16(b + bps, 16);
    put16(b + bps + 2, 16);
    put16(b + bps + 4, 16);
    for (i = 0; i < h; i++) {
        put32(b + offs + 4 * i, pix);
        put32(b + lens + 4 * i, w * 6);
    }
    return b;
}

int main(void)
{
    const char *path = "test/dng_test_out.dng";
    ft_image a, b;
    ft_err e;
    unsigned char *buf, *bad;
    size_t n, k, p;
    int i;

    /* 300 x 400: several strips (262144 B each) */
    ft_image_init(&a);
    CHECK(ft_image_alloc(&a, 300, 400, &e) == 0);
    for (k = 0; k < (size_t)300 * 400 * 3; k++)
        a.rgb[k] = (float)((k * 7919) % 70001) / 70000.0f;
    a.rgb[0] = -0.5f;                       /* clamps to 0 */
    a.rgb[1] = 1.7f;                        /* clamps to 1 */
    a.ppi = 1500;
    a.xyz_to_rgb[4] = 1.8760108;
    strcpy(a.desc, "share frame 4, 1500 ppi, bin 1");
    CHECK(ft_dng_write(path, &a, &e) == 0);

    ft_image_init(&b);
    strcpy(b.make, "x");
    CHECK(ft_dng_read(path, &b, &e) == 0);
    CHECK(b.w == 300 && b.h == 400);
    /* the same 16-bit code back (codes, not floats: x87 excess precision) */
    for (k = 0; k < (size_t)300 * 400 * 3; k++) {
        float v = a.rgb[k];
        long want = v <= 0 ? 0 : v >= 1 ? 65535 : (long)(v * 65535.0f + 0.5f);
        long got = (long)floor(b.rgb[k] * 65535.0 + 0.5);
        if (got != want)
            CHECK(got == want);
    }
    CHECK(b.rgb[0] == 0 && b.rgb[1] == 1);
    CHECK_NEAR(b.ppi, 1500, 1e-9);
    CHECK_NEAR(b.xyz_to_rgb[4], 1.8760108, 1e-7);
    CHECK_NEAR(b.xyz_to_rgb[0], a.xyz_to_rgb[0], 1e-7);
    CHECK(strcmp(b.desc, a.desc) == 0);
    CHECK(strcmp(b.make, "Imacon") == 0);
    CHECK(strcmp(b.model, "Flextight Precision II") == 0);
    /* write -> read -> write is byte-identical in the pixels */
    {
        ft_image c;
        ft_dng_write("test/dng_test_out2.dng", &b, &e);
        ft_image_init(&c);
        CHECK(ft_dng_read("test/dng_test_out2.dng", &c, &e) == 0);
        CHECK(memcmp(b.rgb, c.rgb, (size_t)300 * 400 * 3 * sizeof *b.rgb)
              == 0);
        ft_image_free(&c);
        remove("test/dng_test_out2.dng");
    }
    ft_image_free(&b);

    /* refusals: each with the reason, never a crash */
    buf = slurp(path, &n);
    bad = malloc(n);
    CHECK(bad != NULL);
    CHECK(ft_dng_parse(buf, n, &b, &e) == 0);
    ft_image_free(&b);
    CHECK(ft_dng_parse(buf, 7, &b, &e) == -1);
    CHECK(ft_dng_parse(buf, n - 1, &b, &e) == -1);     /* last strip short */
    CHECK(strstr(e.msg, "strip") != NULL);
    CHECK(b.rgb == NULL);
    CHECK(ft_dng_parse(buf, n / 2, &b, &e) == -1);
    memcpy(bad, buf, n);
    bad[0] = 'M';                                       /* big-endian */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "little-endian") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 259);
    CHECK(p);
    bad[p + 8] = 7;                                     /* JPEG compressed */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "compressed") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 262);
    bad[p + 8] = 0x23;                                  /* 32803: CFA */
    bad[p + 9] = 0x80;
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "LinearRaw") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 50706);
    bad[p] = 0x13;                                      /* tag 50707 + ... */
    bad[p + 1] = 0x00;                                  /* -> tag 19 */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "not a DNG") != NULL);
    /* random corruption of the header and IFD: no crash, no leak of a
     * half-filled image */
    srand(5);
    for (i = 0; i < 3000; i++) {
        size_t hdr = entry_of(buf, 50721) + 12;
        int j;
        memcpy(bad, buf, n);
        for (j = 0; j < 1 + i % 4; j++)
            bad[(size_t)rand() % hdr] = (unsigned char)rand();
        ft_image_init(&b);
        if (ft_dng_parse(bad, n, &b, &e) == 0) {
            CHECK(b.rgb != NULL && b.w >= 1 && b.h >= 1);
            ft_image_free(&b);
        } else {
            CHECK(b.rgb == NULL);
        }
    }
    /* overlapping strips: w x h x 6 wraps a 32-bit size_t */
    {
        unsigned char *ov = overlap_file(34179, 41887, &k);
        ft_image_init(&b);
        CHECK(ft_dng_parse(ov, k, &b, &e) == -1);
        CHECK(strstr(e.msg, "do not fit") != NULL);
        CHECK(b.rgb == NULL);
        free(ov);
    }
    /* IFD offset near 2^32: ifd + 2 wraps a 32-bit unsigned long */
    memcpy(bad, buf, n);
    put32(bad + 4, 0xfffffffeUL);
    ft_image_init(&b);
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(b.rgb == NULL);
    /* a size no platform can hold in floats */
    ft_image_init(&b);
    CHECK(ft_image_alloc(&b, 2000000000, 2000000000, &e) == -1);
    CHECK(strstr(e.msg, "too large") != NULL);
    CHECK(b.rgb == NULL);
    CHECK(ft_dng_read("test/no_such_file.dng", &b, &e) == -1);

    /* NoiseProfile (51041, 6 doubles): none without a profile, an exact
     * round trip with one, and a bad value reads as none */
    CHECK(entry_of(buf, 51041) == 0);
    {
        static const double np[6] = { 1.5e-4, 3e-8, 1.4e-4, 3.2e-8,
                                      1.6e-4, 2.8e-8 };
        size_t m, at;
        unsigned char *nb;
        double neg = -1;
        uint64_t u;
        a.has_noise = 1;
        memcpy(a.noise, np, sizeof np);
        CHECK(ft_dng_write(path, &a, &e) == 0);
        nb = slurp(path, &m);
        k = entry_of(nb, 51041);
        CHECK(k != 0);
        CHECK((nb[k + 2] | nb[k + 3] << 8) == 12);         /* DOUBLE */
        CHECK(nb[k + 4] == 6 && nb[k + 5] == 0);
        ft_image_init(&b);
        CHECK(ft_dng_parse(nb, m, &b, &e) == 0);
        CHECK(b.has_noise);
        for (i = 0; i < 6; i++)
            CHECK(b.noise[i] == np[i]);
        ft_image_free(&b);
        /* S_r = -1 */
        at = nb[k + 8] | nb[k + 9] << 8 | (size_t)nb[k + 10] << 16 |
             (size_t)nb[k + 11] << 24;
        memcpy(&u, &neg, 8);
        for (i = 0; i < 8; i++)
            nb[at + i] = (unsigned char)(u >> 8 * i);
        ft_image_init(&b);
        CHECK(ft_dng_parse(nb, m, &b, &e) == 0);
        CHECK(!b.has_noise);
        ft_image_free(&b);
        free(nb);
        /* the writer drops an unusable profile */
        a.noise[1] = 0;
        CHECK(ft_dng_write(path, &a, &e) == 0);
        nb = slurp(path, &m);
        CHECK(entry_of(nb, 51041) == 0);
        free(nb);
        a.has_noise = 0;
    }
    CHECK(strstr(e.msg, "no_such_file") != NULL);
    /* ft_dng_as_stored is the write + read round trip, exactly: pixels
     * (edges, out of range), ppi over 1000, a usable noise profile */
    {
        ft_image s, r;
        int x;
        ft_image_init(&s);
        ft_image_init(&r);
        CHECK(ft_image_alloc(&s, 37, 23, &e) == 0);
        for (x = 0; x < 37 * 23 * 3; x++)
            s.rgb[x] = (float)((x * 7919 % 10007) / 9000.0 - 0.05);
        s.rgb[0] = 0.5f / 65535.0f;
        s.rgb[1] = 1.0f - 0.4f / 65535.0f;
        s.ppi = 2449.87654321;
        s.has_noise = 1;
        for (x = 0; x < 6; x++)
            s.noise[x] = 1.234567891e-5 * (x + 1);
        CHECK(ft_dng_write(path, &s, &e) == 0);
        CHECK(ft_dng_read(path, &r, &e) == 0);
        ft_dng_as_stored(&s);
        CHECK(memcmp(s.rgb, r.rgb, sizeof(float) * 37 * 23 * 3) == 0);
        CHECK(s.ppi == r.ppi && s.ppi != 2449.87654321);
        CHECK(s.has_noise && r.has_noise &&
              memcmp(s.noise, r.noise, sizeof s.noise) == 0);
        s.noise[3] = 0;                 /* unusable: dropped by both */
        CHECK(ft_dng_write(path, &s, &e) == 0);
        ft_image_free(&r);
        ft_image_init(&r);
        CHECK(ft_dng_read(path, &r, &e) == 0);
        ft_dng_as_stored(&s);
        CHECK(!s.has_noise && !r.has_noise);
        ft_image_free(&s);
        ft_image_free(&r);
    }
    free(buf);
    free(bad);
    ft_image_free(&a);
    remove(path);
    return TEST_DONE();
}
