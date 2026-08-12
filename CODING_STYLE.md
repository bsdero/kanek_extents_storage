# Coding Style — Kanek Foundation Library (KFL)

This document records binding style rules for C code in this repository,
beyond what is already covered in `AGENTS.md`. When the two disagree,
this file wins for the topics it covers.

## Rule 1 — Every control-flow body uses braces, on its own line

`if`, `else`, `while`, and `for` bodies are always wrapped in `{ }`, and
the body is never written on the same line as the condition — even for a
single statement. The opening brace goes on the same line as the
condition; the closing brace gets its own line, aligned with the start
of the statement.

**Bad:**

```c
if (!p) return NULL;

if (rc < 0) return -1;

if (!p) { return NULL; }

while (n--) process(n);

for (i = 0; i < n; i++) buf[i] = 0;
```

**Good:**

```c
if (p == NULL) {
    return NULL;
}

if (rc < 0) {
    return -1;
}

while (n--) {
    process(n);
}

for (i = 0; i < n; i++) {
    buf[i] = 0;
}
```

This applies regardless of what the condition tests (pointer, error
code, loop counter, etc.) and regardless of how short the body is.

## Rule 2 — Pointer comparisons against NULL are always explicit

Never rely on implicit pointer truthiness. Write the comparison out.

**Bad:**

```c
if (!p) { ... }
if (p) { ... }
```

**Good:**

```c
if (p == NULL) { ... }
if (p != NULL) { ... }
```

This rule is specifically about pointers. Plain integer/boolean
conditions (`if (rc < 0)`, `if (count > 0)`, `if (!done)` where `done`
is a genuine boolean flag) are unaffected — only pointer-vs-NULL checks
require the explicit form.

## Rule 3 — Line length is capped at 78 characters

No line may exceed 78 characters. If a line would run longer, break it
into multiple statements or expressions wherever possible instead of
letting it run on.

## Rule 4 — No tab characters in code

The tab character (`\t`) is forbidden in source code. The only
exception is inside string/format literals used to produce program
output (e.g. a `printf` format string that intentionally emits a tab
for alignment) — that is output formatting, not code indentation.

## Rule 5 — Indentation is 4 spaces, never tabs

Code is indented with 4 spaces per level. Tabs are never used for
indentation (see Rule 4).

## Rule 6 — Function braces use K&R style

The opening brace of a function definition goes on the same line as
the signature, not on its own line. This applies only to function
definitions — control-flow braces still follow Rule 1.

**Bad:**

```c
int kfs_open(kfs_config_t *config)
{
    ...
}
```

**Good:**

```c
int kfs_open( kfs_config_t *config){
    ...
}
```

## Rule 7 — Return statements parenthesize their value

Write `return(value);`, not `return value;`. This applies even to
trivial values.

**Bad:**

```c
return -1;
return rc;
return 0;
```

**Good:**

```c
return( -1);
return( rc);
return(0);
```

## Rule 8 — Pointer declarators bind to the variable, not the type

The `*` sits next to the variable name, never next to the type.

**Bad:**

```c
char* p;
unsigned char* bitmap;
```

**Good:**

```c
char *p;
unsigned char *bitmap;
```

## Rule 9 — Comments use `/* */`, never `//`

C99/GNU C allows `//` line comments, but this codebase does not use
them. All comments are block-style, even single-line ones.

**Bad:**

```c
// increment retry count
```

**Good:**

```c
/* increment retry count */
```

## Rule 10 — Struct typedefs are anonymous, with no `_s` tag

Define the struct and its typedef together, without a separate
tagged-struct name.

**Bad:**

```c
struct dict_s {
    ...
};
typedef struct dict_s dict_t;
```

**Good:**

```c
typedef struct{
    ...
} dict_t;
```

## Rule 11 — Every early-return failure path logs first

Before a `return` on an error path, call the appropriate trace macro
(`TRACE_ERR`, `TRACE_SYSERR`, or `TRACE_ERRNO`) so the failure is
always logged at the point it's detected, not just propagated silently.

**Bad:**

```c
if (fd < 0) {
    return( -1);
}
```

**Good:**

```c
if (fd < 0) {
    TRACE_ERR("failed to open slot file");
    return( -1);
}
```

## Rule 12 — No file-header banner comments

Source files start directly with `#include` lines — no license,
author, or file-description banner comment block at the top of the
file.

## Rule 13 — Space after an opening paren, none before a closing paren

In both function calls and control-flow conditions, put a space after
`(` before the first token, and no space before `)`.

**Bad:**

```c
if (rc < 0) {
bm_find(bitmap, total_slots, 0, gap);
```

**Good:**

```c
if ( rc < 0) {
bm_find( bitmap, total_slots, 0, gap);
```

Empty-argument calls have no space either way: `dict_new()`.

## Scope

- These rules apply to all `.c`/`.h` files in `src/`, including test
  files.
- They apply to new code immediately, and the existing tree has been
  swept to conform (see git history for the retrofit commit).
- Rules 6-13 were inferred from the author's established style in the
  companion `kanekfs` repository (github.com/bsdero/kanekfs), which
  this library's code is written to be consistent with.
- Everything else in `AGENTS.md`'s "Coding Conventions" section
  (header guard naming, `USER_SPACE` portability guards, `container_of`
  usage, etc.) is unchanged by this document.
