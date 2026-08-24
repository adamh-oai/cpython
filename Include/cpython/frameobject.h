/* Frame object interface */

#ifndef Py_CPYTHON_FRAMEOBJECT_H
#  error "this header file must not be included directly"
#endif

/* Standard object interface */

PyAPI_FUNC(PyFrameObject *) PyFrame_New(PyThreadState *, PyCodeObject *,
                                        PyObject *, PyObject *);

/* The rest of the interface is specific for frame objects */

/* Conversions between "fast locals" and locals in dictionary */

PyAPI_FUNC(void) PyFrame_LocalsToFast(PyFrameObject *, int);

/* -- Caveat emptor --
 * The concept of entry frames is an implementation detail of the CPython
 * interpreter. This API is considered unstable and is provided for the
 * convenience of debuggers, profilers and state-inspecting tools. Notice that
 * this API can be changed in future minor versions if the underlying frame
 * mechanism change or the concept of an 'entry frame' or its semantics becomes
 * obsolete or outdated. */

PyAPI_FUNC(int) _PyFrame_IsEntryFrame(PyFrameObject *frame);

PyAPI_FUNC(int) PyFrame_FastToLocalsWithError(PyFrameObject *f);
PyAPI_FUNC(void) PyFrame_FastToLocals(PyFrameObject *);


typedef struct {
    PyObject_HEAD
    PyFrameObject* frame;
} PyFrameLocalsProxyObject;

/* Explicit source-lifetime ownership, never native-bytecode permission.
 * This private prototype is GIL-build-only. */
#define PySoac_LIFETIME_FRAME_ABI_VERSION 2

/* The caller owns exactly one active reference. No source binding, function,
 * globals, or closure is copied into this frame while the source runs. */
PyAPI_FUNC(PyFrameObject *) PyFrame_NewSoacLifetime(PyCodeObject *source_code);

/* Prepend this same active frame to the current raised exception. The caller
 * supplies the authenticated source site. A known instruction_offset is an
 * aligned byte offset in source_code and must map to source_lineno. -1 means
 * unavailable: tb_lineno remains accurate, but tb_lasti inspection refuses.
 * The pair (-1, -1) denotes an unavailable synthetic source event: both
 * position getters and traceback formatting explicitly refuse rather than
 * inventing a line. A known offset may not be paired with an unknown line.
 * This records a traceback site, not a continuously maintained execution PC. */
PyAPI_FUNC(int) PyFrame_AddSoacTraceback(
    PyFrameObject *frame, int instruction_offset, int source_lineno);

/* Validate first, then close the lifetime frame without allocation, Python
 * callbacks, or decrefs on the valid path. If an external traceback/frame
 * reference exists, copy the actual source owners in co_localsplus order and
 * the actual active function; NULL slots are unbound, cell/free slots are the
 * original cells. Never reread the function's mutable current closure/code.
 * captured_globals/captured_builtins are non-NULL borrowed activation snapshots,
 * independently kept alive by the caller. They are not reread from function
 * fields, which cyclic GC may already have cleared. No mapping-type restriction
 * is imposed on captured builtins. Retained frames own any environment object
 * no longer covered by the function or whose function is pending cyclic-GC
 * clearing, without adding any ACTIVE frame edges.
 * source_namespace is NULL for optimized function code and the actual owned
 * mapping for nonoptimized module/class code. It is never inferred from the
 * function's globals or copied into a dictionary snapshot.
 * Return 1 if retained, 0 otherwise, -1 on invalid input without changing the
 * active frame. The compiler subsequently releases its primary owners and
 * active frame reference. Explicit source del/rebinding must already have
 * updated those primaries. Finish is required on terminal/deallocation paths,
 * including an unstarted generator's valid throw, and is one-use. */
PyAPI_FUNC(int) PyFrame_FinishSoacLifetimeWithEnvironment(
    PyFrameObject *frame, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins, PyObject *source_namespace,
    PyObject *const *source_owners, Py_ssize_t count);
