# ADR-0037: Native out-of-process plugin bridge

**Status:** Accepted, 2026-10-14. Amends ADR-0013 and ADR-0031; builds on
ADR-0035's wakes and ADR-0036's native hosting and child-process ownership.
Contracts: engine spec §5 and sandbox spec §1.

## Decision

- Preserve the Linux memfd/mmap, SCM_RIGHTS socketpair, shared futex words,
  posix_spawn/fd 3, pidfd death/kill and main-thread waitpid path. Neither the
  shared Header layout nor POSIX protocol version 13 changes.
- macOS uses a private shm_open object, mode 0600 with a random 128-bit suffix,
  unlinked immediately; SCM_RIGHTS transfers its live descriptor. Both sides
  mmap it shared. Address waits and wakes use Apple's corresponding SHARED
  flags (macOS 14.4+), never the private core-base address-wait functions.
  The socketpair uses explicit CLOEXEC and SO_NOSIGPIPE. A nonblocking pipe
  carries outward notifications. A kqueue EVFILT_PROC/NOTE_EXIT watch signals
  child exit. The child helper retains its leader unreaped until teardown,
  so a concurrent bounded-wait kill cannot target a recycled PID.
- Windows uses unnamed, pagefile-backed CreateFileMapping/MapViewOfFile and
  one pair of unnamed auto-reset events per active instance. Mapping/event
  handles are duplicated into the child, never inherited broadly. Atomic
  release/acquire on request/done publishes data; SetEvent only wakes the
  single corresponding waiter. Each waiter checks the word before waiting
  and after every return, including stale signals. The wake is one nonblocking
  syscall scoped with ScopedSanctionedSyscall. WaitOnAddress is not used
  across processes. Shutdown publishes quit and advances request before wake.
- Windows control uses a private, randomly named local duplex byte pipe with
  overlapped I/O and separate manual-reset read/write completion events.
  A bounded read-ahead buffer supplies native readiness to the bridge main
  loop. Complete frames retain one absolute steady-clock deadline; timeout
  cancels/drains outstanding I/O before its operation storage is released.
  Native Windows protocol version 14 adds an eight-byte recipient-owned handle
  after the ordinary 12-byte frame prefix and two event-handle values after
  Activate's ordinary 20-byte payload. No asynchronous frames are introduced.
  The child accepts parent-duplicated handles. The parent rejects handle-bearing
  replies without closing the claimed numeric value: untrusted child code
  cannot choose a parent handle to close.
- Parent notifications use WakeEvent: eventfd on Linux, the pipe's write end
  on macOS, a duplicated manual-reset event on Windows. Subscription validates
  the native resource before accepting it. Publication/ACK/rearm and retained
  snapshot semantics remain ADR-0031's, independent of the native object.
- Native launch reuses ChildProcess with an explicitly inherited control
  resource and stdout directed to the null device. POSIX remaps only that
  resource to fd 3; Windows adds it to the explicit inherited-handle list and
  retains Job ownership. No bridge logging can fill an undrained stdout pipe.
  A process HANDLE is the Windows death watch; tree termination is through the
  owned Job. Native exit status remains observable until final owner cleanup.
- Bridge execution is enabled wherever native in-process hosting builds.
  Engine/device and editor embedding gates remain separate. The existing
  synchronous bridge exception still applies only to background/offline
  processing. Device callbacks neither wait nor call bridge lifecycle methods.
  Process deadlines, silence-on-failure, structured control errors and shared
  versus dedicated crash scope remain unchanged.

## Validation

Native tests cover exact CLAP/VST3 audio and state, repeated cross-process
publication, shared/dedicated crash isolation, hang deadlines, notification
ACK/rearm, idle child death, malformed payload/resource rejection, and
Windows parent-handle preservation. Existing Linux protocol, futex, descriptor,
notification and engine crash-fixture coverage remains enabled. RTSan and
TSan cover the Linux bridge/engine area; native Actions runs verify OS APIs.

## Tradeoffs

Windows overlapped-I/O cancellation is drained before releasing its storage;
this cleanup is control-plane only. The sanctioned audio wait uses only
precreated events and the existing bounded failure deadline. Native process
adapters borrow their lifetime handles from ChildProcess. macOS retains an
exited leader until the final bridge owner dies, trading a temporary zombie
for safe PID identity during concurrent audio-side failure checks. Native
cross-process GUI embedding and pipelined live bridging remain future work.

## Launch integration note (2026-10-14)

After ADR-0036's helper landed on the port branch, Linux launch and teardown
also moved onto ChildProcess. This amends the separate posix_spawn/waitpid
ownership in the first decision bullet: the helper now owns the private
process group and final reaping, and timeout termination kills that group.
The bridge retains its pidfd death watch and fast liveness poll (socket HUP
fallback), observes exit with WNOWAIT, and keeps the leader unreaped until
final tree cleanup to prevent PID reuse. Linux memfd, futex, protocol, spin
and deadline behavior are unchanged. All platforms now have one spawn path.

## Diagnostic inheritance note (2026-10-14)

The bridge sets ChildProcess::LaunchOptions::inheritStderr to true, preserving
Linux's pre-helper diagnostic stream and providing the same facility natively.
POSIX explicitly inherits fd 2; Windows uses an inheritable duplicate of the
parent's valid STD_ERROR_HANDLE in its strict handle list, or NUL if duplication
is unavailable. The parent handle's inheritance flags are not changed. The
scanner retains the default false/null stderr, and bridge stdout stays null.
