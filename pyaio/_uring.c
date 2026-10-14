/*
 * io_uring backend for pyaio.
 *
 * A Ring owns one io_uring instance plus an eventfd that the kernel signals
 * whenever a completion is posted. The ring does no threading of its own:
 * callers queue requests (submit_read/submit_write/submit_fsync), push them
 * to the kernel with submit(), watch fileno() in their event loop and call
 * reap() when it becomes readable.
 *
 * Requests live in a slot table. The SQE user_data is a handle made of the
 * slot index and a generation number, so a stale handle (for a request that
 * already completed and whose slot was reused) never matches a live request.
 * user_data 0 is reserved for internal cancel SQEs.
 *
 * Buffer safety rule: a buffer handed to the kernel is only released after
 * its CQE has been seen. If shutdown gives up waiting, the buffer is leaked
 * on purpose instead of freed.
 *
 * All ring access happens with the GIL held, so a Ring must not be shared
 * with a free-threaded build without a lock (the module does not declare
 * Py_mod_gil_not_used).
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <liburing.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

enum { OP_READ, OP_WRITE, OP_FSYNC };

#define DEALLOC_TIMEOUT_NS  (1000LL * 1000 * 1000)   /* 1 second */

typedef struct {
    PyObject *token;    /* opaque object handed back by reap() */
    PyObject *buffer;   /* bytearray being read into (OP_READ only) */
    Py_buffer view;     /* pinned source buffer (OP_WRITE only) */
    unsigned int nbytes;
    int op;
    uint32_t slot;
    uint32_t gen;
} Request;

typedef struct {
    PyObject_HEAD
    struct io_uring ring;
    int efd;
    int open;
    unsigned long fork_gen;     /* fork_generation at creation time */
    Request **slots;
    uint32_t *free_slots;       /* stack of unused slot indices */
    uint32_t nslots;
    uint32_t nfree;
    uint32_t next_gen;
    Py_ssize_t inflight;        /* requests queued or submitted, no CQE yet */
} RingObject;

/* Bumped in every child after fork(); rings created before it are dead. */
static volatile unsigned long fork_generation = 0;

static void
on_fork_child(void)
{
    fork_generation++;
}

static inline uint64_t
request_handle(Request *req)
{
    return ((uint64_t)req->gen << 32) | req->slot;
}

static int
slots_grow(RingObject *self)
{
    uint32_t n = self->nslots ? self->nslots * 2 : 64, i;
    Request **slots;
    uint32_t *free_slots;

    if (n <= self->nslots) {    /* 2^32 requests in flight: not happening */
        PyErr_SetString(PyExc_OverflowError, "too many requests in flight");
        return -1;
    }
    slots = PyMem_Realloc(self->slots, n * sizeof(Request *));
    if (!slots) {
        PyErr_NoMemory();
        return -1;
    }
    self->slots = slots;
    free_slots = PyMem_Realloc(self->free_slots, n * sizeof(uint32_t));
    if (!free_slots) {
        PyErr_NoMemory();
        return -1;
    }
    self->free_slots = free_slots;
    /* Push in reverse so lower indices are handed out first. */
    for (i = n; i > self->nslots; i--) {
        self->slots[i - 1] = NULL;
        self->free_slots[self->nfree++] = i - 1;
    }
    self->nslots = n;
    return 0;
}

static Request *
request_new(RingObject *self, int op)
{
    Request *req;

    if (self->nfree == 0 && slots_grow(self) < 0)
        return NULL;
    req = PyMem_Calloc(1, sizeof(Request));
    if (!req) {
        PyErr_NoMemory();
        return NULL;
    }
    req->op = op;
    if (++self->next_gen == 0)  /* gen 0 would allow handle 0 */
        ++self->next_gen;
    req->gen = self->next_gen;
    req->slot = self->free_slots[--self->nfree];
    self->slots[req->slot] = req;
    return req;
}

/* Only call once the kernel is done with the request's buffer. */
static void
request_release(RingObject *self, Request *req)
{
    self->slots[req->slot] = NULL;
    self->free_slots[self->nfree++] = req->slot;
    if (req->op == OP_WRITE && req->view.obj)
        PyBuffer_Release(&req->view);
    Py_XDECREF(req->buffer);
    Py_XDECREF(req->token);
    PyMem_Free(req);
}

/* The kernel may still touch the buffer: drop everything but the buffer. */
static void
request_abandon(RingObject *self, Request *req)
{
    self->slots[req->slot] = NULL;
    Py_XDECREF(req->token);
    PyMem_Free(req);    /* req->buffer and req->view are leaked */
}

static Request *
request_lookup(RingObject *self, uint64_t handle)
{
    uint32_t slot = (uint32_t)handle, gen = (uint32_t)(handle >> 32);
    Request *req;

    if (slot >= self->nslots)
        return NULL;
    req = self->slots[slot];
    return (req && req->gen == gen) ? req : NULL;
}

/*
 * Build (token, result, payload) without freeing the request, so a failure
 * here leaves the CQE in the ring to be retried by the next reap().
 */
static PyObject *
request_result(Request *req, int res)
{
    PyObject *payload = Py_None;

    if (req->op == OP_READ && res >= 0) {
        if ((unsigned int)res < req->nbytes &&
            PyByteArray_Resize(req->buffer, res) < 0)
            return NULL;
        payload = req->buffer;
    }
    return Py_BuildValue("(OiO)", req->token, res, payload);
}

static int
ring_check_open(RingObject *self)
{
    if (self->open && self->fork_gen != fork_generation) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Ring cannot be used in a child process after "
                        "fork(); create a new one");
        return -1;
    }
    if (!self->open) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed ring");
        return -1;
    }
    return 0;
}

static PyObject *
set_errno_error(int err)
{
    errno = err;
    return PyErr_SetFromErrno(PyExc_OSError);
}

/* Get a free SQE, flushing the submission queue once if it is full. */
static struct io_uring_sqe *
ring_get_sqe(RingObject *self)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&self->ring);
    int ret;

    if (sqe)
        return sqe;
    ret = io_uring_submit(&self->ring);
    if (ret < 0) {
        set_errno_error(-ret);
        return NULL;
    }
    sqe = io_uring_get_sqe(&self->ring);
    if (!sqe)
        PyErr_SetString(PyExc_BlockingIOError, "submission queue is full");
    return sqe;
}

/* Queue an IORING_OP_ASYNC_CANCEL for one request (kernel 5.5+). */
static int
ring_queue_cancel(RingObject *self, Request *req)
{
    struct io_uring_sqe *sqe = ring_get_sqe(self);

    if (!sqe)
        return -1;
    io_uring_prep_cancel64(sqe, request_handle(req), 0);
    io_uring_sqe_set_data64(sqe, 0);
    return 0;
}

static int64_t
monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/*
 * Cancel everything outstanding and wait (up to timeout_ns, or forever when
 * negative) until the kernel has released every buffer, then tear the ring
 * down. Requests still running at the deadline are abandoned: their token is
 * reported with -ECANCELED and their buffer is leaked. Completions go to
 * `collect` when it is non-NULL. Returns -1 if building a result failed.
 */
static int
ring_shutdown(RingObject *self, PyObject *collect, int64_t timeout_ns,
              Py_ssize_t *abandoned)
{
    struct io_uring_cqe *cqe;
    int64_t deadline = timeout_ns >= 0 ? monotonic_ns() + timeout_ns : -1;
    int ret, status = 0;
    uint32_t i;

    if (abandoned)
        *abandoned = 0;
    if (!self->open)
        return 0;
    self->open = 0;   /* other threads see a closed ring while we wait */

    if (self->fork_gen != fork_generation) {
        /* Child after fork: the ring is shared with the parent, so do not
         * submit or reap. Unmapping our copy and closing our fds is safe;
         * the requests belong to the parent and are simply leaked here. */
        io_uring_queue_exit(&self->ring);
        close(self->efd);
        self->efd = -1;
        return 0;
    }

    if (self->inflight > 0) {
        for (i = 0; i < self->nslots; i++) {
            if (self->slots[i] && ring_queue_cancel(self, self->slots[i]) < 0) {
                PyErr_Clear();   /* best effort: we still wait below */
                break;
            }
        }
        io_uring_submit(&self->ring);
    }

    while (self->inflight > 0) {
        if (deadline >= 0) {
            int64_t left = deadline - monotonic_ns();
            struct __kernel_timespec ts;
            if (left < 0)
                left = 0;
            ts.tv_sec = left / 1000000000;
            ts.tv_nsec = left % 1000000000;
            Py_BEGIN_ALLOW_THREADS
            ret = io_uring_wait_cqe_timeout(&self->ring, &cqe, &ts);
            Py_END_ALLOW_THREADS
        }
        else {
            Py_BEGIN_ALLOW_THREADS
            ret = io_uring_wait_cqe(&self->ring, &cqe);
            Py_END_ALLOW_THREADS
        }
        if (ret == -EINTR)
            continue;
        if (ret < 0)
            break;   /* -ETIME, or the ring is unusable */

        uint64_t handle = cqe->user_data;
        int res = cqe->res;
        Request *req = handle ? request_lookup(self, handle) : NULL;
        io_uring_cqe_seen(&self->ring, cqe);
        if (!req)
            continue;   /* a cancel request's own completion */
        self->inflight--;
        if (collect) {
            PyObject *item = request_result(req, res);
            if (!item || PyList_Append(collect, item) < 0)
                status = -1;
            Py_XDECREF(item);
        }
        request_release(self, req);
    }

    for (i = 0; i < self->nslots; i++) {
        Request *req = self->slots[i];
        if (!req)
            continue;
        if (collect && status == 0) {
            PyObject *item = Py_BuildValue("(OiO)", req->token, -ECANCELED,
                                           Py_None);
            if (!item || PyList_Append(collect, item) < 0)
                status = -1;
            Py_XDECREF(item);
        }
        request_abandon(self, req);
        self->inflight--;
        if (abandoned)
            (*abandoned)++;
    }

    io_uring_queue_exit(&self->ring);
    close(self->efd);
    self->efd = -1;
    return status;
}

static int
check_kernel_support(struct io_uring *ring)
{
    static const int needed[] = {
        IORING_OP_READ, IORING_OP_WRITE, IORING_OP_FSYNC,
        IORING_OP_ASYNC_CANCEL,
    };
    struct io_uring_probe *probe = io_uring_get_probe_ring(ring);
    size_t i;

    if (!probe) {   /* probing itself needs 5.6, same as IORING_OP_READ */
        PyErr_SetString(PyExc_OSError,
                        "io_uring is too old: Linux 5.6 or newer required");
        return -1;
    }
    for (i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
        if (!io_uring_opcode_supported(probe, needed[i])) {
            io_uring_free_probe(probe);
            PyErr_Format(PyExc_OSError,
                         "io_uring opcode %d is not supported by this kernel",
                         needed[i]);
            return -1;
        }
    }
    io_uring_free_probe(probe);
    return 0;
}

static PyObject *
Ring_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"entries", NULL};
    unsigned int entries = 256;
    RingObject *self;
    int ret;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|I:Ring", kwlist, &entries))
        return NULL;

    self = (RingObject *)type->tp_alloc(type, 0);
    if (!self)
        return NULL;
    self->efd = -1;
    self->fork_gen = fork_generation;

    ret = io_uring_queue_init(entries, &self->ring, 0);
    if (ret < 0) {
        Py_DECREF(self);
        return set_errno_error(-ret);
    }
    if (check_kernel_support(&self->ring) < 0)
        goto fail;
    self->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (self->efd < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto fail;
    }
    ret = io_uring_register_eventfd(&self->ring, self->efd);
    if (ret < 0) {
        set_errno_error(-ret);
        goto fail;
    }
    self->open = 1;
    return (PyObject *)self;

fail:
    io_uring_queue_exit(&self->ring);
    if (self->efd >= 0)
        close(self->efd);
    self->efd = -1;
    Py_DECREF(self);
    return NULL;
}

static void
Ring_dealloc(RingObject *self)
{
    PyObject *type, *value, *tb;

    /* Fetch/Restore rather than GetRaisedException: works on 3.10+ */
    PyErr_Fetch(&type, &value, &tb);
    ring_shutdown(self, NULL, DEALLOC_TIMEOUT_NS, NULL);
    PyErr_Restore(type, value, tb);
    PyMem_Free(self->slots);
    PyMem_Free(self->free_slots);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(Ring_submit_read_doc,
"submit_read(fd, offset, nbytes, token) -> handle\n\n"
"Queue a read of nbytes at offset (-1 for the current file position).\n"
"reap() returns (token, result, bytearray) once it completes.");

static PyObject *
Ring_submit_read(RingObject *self, PyObject *args)
{
    int fd;
    long long offset;
    Py_ssize_t nbytes;
    PyObject *token, *buffer;
    Request *req;
    struct io_uring_sqe *sqe;

    if (!PyArg_ParseTuple(args, "iLnO:submit_read", &fd, &offset, &nbytes,
                          &token))
        return NULL;
    if (ring_check_open(self) < 0)
        return NULL;
    if (nbytes < 0 || (size_t)nbytes > UINT_MAX) {
        PyErr_SetString(PyExc_ValueError, "nbytes out of range");
        return NULL;
    }
    if (offset < -1) {
        PyErr_SetString(PyExc_ValueError, "offset must be >= -1");
        return NULL;
    }

    buffer = PyByteArray_FromStringAndSize(NULL, nbytes);
    if (!buffer)
        return NULL;
    sqe = ring_get_sqe(self);
    if (!sqe) {
        Py_DECREF(buffer);
        return NULL;
    }
    req = request_new(self, OP_READ);
    if (!req) {
        /* The SQE is already taken: turn it into a no-op. */
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
        Py_DECREF(buffer);
        return NULL;
    }
    req->buffer = buffer;
    req->nbytes = (unsigned int)nbytes;
    req->token = Py_NewRef(token);

    /* The bytearray is not visible to Python until reap(), so nothing can
     * resize it while the kernel writes into it. */
    io_uring_prep_read(sqe, fd, PyByteArray_AS_STRING(buffer), req->nbytes,
                       (__u64)offset);
    io_uring_sqe_set_data64(sqe, request_handle(req));
    self->inflight++;
    return PyLong_FromUnsignedLongLong(request_handle(req));
}

PyDoc_STRVAR(Ring_submit_write_doc,
"submit_write(fd, data, offset, token) -> handle\n\n"
"Queue a write of a bytes-like object at offset (-1 for the current file\n"
"position). data stays pinned until the request completes. reap() returns\n"
"(token, bytes_written, None).");

static PyObject *
Ring_submit_write(RingObject *self, PyObject *args)
{
    int fd;
    long long offset;
    PyObject *data, *token;
    Py_buffer view;
    Request *req;
    struct io_uring_sqe *sqe;

    if (!PyArg_ParseTuple(args, "iOLO:submit_write", &fd, &data, &offset,
                          &token))
        return NULL;
    if (ring_check_open(self) < 0)
        return NULL;
    if (offset < -1) {
        PyErr_SetString(PyExc_ValueError, "offset must be >= -1");
        return NULL;
    }
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0)
        return NULL;
    if ((size_t)view.len > UINT_MAX) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "write buffer too large");
        return NULL;
    }
    sqe = ring_get_sqe(self);
    if (!sqe) {
        PyBuffer_Release(&view);
        return NULL;
    }
    req = request_new(self, OP_WRITE);
    if (!req) {
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
        PyBuffer_Release(&view);
        return NULL;
    }
    req->view = view;
    req->nbytes = (unsigned int)view.len;
    req->token = Py_NewRef(token);

    io_uring_prep_write(sqe, fd, view.buf, req->nbytes, (__u64)offset);
    io_uring_sqe_set_data64(sqe, request_handle(req));
    self->inflight++;
    return PyLong_FromUnsignedLongLong(request_handle(req));
}

PyDoc_STRVAR(Ring_submit_fsync_doc,
"submit_fsync(fd, datasync, token) -> handle\n\n"
"Queue an fsync (or fdatasync). reap() returns (token, 0, None).");

static PyObject *
Ring_submit_fsync(RingObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"fd", "datasync", "token", NULL};
    int fd, datasync = 0;
    PyObject *token;
    Request *req;
    struct io_uring_sqe *sqe;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ipO:submit_fsync", kwlist,
                                     &fd, &datasync, &token))
        return NULL;
    if (ring_check_open(self) < 0)
        return NULL;

    sqe = ring_get_sqe(self);
    if (!sqe)
        return NULL;
    req = request_new(self, OP_FSYNC);
    if (!req) {
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
        return NULL;
    }
    req->token = Py_NewRef(token);

    io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
    io_uring_sqe_set_data64(sqe, request_handle(req));
    self->inflight++;
    return PyLong_FromUnsignedLongLong(request_handle(req));
}

PyDoc_STRVAR(Ring_cancel_doc,
"cancel(handle) -> bool\n\n"
"Queue a kernel cancellation for a request. Returns False if the handle no\n"
"longer refers to a live request. The request still completes through\n"
"reap(), usually with -ECANCELED; I/O the kernel already started on a\n"
"regular file typically runs to completion. Call submit() afterwards.");

static PyObject *
Ring_cancel(RingObject *self, PyObject *arg)
{
    unsigned long long handle = PyLong_AsUnsignedLongLong(arg);
    Request *req;

    if (handle == (unsigned long long)-1 && PyErr_Occurred())
        return NULL;
    if (ring_check_open(self) < 0)
        return NULL;
    req = request_lookup(self, handle);
    if (!req)
        Py_RETURN_FALSE;
    if (ring_queue_cancel(self, req) < 0)
        return NULL;
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(Ring_cancel_all_doc,
"cancel_all() -> int\n\n"
"Queue a cancellation for every live request and return how many were\n"
"queued. Call submit() afterwards.");

static PyObject *
Ring_cancel_all(RingObject *self, PyObject *Py_UNUSED(ignored))
{
    long n = 0;
    uint32_t i;

    if (ring_check_open(self) < 0)
        return NULL;
    for (i = 0; i < self->nslots; i++) {
        if (!self->slots[i])
            continue;
        if (ring_queue_cancel(self, self->slots[i]) < 0)
            return NULL;
        n++;
    }
    return PyLong_FromLong(n);
}

PyDoc_STRVAR(Ring_submit_doc,
"submit() -> int\n\n"
"Hand every queued request to the kernel in one system call.");

static PyObject *
Ring_submit(RingObject *self, PyObject *Py_UNUSED(ignored))
{
    int ret;

    if (ring_check_open(self) < 0)
        return NULL;
    ret = io_uring_submit(&self->ring);
    if (ret < 0)
        return set_errno_error(-ret);
    return PyLong_FromLong(ret);
}

PyDoc_STRVAR(Ring_reap_doc,
"reap() -> list of (token, result, payload)\n\n"
"Collect finished requests without blocking. result is the byte count, or\n"
"a negative errno. payload is the read bytearray, or None. If reap()\n"
"raises (e.g. MemoryError), no completion is lost: call it again.");

static PyObject *
Ring_reap(RingObject *self, PyObject *Py_UNUSED(ignored))
{
    struct io_uring_cqe *cqe;
    uint64_t counter;
    PyObject *list;

    if (ring_check_open(self) < 0)
        return NULL;
    list = PyList_New(0);
    if (!list)
        return NULL;

    /* Reset the eventfd before draining: a completion that lands after the
     * read signals it again, so no wakeup is lost. */
    if (read(self->efd, &counter, sizeof(counter)) < 0 && errno != EAGAIN) {
        Py_DECREF(list);
        return PyErr_SetFromErrno(PyExc_OSError);
    }

    while (io_uring_peek_cqe(&self->ring, &cqe) == 0) {
        uint64_t handle = cqe->user_data;
        Request *req = handle ? request_lookup(self, handle) : NULL;
        PyObject *item;

        if (!req) {     /* internal cancel/nop completion */
            io_uring_cqe_seen(&self->ring, cqe);
            continue;
        }
        item = request_result(req, cqe->res);
        if (!item || PyList_Append(list, item) < 0) {
            /* Leave this CQE in the ring and re-arm the eventfd so the loop
             * calls reap() again. Completions already in `list` were
             * consumed, so hand them back instead of failing. */
            Py_XDECREF(item);
            eventfd_write(self->efd, 1);
            if (PyList_GET_SIZE(list) > 0) {
                PyErr_Clear();
                return list;
            }
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(item);
        io_uring_cqe_seen(&self->ring, cqe);
        self->inflight--;
        request_release(self, req);
    }
    return list;
}

PyDoc_STRVAR(Ring_close_doc,
"close(timeout=None) -> list of (token, result, payload)\n\n"
"Cancel outstanding requests, wait for the kernel to release their buffers\n"
"and tear down the ring. With a timeout (seconds), requests still running\n"
"at the deadline are abandoned: they are reported with -ECANCELED, their\n"
"buffers are leaked rather than freed, and a ResourceWarning is emitted.\n"
"Returns every completion collected.");

static PyObject *
Ring_close(RingObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"timeout", NULL};
    PyObject *timeout_obj = Py_None, *list;
    int64_t timeout_ns = -1;
    Py_ssize_t abandoned;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:close", kwlist,
                                     &timeout_obj))
        return NULL;
    if (timeout_obj != Py_None) {
        double t = PyFloat_AsDouble(timeout_obj);
        if (t == -1.0 && PyErr_Occurred())
            return NULL;
        if (t < 0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be >= 0");
            return NULL;
        }
        timeout_ns = (int64_t)(t * 1e9);
    }
    list = PyList_New(0);
    if (!list)
        return NULL;
    if (ring_shutdown(self, list, timeout_ns, &abandoned) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    if (abandoned > 0 &&
        PyErr_WarnFormat(PyExc_ResourceWarning, 1,
                         "pyaio: %zd io_uring request(s) did not finish "
                         "before the close timeout; their buffers were "
                         "leaked", abandoned) < 0) {
        Py_DECREF(list);
        return NULL;
    }
    return list;
}

static PyObject *
Ring_fileno(RingObject *self, PyObject *Py_UNUSED(ignored))
{
    if (ring_check_open(self) < 0)
        return NULL;
    return PyLong_FromLong(self->efd);
}

static PyObject *
Ring_get_inflight(RingObject *self, void *Py_UNUSED(closure))
{
    return PyLong_FromSsize_t(self->inflight);
}

static PyObject *
Ring_get_closed(RingObject *self, void *Py_UNUSED(closure))
{
    return PyBool_FromLong(!self->open);
}

static PyMethodDef Ring_methods[] = {
    {"submit_read", (PyCFunction)Ring_submit_read, METH_VARARGS,
     Ring_submit_read_doc},
    {"submit_write", (PyCFunction)Ring_submit_write, METH_VARARGS,
     Ring_submit_write_doc},
    {"submit_fsync", (PyCFunction)(void (*)(void))Ring_submit_fsync,
     METH_VARARGS | METH_KEYWORDS, Ring_submit_fsync_doc},
    {"cancel", (PyCFunction)Ring_cancel, METH_O, Ring_cancel_doc},
    {"cancel_all", (PyCFunction)Ring_cancel_all, METH_NOARGS,
     Ring_cancel_all_doc},
    {"submit", (PyCFunction)Ring_submit, METH_NOARGS, Ring_submit_doc},
    {"reap", (PyCFunction)Ring_reap, METH_NOARGS, Ring_reap_doc},
    {"close", (PyCFunction)(void (*)(void))Ring_close,
     METH_VARARGS | METH_KEYWORDS, Ring_close_doc},
    {"fileno", (PyCFunction)Ring_fileno, METH_NOARGS,
     "fileno() -> eventfd signalled when completions are ready"},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef Ring_getset[] = {
    {"inflight", (getter)Ring_get_inflight, NULL,
     "number of requests awaiting completion", NULL},
    {"closed", (getter)Ring_get_closed, NULL, "True once close() ran", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject RingType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pyaio._uring.Ring",
    .tp_basicsize = sizeof(RingObject),
    .tp_dealloc = (destructor)Ring_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Ring(entries=256)\n\nAn io_uring instance with an eventfd for "
              "completion notification.",
    .tp_methods = Ring_methods,
    .tp_getset = Ring_getset,
    .tp_new = Ring_new,
};

static struct PyModuleDef uring_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "pyaio._uring",
    .m_doc = "io_uring bindings for pyaio",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit__uring(void)
{
    static int atfork_registered = 0;
    PyObject *m;

    if (!atfork_registered) {
        if (pthread_atfork(NULL, NULL, on_fork_child) != 0) {
            PyErr_SetString(PyExc_OSError, "pthread_atfork failed");
            return NULL;
        }
        atfork_registered = 1;
    }
    if (PyType_Ready(&RingType) < 0)
        return NULL;
    m = PyModule_Create(&uring_module);
    if (!m)
        return NULL;
    if (PyModule_AddObjectRef(m, "Ring", (PyObject *)&RingType) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
