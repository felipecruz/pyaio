import asyncio
import errno
import gc
import os
import sys

import pytest

from pyaio import uring
from pyaio.uring import Ring


@pytest.fixture
def path(tmp_path):
    p = tmp_path / 'data.bin'
    p.write_bytes(b'0123456789' * 100)
    return str(p)


def run(body):
    async def main():
        io = uring.UringIO()
        try:
            return await body(io)
        finally:
            io.close()
    return asyncio.run(main())


def test_read(path):
    fd = os.open(path, os.O_RDONLY)

    async def body(io):
        assert await io.read(fd, 0, 10) == b'0123456789'
        assert await io.read(fd, 995, 10) == b'56789'   # short read
        assert await io.read(fd, 5000, 10) == b''       # past EOF
        assert await io.read(fd, 0, 0) == b''
    run(body)
    os.close(fd)


def test_read_current_position(path):
    fd = os.open(path, os.O_RDONLY)
    os.lseek(fd, 3, os.SEEK_SET)

    async def body(io):
        assert await io.read(fd, -1, 4) == b'3456'
        assert await io.read(fd, -1, 4) == b'7890'
    run(body)
    os.close(fd)


def test_write_read_back(tmp_path):
    fd = os.open(str(tmp_path / 'w.bin'), os.O_RDWR | os.O_CREAT)

    async def body(io):
        assert await io.write(fd, b'pyaio', 0) == 5
        assert await io.write(fd, bytearray(b'URING'), 5) == 5
        assert await io.write(fd, memoryview(b'xx!!')[2:], 10) == 2
        assert await io.fsync(fd) == 0
        assert await io.fsync(fd, datasync=True) == 0
        assert await io.read(fd, 0, 100) == b'pyaioURING!!'
    run(body)
    os.close(fd)


def test_concurrent(tmp_path):
    fd = os.open(str(tmp_path / 'c.bin'), os.O_RDWR | os.O_CREAT)
    n = 2000

    async def body(io):
        written = await asyncio.gather(
            *[io.write(fd, b'%010d' % i, i * 10) for i in range(n)])
        assert written == [10] * n
        chunks = await asyncio.gather(
            *[io.read(fd, i * 10, 10) for i in range(n)])
        assert chunks == [b'%010d' % i for i in range(n)]
    run(body)
    os.close(fd)


def test_more_requests_than_ring_entries(path):
    """Queuing past the SQ size must flush, not fail."""
    fd = os.open(path, os.O_RDONLY)

    async def body(io):
        reads = [io.read(fd, i, 1) for i in range(500)]
        return await asyncio.gather(*reads)

    async def main():
        io = uring.UringIO(entries=8)
        try:
            return await body(io)
        finally:
            io.close()

    out = asyncio.run(main())
    assert out == [str(i % 10).encode() for i in range(500)]
    os.close(fd)


def test_errors(tmp_path):
    rfd = os.open(str(tmp_path / 'ro'), os.O_RDONLY | os.O_CREAT)

    async def body(io):
        with pytest.raises(OSError) as e:
            await io.read(987654, 0, 10)
        assert e.value.errno == errno.EBADF
        with pytest.raises(OSError) as e:
            await io.write(rfd, b'nope', 0)
        assert e.value.errno == errno.EBADF
    run(body)
    os.close(rfd)


def test_invalid_arguments():
    ring = Ring()
    with pytest.raises(ValueError):
        ring.submit_read(0, 0, -1, None)
    with pytest.raises(TypeError):
        ring.submit_write(0, 'not bytes', 0, None)
    assert ring.inflight == 0
    ring.close()


def test_closed_ring():
    ring = Ring()
    assert ring.close() == []
    assert ring.closed
    for call in (ring.submit, ring.reap, ring.fileno):
        with pytest.raises(ValueError):
            call()
    with pytest.raises(ValueError):
        ring.submit_read(0, 0, 1, None)
    ring.close()   # idempotent


def test_write_buffer_pinned_until_completion(tmp_path):
    """The kernel holds a pointer into the buffer, so it must not be
    resizable until the write completes."""
    fd = os.open(str(tmp_path / 'p.bin'), os.O_RDWR | os.O_CREAT)
    ring = Ring()
    buf = bytearray(b'x' * 64)
    ring.submit_write(fd, buf, 0, 'tok')
    with pytest.raises(BufferError):
        buf.extend(b'y')
    ring.close()
    buf.extend(b'y')   # released after close
    os.close(fd)


def test_cancelled_waiter(path):
    """Cancelling the awaiting task must not break later completions."""
    fd = os.open(path, os.O_RDONLY)

    async def body(io):
        task = asyncio.ensure_future(io.read(fd, 0, 10))
        await asyncio.sleep(0)
        task.cancel()
        assert await io.read(fd, 10, 5) == b'01234'
        while io.inflight:
            await asyncio.sleep(0.001)
    run(body)
    os.close(fd)


def test_close_with_requests_in_flight():
    """close() cancels pending work and waits for the kernel to finish."""
    r, w = os.pipe()   # a pipe read blocks until we close the ring

    async def main():
        io = uring.UringIO()
        fut = io.read(r, -1, 10)
        await asyncio.sleep(0.01)
        assert io.inflight == 1
        io.close()
        assert io.inflight == 0
        with pytest.raises(OSError) as e:
            await fut
        assert e.value.errno in (errno.ECANCELED, errno.EINTR)

    asyncio.run(main())
    os.close(r)
    os.close(w)


def test_dealloc_with_requests_in_flight():
    r, w = os.pipe()
    ring = Ring()
    ring.submit_read(r, -1, 10, object())
    ring.submit()
    del ring
    gc.collect()
    os.close(r)
    os.close(w)


def test_no_reference_leaks(tmp_path):
    fd = os.open(str(tmp_path / 'r.bin'), os.O_RDWR | os.O_CREAT)
    token, data = object(), b'leakcheck'
    before = sys.getrefcount(token), sys.getrefcount(data)
    ring = Ring()
    for _ in range(100):
        ring.submit_write(fd, data, 0, token)
        ring.submit_read(fd, 0, 9, token)
    ring.submit()
    done = []
    while len(done) < 200:
        done.extend(ring.reap())
    del done
    ring.close()
    assert (sys.getrefcount(token), sys.getrefcount(data)) == before
    os.close(fd)




def test_cancel_stale_handle(path):
    fd = os.open(path, os.O_RDONLY)
    ring = Ring()
    handle = ring.submit_read(fd, 0, 4, 'tok')
    ring.submit()
    done = []
    while not done:
        done = ring.reap()
    assert ring.cancel(handle) is False     # already completed
    r, w = os.pipe()
    live = ring.submit_read(r, -1, 4, 'pipe')
    assert live != handle
    assert ring.cancel(live) is True
    ring.submit()
    out = []
    while not out:
        out = ring.reap()
    assert out[0][:2] == ('pipe', -errno.ECANCELED)
    ring.close()
    for x in (fd, r, w):
        os.close(x)


def test_aclose_cancels_without_blocking_loop():
    r, w = os.pipe()

    async def main():
        io = uring.UringIO()
        futs = [io.read(r, -1, 10) for _ in range(5)]
        await asyncio.sleep(0.01)
        await io.aclose(timeout=5)
        assert io.closed
        for fut in futs:
            with pytest.raises(OSError) as e:
                await fut
            assert e.value.errno == errno.ECANCELED
        with pytest.raises(ValueError):
            io.read(r, -1, 1)

    asyncio.run(main())
    os.close(r)
    os.close(w)


def test_backpressure(path):
    fd = os.open(path, os.O_RDONLY)

    async def main():
        io = uring.UringIO(entries=8, max_inflight=4)
        futs = [io.read(fd, i, 1) for i in range(100)]
        assert io.inflight == 4
        assert io.queued == 96
        futs[50].cancel()     # cancelled while queued: never submitted
        peak = 0
        while not all(f.done() for f in futs):
            peak = max(peak, io.inflight)
            await asyncio.sleep(0)
        assert peak <= 4
        assert [f.result() for i, f in enumerate(futs) if i != 50] == \
            [str(i % 10).encode() for i in range(100) if i != 50]
        await io.aclose()

    asyncio.run(main())
    os.close(fd)


class _FailingRing(object):
    """Delegates to a real Ring but makes submit() fail hard."""

    def __init__(self, ring):
        self._ring = ring

    def __getattr__(self, name):
        return getattr(self._ring, name)

    def submit(self):
        raise OSError(errno.EBADF, 'simulated ring failure')


def test_hard_submit_error_fails_futures_instead_of_hanging(path):
    fd = os.open(path, os.O_RDONLY)

    async def main():
        io = uring.UringIO(max_inflight=2)
        io._ring = _FailingRing(io._ring)
        futs = [io.read(fd, 0, 1) for _ in range(5)]   # 2 queued in ring
        results = await asyncio.wait_for(
            asyncio.gather(*futs, return_exceptions=True), 5)

        def failed(r):
            return isinstance(r, OSError) and r.errno == errno.EBADF
        # The 2 requests already in the submission queue reach the kernel
        # when teardown submits its cancels; on a cached file a read may
        # finish before its cancel (kernel dependent), and its real result
        # is delivered. Everything else must fail with the submit error.
        assert all(failed(r) or r == b'0' for r in results[:2])
        assert all(failed(r) for r in results[2:])     # never submitted
        errors = [r for r in results if failed(r)]
        assert len({id(r) for r in errors}) == len(errors)  # one each
        with pytest.raises(OSError):
            io.read(fd, 0, 1)

    asyncio.run(main())
    os.close(fd)


def test_errors_map_to_oserror_subclasses(tmp_path):
    dfd = os.open(str(tmp_path), os.O_RDONLY | os.O_DIRECTORY)
    r, w = os.pipe()
    os.close(r)

    async def body(io):
        with pytest.raises(IsADirectoryError):
            await io.read(dfd, 0, 10)
        with pytest.raises(BrokenPipeError):
            await io.write(w, b'x', -1)
    run(body)
    os.close(dfd)
    os.close(w)


def test_wrong_thread_rejected(path):
    import threading
    fd = os.open(path, os.O_RDONLY)

    async def main():
        io = uring.UringIO()
        errors = []

        def other():
            try:
                io.read(fd, 0, 1)
            except RuntimeError as e:
                errors.append(e)
        t = threading.Thread(target=other)
        t.start()
        t.join()
        assert errors
        await io.aclose()

    asyncio.run(main())
    os.close(fd)


@pytest.mark.filterwarnings('ignore::DeprecationWarning')
def test_ring_unusable_after_fork(path):
    fd = os.open(path, os.O_RDONLY)
    ring = Ring()
    pid = os.fork()
    if pid == 0:
        code = 1
        try:
            ring.reap()
        except RuntimeError:
            code = 0
        del ring            # must not touch the parent's ring
        gc.collect()
        os._exit(code)
    _, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0
    ring.submit_read(fd, 0, 4, 'parent')    # parent ring still healthy
    ring.submit()
    out = []
    while not out:
        out = ring.reap()
    assert out == [('parent', 4, bytearray(b'0123'))]
    ring.close()
    os.close(fd)


def test_write_all_retries_short_writes():
    calls = []

    async def short_write(fd, data, offset):
        calls.append((bytes(data), offset))
        return min(3, len(data))

    async def main():
        io = uring.UringIO()
        io.write = short_write
        try:
            return await io.write_all(1, b'abcdefgh', 10)
        finally:
            await io.aclose()

    assert asyncio.run(main()) == 8
    assert calls == [(b'abcdefgh', 10), (b'defgh', 13), (b'gh', 16)]


def test_write_all(tmp_path):
    fd = os.open(str(tmp_path / 'a.bin'), os.O_RDWR | os.O_CREAT)

    async def body(io):
        assert await io.write_all(fd, b'x' * 100000, 0) == 100000
        assert os.fstat(fd).st_size == 100000
    run(body)
    os.close(fd)




def test_import_does_not_load_legacy_core(tmp_path):
    import subprocess
    import pyaio
    code = ('import sys, pyaio.uring; '
            'assert "pyaio.core" not in sys.modules; '
            'import pyaio; pyaio.aio_read; '
            'assert "pyaio.core" in sys.modules')
    # Import the same pyaio this test process uses (installed or built in
    # place), not whatever a source checkout in the cwd would shadow it with.
    env = dict(os.environ,
               PYTHONPATH=os.path.dirname(os.path.dirname(pyaio.__file__)))
    subprocess.run([sys.executable, '-c', code], check=True, cwd=tmp_path,
                   env=env)


def test_blocked_io_uring_explains_why(monkeypatch):
    """Where io_uring is denied (seccomp, sysctl) there is no fallback:
    creating a UringIO must say what is going on."""
    def denied(entries):
        raise PermissionError(errno.EPERM, 'Operation not permitted')
    monkeypatch.setattr(uring, 'Ring', denied)

    async def main():
        uring.UringIO()
    with pytest.raises(OSError) as e:
        asyncio.run(main())
    assert e.value.errno == errno.EPERM
    assert 'seccomp' in str(e.value) and 'io_uring_disabled' in str(e.value)


def test_task_cancel_never_raises_when_kernel_cancel_fails():
    """Cancelling a task must not raise even if the kernel cancel cannot be
    queued; the request then just completes and its result is dropped."""
    r, w = os.pipe()

    class NoCancelRing(_FailingRing):
        def submit(self):
            return self._ring.submit()

        def cancel(self, handle):
            raise BlockingIOError('submission queue is full')

    async def main():
        io = uring.UringIO()
        io._ring = NoCancelRing(io._ring)
        task = asyncio.ensure_future(io.read(r, -1, 10))
        await asyncio.sleep(0.01)
        assert task.cancel() is True        # must not raise
        with pytest.raises(asyncio.CancelledError):
            await task
        os.write(w, b'x')                   # let the read finish
        for _ in range(100):
            if io.inflight == 0:
                break
            await asyncio.sleep(0.001)
        assert io.inflight == 0
        await io.aclose()

    asyncio.run(main())
    os.close(r)
    os.close(w)

