# Proposal: the include block grows the three headers it uses (fix #32)

Status: [APPLIED 2026-10-29 on the user's word]

Apply verification, 2026-10-29: a compile check only, at the user's word ("you can try a
compilation to pass it, but no need to start tests for now"): the canary test, landed as
`018-048`, compiles and links with MSVC 19.43 against the header as applied, and g++ 15.2 (WSL
Ubuntu) accepts it and `005-001` with `-fsyntax-only`. The suites were not run; their numbers
come with the next run. The check note is item 9 of "How it was checked".

## The bug

colib.h's unconditional include block, directly under the HEADER banner, is the complete list of
standard-library headers the library asks for — the only other includes in the file are platform
headers picked by the `COLIB_OS_*` guards (`winsock2.h`/`mswsock.h`/`windows.h`, `sys/epoll.h`,
`sys/event.h`) plus `<atomic>`/`<mutex>` behind `COLIB_ENABLE_MULTITHREAD_SCHED`, which defaults
false. Against that list, the library uses six symbols whose guaranteed providers are nowhere in
it:

| Symbol                | Use site(s)                                                          | Guaranteed provider |
|-----------------------|----------------------------------------------------------------------|---------------------|
| printf (unqualified)  | the default `COLIB_LOG_FUNCTION` lambda — compiled into every        | `<cstdio>`          |
|                       | logging-enabled TU, i.e. every default TU                            |                     |
| snprintf (unqual.)    | `dbg_to_str` (both OS variants — unconditional per-OS) and the       | `<cstdio>`          |
|                       | `dbg_format` template (on every instantiation)                       |                     |
| sprintf (unqualified) | the Windows `get_last_error` (spotted while re-validating this fix;  | `<cstdio>`          |
|                       | same provider, same fix)                                             |                     |
| std::remove_if        | `rm_modifs_from_table` (every build) and the Linux epoll waiter      | `<algorithm>`       |
|                       | erase (Linux builds)                                                 |                     |
| std::min              | `sem_t::signal_all` (every build)                                    | `<algorithm>`       |
| std::tuple/apply      | `allocator_memory_t`'s buckets (every build — plain struct),         | `<tuple>`           |
|                       | `wait_all` (template, on use), every Windows IO dispatcher (every    |                     |
|                       | Windows build)                                                       |                     |

The trigger is any translation unit that includes colib.h: the non-template sites compile
unconditionally, the two template sites (`dbg_format`, `wait_all`) on first use, and no
configuration macro turns any of them off. The confirmed consequence is STATIC-CONFIRMED — the
verdict is by reading, with the full evidence table and the header-by-header sweep in
`repro/32-missing_includes.md`: on this box's MSVC STL the header compiles only because other
headers' transitive includes happen to provide the symbols (`std::remove_if` physically lives in
the internal `xmemory`, defined there for the containers' `erase_if`; `std::min` in `xutility`;
`cstdio` arrives via the `limits` -> `cwchar` chain; `tuple` via the `xmemory` chain). The
standard promises none of that, so any leaner or refactored standard library — and moving these
symbols back into their owning headers is the direction of travel — breaks every translation unit
that includes colib.h with plain "identifier not found" / "no member named" errors.

## Where the fix lands

| colib.h region                    | what changes                                                     | +adds/-dels |
|-----------------------------------|------------------------------------------------------------------|-------------|
| the include block under the       | three headers added in their alphabetical slots: `<algorithm>`   | +3/-0       |
| HEADER banner                     | first, `<cstdio>` after `<coroutine>`, `<tuple>` after           |             |
|                                   | `<string.h>`                                                     |             |

Nothing else moves: no code, no macro, no comment. The restored `<optional>` (your ruling of
30-09-2026) sits between `<memory>` and `<set>`, untouched — it appears only as context in the
diff below.

## The change

Built on the current tree (the user's accept rework plus the restored `<optional>`); the salvage
workspace from 29-09 predated the `<optional>` restore and was re-based on the fresh tree for
exactly that.

```diff
--- colib-base.h	2026-09-30 08:05:06.715655600 +0300
+++ colib.h	2026-09-30 08:05:23.642435100 +0300
@@ -393,11 +393,13 @@
 =================================================================================================
 ================================================================================================= */
 
+#include <algorithm>
 #include <array>
 #include <cerrno>
 #include <chrono>
 #include <cinttypes>
 #include <coroutine>
+#include <cstdio>
 #include <deque>
 #include <exception>
 #include <functional>
@@ -409,6 +411,7 @@
 #include <source_location>
 #include <stack>
 #include <string.h>
+#include <tuple>
 #include <unordered_set>
 #include <utility>
 #include <variant>
```

The three lines land in the slots the block's own convention gives them: the block is ordered
alphabetically, case-insensitively, so `<algorithm>` takes the first position ahead of `<array>`,
`<cstdio>` sits between `<coroutine>` and `<deque>`, and `<tuple>` between `<string.h>` and
`<unordered_set>`. `<cstdio>` takes the block's prevailing `<c*>` spelling rather than
`<stdio.h>` — the one C header already present, `<string.h>`, is the block's only `.h`-form
holdout, and the findings prescribed `<cstdio>` (see open questions for the nuance). The change
is include-only: it cannot alter behavior on any toolchain that already compiled the header, and
on a toolchain that respects header ownership it turns a hard compile error into the same
compilation everyone gets today.

## The regression test

Landed as `tests/018-048-reproduced_missing_includes.cpp` (Test104) on 2026-10-29; it was first
written as 018-043 / Test86, the names the older logs below carry. One line changed at landing:
its `wait_all` comparison reads the tuple of pairs fix #24 made of it (2026-10-29).

It is a compile canary: the bug lives in the header's include surface, so the test puts every
flagged use site into one translation unit, the unconditional ones by mere inclusion (anchored by
address), the two template-only ones (`dbg_format`, `wait_all`) by instantiating and running
them. It is an ordinary positive test that builds, runs and passes on every toolchain that
compiles colib.h at all; the day a toolchain stops providing the symbols transitively, this file
stops building, with plain "identifier not found" errors pointing at the include block.

At apply time it was built, not run, on the user's word ("no need to start tests for now"): MSVC
19.43 compiles and links it against the header as applied, and g++ 15.2 (WSL Ubuntu) accepts it
and `005-001` with `-fsyntax-only`; the runs follow with the next suite run.

The file as landed:

```cpp
/* Test104 - Reproduced Bugs: colib.h used <cstdio>/<algorithm>/<tuple> symbols it never included
================================================================================================= */

/* colib.h's include block is the complete list of standard headers the library asks for, yet the
library used printf (the default COLIB_LOG_FUNCTION), snprintf (dbg_to_str and dbg_format) and
sprintf (the Windows get_last_error), std::remove_if (rm_modifs_from_table and the Linux epoll
waiter erase), std::min (sem_t::signal_all) and std::tuple with std::apply
(allocator_memory_t's buckets, wait_all, and every Windows IO dispatcher) without including
<cstdio>, <algorithm> or <tuple>. The header compiled only through other headers' transitive
includes, which the standard does not promise; a leaner or refactored standard library fails to
compile every translation unit that includes colib.h.

The fix adds those three headers to the include block. This file is the compile canary: it
includes colib.h with default configuration and forces every flagged use site into the
translation unit — the unconditional ones compile by mere inclusion and are anchored here by
address, the two template-only ones (dbg_format's snprintf, wait_all's tuple and apply over its
create_future futures) are instantiated and run. On a toolchain whose transitive includes cover
the omission this file passes before the fix as well; MSVC 19.43 on this box is one, so the fix
documentation shows both runs instead of claiming a failure/recovery pair. On a toolchain
without that coverage the unfixed header fails to compile this file, and the fixed header
compiles and passes it unchanged. */

#include "../colib.h"
#include "tests_common.h"

co::task_t test104_co_wait_all() {
    auto ret = co_await co::wait_all(
        []() -> co::task<int> { co_return 4; }(),
        []() -> co::task<int> { co_return 5; }()
    );

    /* each member reports its value and ERROR_OK (the pair create_future resolves to) */
    bool is_ok = ret == std::tuple<std::pair<int, co::error_e>, std::pair<int, co::error_e>>{
            {4, co::ERROR_OK}, {5, co::ERROR_OK} };
    ASSERT_COFN(CHK_BOOL(is_ok));

    co_return 0;
}

int test104_missing_includes() {
    /* Mere inclusion compiles the unconditional users: sem_t::signal_all (std::min),
       rm_modifs_from_table (std::remove_if), allocator_memory_t (std::tuple member, std::apply)
       and, on Windows, dbg_to_str and the IO dispatchers (snprintf, tuple CTAD, std::apply).
       Taking two of their addresses keeps that fact anchored in this translation unit. */
    auto signal_all = &co::sem_t::signal_all;
    (void)signal_all;
    auto rm_modifs = &co::rm_modifs_from_table;
    (void)rm_modifs;

    /* dbg_format is a template, so its unqualified snprintf compiles only when instantiated. */
    co::dbg_string_t fmt = co::dbg_format("%d-%s", 42, "instantiated");
    ASSERT_FN(CHK_BOOL(fmt.compare("42-instantiated") == 0));

    /* Calling log_str executes the default COLIB_LOG_FUNCTION lambda, whose body is printf. */
    ASSERT_FN(CHK_BOOL(co::log_str(fmt) >= 0));

    /* wait_all is a template, so its std::tuple and std::apply over create_future futures
       compile only when instantiated — and run here for real. */
    auto pool = co::create_pool();
    pool->sched(test104_co_wait_all());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    return 0;
}

int main() {
    int ret = test104_missing_includes();
    print_test_result("018-048-reproduced_missing_includes.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All runs fresh, 30-09-2026, from `repro/fix/32/` (Git Bash, `cl 19.43.34810`, the workspace's
private header copies: `colib-base.h` is the unpatched current tree — the user's accept rework
plus the restored `<optional>` — and `colib.h` is the same copy plus this diff). Log names are
files in that directory; the base-side controls live in `base/` beside the unpatched copy.

1. The standalone block probe, patched (`32-probe-block.cpp`, build `32-probe-block-build-v2.log`
   exit 0, run `32-probe-block-run.log` exit 0): a translation unit whose complete include set is
   the patched block verbatim, using every symbol in colib.h's own shapes — the erase idiom of
   `rm_modifs_from_table`, the `std::min` of `signal_all`, the tuple CTAD and `std::apply` of the
   dispatchers and `allocator_memory_t`, unqualified `printf`/`snprintf`/`sprintf`:

   ```
   probe: remove_if kept=3 min=0 apply=6
   PASS: the patched include block alone, with no colib.h and no other headers, provides
   every symbol colib.h uses from <cstdio>, <algorithm> and <tuple> on this toolchain.
   ```

2. The probe's control, unpatched block (`base/32-probe-block.cpp`, run exit 0): the identical
   body against the unpatched 20-header block also compiles and passes on this box — the null,
   freshly demonstrated against the same compiler invocation rather than inherited from the 29-09
   probe:

   ```
   probe: remove_if kept=3 min=0 apply=6
   PASS (expected null on this box): the UNPATCHED block also provides every symbol here,
   through transitive includes the standard does not promise — the bug is invisible on
   this toolchain; only the patched block carries the guarantee. See repro/32-*.md.
   ```

3. Why the probe exists, recorded as a run (`32-showincludes-v2.log`): `-showIncludes` on the
   probe prints a 171-entry closure in which exactly ONE of the 23 direct headers appears as a
   depth-1 open (`algorithm`, the first) — the other 22, `cstdio` and `tuple` among them, are
   already open by the time their `#include` lines are parsed. `-showIncludes` therefore cannot
   attribute providers on this toolchain; the probe replaces attribution with sufficiency.

4. The original repro against the patched copy (`32-missing_includes-run-v2.log`, exit 0): the
   default-config include plus forced `dbg_format` and `log_str` — the expected null, unchanged
   by the fix.

5. The regression test, both copies — shown in its section above: `[PASSED]` against the patched
   copy and `[PASSED]` against the unpatched one, the documented null pair.

6. No-regression, patched copy: `005-001-io.cpp` (`005-001-run-v2.log`), `011-003-modifs_
   lifecycle.cpp` (`011-003-run-v2.log`) and the landed `018-029-reproduced_winevent_handle_
   leak.cpp` (`018-029-run-v2.log`) — each copied with its include pointed at the workspace
   header — all exit 0:

   ```
   [PASSED]: 005-001-io.cpp
   [PASSED]: 011-003-modifs_lifecycle.cpp
   [PASSED]: 018-029-reproduced_winevent_handle_leak.cpp
   ```

7. NOT-CHECKED: the leaner-toolchain failure mode itself. No gcc/clang and no second STL exist on
   this box, and MSVC 19.43's transitives cover every missing symbol, so no process here can make
   the unpatched header fail. The claim that it fails there stays reading-validated, on the
   standard's header ownership plus the transitive-provider table in `repro/32-missing_includes.md`.
8. NOT-CHECKED: the Linux/kqueue side (the epoll waiter's `std::remove_if` never compiles on this
   box). The diff adds platform-neutral includes only, and the probe exercises `remove_if`'s
   shape cross-platform, but the Linux build as a whole is unrun here.

9. At apply time, 2026-10-29, on the header of that day: the three includes added in their
   alphabetical slots; the canary built (MSVC, through the tests makefile) and syntax-checked with
   g++ 15.2 on Linux together with `005-001` (the epoll block's `std::remove_if`), so the Linux
   NOT-CHECKED items above are now checked at the compile level; nothing was run, on the user's
   word, so the suites' numbers are not claimed here. `02_api.md` has nothing to mirror: no
   declaration changed.

## Interactions

The tracker's row for this fix is "allocator/config: 29 31 (32 = include block, independent)".

- FIX_29 (`allocator_bucket48`) adds its `static_assert` directly under `allocator_bucket_sizes`;
  FIX_31 adds its own at the bucket table / SCALE macro doc. Both land in the allocator region,
  hundreds of lines below the include block, sharing no lines with this diff — the edits are
  disjoint and compose trivially in either order. Their own proposals say the same about #32.
- The restored `<optional>` (the 30-09 ruling): this diff is built on the tree that already
  carries it, between `<memory>` and `<set>`, and does not touch it — applying this fix to the
  current tree needs no reconciliation.
- The user's uncommitted accept rework is part of the base; every no-regression run above was
  made on top of it.

## Open questions for the user

1. `<cstdio>` or `<stdio.h>`. colib.h spells the C calls unqualified (`printf`, `snprintf`,
   `sprintf`), and `<cstdio>` strictly guarantees them only in namespace `std` — the
   global-namespace availability the unqualified calls rely on is unspecified-but-universal
   (every implementation this library targets declares the global names, and the calls already
   compile unqualified today through the transitive `cstdio`). The diff carries `<cstdio>`,
   matching the block's prevailing `<c*>` spelling and the finding's prescription; say the word
   if you prefer `<stdio.h>`, which guarantees the global names outright and would match the
   block's one `.h`-form holdout instead.
2. While in the block, `<string.h>` could become `<cstring>` for a uniform `<c*>` spelling. The
   diff deliberately does not touch it — the fix stays minimal, three adds for six symbols — but
   the tidy-up is a one-line variant if you want it.

Decided at apply time, 2026-10-29, on the user's word: (1) `<cstdio>`, as he spelled it; (2)
`<string.h>` stays as it is.
