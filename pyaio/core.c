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

PyDoc_STRVAR(pyaio_read_doc,
        "aio_read(fileno, offset, len, callback)\n");

PyDoc_STRVAR(pyaio_write_doc,
        "aio_write(fileno, buffer, offset, callback)\n");

static int _async_callback(void *arg)
{
    Pyaio_cb *aio = (Pyaio_cb *)arg;
    struct aiocb *cb;
    PyObject *callback, *args, *result, *buffer;
    Py_buffer *buffer_view;

    cb = aio->cb;
    callback = aio->callback;
    buffer_view = &(aio->buffer_view);
    buffer = aio->buffer; /* Should have ref count of +2 */

    PyBuffer_Release(buffer_view); /* -1 refcount we are done with it */

    if (aio->read) {
        if (aio_return(cb) > 0) {
            if (aio_return(cb) < cb->aio_nbytes) {
                PyByteArray_Resize(buffer, aio_return(cb));
            }
        }
        else {
            PyByteArray_Resize(buffer, 0);
        }
        args = Py_BuildValue("(Oni)", buffer, aio_return(cb), aio_error(cb));
    }
    else { /* WRITE */
        args = Py_BuildValue("(ni)", aio_return(cb), aio_error(cb));
    }
    result = PyObject_CallObject(callback, args);
    if (result == NULL) {
        printf("Exception in aio callback, dying!\n");
        kill(getpid(), SIGKILL);  // DIE FAST
    }
    Py_XDECREF(buffer); /* -1 refcount. we should be at 0 now */
    Py_XDECREF(result);
    Py_XDECREF(callback);
    Py_XDECREF(args);
    free((struct aiocb *)cb);
    free(aio);
    return 0;
}

static void aio_completion_handler(sigval_t sigval)
{
    Pyaio_cb *aio;
    int tries = 1;
    aio = (Pyaio_cb*) sigval.sival_ptr;

    //should set an upper limit like 50 retries or something
    //while(Py_AddPendingCall(&_async_callback, aio) < 0) {
    //    usleep(500*(tries/2)); //Step off timer
    //    tries += 1;
    //}
    PyGILState_STATE state = PyGILState_Ensure();
        _async_callback(aio);
    PyGILState_Release(state);

    return;
}

static PyObject *
pyaio_read(PyObject *dummy, PyObject *args) {

    int fd;
    Py_ssize_t numbytes, offset, ret;
    Pyaio_cb *aio;
    PyObject *callback, *return_, *buffer;
    Py_buffer *buffer_view;

    Py_XINCREF(args);
    if (PyArg_ParseTuple(args, "innO:set_callback", &fd, &offset, &numbytes,
                                 &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
    }
    Py_XDECREF(args);
    aio = malloc(sizeof(Pyaio_cb));
    buffer_view = &(aio->buffer_view);

    aio->cb = malloc(sizeof(struct aiocb));
    bzero((char *) aio->cb, sizeof(struct aiocb));

    /* Empty ByteArray of the requested read size INCREF*/
    buffer = PyByteArray_FromStringAndSize(NULL, numbytes);
    /* Get the buffer view / put it in the aio struct INCREF */
    PyObject_GetBuffer(buffer, buffer_view, PyBUF_CONTIG);

    aio->callback = callback;
    aio->buffer = buffer;
    aio->read = 1;  /* Read Operation */

    aio->cb->aio_buf = buffer_view->buf;
    aio->cb->aio_fildes = fd;
    aio->cb->aio_nbytes = buffer_view->len;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify = SIGEV_THREAD;  /* EvIL */
    aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_notify_function = aio_completion_handler;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_read(aio->cb);

    if (ret < 0) {
        return PyErr_SetFromErrno(PyExc_IOError);
    }
    else {
        return_ = Py_BuildValue("n", ret);
        return return_;
    }
}

static PyObject *
pyaio_write(PyObject *dummy, PyObject *args) {

    PyObject *buffer;
    Py_buffer *buffer_view;

    int fd;
    Py_ssize_t offset, ret;

    Pyaio_cb *aio;
    PyObject *callback, *return_;
    Py_XINCREF(args);
    if (PyArg_ParseTuple(args, "iOnO:set_callback", &fd, &buffer,
                          &offset, &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        if (!PyObject_CheckBuffer(buffer)) {
            PyErr_SetString(PyExc_TypeError,
                    "write buffer must support buffer interface");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
        Py_XINCREF(buffer);
    }
    Py_XDECREF(args);

    aio = malloc(sizeof(Pyaio_cb));
    buffer_view = &(aio->buffer_view);
    /* Get a Buffer INCREF */
    PyObject_GetBuffer(buffer, buffer_view, PyBUF_CONTIG_RO);


    aio->cb = malloc(sizeof(struct aiocb));
    bzero((void *) aio->cb, sizeof(struct aiocb));
    aio->read = 0;  /* Write Operation */
    aio->callback = callback;
    aio->buffer = buffer;
    aio->cb->aio_buf = buffer_view->buf;
    aio->cb->aio_fildes = fd;
    aio->cb->aio_nbytes = buffer_view->len;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify = SIGEV_THREAD;
    aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_notify_function = aio_completion_handler;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_write(aio->cb);

    if (ret < 0) {
        return PyErr_SetFromErrno(PyExc_IOError);
    }
    else {
        return_ = Py_BuildValue("n", ret);
        return return_;
    }
}


static PyMethodDef PyaioMethods[] = {

        { "aio_read", pyaio_read,
        METH_VARARGS, pyaio_read_doc },

        { "aio_write", pyaio_write,
        METH_VARARGS, pyaio_write_doc },

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
    PyEval_InitThreads();

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
