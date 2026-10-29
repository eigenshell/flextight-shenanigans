/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft.h"
#include "test/ftest.h"

int main(void) {
  ft_err e;
  CHECK(ft_fail(&e, "line %d: %s", 7, "bad") == -1);
  CHECK(strcmp(e.message, "line 7: bad") == 0);
  CHECK(ft_fail(NULL, "ignored") == -1);
  return TEST_DONE();
}
