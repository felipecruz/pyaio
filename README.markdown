Python Asynchronous I/O bindings (aio.h)
========================================

Version 0.2
Working on Linux only

Reading

```python
import pyaio
	
def aio_callback(buf):
    print 'python callback %s' % buf
	
pyaio.aio_read('/tmp/pyaio.txt', 10, 20, aio_callback)
```

Writing

```python
import pyaio

def aio_callback():
print 'done writing!'

pyaio.aio_write('/tmp/pyaio.txt', "Writing Test.......", 30, 15, aio_callbac)
```
