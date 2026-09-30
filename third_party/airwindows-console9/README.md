# Airwindows Console 9 reference

Upstream: <https://github.com/airwindows/airwindows>, commit
`d22a25b7f7c0c8c05e9f3ae480e7f1da9769f49d`. MIT, © 2018 Chris Johnson.

Unmodified files from `plugins/LinuxVST/src/Console9Channel/` and
`plugins/LinuxVST/src/Console9Buss/` are built against an original minimal
test stand-in header in `libs/console9/tests/reference/`. No VST2 SDK is used.
The two `.cpp` files in `mono/` are from
`plugins/MacSignedAU/Console9ChannelMono/Console9ChannelMono.cpp` and
`plugins/MacSignedAU/Console9BussMono/Console9BussMono.cpp`; they are reference
text only and are not compiled. All original notices are retained.

Test-only, never linked into a shipped binary. The port is `libs/console9`.
