import pyaio
import time
import os
from threading import Semaphore

def test_aio_read():
    s = Semaphore(1)
    def callback(buf, rt, er):
        assert buf == "#define PY_SSIZE_T_CLEAN"
        assert rt == len(buf)
        assert er == 0
        s.release()

    fileno = os.open('./pyaio/core.c', os.O_RDONLY)
    s.acquire()
    ret = pyaio.aio_read(fileno, 0, 24, callback)
    assert ret == 0
    s.acquire()

def test_aio_read_stress():
    s = Semaphore(1000)
    def callback(buf, rt, er):
        assert buf == "#define PY_SSIZE_T_CLEAN"
        assert rt == len(buf)
        assert er == 0
        s.release()

    fileno = os.open('./pyaio/core.c', os.O_RDONLY)
    for x in range(1000):
        s.acquire()
        ret = pyaio.aio_read(fileno, 0, 24, callback)
        time.sleep(0.0001)  # Don't Stack up the request too high
        assert ret == 0

    # SO we know we are done
    for x in range(1000):
        s.acquire()

def test_aio_write():
    s = Semaphore(1)
    def callback2(rt, er):
        assert rt == 10
        assert er == 0
        f = file('/tmp/c.txt', 'r')
        content = f.read()
        assert content == "pyaiopyaio"
        s.release()

    fileno = os.open('/tmp/c.txt', os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    s.acquire()
    ret = pyaio.aio_write(fileno, "pyaiopyaio", 0, callback2)
    assert ret == 0
    s.acquire()

def test_aio_write_read_stress():
    s = Semaphore(1000)
    r = Semaphore(1)  # Read Before Write Please
    def callback2(rt, er):
        assert rt == 10
        assert er == 0
        s.release()
        r.release()

    def callback(buf, rt, er):
        if rt > 0:
            assert len(buf) == rt
        else:
            # EOF
            assert rt == 0
        assert er == 0
        r.release()


    fileno = os.open('/tmp/c.txt', os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    # These could hit in any order so its not safe to say the
    for x in range(1000):
        r.acquire()
        s.acquire()
        ret = pyaio.aio_write(fileno,
                              "pyaiopyaio", x * 10, callback2)
        assert ret == 0
        time.sleep(0.0001)
        r.acquire()
        ret = pyaio.aio_read(fileno,  x * 10, 10, callback)
        assert ret == 0
        time.sleep(0.0001)  # Not sure why this breaks down at ultra high speed

    # Catch them all
    for x in range(1000):
        s.acquire()

def run(method):
    ts = time.time()
    print "Running %s" % method.__name__
    method()
    print "Passed!"
    print "Total Time (%.2f)." % (time.time() - ts)

if __name__ == '__main__':
    run(test_aio_read)
    run(test_aio_write)
    run(test_aio_read_stress)
    run(test_aio_write_read_stress)
