# Project metadata lives in pyproject.toml; this file only declares the
# C extensions, which setuptools cannot express declaratively.
import re

from setuptools import setup, Extension

with open('pyaio/__init__.py') as f:
    version = re.search(r"^__version__ = '([^']+)'", f.read(), re.M).group(1)

pyaiocore = \
    Extension('pyaio.core',
    sources = ['pyaio/core.c'],
    libraries = ['rt'],
    extra_compile_args = ['-D_FILE_OFFSET_BITS=64'],
    define_macros=[('PYAIO_VERSION', '"{0}"'.format(version))])

# io_uring backend; skipped with a warning when liburing is not installed
pyaiouring = \
    Extension('pyaio._uring',
    sources = ['pyaio/_uring.c'],
    libraries = ['uring'],
    optional = True)

setup(ext_modules = [pyaiocore, pyaiouring])
