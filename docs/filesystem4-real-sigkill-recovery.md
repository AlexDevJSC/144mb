# FileSystem4 — Real SIGKILL replay and all-grant startup preflight

The existing FileSystem4 synthetic snapshots verify recorded states, but cannot
prove what happens if the actual NT mediator dies during a syscall. This
increment adds **real process termination** via a test-only LD_PRELOAD
interceptor; the production runtime has no fault-injection switch.

## Reproducible Linux gate

    make check-dos83-sigkill
    make check-dos83-wal
    make -j2 check

The test fixture starts a real nt-dos83-mediator --move process on a
disposable tree, kills that exact process by SIGKILL in four places:

- after the fsync of a pending WAL, before alias changes;
- immediately after the destination DOS 8.3 alias xattr is written;
- after staged inode fsync, immediately before the pending rename;
- after clearing the old alias key, which occurs only after successful
  direct SYS_renameat2;
- after the source parent's fsync, before the WAL is cleared.

The runtime calls SYS_renameat2 directly and bypasses the libc wrapper,
so the test observes the immediately adjacent durable operations instead
of falsely claiming to intercept that syscall itself.

A **new independent process** then executes --recover. Assertions require
the original inode and bytes to remain intact, exactly one usable per-entry
DOS alias, a zero-length journal after replay, idempotent subsequent
recovery, and successful unrelated follow-up renames. No user files are used.

The existing on-disk recovery matrix also exercises malformed WAL records,
ambiguous disappearance of physical entries and an adversarial out-of-band
overwrite of the pending DOS xattr, both pre-rename and post-rename. Unknown
metadata must make recovery fail without clearing the WAL.

## Extra grants

The prior PE preflight covered only C:. New host-only
nt_runtime_recover_all_dos83() snapshots pinned descriptors of C:, explicitly
mapped D:-Z: roots, UNC grants and redirected Linux user folders, deduplicates
identical physical roots by dev/ino, then acquires the existing DOS83 mutex and
replays any journal *before* executing guest instructions, TLS or DllMain.
If a single root has an unreadable, malformed or ambiguous WAL, the whole
session fails rather than silently using that root.

An additional native runtime test checks that a bad WAL in D:, an UNC grant
or a mapped known folder aborts all-grant preflight even if C: is healthy,
then verifies recovery once the corrupt fixture has been cleared.

## Limits, not acceptance

- SIGKILL is not power loss; dirty page cache and drive cache are not reset.
- This remains a cooperative WAL for Win32 same-grant rename, not complete
  NTFS transactionality, all filesystem operations, or Windows parity.
- External Linux writers with backing access can bypass the WAL. Unexpected
  xattrs are now rejected rather than overwritten; simultaneous host rename
  races require separate testing and eventually a service UID + IPC broker.
- Short-name allocation without rename, junction SET/DELETE and cross-root
  copy/delete still require their own durability contracts.
- A separate physical Fedora Enforcing test, power-cycle procedure on a
  disposable volume, and native Windows NTFS comparative evidence are still
  mandatory for the final acceptance gate.

No percent-complete claim follows from these tests.
