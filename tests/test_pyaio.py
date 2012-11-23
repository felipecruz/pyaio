import pyaio
import time
import os

def test_aio_read():
    def callback(buf, rt, er):
        assert buf == "#include <Python.h>\n"
        assert rt == len(buf)
        assert er == 0

    fileno = os.open('./pyaio/pyaio.c', os.O_RDONLY)
    ret = pyaio.aio_read(fileno, 0, 20, callback)
    assert ret == 0
    time.sleep(0.2)

def test_aio_read_stress():
    def callback(buf, rt, er):
        assert buf == "#include <Python.h>\n"
        assert rt == len(buf)
        assert er == 0

    fileno = os.open('./pyaio/pyaio.c', os.O_RDONLY)
    for x in range(1000):
        ret = pyaio.aio_read(fileno, 0, 20, callback)
        time.sleep(0.001)
        assert ret == 0

    time.sleep(0.3)

def test_aio_write():
    def callback2(rt, er):
        assert rt == 10
        assert er == 0
        f = file('/tmp/c.txt', 'r')
        content = f.read()
        assert content == "pyaiopyaio"

    fileno = os.open('/tmp/c.txt', os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
    ret = pyaio.aio_write(fileno, "pyaiopyaio", 0, callback2)
    assert ret == 0
    time.sleep(0.2)

def test_aio_write_read_stress():
    def callback2(rt, er):
        assert rt == 10
        assert er == 0

    def callback(buf, rt, er):
        if rt > 0:
            assert len(buf) == rt
        else:
            # EOF
            assert rt == 0
        assert er == 0


    fileno = os.open('/tmp/c.txt', os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    # These could hit in any order so its not safe to say the
    for x in range(1000):
        ret = pyaio.aio_write(fileno,
                              "pyaiopyaio", x * 10, callback2)
        time.sleep(0.001)
        ret = pyaio.aio_read(fileno,  x * 10, 10, callback)
        time.sleep(0.001)
    assert ret == 0
    time.sleep(0.3)

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
