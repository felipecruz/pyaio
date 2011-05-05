from distutils.core import setup, Extension

version = "0.1"

mymodule = Extension('pyaio', 
                     sources = ['pyaio/pyaio.c'], 
                     libraries = ['rt'],
                     define_macros=[
                                ("PYAIO_VERSION",
                                 "\"{0}\"".format(version))
                               ]
                    )

setup(
    name = 'pyaio', 
    version = '0.1',
    description = 'Python Asynchronous I/O bindings (aio.h)',
    author = 'Felipe Cruz',
    author_email = 'felipecruz@loogica.net',
    classifiers =   [
                    'Operating System :: POSIX',
                    'Development Status :: 4 - Beta'
                    'License :: OSI Approved :: BSD License'
                    ],
    ext_modules = [mymodule])

