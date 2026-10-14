# Security

> [!WARNING]
> **pyaio is a learning project.** Its C code has not been audited. It talks
> to one of the most security-sensitive interfaces in the Linux kernel. Do not
> rely on it to protect data or systems that matter.

## io_uring is a large, frequently exploited kernel attack surface

`pyaio.uring` uses io_uring and nothing else. Before enabling it, know that:

* **It has an exploit-heavy history.** In Google's kCTF vulnerability reward
  program, [60% of the Linux kernel exploit submissions targeted
  io_uring](https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html),
  and io_uring bugs were used in every submission that bypassed Google's
  mitigations. Google's conclusion was to consider it "safe only for use by
  trusted components". It went on to disable io_uring on ChromeOS and its
  production servers and to restrict it for Android apps
  ([summary](https://securityboulevard.com/2025/04/armo-io_uring-interface-creates-security-blind-spot-in-linux/)).
* **Containers usually block it.** Docker's default seccomp profile denies
  `io_uring_setup`, `io_uring_enter` and `io_uring_register`
  ([Docker docs](https://docs.docker.com/engine/security/seccomp/)). pyaio
  then refuses to start and says why. Do not weaken a container's seccomp
  profile just to run pyaio. (pyaio's own CI does this, but only in
  throwaway build containers on disposable runners, to test the wheels.)
* **It can hide activity from security monitoring.** Operations submitted
  through io_uring do not appear as their own syscalls. Tools that watch
  syscalls (some EDR agents, `auditd` rules, seccomp-based sandboxes) see
  only `io_uring_enter`
  ([ARMO research](https://securityboulevard.com/2025/04/armo-io_uring-interface-creates-security-blind-spot-in-linux/)).
  If your organisation relies on such tooling, ask before deploying anything
  built on io_uring.
* **Administrators can turn it off.** Since late 2023 the kernel has the
  `kernel.io_uring_disabled` sysctl: `0` allows everyone (the default), `1`
  allows only `CAP_SYS_ADMIN` or members of `kernel.io_uring_group`, and `2`
  disables it for everyone
  ([patch](https://lkml.iu.edu/hypermail/linux/kernel/2308.2/00994.html)).
  On shared or multi-tenant machines, `1` or `2` is a reasonable default.
  pyaio raises `PermissionError` when it is denied.

**Recommendation:** use pyaio on machines you control for workloads you
trust. Everywhere else, keep io_uring disabled and use something else for
file I/O (a thread pool works anywhere). pyaio deliberately has no
fallback, so it never quietly runs in an environment its maintainers did
not intend.

## What pyaio itself does (and does not do)

* It needs **no privileges** and does not use features that need them
  (no `SQPOLL` kernel threads, no registered buffers or files).
* Buffers shared with the kernel are kept alive until the kernel reports
  completion. If a close timeout forces pyaio to give up on a request, the
  buffer is **leaked, never freed** (see
  [docs/architecture.md](docs/architecture.md#3-request-states)).
* Reads return only the bytes the kernel actually wrote. Short reads are
  truncated, so no uninitialised memory reaches Python.
* A `Ring` refuses to run in a child process after `fork()`, so a child
  cannot corrupt the parent's queues.
* File descriptors and offsets are passed to the kernel exactly as given,
  with the same permission checks as `os.pread`/`os.pwrite`. Validate them
  yourself if they come from untrusted input.

## Reporting a vulnerability

Please report vulnerabilities privately with a
[GitHub security advisory](https://github.com/felipecruz/pyaio/security/advisories/new)
rather than in a public issue.
