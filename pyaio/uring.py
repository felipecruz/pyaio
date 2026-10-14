"""asyncio file I/O on io_uring.

    async with pyaio.uring.UringIO() as io:
        data = await io.read(fd, 0, 4096)
        await io.write_all(fd, b"hello", 0)
        await io.fsync(fd)

Requests made in the same event loop iteration are submitted to the kernel
with a single system call. Completions arrive through an eventfd watched by
the loop, so results are always delivered on the loop thread. Cancelling the
awaiting task cancels the kernel request too.

pyaio needs Linux 6.1+ with io_uring allowed. It does not fall back to
threads: where io_uring is blocked (Docker's default seccomp profile,
kernel.io_uring_disabled) creating a UringIO raises OSError explaining why.

See docs/architecture.md for how this works and SECURITY.md before enabling
io_uring on shared machines.
"""
import asyncio
import collections
import errno
import logging
import os

from ._uring import Ring

__all__ = ['UringIO']

logger = logging.getLogger(__name__)


def _copy_error(exc):
    # One exception instance per future, so tracebacks don't pile up.
    return OSError(exc.errno, exc.strerror or str(exc))


class _Op(asyncio.Future):
    """Future for one ring request. Cancelling it cancels the kernel request
    (asyncio cancels the awaited future when the awaiting task is
    cancelled)."""

    def cancel(self, msg=None):
        if not super().cancel(msg):
            return False
        if self._handle is not None:
            self._io._cancel_request(self._handle)
        return True


class UringIO(object):
    """io_uring-backed I/O bound to one event loop.

    entries       size of the submission queue.
    max_inflight  most requests handed to the kernel at once (default
                  2 * entries, the completion queue size). Extra requests
                  wait in a backlog and their buffers are not allocated yet.

    Every method must be called from the event loop's thread.
    """

    def __init__(self, entries=256, max_inflight=None, loop=None):
        if max_inflight is not None and max_inflight < 1:
            raise ValueError('max_inflight must be >= 1')
        self._loop = loop or asyncio.get_running_loop()
        self._ring = _create_ring(entries)
        self._max_inflight = max_inflight or 2 * entries
        self._backlog = collections.deque()
        self._submit_scheduled = False
        self._closing = False
        self._broken = None     # OSError that made the ring unusable
        self._idle = None       # future aclose() waits on
        self._fd = self._ring.fileno()
        self._loop.add_reader(self._fd, self._on_ready)

    @property
    def inflight(self):
        """Requests handed to the kernel and not yet completed."""
        return self._ring.inflight

    @property
    def queued(self):
        """Requests waiting in the backlog because of max_inflight."""
        return len(self._backlog)

    @property
    def closed(self):
        return self._closing or self._ring.closed

    def read(self, fd, offset, nbytes):
        """Read up to nbytes at offset; resolves to a bytearray (empty at
        EOF). Use offset -1 for the current file position."""
        return self._start(self._ring.submit_read, (fd, offset, nbytes))

    def write(self, fd, data, offset):
        """Write a bytes-like object at offset; resolves to the number of
        bytes written, which can be short just like os.pwrite. data must not
        be resized until the write completes (bytearray raises BufferError).
        """
        return self._start(self._ring.submit_write, (fd, data, offset))

    def fsync(self, fd, datasync=False):
        """fsync (or fdatasync) fd; resolves to 0."""
        return self._start(self._ring.submit_fsync, (fd, datasync))

    async def write_all(self, fd, data, offset):
        """Write all of data, retrying short writes. Returns len(data)."""
        view = memoryview(data).cast('B')
        total = 0
        while total < len(view):
            n = await self.write(fd, view[total:], offset)
            if n == 0:
                raise OSError(errno.EIO, 'write made no progress')
            total += n
            if offset != -1:
                offset += n
        return total

    # -- submission -------------------------------------------------------

    def _start(self, submit, args):
        if self._broken is not None:
            raise OSError(self._broken.errno,
                          'io_uring ring failed: %s' % self._broken)
        if self.closed:
            raise ValueError('I/O operation on closed UringIO')
        if asyncio.get_running_loop() is not self._loop:
            raise RuntimeError('UringIO used outside its event loop')
        fut = _Op(loop=self._loop)
        fut._io = self
        fut._handle = None
        if not self._backlog and self._ring.inflight < self._max_inflight:
            fut._handle = submit(*args, fut)
            self._schedule_submit()
        else:
            self._backlog.append((submit, args, fut))
        return fut

    def _drain_backlog(self):
        while self._backlog and self._ring.inflight < self._max_inflight:
            submit, args, fut = self._backlog.popleft()
            if fut.done():      # cancelled while waiting
                continue
            try:
                fut._handle = submit(*args, fut)
            except Exception as e:
                fut.set_exception(e)
                continue
            self._schedule_submit()

    def _schedule_submit(self):
        if not self._submit_scheduled:
            self._submit_scheduled = True
            self._loop.call_soon(self._submit)

    def _submit(self):
        self._submit_scheduled = False
        if self._ring.closed:
            return
        try:
            self._ring.submit()
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EBUSY, errno.EINTR):
                # Completion queue is backed up; retry after reaping.
                self._schedule_submit()
            else:
                self._break(e)

    def _cancel_request(self, handle):
        if self._ring.closed:
            return
        if self._ring.cancel(handle):
            self._schedule_submit()

    # -- completion -------------------------------------------------------

    def _on_ready(self):
        try:
            completions = self._ring.reap()
        except MemoryError:
            return  # nothing was lost; the ring re-armed its eventfd
        self._dispatch(completions)
        self._drain_backlog()
        if (self._idle is not None and not self._idle.done()
                and not self._ring.inflight):
            self._idle.set_result(None)

    @staticmethod
    def _dispatch(completions, error=None):
        for fut, res, payload in completions:
            if fut.done():  # the awaiting task was cancelled
                continue
            if res < 0:
                # OSError(errno, msg) picks the matching subclass, e.g.
                # FileNotFoundError or BrokenPipeError.
                fut.set_exception(_copy_error(error) if error else
                                  OSError(-res, os.strerror(-res)))
            elif payload is not None:
                fut.set_result(payload)
            else:
                fut.set_result(res)

    # -- shutdown ---------------------------------------------------------

    def _teardown(self, timeout, error=None):
        if self._ring.closed:
            return
        self._loop.remove_reader(self._fd)
        backlog, self._backlog = self._backlog, collections.deque()
        self._dispatch(self._ring.close(timeout), error)
        for _, _, fut in backlog:
            if fut.done():
                continue
            if error is not None:
                fut.set_exception(_copy_error(error))
            else:
                fut.cancel()

    def _break(self, exc):
        """The ring cannot submit anymore: fail everything outstanding
        instead of leaving futures pending forever."""
        logger.error('io_uring submit failed, closing ring: %s', exc)
        self._broken = exc
        self._teardown(0, exc)

    def close(self, timeout=None):
        """Cancel outstanding requests and release the ring.

        Blocks the calling thread until the kernel has released every
        buffer, or for at most `timeout` seconds; requests still running
        then are abandoned (their buffers are leaked, never freed while the
        kernel may write to them) and a ResourceWarning is emitted. Prefer
        `await aclose()` inside a running loop.
        """
        self._closing = True
        self._teardown(timeout)

    async def aclose(self, timeout=None):
        """Like close(), but waits for cancellations without blocking the
        event loop."""
        if self._ring.closed:
            return
        self._closing = True
        backlog, self._backlog = self._backlog, collections.deque()
        for _, _, fut in backlog:
            fut.cancel()
        if self._ring.inflight:
            self._ring.cancel_all()
            self._schedule_submit()
            self._idle = self._loop.create_future()
            try:
                await asyncio.wait_for(asyncio.shield(self._idle), timeout)
            except asyncio.TimeoutError:
                pass
        self._teardown(0)

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc_value, traceback):
        await self.aclose()


_BLOCKED = (errno.EPERM, errno.EACCES)


def _create_ring(entries):
    try:
        return Ring(entries)
    except OSError as e:
        if e.errno in _BLOCKED:
            raise OSError(e.errno,
                          'io_uring is blocked here (%s). Common causes: a '
                          'container seccomp profile such as Docker\'s '
                          'default, or the kernel.io_uring_disabled sysctl. '
                          'See SECURITY.md before allowing it.'
                          % e.strerror) from e
        raise
