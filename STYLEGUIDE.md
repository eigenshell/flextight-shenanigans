# Style Guide

Write code that a beginner can read top to bottom and understand without
looking anything up. When in doubt, choose the longer, plainer version.

The examples come from our own code (sancho/, libft/ft_endian.c).


## 1. Names

### Use snake_case, never camelCase
`device_handle`, `inquiry_response`, `read_big_endian32`.

### Say what it is, in full words
No abbreviations unless everybody knows them (`rgb`, `scsi`, `ppi`, `mm`).

| avoid | write |
|---|---|
| `h`, `buf`, `r` | `candidate`, `response_buffer`, `response` |
| `g_inq`, `g_inq_n` | `inquiry_response`, `inquiry_length` |
| `put_px`, `mm_to_idx` | `draw_pixel`, `get_index_from_mm` |
| `srb_exec`, `ioq_reap` | `scsi_send_command`, `read_queue_wait` |

### Put the unit in the name
`timeout_ms`, `origin_mm`, `length_bytes`, `position_steps`. A bare
`timeout = 3000` makes the reader guess.

### Booleans read as a yes/no question
`is_loader`, `keep_candidate`, `done_asking`.

### Functions: verb_subject
`read_users`, `write_users`, `find_and_open_scanner`, `classify_answer`,
`draw_pixel`. The name says what happens to what.

### Prefix public names with their layer
C has no namespaces: every public name shares one big pool.

| prefix | layer |
|---|---|
| `io_` | ftio, the OS / wire layer |
| `sc_` | Sancho, the scanner bridge |
| `qx_` | Quixote, the page |
| `ft_` | libft, shared calculations |

Helpers used inside one `.c` file only are `static` and need no prefix
(`static inquiry_answer classify_answer(...)`).

### Use the project's words
scanner = the hardware, Sancho = the scanner-side app, Quixote = the page,
libft = the shared library. Not "the box", not "poc".


## 2. Types

### Fixed-width integers
Use `uint8_t`, `uint16_t`, `uint32_t`, `int32_t`, `int64_t` from
`<stdint.h>`. `int` and `long` change size between Windows, Linux, the
browser (wasm) and the RP2350; these don't.

- Unsigned (`uint32_t`) for things that can never be negative: sizes,
  counts, positions.
- Signed (`int32_t`) whenever a negative number can mean an error.
  `uint32_t` can't hold -1: it silently becomes 4294967295.

### Bytes are `uint8_t`, text is `char`
Raw data (commands, scanner answers, image lines) is `uint8_t`. Text (file
paths, names, messages) has to be `char`, because every text function
(`open`, `printf`, `strcmp`) wants `char *`. Keep `char` at the edges (ftio),
so Sancho itself only handles numbers and bytes.

### `bool` for yes/no
`#include <stdbool.h>`, then `true` / `false`.

### Floating point: `float` and `double`, not `_Float32` / `_Float64`
`_Float64` doesn't compile with clang, which builds Quixote (wasm): "unknown
type name". `float` (32-bit) and `double` (64-bit) are the same size on
every platform we target. On the RP2350, `float` is the fast one.

### Operating-system types stay in ftio
`BYTE`, `WORD`, `DWORD`, `HANDLE` come from Windows. Only `ftio_*.c` uses
them; everything above uses the `uintN_t` types.


## 3. Layout

### Two spaces, 80 columns at most
A short `if`, loop or function may stay on one line without braces, as
long as the whole line fits in 80 columns:
```c
if(name == NULL) return NULL;
```
Anything longer, or anything with an `else`, gets braces.

No space between a keyword and its bracket: `if(`, `for(`, `while(`,
`switch(`. clang-format (`.clang-format`) does the spacing.

### `if` / `else`: write the `else` only when there is a real alternative
```c
if(keep_candidate == false) {
  scanner_close(candidate);
}
```
Don't add an empty `else { }` just for form.

### Three or more cases: an `enum` plus a `switch`
Name the outcomes first, then handle each one. It reads like SQL's
`CASE WHEN ... END`.

```c
typedef enum {
  ANSWER_NONE,        /* no reply (yet) */
  ANSWER_NOT_IMACON,  /* some other device */
  ANSWER_FIRMWARE,    /* Imacon, firmware running */
  ANSWER_LOADER       /* Imacon, boot loader */
} inquiry_answer;

switch(classify_answer(response, response_buffer)) {
  case ANSWER_NONE:
    break;
  case ANSWER_NOT_IMACON:
    done_asking = true;
    break;
  ...
}
```
- Leave out `default:`. Then the compiler warns when a case is missing.
- `break` inside a `switch` leaves the switch, **not** the loop around it.
  To stop the loop, set a flag and test it in the loop condition:
  `attempt < 8 && done_asking == false`.

### Separate "what is it?" from "what do we do?"
A small function decides (`classify_answer`). The caller acts on the answer
(the `switch`). Each half can be read, and tested, on its own.

### Compare explicitly
`if(done_asking == false)`, `if(ft_upload_is_loader(...) == false)`,
`if(candidate == NO_SCANNER)`. Don't make the reader decode `!`.

### Declare variables where you first need them
```c
for(uint32_t attempt = 0; attempt < 8; attempt++) {
  uint8_t response_buffer[96];
```
Not all at the top of the function (that was old C89 style).

### Long calls: one argument group per line, closing bracket on its own line
```c
response = io_send_command_to(candidate, inquiry_command, 6,
  SCSI_FROM_DEVICE, response_buffer, sizeof response_buffer, 3000
);
```


## 4. Numbers

### No unexplained numbers
Give every number that means something a name:
```c
#define INQUIRY_LENGTH 96       /* bytes the scanner answers */
#define INQUIRY_ATTEMPTS 8      /* a fresh device can miss the first few */
#define INQUIRY_TIMEOUT_MS 3000
```
Fixed scanner byte layouts (offsets in a reply or descriptor) get a comment
saying what lives there: `response_buffer + 8 /* vendor name */`.

### Let the compiler count for you
`sizeof response_buffer` instead of repeating `96`; `sizeof inquiry_command`
instead of `6`. Change the array and everything follows.

### Casts get a reason
```c
/* safe: inside this branch response is never negative */
result.inquiry_length = (uint32_t)response;
```


## 5. Comments

### Comments go on their own line, above the code
End-of-line notes don't fit in 80 columns, and the formatter can't rewrap
them. Above the code, a comment can be as long as it needs.

### Explain why, and explain C tricks
- **Why** something is done, especially when it isn't obvious: why we retry
  INQUIRY 8 times, why the firmware wins over the loader.
- **How**, for anything a beginner wouldn't recognise: bit shifts, masks,
  pointer arithmetic, casts.
  ```c
  /* Shift the value left by 8 bits to make room for the new byte,
   * then OR the new byte into the bottom 8 bits. */
  value = (value << 8) | buffer[offset + i];
  ```
- Don't repeat the code in words (`i++; /* add one to i */`).

### Every struct field gets a comment
```c
typedef struct scanner_info {
  /* open file number for the scanner; NO_SCANNER = none found */
  io_scanner_handle device_handle;
  /* the scanner's answer to "who are you?" */
  uint8_t inquiry_response[96];
  ...
} scanner_info;
```

### Every public function gets a comment in its header
What it does, what its arguments are, what it returns, and what "failed"
looks like.

### `//` or `/* */`: either, but one style per file
Every file starts with `/* SPDX-License-Identifier: GPL-2.0-or-later */`.


## 6. Functions and files

### Function arguments
- Every argument has a meaningful name (also in the header).
- Arguments with default values go last.

### One star is usually enough
- Pass `thing *name` when the function has to change the caller's variable
  (`sc_close_scanner(&scanner)` resets its handle).
- Pass `const thing *name` when it only reads.
- If a function produces one small struct, just return it:
  `scanner_info sc_find_and_open_scanner(void);`.
- `**` (a pointer to a pointer) only when the function itself allocates
  memory for the caller. That's rare here.

### `(void)` for "no arguments"
`scanner_info sc_find_and_open_scanner(void);`. Empty `()` means "unknown
arguments" in older C.

### "Nothing found" is a value, not a crash
Use one clear marker value (`NO_SCANNER`) and say in the header which value
means failure. Library code (libft, Sancho) **never prints and never calls
`exit()`**: it hands the result back, and the caller decides what to tell
the user.

### Closing is always safe
A close or free function does nothing when there's nothing open, and resets
the handle afterwards, so callers can always call it at the end.

### One file, one job
If the sentence that describes a file needs "and", it's two files. Sancho is
split by job: `sc_connect.c`, `sc_firmware.c`, `sc_motion.c`, ...

### Headers hold only what other files need
Types and helpers used by one `.c` file stay in that file. A header has:
the SPDX line, an include guard named after the file (`SANCHO_H` for
`sancho.h`), its includes, public types and public functions.

### Include order
Standard headers first (`<stdio.h>`, `<stdint.h>`), then our own
(`"sancho.h"`, `"ft_upload.h"`). Each `.c` file includes its own header.
