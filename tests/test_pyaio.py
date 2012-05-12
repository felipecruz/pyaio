import pyaio

def test_aio_read():
    def callback(buf):
        print(buf)

    ret = pyaio.aio_read("./pyaio/pyaio.c", 0, 20, callback)
    assert ret == 0

def test_aio_write():
    def callback2():
        print("Done")

    ret = pyaio.aio_write("/tmp/c.txt", "pyaiopyaio", 0, 10, callback2)
    assert ret == 0
