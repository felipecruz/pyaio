"""Linux asynchronous file I/O for Python.

pyaio.uring         asyncio API on io_uring
pyaio.aio_read      legacy callback API on glibc POSIX AIO: deprecated
pyaio.aio_write     since 0.5 and removed in 0.6; use pyaio.uring.UringIO
pyaio.aio_init

The legacy functions are loaded on first use, so importing pyaio.uring does
not start glibc's AIO machinery.
"""
import warnings

__version__ = '0.5.0rc2'

_LEGACY = ('aio_read', 'aio_write', 'aio_init')
__all__ = list(_LEGACY)

_DEPRECATION = ('pyaio.{name} (the POSIX AIO callback API) is deprecated '
                'and will be removed in pyaio 0.6; use pyaio.uring.UringIO')

_core = None


def _load_core():
    global _core
    if _core is None:
        from multiprocessing import cpu_count
        with warnings.catch_warnings():
            # Importing pyaio.core directly warns; we warn per name below.
            warnings.simplefilter('ignore', DeprecationWarning)
            from . import core

        threads = cpu_count() * 5
        queue_size = threads * 4
        if threads > 20:   # glibc defaults to 20 threads, a 64-entry queue
            core.aio_init(threads, queue_size, 1)
        _core = core
    return _core


def __getattr__(name):
    # Not cached in globals(), so every legacy access goes through here and
    # warns at the caller's line.
    if name in _LEGACY:
        warnings.warn(_DEPRECATION.format(name=name), DeprecationWarning,
                      stacklevel=2)
        return getattr(_load_core(), name)
    raise AttributeError('module %r has no attribute %r' % (__name__, name))


def __dir__():
    return sorted(list(globals()) + list(_LEGACY))
