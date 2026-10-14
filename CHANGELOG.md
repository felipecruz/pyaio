# Changelog

## 0.5.0rc1 (unreleased)

First release candidate of the io_uring backend. pyaio is a learning
project: see the warnings in the README and [SECURITY.md](SECURITY.md).

### Added

- `pyaio.uring`: asyncio file I/O on io_uring (`UringIO`), with a thread-pool
  fallback (`ThreadIO`) and `open_io()` to pick one. Supports `read`, `write`,
  `write_all` and `fsync`/`fdatasync`.
- Requests issued in the same event loop iteration are submitted with a
  single system call.
- Cancelling an awaiting task cancels the kernel request.
- Backpressure through `max_inflight`. Excess requests wait in a queue
  without allocating buffers.
- `aclose()`/`close()` with an optional timeout. Requests that outlive it are
  abandoned safely: their buffers are leaked and a `ResourceWarning` is
  emitted, rather than freeing memory the kernel may still write to.
- Kernel feature probing at startup. Kernels without the needed opcodes fall
  back to threads.
- Detection of `fork()`: a ring is unusable in the child, and the parent's
  ring is unaffected.
- Documentation: `docs/architecture.md` (with diagrams), `SECURITY.md`,
  `RELEASING.md`.
- License text: BSD-3-Clause (`LICENSE`). The project has always declared
  itself BSD-licensed but never said which variant or shipped the text.
- manylinux wheels for x86_64 and aarch64 (CPython 3.10 to 3.14) with
  liburing 2.15 bundled, built by cibuildwheel.
- GitHub Actions CI: Python 3.10 to 3.14 on two Ubuntu releases, an
  AddressSanitizer/UBSan job, and a job that checks the fallback when
  Docker's seccomp profile blocks io_uring.

### Changed

- Requires Python 3.10+. Python 2 support is gone.
- The build uses setuptools and `pyproject.toml`, since distutils was
  removed in Python 3.12.
- `import pyaio` no longer loads the POSIX AIO extension or calls
  `aio_init()`. Both happen on first use of `pyaio.aio_read`, `aio_write`
  or `aio_init`.
- Legacy API: an exception raised in a callback is now reported through
  `sys.unraisablehook`. It used to kill the process with `SIGKILL`.
- `README.markdown` was renamed to `README.md`.
- Legacy tests use their own temporary files and fail (instead of hanging)
  when an assertion inside a callback fails.

### Fixed

- Legacy API: invalid arguments no longer continue with uninitialised values
  (they raise `TypeError`). Allocation failures raise `MemoryError` in
  `aio_write` too. Memory and references are no longer leaked when
  submission fails.

### Removed

- `pyaio.gevent` (`aioFile`). It depended on `gevent.coros`, which current
  gevent no longer provides, and is superseded by the asyncio API.
