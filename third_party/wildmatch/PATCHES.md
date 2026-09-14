# libgit2 wildmatch adaptation

The source is from libgit2 v1.9.7. `source.json` records the **upstream**, unmodified
file hashes and URLs. `COPYING` is unchanged: GNU GPL v2 with the libgit2 Linking
Exception. This does not make the source MIT licensed.

Local changes to `wildmatch.c` and `wildmatch.h` rename the private entry point to
`iilocal_wildmatch`, accept a shared operation budget and cancellation callback,
limit recursion to 128, and propagate exhaustion as `WM_ABORT_LIMIT`, including
from the optional `**/` branch. The C++ caller rejects the decision on exhaustion;
it never interprets exhaustion as a nonmatching deny. The standard-header adapter
`git2_util.h` avoids linking libgit2 or consulting ambient repository ignore files.

The C++ adapter lowercases UTF-16 input and maps up to 128 distinct non-ASCII code
units to a sorted byte alphabet. This preserves wildcard character units/ranges
without changing wildmatch's byte parser. Inputs with a larger alphabet fail
explicitly. Unicode lowercase behavior follows Qt, not JavaScript RegExp's exact
Unicode case-folding rules. This difference is exposed in the settings documentation.

Rebuild this private object with a C11 compiler and `-fPIC -fvisibility=hidden` on Unix. `git2_util.h` supplies only standard C headers. The exact integration and install commands are in `cmake/PermissionParsers.cmake`.
