import pyaio
import time

def test_aio_read():
    def callback(buf):
        assert buf == "#include <Python.h>\n"

    ret = pyaio.aio_read("./pyaio/pyaio.c", 0, 20, callback)
    assert ret == 0

def test_aio_read_stress():
    def callback(buf):
        assert buf == "#include <Python.h>\n"

    for x in range(1000):
        ret = pyaio.aio_read("./pyaio/pyaio.c", 0, 20, callback)
        assert ret == 0

    time.sleep(0.2)

def test_aio_write():
    def callback2():
        f = file('/tmp/c.txt', 'r')
        content = f.read()
        assert content == "pyaiopyaio"

    ret = pyaio.aio_write("/tmp/c.txt", "pyaiopyaio", 0, 10, callback2)
    assert ret == 0

def test_aio_write_read_stress():
    def callback2():
        pass

    def callback(buf):
        assert buf == "pyaiopyaio"

    for x in range(1000):
        ret = pyaio.aio_write("/tmp/c.txt", 
                              "pyaiopyaio", x * 10, 10, callback2)
        ret = pyaio.aio_read("/tmp/c.txt",  x * 10, 10, callback)

    assert ret == 0
    time.sleep(0.3)

