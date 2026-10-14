# How pyaio works

pyaio has two independent backends:

| | `pyaio.uring` (new) | `pyaio.aio_read` / `aio_write` (legacy) |
|---|---|---|
| Kernel interface | io_uring (Linux 6.1+) | glibc POSIX AIO (`aio.h`) |
| Who does the I/O | the kernel, asynchronously | a glibc **user-space thread pool** running blocking `pread`/`pwrite` |
| API style | asyncio futures | callbacks |
| Batching | one syscall per loop iteration | one request at a time |
| Cancellation | yes, through the kernel | no |

Most of this document is about the io_uring backend. The legacy path is
summarised at the end for comparison.

## 1. The moving parts

```mermaid
flowchart LR
    subgraph Python["Python process"]
        App["your coroutine"] -->|"await io.read()"| UIO["UringIO<br/>(pyaio/uring.py)"]
        UIO -->|"submit_read / submit()"| Ring["Ring<br/>(pyaio/_uring.c)"]
        Loop["asyncio event loop"] -->|"eventfd readable:<br/>_on_ready()"| UIO
        UIO -->|"reap()"| Ring
    end
    subgraph Kernel["Linux kernel"]
        SQ[("submission<br/>queue")] --> Exec["io_uring<br/>executes I/O"]
        Exec --> CQ[("completion<br/>queue")]
        CQ -->|"signal"| EFD["eventfd"]
    end
    Ring -->|"liburing:<br/>write SQEs + io_uring_enter"| SQ
    CQ -->|"shared memory"| Ring
    EFD -->|"epoll"| Loop
```

* **`Ring`** (C) owns one io_uring instance and one eventfd. It knows nothing
  about asyncio: it queues requests, submits them, and returns finished ones
  as `(token, result, payload)` tuples. The token is any Python object; the
  asyncio layer uses the request's future.
* **`UringIO`** (Python) binds a ring to one event loop. It turns calls into
  futures, batches submissions, enforces backpressure and handles shutdown.
* The **eventfd** is the bridge between the two: the kernel bumps it whenever
  a completion is posted, and the loop watches it with `add_reader`, the same
  way it watches a socket. Results are therefore always delivered on the loop
  thread with no locks, threads or signals involved.

## 2. Life of a read

```mermaid
sequenceDiagram
    autonumber
    participant C as coroutine
    participant U as UringIO
    participant R as Ring (C)
    participant K as kernel
    participant L as event loop

    C->>U: io.read(fd, 0, 4096)
    U->>R: submit_read(fd, 0, 4096, future)
    Note over R: allocate bytearray(4096)<br/>take a slot, fill an SQE<br/>(nothing sent yet)
    R-->>U: handle
    U->>L: call_soon(_submit) (once per iteration)
    U-->>C: future
    C->>C: await future (suspends)
    Note over U,L: other reads issued in the same<br/>iteration join the same batch
    L->>U: _submit()
    U->>R: submit()
    R->>K: io_uring_enter (one syscall for the whole batch)
    K->>K: perform the read into the bytearray
    K->>L: completion posted, eventfd signalled
    L->>U: _on_ready()
    U->>R: reap()
    Note over R: shrink bytearray on short read<br/>free the slot
    R-->>U: [(future, 4096, bytearray)]
    U->>C: future.set_result(bytearray)
```

The bytearray is created in C and is not visible to Python until `reap()`
returns it, so nothing can resize or free it while the kernel is writing into
it. For writes, the caller's buffer is *pinned* with the buffer protocol
(`PyObject_GetBuffer`). A pinned `bytearray` refuses to resize
(`BufferError`) until the write completes.

## 3. Request states

```mermaid
stateDiagram-v2
    direction LR
    [*] --> Backlog: inflight >= max_inflight
    [*] --> Queued: submit_*()
    Backlog --> Queued: a slot frees up
    Backlog --> [*]: cancelled (never reached the kernel)
    Queued --> InKernel: submit() / io_uring_enter
    InKernel --> Completed: CQE posted
    InKernel --> Cancelling: task cancelled, ASYNC_CANCEL sent
    Cancelling --> Completed: CQE (usually -ECANCELED)
    Completed --> Released: reap() delivers result, frees buffer
    InKernel --> Abandoned: close(timeout) expired
    Released --> [*]
    Abandoned --> [*]: buffer leaked on purpose
```

* **Backlog** is a plain Python deque in `UringIO`. Requests there have no
  buffer yet, so a burst of a million reads does not allocate a million
  buffers. `max_inflight` defaults to the completion queue size, which also
  keeps the kernel from overflowing it.
* **Cancelling** a request on a pipe or socket stops it immediately. Reads
  and writes on regular files that the kernel has already started generally
  run to completion; the result is simply discarded.
* **Abandoned** only happens when a close timeout expires. The kernel might
  still write into the buffer, so pyaio never frees it: the memory is leaked
  and a `ResourceWarning` is emitted. A leak is a bug you can see; a
  use-after-free is memory corruption you cannot.

## 4. Handles: why not just use pointers?

Each in-flight request lives in a slot table. The 64-bit `user_data` the
kernel carries for each request is a handle, not a memory address:

```mermaid
flowchart LR
    H["user_data (64 bits)"] --> G["high 32 bits:<br/>generation"]
    H --> S["low 32 bits:<br/>slot index"]
    S --> T["slots[slot]"]
    G --> V{"slots[slot].gen<br/>== generation?"}
    T --> V
    V -->|yes| Live["live request"]
    V -->|no| Stale["stale handle:<br/>ignore"]
```

asyncio runs a task's cancellation one loop iteration *after* `cancel()` is
called. In between, the request may finish, its memory may be freed, and a
new request may land at the same address. Cancelling "by pointer" would then
cancel the wrong request. With a generation number, a stale handle can never
match the new occupant of its slot. `user_data == 0` is reserved for pyaio's
internal cancel requests.

## 5. Shutting down

```mermaid
flowchart TD
    A["aclose(timeout) or close(timeout)"] --> B["reject new requests,<br/>cancel the backlog"]
    B --> C["queue one ASYNC_CANCEL<br/>matching every request,<br/>submit"]
    C --> D{"all requests<br/>completed?"}
    D -->|yes| F["tear down ring,<br/>close eventfd"]
    D -->|"no, keep waiting"| E{"timeout<br/>expired?"}
    E -->|no| D
    E -->|yes| G["abandon the rest:<br/>futures get ECANCELED,<br/>buffers leaked,<br/>ResourceWarning"]
    G --> F
```

* `aclose()` waits on the event loop, so other tasks keep running.
  `close()` blocks the calling thread; use it from synchronous code.
* A single `IORING_ASYNC_CANCEL_ANY` request cancels everything at once.
* If the `Ring` is garbage collected while requests are still in flight, it
  runs the same procedure with a 1 second limit.

## 6. When things go wrong

| Failure | What pyaio does |
|---|---|
| Kernel older than 6.1 | `UringIO()` raises `OSError(ENOSYS)` naming the running kernel |
| io_uring blocked by seccomp or `kernel.io_uring_disabled` | `UringIO()` raises `PermissionError` naming the likely causes |
| `submit()` fails with `EAGAIN`/`EBUSY`/`EINTR` | retries on the next loop iteration |
| `submit()` fails with anything else | the ring is closed and **every** pending future fails with that error, so nothing hangs |
| Out of memory while reaping | the completion stays in the kernel's queue, the eventfd is re-armed, and the next iteration retries |
| Request fails in the kernel | the future raises the matching `OSError` subclass (`FileNotFoundError`, `BrokenPipeError`, ...) |
| `UringIO` called from another thread | `RuntimeError` |
| `Ring` used in a child after `fork()` | `RuntimeError`; the child never touches the parent's queues |

## 7. Where it runs

There is deliberately no fallback. Supporting environments where io_uring
is usually unavailable would mean a second implementation to maintain and
test, for places pyaio is not meant for.

```mermaid
flowchart TD
    S["UringIO()"] --> V{"Linux 6.1<br/>or newer?"}
    V -->|no| E1["OSError ENOSYS:<br/>needs Linux 6.1+"]
    V -->|yes| Y{"io_uring_setup()<br/>allowed?"}
    Y -->|"EPERM: seccomp<br/>(Docker default) or<br/>io_uring_disabled"| E2["PermissionError<br/>explaining the cause"]
    Y -->|yes| U["ready"]
```

## 8. The legacy POSIX AIO path

> **Deprecated since 0.5, removed in 0.6.** Kept here for comparison.

For comparison, this is what `pyaio.aio_read` does. glibc implements POSIX
AIO in user space: a pool of threads performs ordinary blocking calls.

```mermaid
sequenceDiagram
    participant P as Python (main thread)
    participant G as glibc AIO thread
    participant K as kernel
    P->>G: aio_read(aiocb) queues the request
    G->>K: pread() (blocking)
    K-->>G: data
    G->>G: SIGEV_THREAD: new notification thread
    G->>P: Py_AddPendingCall(callback)
    Note over P: runs only when the main thread<br/>next executes Python bytecode
    P->>P: callback(buf, rcode, errno)
```

This is why the README says to tune `aio_init(threads, ...)`: the thread pool
*is* the asynchrony. io_uring removes the threads, the per-request
notification thread and the pending-call hop.

## Source map

| File | Role |
|---|---|
| `pyaio/_uring.c` | `Ring`: slot table, SQE/CQE handling, cancellation, shutdown, fork detection, kernel check |
| `pyaio/uring.py` | `UringIO`: asyncio integration |
| `pyaio/core.c` | legacy POSIX AIO bindings (deprecated, removed in 0.6) |
| `pyaio/__init__.py` | version, lazy loading of the legacy API |
| `tests/test_uring.py` | io_uring tests, including the failure modes above |
| `tests/test_pyaio.py` | legacy API tests |
| `tools/install-liburing.sh` | builds the pinned liburing for CI and wheels |
| `tools/ci-pytest.sh` | runs pytest in CI and reports results as GitHub annotations |
| `.github/workflows/` | CI (`ci.yml`) and wheel builds and publishing (`release.yml`) |
