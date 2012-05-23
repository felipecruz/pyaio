#include <Python.h>
#include <aio.h>
#include <signal.h>
#include <string.h>

typedef struct py_aio_callback {
    struct aiocb *cb;
    PyObject *callback;
} Pyaio_cb;

PyDoc_STRVAR(pyaio_read_doc,
        "aio_read(filename, offset, len, callback)\n");

PyDoc_STRVAR(pyaio_write_doc,
        "aio_write(filename, buffer, offset, len, callback)\n");

static void aio_read_completion_handler(int sig, siginfo_t *info, void *context)
{
    Pyaio_cb *aio;
    struct aiocb *cb;
    PyObject *callback, *args;
    char* buff;

    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    aio = (Pyaio_cb*) info->si_value.sival_ptr;

    cb = aio->cb;
    callback = aio->callback;

    buff = malloc((cb->aio_nbytes + 1) * sizeof(char));
    strncpy(buff, (char*)cb->aio_buf, cb->aio_nbytes);
    buff[cb->aio_nbytes] = '\0';
    
    args = Py_BuildValue("(s)", buff);

    Py_XINCREF(callback);
    Py_XINCREF(args);

    if (aio_error(cb) == 0 && aio_return(cb) > 0) {
        PyObject_CallObject(callback, args);
    }

    Py_XDECREF(callback);
    Py_XDECREF(args);

    close(cb->aio_fildes);
    free(cb->aio_buf);
    free(cb);
    free(buff);
    
    PyGILState_Release(gstate);
}

static void aio_write_completion_handler(int sig, siginfo_t *info, void *context)
{
    Pyaio_cb *aio;
    struct aiocb *cb;
    PyObject *callback;

    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    aio = (Pyaio_cb*) info->si_value.sival_ptr;
    cb = aio->cb;
    callback = aio->callback;

    Py_XINCREF(callback);

    if (aio_error(cb) == 0) {
        PyObject_CallObject(callback, NULL);
    }

    Py_XDECREF(callback);

    close(cb->aio_fildes);
    free(cb->aio_buf);
    free(cb);
        
    PyGILState_Release(gstate);
}

static PyObject *
pyaio_read(PyObject *dummy, PyObject *args) {

    const char *filename;
    int ret, offset, numbytes;

    Pyaio_cb *aio;
    PyObject *callback, *return_;
    FILE *file = NULL;
    struct sigaction *sa;

    if (PyArg_ParseTuple(args, "siiO:set_callback", &filename, &offset, &numbytes, &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
    }

    file = fopen(filename, "r");
    if (file == NULL) {
        PyErr_SetString(PyExc_IOError, "No such file or directory");
        return NULL;
    }

    aio = malloc(sizeof(Pyaio_cb));

    aio->cb = malloc(sizeof(struct aiocb));
    bzero((char *) aio->cb, sizeof(struct aiocb));

    aio->callback = callback;

    sa = malloc(sizeof(struct sigaction));
    sa->sa_flags = SA_RESTART;
    sigemptyset(&sa->sa_mask);
    sa->sa_sigaction = aio_read_completion_handler;
    sa->sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, sa, NULL);

    aio->cb->aio_buf = malloc((numbytes) * sizeof(char));
    aio->cb->aio_fildes = fileno(file);
    aio->cb->aio_nbytes = numbytes;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify =  SIGEV_SIGNAL;
    aio->cb->aio_sigevent.sigev_signo = SIGUSR1;
    aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_read(aio->cb);

    if (ret < 0)
        perror("aio_read");

    return_ = Py_BuildValue("i", ret);

    Py_XINCREF(return_);

    return return_;

}


static PyObject *
pyaio_write(PyObject *dummy, PyObject *args) {

    const char *filename;
    char *buffer;
    int ret, offset, numbytes;

    Pyaio_cb *aio;
    PyObject *callback, *return_;
    FILE *file = NULL;
    struct sigaction *sa;

    if (PyArg_ParseTuple(args, "ssiiO:set_callback", &filename, &buffer, &offset, &numbytes, &callback)) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError,
                    "parameter must be callable");
            return NULL;
        }
        Py_XINCREF(callback); /* Add a reference to new callback */
    }

    file = fopen(filename, "w");
    if (file == NULL) {
        PyErr_SetString(PyExc_IOError, "No such file or directory");
        return NULL;
    }
    aio = malloc(sizeof(Pyaio_cb));

    aio->cb = malloc(sizeof(struct aiocb));
    bzero((char *) aio->cb, sizeof(struct aiocb));

    aio->callback = callback;

    sa = malloc(sizeof(struct sigaction));

    sa->sa_flags = SA_RESTART;
    sigemptyset(&sa->sa_mask);
    sa->sa_sigaction = aio_write_completion_handler;
    sa->sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, sa, NULL);

    aio->cb->aio_buf = malloc((numbytes+1) * sizeof(char));

    strncpy((char*)aio->cb->aio_buf, buffer , numbytes);

    ((char*)aio->cb->aio_buf)[numbytes] = '\0';

    aio->cb->aio_fildes = fileno(file);
    aio->cb->aio_nbytes = numbytes;
    aio->cb->aio_offset = offset;
    aio->cb->aio_sigevent.sigev_notify =  SIGEV_SIGNAL;
    aio->cb->aio_reqprio = 0;
    aio->cb->aio_sigevent.sigev_signo = SIGUSR1;
    //aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
    aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

    ret = aio_write(aio->cb);

    if (ret < 0)
        perror("aio_write");

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
