#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The browser tier: units the web page runs as WASM with a libc
# (wasi-libc). There the heap, string, ctype and formatting into buffers
# (snprintf, sscanf) work; files, streams, processes, threads, signals,
# clocks and the environment do not. purity.sh is the stricter box tier.
# Reads Linux/ELF `nm -u` output.
# Usage: sh test/purity_web.sh file.o...
bad=$(nm -u "$@" | awk 'NF == 2 { print $2 }' |
      grep -E '^(_IO_[a-z_]+|stdin|stdout|stderr|fopen(64)?|fdopen|freopen(64)?|fclose|fread|fwrite|fgets|fgetc|fputs|fputc|fprintf|vfprintf|fscanf|vfscanf|__isoc(99|23)_(v?f)?scanf|fflush|fseeko?(64)?|ftello?(64)?|feof|ferror|fileno|rewind|getc|putc|ungetc|setvbuf|tmpfile(64)?|printf|vprintf|puts|putchar|getchar|perror|__(v?f)?printf_chk|__fread_chk|__fgets_chk|remove|rename|open(at)?(64)?|close|read|write|lseek(64)?|f?stat(64)?|pthread_[a-z_]+|fork|exec[a-z]*|system|popen|signal|sigaction|raise|time|clock|clock_gettime|gettimeofday|getenv|exit)$' |
      sort -u)
if [ -n "$bad" ]; then
    echo "purity-web: forbidden imports in $*:" $bad
    exit 1
fi
echo "purity-web: $* use no files, streams, processes or threads"
