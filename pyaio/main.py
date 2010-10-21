import pyaio
import time

def aio_callback(buf):
    print 'python callback %s' % buf
    
def aio_callback2(buf):
    print '2 python callback %s' % buf
    

pyaio.aio_read('/tmp/a.txt', 0, 10, aio_callback2)

pyaio.aio_read('/tmp/b.txt', 10, 20, aio_callback)

time.sleep(1)

#print "Done!"