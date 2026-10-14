import pyaio
import tempfile
import time
import os

# Read this file itself, so the tests work from any directory.
SOURCE = os.path.abspath(__file__)
with open(SOURCE, 'rb') as _f:
    HEAD = _f.read(24)

def scratch_path():
    """A fresh file per test, so parallel runs don't collide."""
    fd, path = tempfile.mkstemp(prefix='pyaio-test-')
    os.close(fd)
    return path

class hax(object):
    """Counts outstanding callbacks and records failures inside them.

    Callback exceptions no longer kill the process, so an assert failing in
    a callback would otherwise leave the counter stuck and the test hanging.
    """
    def __init__(self):
        self.x = 0
        self.errors = []

    def track(self, fn):
        def wrapper(*args):
            try:
                fn(*args)
            except BaseException as e:
                self.errors.append(e)
                self.x -= 1
        return wrapper

    def wait(self, timeout=10):
        deadline = time.monotonic() + timeout
        while self.x != 0 and not self.errors:
            assert time.monotonic() < deadline, \
                '%d callbacks still pending after %ss' % (self.x, timeout)
            time.sleep(0.01)
        if self.errors:
            raise self.errors[0]

def test_aio_read():
    s = hax()
    def callback(buf, rt, er):
        assert buf == HEAD
        assert rt == len(buf)
        assert er == 0
        s.x -= 1

    fileno = os.open(SOURCE, os.O_RDONLY)
    s.x += 1
    ret = pyaio.aio_read(fileno, 0, 24, s.track(callback))
    assert ret == 0
    s.wait()

def test_aio_read_stress():
    s = hax()
    def callback(buf, rt, er):
        assert buf == HEAD
        assert rt == len(buf)
        assert er == 0
        s.x -= 1

    fileno = os.open(SOURCE, os.O_RDONLY)
    for x in range(1000):
        s.x += 1
        ret = pyaio.aio_read(fileno, 0, 24, s.track(callback))
        time.sleep(0.0001)
        assert ret == 0

    s.wait()

def test_aio_write():
    path = scratch_path()
    s = hax()
    def callback2(rt, er):
        assert rt == 10
        assert er == 0
        with open(path, 'r') as f:
            content = f.read()
        assert content == "pyaiopyaio"
        s.x -= 1

    fileno = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    s.x += 1
    ret = pyaio.aio_write(fileno, b"pyaiopyaio", 0, s.track(callback2))
    assert ret == 0

    s.wait()

def test_aio_write_read_stress():
    path = scratch_path()
    s = hax()
    def callback2(rt, er):
        assert rt == 10
        assert er == 0
        s.x -= 1

    def callback(buf, rt, er):
        if rt > 0:
            assert len(buf) == rt
        else:
            # EOF
            assert rt == 0
        assert er == 0
        s.x -= 1

    fileno = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    # These could hit in any order so its not safe to say the
    for x in range(1000):
        s.x += 1
        ret = pyaio.aio_write(fileno,
                              b"pyaiopyaio", x * 10, s.track(callback2))
        assert ret == 0
        time.sleep(0.0001)
        s.x += 1
        ret = pyaio.aio_read(fileno,  x * 10, 10, s.track(callback))
        assert ret == 0
        time.sleep(0.0001)
    s.wait()

def test_callback_exception_does_not_kill_process():
    import sys
    seen = []
    old_hook = sys.unraisablehook
    sys.unraisablehook = lambda u: seen.append(u.exc_value)
    try:
        def callback(buf, rt, er):
            raise ValueError('boom')

        fileno = os.open(SOURCE, os.O_RDONLY)
        pyaio.aio_read(fileno, 0, 10, callback)
        deadline = time.monotonic() + 10
        while not seen and time.monotonic() < deadline:
            time.sleep(0.01)
        os.close(fileno)
    finally:
        sys.unraisablehook = old_hook
    assert isinstance(seen[0], ValueError)

def test_bad_arguments_raise():
    import pytest
    with pytest.raises(TypeError):
        pyaio.aio_read('not an fd', 0, 10, lambda *a: None)
    with pytest.raises(TypeError):
        pyaio.aio_write(1, b'x', 0, 'not callable')
    with pytest.raises(TypeError):
        pyaio.aio_init(1, 2)

def run(method):
    ts = time.time()
    print("Running %s" % method.__name__)
    method()
    print("Passed!")
    print("Total Time (%.2f)." % (time.time() - ts))

if __name__ == '__main__':
    run(test_aio_read)
    run(test_aio_write)
    run(test_aio_read_stress)
    run(test_aio_write_read_stress)
