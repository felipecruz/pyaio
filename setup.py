from distutils.core import setup, Extension

version = '0.3'

mymodule = \
    Extension('pyaio',
    sources = ['pyaio/pyaio.c'],
    libraries = ['rt'],
    extra_compile_args = ['-D_FILE_OFFSET_BITS=64'],
    define_macros=[('PYAIO_VERSION', '"{0}"'.format(version))])

setup(
    name = 'pyaio',
    version = version,
    description = 'Python Asynchronous I/O bindings (aio.h)',
    author = 'Felipe Cruz',
    author_email = 'felipecruz@loogica.net',
    classifiers =   [
                    'Operating System :: POSIX',
                    'Development Status :: 4 - Beta',
                    'License :: OSI Approved :: BSD License'
                    ],
    ext_modules = [mymodule])

