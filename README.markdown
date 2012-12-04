Python Asynchronous I/O bindings (aio.h)
========================================

Version 0.3
**Linux only**

You should wait for the callback to finish before queuing more requests in
a tight loop. pyaio could hang if you hit the max aio queue size.

Tuning
-------

pyaio.aio_init(max threads, max aio queue, max thread sleep time)

Linux Defaults to 20 threads and 64 queue size.
Pyaio will use 5*cores and 4*threads instead of those values if larger.

Reading
-------

API

``python
view = aio_read(fileno, file-offset, length, callback)
``

Example

```python
import pyaio
import os
	
def aio_callback(buf, rcode, errno):
    if rcode > 0:
        print 'python callback %s' % buf
    elif rcode == 0:
        print "EOF"
    else:
        print "Error: %d" % errno
        
fd = os.open('/tmp/pyaio.txt', os.O_RDONLY)
pyaio.aio_read(fd, 10, 20, aio_callback)
```

Writing
-------

API

``python
aio_write(fileno, buffer-object, file-offset, callback)
``

Example

```python
import pyaio
import os

def aio_callback(rt, errno):
    if rt > 0:
        print "Wrote %d bytes" % rt
    else:
        print "Got error: %d" % errno

fd = os.open('/tmp/pyaio.txt', os.O_WRONLY)
pyaio.aio_write(fd, "Writing Test.......", 30, aio_callback)
```

gevent Wrapper
--------------

For a file() like wrapper around aio_read and aio_write using gevent
a 'buffer' keyword argument to aioFile controls its internal buffer size

```python
from pyaio.gevent import aioFile
with aioFile('/tmp/pyaio.txt') as fr:
    data = fr.read()  # Entire File

with aioFile('/tmp/pyaio.txt', 'w') as fw:
    fw.write(data)
```
