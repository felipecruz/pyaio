from distutils.core import setup, Extension

version = "0.1"

mymodule = Extension('pyaio', 
                     sources = ['pyaio.c'], 
                     libraries = ['rt'],
                     define_macros=[
                                ("PYAIO_VERSION",
                                 "\"{0}\"".format(version))
                               ]
                    )

setup(name = 'pyaio', version = '0.1',
   description = 'Python Asynchronous I/O bindings (aio.h)',
   ext_modules = [mymodule])

