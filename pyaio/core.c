#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <aio.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

typedef struct py_aio_callback {
    struct aiocb *cb;
    PyObject *callback;
    Py_buffer buffer_view;
    PyObject *buffer;
    int8_t read;
} Pyaio_cb;

#define DEPRECATED_NOTE \
    "\nDeprecated since pyaio 0.5, removed in 0.6: use pyaio.uring.UringIO.\n"

PyDoc_STRVAR(pyaio_read_doc,
        "aio_read(fileno, offset, len, callback)\n" DEPRECATED_NOTE);

PyDoc_STRVAR(pyaio_write_doc,
        "aio_write(fileno, buffer, offset, callback)\n" DEPRECATED_NOTE);

PyDoc_STRVAR(pyaio_init_doc,
        "aio_init(max_threads, max_queue_size, max_thread_idle_time)\n"
        DEPRECATED_NOTE);


static void
pyaio_cb_free(Pyaio_cb *aio)
{
    if (aio->buffer_view.obj)
        PyBuffer_Release(&aio->buffer_view);
    Py_XDECREF(aio->buffer);
    Py_XDECREF(aio->callback);
    free(aio->cb);
    free(aio);
}

/* Runs on the main thread (pending call) or with the GIL held. */
static int _async_callback(void *arg)
{
    Pyaio_cb *aio = (Pyaio_cb *)arg;
    struct aiocb *cb = aio->cb;
    PyObject *args, *result = NULL;
    int err = aio_error(cb);
    ssize_t ret = aio_return(cb);

    PyBuffer_Release(&aio->buffer_view); /* we are done with the memory */

    if (aio->read) {
        Py_ssize_t size = ret > 0 ? ret : 0;
        if ((size_t)size < cb->aio_nbytes &&
            PyByteArray_Resize(aio->buffer, size) < 0)
            PyErr_WriteUnraisable(aio->callback);
        args = Py_BuildValue("(Oni)", aio->buffer, (Py_ssize_t)ret, err);
    }
    else { /* WRITE */
        args = Py_BuildValue("(ni)", (Py_ssize_t)ret, err);
    }
    if (args) {
        result = PyObject_CallObject(aio->callback, args);
        Py_DECREF(args);
    }
    if (result == NULL) {
        /* Report it like an exception in a __del__ method. This used to
         * SIGKILL the whole process. */
        PyErr_WriteUnraisable(aio->callback);
    }
    Py_XDECREF(result);
    pyaio_cb_free(aio);
    return 0;
}

static void aio_completion_handler(sigval_t sigval)
{
    Pyaio_cb *aio;
    aio = (Pyaio_cb*) sigval.sival_ptr;

    /* Hybrid Approach if Pending fails grab GIL do it directly */
    if(Py_AddPendingCall(&_async_callback, aio) < 0) {
        PyGILState_STATE state = PyGILState_Ensure();
        _async_callback(aio);
        PyGILState_Release(state);
    }

    return;
}

static PyObject *
pyaio_init(PyObject *dummy, PyObject *args) {
    struct aioinit init;
    int aio_threads, aio_num, aio_idle_time;

    if (!PyArg_ParseTuple(args, "iii:aio_init", &aio_threads, &aio_num,
                          &aio_idle_time))
        return NULL;
    memset(&init, 0, sizeof(init));
    init.aio_threads = aio_threads;
    init.aio_num = aio_num;
    init.aio_idle_time = aio_idle_time;
    aio_init(&init);
    Py_RETURN_NONE;
}

/* Fill in the aiocb fields shared by reads and writes. */
static void
pyaio_prepare(Pyaio_cb *aio, int fd, Py_ssize_t offset)
{
    aio->cb->aio_buf = aio->buffer_view.buf;
    aio->cb->aio_fildes = fd;
    aio->cb->aio_nbytes = aio->buffer_view.len;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify = SIGEV_THREAD;  /* EvIL */
    aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_notify_function = aio_completion_handler;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;
}

static Pyaio_cb *
pyaio_cb_new(PyObject *callback)
{
    Pyaio_cb *aio;

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "parameter must be callable");
        return NULL;
    }
    aio = calloc(1, sizeof(Pyaio_cb));
    if (!aio) {
        PyErr_NoMemory();
        return NULL;
    }
    aio->cb = calloc(1, sizeof(struct aiocb));
    if (!aio->cb) {
        free(aio);
        PyErr_NoMemory();
        return NULL;
    }
    Py_INCREF(callback);
    aio->callback = callback;
    return aio;
}

static PyObject *
pyaio_read(PyObject *dummy, PyObject *args) {
    int fd;
    Py_ssize_t numbytes, offset;
    Pyaio_cb *aio;
    PyObject *callback;

    if (!PyArg_ParseTuple(args, "innO:aio_read", &fd, &offset, &numbytes,
                          &callback))
        return NULL;
    if (numbytes < 0) {
        PyErr_SetString(PyExc_ValueError, "length must be >= 0");
        return NULL;
    }
    aio = pyaio_cb_new(callback);
    if (!aio)
        return NULL;
    aio->read = 1;  /* Read Operation */

    /* Empty ByteArray of the requested read size */
    aio->buffer = PyByteArray_FromStringAndSize(NULL, numbytes);
    if (!aio->buffer)
        goto fail;
    if (PyObject_GetBuffer(aio->buffer, &aio->buffer_view, PyBUF_CONTIG) < 0)
        goto fail;
    pyaio_prepare(aio, fd, offset);

    if (aio_read(aio->cb) < 0) {
        PyErr_SetFromErrno(PyExc_IOError);
        goto fail;
    }
    return PyLong_FromLong(0);

fail:
    pyaio_cb_free(aio);
    return NULL;
}

static PyObject *
pyaio_write(PyObject *dummy, PyObject *args) {
    int fd;
    Py_ssize_t offset;
    Pyaio_cb *aio;
    PyObject *buffer, *callback;

    if (!PyArg_ParseTuple(args, "iOnO:aio_write", &fd, &buffer, &offset,
                          &callback))
        return NULL;
    if (!PyObject_CheckBuffer(buffer)) {
        PyErr_SetString(PyExc_TypeError,
                "write buffer must support buffer interface");
        return NULL;
    }
    aio = pyaio_cb_new(callback);
    if (!aio)
        return NULL;
    aio->read = 0;  /* Write Operation */

    Py_INCREF(buffer);
    aio->buffer = buffer;
    if (PyObject_GetBuffer(buffer, &aio->buffer_view, PyBUF_CONTIG_RO) < 0)
        goto fail;
    pyaio_prepare(aio, fd, offset);

    if (aio_write(aio->cb) < 0) {
        PyErr_SetFromErrno(PyExc_IOError);
        goto fail;
    }
    return PyLong_FromLong(0);

fail:
    pyaio_cb_free(aio);
    return NULL;
}

static PyMethodDef PyaioMethods[] = {

        { "aio_read", pyaio_read,
        METH_VARARGS, pyaio_read_doc },

        { "aio_write", pyaio_write,
        METH_VARARGS, pyaio_write_doc },

        { "aio_init", pyaio_init,
        METH_VARARGS, pyaio_init_doc },

        { NULL, NULL, 0, NULL }
};
#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "core",                             /* m_name      */
    "Python POSIX aio (aio.h) bindings", /* m_doc       */
    -1,                                  /* m_size      */
    PyaioMethods,                        /* m_methods   */
    NULL,                                /* m_reload    */
    NULL,                                /* m_traverse  */
    NULL,                                /* m_clear     */
    NULL,                                /* m_free      */
};
#endif

PyObject *
init_pyaio(void) {
    PyObject *m;
    PyObject *__version__;

#if PY_VERSION_HEX < 0x03090000
    /* We will be using threads (automatic since Python 3.7) */
    PyEval_InitThreads();
#endif
#if PY_MAJOR_VERSION >= 3
    __version__ = PyUnicode_FromFormat("%s", PYAIO_VERSION);
#else
    __version__ = PyString_FromFormat("%s", PYAIO_VERSION);
#endif

    if (!__version__) {
        return NULL;
    }

#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create(&moduledef);
#else
    m = Py_InitModule3("core", PyaioMethods, NULL);
#endif

    if (!m) {
        Py_DECREF(__version__);
        return NULL;
    }

    if (PyModule_AddObject(m, "__version__", __version__)) {
        Py_DECREF(__version__);
        Py_DECREF(m);
        return NULL;
    }

    if (PyErr_WarnEx(PyExc_DeprecationWarning,
                     "pyaio.core (the POSIX AIO callback API) is deprecated "
                     "and will be removed in pyaio 0.6; use "
                     "pyaio.uring.UringIO", 1) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit_core(void)
{
    return init_pyaio();
}
#else
PyMODINIT_FUNC initcore(void) {
    init_pyaio();
}
#endif
