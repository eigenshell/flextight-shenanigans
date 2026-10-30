#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Public repo hygiene: no vendor data, recon notes, traces or scans.
# Usage: .github/hygiene.sh [<rev-range>]
#   Checks every file in HEAD, and with a range every path its commits
#   added too: a file added and then deleted stays in the history.
set -eu
shopt -s nocasematch

MAX_FILE_KB=512
bad=0

fail() {
  echo "::error file=$1::$1: $2"
  bad=1
}

# the reason a path may not be public, or nothing
reason_of() {
  case "$1" in
    notes/* | docs/* | traces/* | extracted/* | styles/* | fc403_eng/* | \
      fc4891win/* | flexcolor-*/* | tools/ghidra_*/* | tools/*-src/* | \
      tools/unshield-build/*)
      echo "private or vendor directory" ;;
    *MICROCOD* | *.dll | *.exe | *.cab | *.pkg | *.icc | *.icm)
      echo "vendor binary, firmware or profile" ;;
    *.fff | *.3fr | *.raw | *.raw.* | *.dng | *.tif | *.tiff | *.jpg | \
      *.jpeg | *.png)
      echo "scan or image" ;;
    *.jsonl | *.bin | *.pdf)
      echo "trace, dump or document" ;;
  esac
}

paths() {
  git ls-files
  if [ $# -gt 0 ]; then
    git log --format= --name-only --diff-filter=AR "$1"
  fi
}

while IFS= read -r path; do
  [ -n "$path" ] || continue
  why=$(reason_of "$path")
  if [ -n "$why" ]; then fail "$path" "$why"; fi
done < <(paths "$@" | sort -u)

# git sees no text in it: firmware, scans and dumps all look like this
while IFS=$'\t' read -r eol path; do
  case "$eol" in
    i/-text*) fail "$path" "binary file" ;;
  esac
done < <(git ls-files --eol | sed -E 's/^([^ ]+) .*\t/\1\t/')

while IFS=$'\t' read -r meta path; do
  bytes=${meta##* }
  if [ "$bytes" -gt $((MAX_FILE_KB * 1024)) ]; then
    fail "$path" "$((bytes / 1024)) KB, over $MAX_FILE_KB KB"
  fi
done < <(git ls-tree -r -l HEAD)

if [ "$bad" -ne 0 ]; then
  echo "hygiene: see the errors above; such files belong in ignored notes/"
  exit 1
fi
echo "hygiene: ok"
