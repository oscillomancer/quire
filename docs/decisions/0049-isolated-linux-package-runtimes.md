# ADR-0049: Isolated C++ runtimes in Linux packages

**Status:** Accepted, 2026-10-24. Amends ADR-0048's Linux runtime choice.

## Context

The DAW hosts third-party plugins in process. Bundling an older shared
libstdc++.so.6 makes its SONAME win before a newer plugin loads; that plugin
can then fail with a missing GLIBCXX symbol even when the user's system
runtime would support it. Stock plugins copied into other DAWs need the same
independence from the host's C++ runtime version.

## Decision

The `linux-package` preset adds `-static-libstdc++ -static-libgcc` and
`-Wl,--exclude-libs,ALL` to executable, shared-library and module linker flags.
The five programs and every stock CLAP carry their own C++ runtime, with its
archive symbols hidden so they cannot interpose on another module. The parent
build forwards those configure flags into isolated plugin builds; the plugin
template and its shared stack remain unchanged. Normal development/release
presets retain their existing runtime choice.

Release CI runs in Ubuntu 22.04 with Clang 22 and GCC 12's libstdc++ headers
and static runtime libraries. It explicitly selects
`--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12`, forwards that compiler flag
to the stock builds and prints compiler versions. The package job suppresses
deprecation diagnostics for this older standard library, whose stable-partition
implementation calls a deprecated helper; native CI retains the normal warning
policy. GNU C declarations (`-D_GNU_SOURCE`) are enabled for the older glibc
headers so the template renderer can use `RTLD_DEFAULT`, without altering its
shared code. The GCC runtime exception
and copyright notices still ship for the statically linked runtime, including
in the optional stock-plugin notices copied for other hosts.
No shared libstdc++ or libgcc_s is bundled or accepted as a packaged ELF
dependency. The audit also rejects runtime exports and missing CLAP entry
points. Optional `--plugins` copies therefore need no host libstdc++ upgrade.
The glibc baseline remains Ubuntu 22.04; graphics, X11 and JACK remain host
facilities, with JACK loaded dynamically only when requested.

## Consequences

Artifacts are larger, but a third-party plugin can select the system C++
runtime independently of the DAW. Static runtime isolation is a packaging
property, verified on the actual staged binaries, rather than a change to
the template's default plugin build or ABI.
