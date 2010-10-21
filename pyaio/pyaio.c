#include <Python.h>
#include <aio.h>
#include <signal.h>
#include <string.h>

typedef struct py_aio_callback {
	struct aiocb *cb;
	PyObject *callback;
} Pyaio_cb;

static PyObject *MyModuleError;

PyDoc_STRVAR(pyaio_read_doc,
		"aio_read(filename, offset, len, callback)\n");

static void aio_completion_handler(int sig, siginfo_t *info, void *context)
{
	Pyaio_cb *aio;
	struct aiocb *cb;
	PyObject *callback, *result;
	char* buff;

	aio = (Pyaio_cb*) info->si_value.sival_ptr;

	cb = aio->cb;
	callback = aio->callback;

	buff = malloc((cb->aio_nbytes + 1) * sizeof(char));
	strncpy(buff, (char*)cb->aio_buf, cb->aio_nbytes);
	buff[cb->aio_nbytes] = '\0';

	printf("buff %s\n", buff);

	if (aio_error( cb ) == 0) {
		result = PyObject_CallObject(callback, Py_BuildValue("(s)", buff));
	}

	return;
}

static PyObject *
pyaio_read(PyObject *dummy, PyObject *args) {

	const char *filename;
	struct aiocb cb;
	int ret, offset, numbytes;

	Pyaio_cb *aio;
	PyObject *result = NULL;
	PyObject *callback;
	FILE *file = NULL;
	struct sigaction sa;

	if (PyArg_ParseTuple(args, "siiO:set_callback", &filename, &offset, &numbytes, &callback)) {
		if (!PyCallable_Check(callback)) {
			PyErr_SetString(PyExc_TypeError,
					"parameter must be callable");
			return NULL;
		}
		Py_XINCREF(callback); /* Add a reference to new callback */
	}

	file = fopen(filename, "r");

	aio = malloc(sizeof(Pyaio_cb));

	aio->cb = malloc(sizeof(struct aiocb));
	bzero((char *) aio->cb, sizeof(struct aiocb));

	aio->callback = callback;

	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = aio_completion_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, NULL);

	aio->cb->aio_buf = malloc((numbytes) * sizeof(char));
	aio->cb->aio_fildes = fileno(file);
	aio->cb->aio_nbytes = numbytes;
	aio->cb->aio_offset = offset;
	aio->cb->aio_sigevent.sigev_notify =  SIGEV_SIGNAL;
	aio->cb->aio_reqprio = 0;
	aio->cb->aio_sigevent.sigev_signo = SIGUSR1;
	aio->cb->aio_sigevent.sigev_notify_attributes = NULL;
	aio->cb->aio_sigevent.sigev_value.sival_ptr = aio;

	ret = aio_read(aio->cb);

	if (ret < 0)
		perror("aio_read");

	return Py_BuildValue("i", ret);

}

static PyMethodDef TutorialMethods[] = {

		{ "aio_read", pyaio_read,
		METH_VARARGS, pyaio_read_doc },

		{ NULL, NULL, 0, NULL }
};

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

	MyModuleError = PyErr_NewException("pyaio.error", NULL, NULL);

	Py_INCREF(MyModuleError);

	if (!MyModuleError) {
		Py_DECREF(__version__);
		return NULL;
	}

#if PY_MAJOR_VERSION >= 3
	m = Py_InitModule("pyaio", TutorialMethods);
#else
	m = Py_InitModule3("pyaio", TutorialMethods, NULL);
#endif

	if (!m) {
		Py_DECREF(__version__);
		Py_DECREF(MyModuleError);
		return NULL;
	}

	if (PyModule_AddObject(m, "__version__", __version__)
			|| PyModule_AddObject(m, "error", MyModuleError)) {
		Py_DECREF(__version__);
		Py_DECREF(MyModuleError);
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
