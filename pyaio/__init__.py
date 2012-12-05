from .core import *

from multiprocessing import cpu_count
threads = cpu_count() * 5
queue_size = threads * 4
if threads > 20:
    aio_init(threads, queue_size, 1)

