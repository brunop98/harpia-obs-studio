# QuickJS-ng (vendored)

Upstream: <https://github.com/quickjs-ng/quickjs>
Version: **v0.15.1**
Licence: MIT (see `LICENSE`) — Fabrice Bellard, Charlie Gordon, Ben Noordhuis
and contributors.

## Why it is here

Harpia runs per-clip transform scripts (`harpia/editor/script/`). That used to
need `QJSEngine` from Qt's Qml module, but the trimmed obs-deps Qt used for the
Windows builds ships no Qml, so scripting could not work there. QuickJS is a
self-contained JS engine with no dependency on Qt, so vendoring it makes
scripting behave identically on every platform we build for.

QuickJS-**ng** rather than Bellard's original: the original relies on GCC/Clang
extensions and does not build with MSVC, which is the Windows toolchain here.

## What was taken

Only the core engine — the four sources upstream's `qjs` library target uses,
plus the headers they include:

    quickjs.c  libregexp.c  libunicode.c  dtoa.c
    quickjs.h  quickjs-atom.h  quickjs-opcode.h  quickjs-c-atomics.h
    cutils.h  dtoa.h  libregexp.h  libregexp-opcode.h
    libunicode.h  libunicode-table.h  list.h
    builtin-array-fromasync.h  builtin-iterator-zip.h  builtin-iterator-zip-keyed.h

`quickjs-libc.c` is **deliberately excluded**. It provides `std`/`os` modules
with file, process and network access; a transform script has no business
touching any of that, and leaving the file out means the capability cannot be
switched on by accident.

The CLI, tests, examples, docs and install rules are not vendored.
`CMakeLists.txt` here is Harpia's own, not upstream's.

## Local modifications

**None.** The sources are byte-for-byte upstream. Keep it that way — if
something needs changing, do it in `harpia/editor/script/` instead, so updating
is a matter of re-downloading the file list above.

## Updating

Fetch the same file list at the new tag, drop them in, update the version at the
top of this file, and rebuild. Check upstream's `CMakeLists.txt` for changes to
the `qjs_sources` list and to the MSVC flag block, both of which are mirrored
here.
