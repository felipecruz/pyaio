import pyaio

def aio_callback(buf):
    print 'python callback %s' % buf
    return
    
def aio_callback2(buf):
    print '2 python callback %s' % buf
    return
    
def aio_callback3():
    print 'done writing!'
    return
    
pyaio.aio_read('/tmp/a.txt', 0, 10, aio_callback2)

pyaio.aio_read('/tmp/b.txt', 10, 20, aio_callback)

pyaio.aio_write('/tmp/c.txt', "Writing Test.......", 0, 10, aio_callback3)

pyaio.aio_write('/tmp/c.txt', "Writing Test.......", 15, 15, aio_callback3)

pyaio.aio_write('/tmp/c.txt', "Writing Test.......", 30, 15, aio_callback3)

import time
time.sleep(1)
