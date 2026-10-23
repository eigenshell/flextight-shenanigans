/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal unit-test macros for libft (any hosted C99 toolchain). */
#ifndef FTEST_H
#define FTEST_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int ft_checks;

#define CHECK(c)                                                               \
  do {                                                                         \
    ft_checks++;                                                               \
    if(!(c)) {                                                                 \
      fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c);            \
      exit(1);                                                                 \
    }                                                                          \
  } while(0)

#define CHECK_NEAR(a, b, tol)                                                  \
  do {                                                                         \
    double a_ = (a), b_ = (b);                                                 \
    ft_checks++;                                                               \
    if(!(fabs(a_ - b_) <= (tol))) {                                            \
      fprintf(                                                                 \
          stderr, "%s:%d: FAIL: %s = %.6g, want %.6g +- %g\n", __FILE__,       \
          __LINE__, #a, a_, b_, (double)(tol)                                  \
      );                                                                       \
      exit(1);                                                                 \
    }                                                                          \
  } while(0)

#define TEST_DONE() (printf("%s: %d checks ok\n", __FILE__, ft_checks), 0)
#endif
