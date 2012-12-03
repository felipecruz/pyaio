from .core import *

from multiprocessing import cpu_count
threads = cpu_count() * 5
aio_init(threads, threads * 4, 1);

