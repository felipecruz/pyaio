import os
import pyaio
import tulip

BUF_SIZE = 4096 # 4k

def aio_read(file_name, file_size=None, loop=None):
    fd = os.open(file_name, os.O_RDONLY)
    if not file_size:
        file_size = os.stat(fd).st_size

    if not loop:
        loop = tulip.get_event_loop()

    aio_future = tulip.futures.Future(loop=loop)
    def _set_aio_future_result(buf, rcode, errno):
        aio_future.set_result((buf, rcode, errno))
        aio.future.loop._write_to_self()

    rc = pyaio.aio_read(fd, 0, file_size, _set_aio_future_result)
    if rc < 0:
        aio_future.set_exception(Exception("pyaio read error"))
    return aio_future


