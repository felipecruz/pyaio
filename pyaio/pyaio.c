#include <Python.h>
#include <aio.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>

typedef struct py_aio_callback {
    struct aiocb *cb;
    PyObject *callback;
} Pyaio_cb;

PyDoc_STRVAR(pyaio_read_doc,
        "aio_read(fileno, offset, len, callback)\n");

PyDoc_STRVAR(pyaio_write_doc,
        "aio_write(fileno, buffer, offset, callback)\n");

static void aio_read_completion_handler(int sig, siginfo_t *info, void *context)
{
    Pyaio_cb *aio;
    struct aiocb *cb;
    PyObject *callback, *args;
    int read_size = 0;
    char *read_buf = NULL;

    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    aio = (Pyaio_cb*) info->si_value.sival_ptr;

    cb = aio->cb;
    callback = aio->callback;

    /* Lets only let python know about how much was actually read */
    if (aio_return(cb) > 0) {
        read_size = aio_return(cb);
        read_buf = cb->aio_buf;
    }

    args = Py_BuildValue("(s#ii)", read_buf, read_size,
                         aio_return(cb), aio_error(cb));

    Py_XINCREF(callback);
    Py_XINCREF(args);

    PyObject_CallObject(callback, args);

    Py_XDECREF(callback);
    Py_XDECREF(args);

    free(cb->aio_buf);
    free(cb);

    PyGILState_Release(gstate);
}

static void aio_write_completion_handler(int sig, siginfo_t *info, void *context)
{
    Pyaio_cb *aio;
    struct aiocb *cb;
    PyObject *callback, *args;

    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    aio = (Pyaio_cb*) info->si_value.sival_ptr;
    cb = aio->cb;
    callback = aio->callback;

    args = Py_BuildValue("(ii)", aio_return(cb), aio_error(cb));

    Py_XINCREF(callback);
    Py_XINCREF(args);

    PyObject_CallObject(callback, args);

    Py_XDECREF(callback);
    Py_XDECREF(args);

    free(cb->aio_buf);
    free(cb);

    PyGILState_Release(gstate);
}

static void init_sig_handlers()
{
    struct sigaction *sa;

    /* Install Read Handler */
    sa = malloc(sizeof(struct sigaction));
    //sa->sa_flags = SA_RESTART;
    sigemptyset(&sa->sa_mask);
    sa->sa_sigaction = aio_read_completion_handler;
    sa->sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN+10, sa, NULL);

    /* Install Write Handler */
    sa = malloc(sizeof(struct sigaction));
    //sa->sa_flags = SA_RESTART;
    sigemptyset(&sa->sa_mask);
    sa->sa_sigaction = aio_write_completion_handler;
    sa->sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN+11, sa, NULL);
}

static PyObject *
pyaio_read(PyObject *dummy, PyObject *args) {

    int ret, numbytes, fd;
    uint64_t offset;

    Pyaio_cb *aio;
    PyObject *callback, *return_;

    if (PyArg_ParseTuple(args, "iKiO:set_callback", &fd, &offset, &numbytes,
                                 &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
    }

    aio = malloc(sizeof(Pyaio_cb));

    aio->cb = malloc(sizeof(struct aiocb));
    bzero((char *) aio->cb, sizeof(struct aiocb));

    aio->callback = callback;

    aio->cb->aio_buf = malloc((numbytes) * sizeof(char));
    aio->cb->aio_fildes = fd;
    aio->cb->aio_nbytes = numbytes;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify =  SIGEV_SIGNAL;
    aio->cb->aio_sigevent.sigev_signo = SIGRTMIN+10;
    aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_read(aio->cb);

    return_ = Py_BuildValue("i", ret);

    Py_XINCREF(return_);

    return return_;

}


static PyObject *
pyaio_write(PyObject *dummy, PyObject *args) {

    const char *buffer;
    int ret, numbytes, fd;
    uint64_t offset;

    Pyaio_cb *aio;
    PyObject *callback, *return_;

    if (PyArg_ParseTuple(args, "is#KO:set_callback", &fd, &buffer,
                         &numbytes, &offset, &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
    }

    aio = malloc(sizeof(Pyaio_cb));

    aio->cb = malloc(sizeof(struct aiocb));
    bzero((char *) aio->cb, sizeof(struct aiocb));

    aio->callback = callback;

    aio->cb->aio_buf = malloc((numbytes) * sizeof(char));

    strncpy((char*)aio->cb->aio_buf, buffer, numbytes);

    aio->cb->aio_fildes = fd;
    aio->cb->aio_nbytes = numbytes;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify =  SIGEV_SIGNAL;
    aio->cb->aio_reqprio = 0;
    aio->cb->aio_sigevent.sigev_signo = SIGRTMIN+11;
    //aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_write(aio->cb);

    return_ = Py_BuildValue("i", ret);

    Py_XINCREF(return_);

    return return_;

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
    "pyaio",                             /* m_name      */
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
    m = Py_InitModule3("pyaio", PyaioMethods, NULL);
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

    init_sig_handlers();

    return m;
}

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit_pyaio(void)
{
    return init_pyaio();
}
#else
PyMODINIT_FUNC initpyaio(void) {
    init_pyaio();
}
#endif
