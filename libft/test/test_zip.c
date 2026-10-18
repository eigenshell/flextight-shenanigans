/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_zip: CRC-32, a stored archive's exact bytes, refusals. With a path
 * argument it writes a two-entry archive there (tests/test_libft.py opens
 * it with Python's zipfile). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_zip.h"
#include "test/ftest.h"

static unsigned rd16(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static unsigned long rd32(const unsigned char *p)
{
    return rd16(p) | (unsigned long)rd16(p + 2) << 16;
}

int main(int argc, char **argv)
{
    /* local header (30 + 5 name) + 3 data + central (46 + 5) + end (22) */
    static const unsigned char ONE[111] = {
        'P', 'K', 3, 4, 10, 0, 0, 0, 0, 0, 0, 0, 0x21, 0,
        0xc2, 0x41, 0x24, 0x35, 3, 0, 0, 0, 3, 0, 0, 0, 5, 0, 0, 0,
        'a', '.', 't', 'x', 't', 'a', 'b', 'c',
        'P', 'K', 1, 2, 10, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0x21, 0,
        0xc2, 0x41, 0x24, 0x35, 3, 0, 0, 0, 3, 0, 0, 0, 5, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        'a', '.', 't', 'x', 't',
        'P', 'K', 5, 6, 0, 0, 0, 0, 1, 0, 1, 0, 51, 0, 0, 0, 38, 0, 0, 0,
        0, 0
    };
    ft_zip z;
    unsigned char *out;
    size_t n;
    ft_err e;
    char longname[70000];

    CHECK(ft_crc32(0, "123456789", 9) == 0xCBF43926UL);
    CHECK(ft_crc32(ft_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926UL);
    CHECK(ft_crc32(0, "", 0) == 0);

    ft_zip_init(&z);
    CHECK(ft_zip_add(&z, "a.txt", "abc", 3, &e) == 0);
    CHECK(ft_zip_finish(&z, &out, &n, &e) == 0);
    CHECK(n == sizeof ONE && memcmp(out, ONE, n) == 0);
    free(out);

    /* an empty archive is just the end record */
    ft_zip_init(&z);
    CHECK(ft_zip_finish(&z, &out, &n, &e) == 0);
    CHECK(n == 22 && rd32(out) == 0x06054b50UL && rd16(out + 10) == 0);
    free(out);

    /* refusals: the archive is still usable after one */
    ft_zip_init(&z);
    CHECK(ft_zip_add(&z, "", "x", 1, &e) == -1);
    CHECK(strcmp(e.msg, "zip: bad name") == 0);
    CHECK(ft_zip_add(&z, "../x", "x", 1, &e) == -1);
    CHECK(strcmp(e.msg, "zip: bad name ../x") == 0);
    CHECK(ft_zip_add(&z, "/x", "x", 1, &e) == -1);
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(ft_zip_add(&z, longname, "x", 1, &e) == -1);
    CHECK(strcmp(e.msg, "zip: name too long") == 0);
    CHECK(ft_zip_add(&z, "frame01.jpg", "\377\330\377\331", 4, &e) == 0);
    CHECK(ft_zip_add(&z, "frame01.jpg.txt", "emulsion=x\n", 11, &e) == 0);
    CHECK(ft_zip_finish(&z, &out, &n, &e) == 0);
    CHECK(rd16(out + n - 12) == 2);                 /* entries */
    CHECK(rd32(out + n - 6) == 30 + 11 + 4 + 30 + 15 + 11); /* cd offset */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        CHECK(f && fwrite(out, 1, n, f) == n && fclose(f) == 0);
    }
    free(out);
    return TEST_DONE();
}
