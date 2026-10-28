# Security policy

## Reporting a vulnerability

Please report security problems privately, not in a public issue. On the
repository's **Security** tab, choose **Report a vulnerability**
(<https://github.com/oscillomancer/quire/security/advisories/new>). Only the
repository's maintainers see the report.

Include what you can of:

- the Quire version or commit and your operating system;
- the steps, file or script that trigger the problem;
- what an attacker gains, for example code run by opening a project.

Quire has a single maintainer, so responses are best effort. Replies come in
the report's thread on GitHub. When the fix ships, the advisory is published
and credits you, unless you would rather not be named.

## Supported versions

Only the latest release receives security fixes. A fix ships in a new
release; older releases are not patched.

## Scope

Reports are welcome for Quire, its stock plugins and its installers, for
example:

- a project, preset, audio or MIDI file that makes Quire or a stock plugin
  run code, corrupt memory or write files it should not;
- a DSP script that the compiler accepts reading or writing memory outside
  its own data, or reaching files, processes or the network;
- a Lua script (`script.run`) doing more than the commands it calls allow;
- another user account reaching a running Quire's control socket;
- an installer or uninstaller changing files outside Quire's own folders.

Some things are by design and are not vulnerabilities on their own:

- Third-party plugins run as native code with your account's rights. Running
  a plugin in its own process protects Quire from its crashes, not from
  hostile code, so install only plugins you trust.
- A connected agent can do whatever Quire's commands allow, including saving
  files and loading plugins. Connect only agents you trust.
