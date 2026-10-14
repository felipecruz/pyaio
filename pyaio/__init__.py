"""Linux asynchronous file I/O for Python.

pyaio.uring         asyncio API on io_uring (recommended)
pyaio.aio_read      legacy callback API on glibc POSIX AIO
pyaio.aio_write
pyaio.aio_init

The legacy functions are loaded on first use, so importing pyaio.uring does
not start glibc's AIO machinery.
"""

__version__ = '0.5.0rc1'

_LEGACY = ('aio_read', 'aio_write', 'aio_init')
__all__ = list(_LEGACY)


def _load_core():
    from multiprocessing import cpu_count
    from . import core

    threads = cpu_count() * 5
    queue_size = threads * 4
    if threads > 20:   # glibc defaults to 20 threads and a 64-entry queue
        core.aio_init(threads, queue_size, 1)
    for name in _LEGACY:
        globals()[name] = getattr(core, name)
    return core


def __getattr__(name):
    if name in _LEGACY:
        return getattr(_load_core(), name)
    raise AttributeError('module %r has no attribute %r' % (__name__, name))


def __dir__():
    return sorted(list(globals()) + list(_LEGACY))
