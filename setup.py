from distutils.core import setup, Extension
import sys

if sys.version < '2.7':
    print "pyaio requires python of at least version 2.7"
    sys.exit(1)

version = '0.3'

pyaiocore = \
    Extension('pyaio.core',
    sources = ['pyaio/core.c'],
    libraries = ['rt'],
    extra_compile_args = ['-D_FILE_OFFSET_BITS=64'],
    define_macros=[('PYAIO_VERSION', '"{0}"'.format(version))])

setup(
    name = 'pyaio',
    version = version,
    description = 'Python Asynchronous I/O bindings (aio.h)',
    author = 'Felipe Cruz',
    author_email = 'felipecruz@loogica.net',
    url = 'https://github.com/felipecruz/pyaio',
    classifiers =   [
                    'Operating System :: POSIX',
                    'Development Status :: 4 - Beta',
                    'License :: OSI Approved :: BSD License'
                    ],
    ext_modules = [pyaiocore],
    py_modules = ['pyaio.gevent'])


