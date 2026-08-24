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
 * globals, or closure is copied into this frame while the source runs.
 * A source-parent scope may install borrowed function/environment views. */
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
 * including an unstarted generator's valid throw, and is one-use.
 * A source-parent scope must have left before Finish is called. */
PyAPI_FUNC(int) PyFrame_FinishSoacLifetimeWithEnvironment(
    PyFrameObject *frame, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins, PyObject *source_namespace,
    PyObject *const *source_owners, Py_ssize_t count);

/* REVIEW DRAFT ONLY: appended to Include/cpython/frameobject.h by make_mirror.py.
 * This is a scoped parent/lifetime link, not a Python bytecode activation. */
#define PySoac_LIFETIME_SCOPE_ABI_VERSION 1
#define PySoac_LIFETIME_SCOPE_V1_WORDS 6

typedef struct {
    uintptr_t _opaque[PySoac_LIFETIME_SCOPE_V1_WORDS];
} PySoacLifetimeScopeV1;

typedef struct {
    uint32_t version;
    uint32_t word_count;
    size_t size;
    size_t alignment;
} PySoacLifetimeScopeLayoutV1;

enum {
    PySoac_LIFETIME_SCOPE_SUSPEND = 0,
    PySoac_LIFETIME_SCOPE_TERMINAL = 1,
};

/* Query before constructing an ABI mirror. Success does not allocate, invoke
 * Python, or change the pending exception. This interface requires the GIL. */
PyAPI_FUNC(int) PyFrame_GetSoacLifetimeScopeLayoutV1(
    uint32_t version, size_t layout_size, PySoacLifetimeScopeLayoutV1 *layout);

/* Scope storage is all-zero initialized and one-use. Do not copy or move it
 * while linked; the storage and all borrowed inputs must remain alive until
 * Leave returns. A later resume uses fresh scope storage with the SAME active
 * source frame. It may run on another attached thread in the same interpreter.
 *
 * Link the actual lifetime frame as the current Python parent throughout the
 * source interval, including descriptor/C/finalizer callbacks. No local owner
 * is mirrored, and no reference to function/globals/builtins is acquired.
 * Source code identity was captured by New; function.__code__ may since have
 * changed. This API neither reauthenticates mutable code nor grants execution.
 *
 * All validation precedes publication. Return 0 on success, -1 on invalid
 * input with no change to the current frame or scope. */
PyAPI_FUNC(int) PyFrame_EnterSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, PyFrameObject *frame,
    PyObject *actual_function, PyObject *captured_globals,
    PyObject *captured_builtins);

/* Validate exact storage, current thread and LIFO position before any mutation.
 * Restore the parent and clear raw previous/borrowed environment BEFORE any
 * terminal source-primary decref. A terminal retained frame records its native
 * f_back parent exactly as ordinary frame retirement does. Parent frame-object
 * allocation may fail: like ordinary take_ownership, that MemoryError is
 * suppressed and the original pending exception is preserved. No Python
 * callback or synchronous GC runs on the valid path.
 *
 * Suspend only detaches; it must NOT capture the resume caller in f_back.
 * Terminal prevents another Enter, but still requires the existing Finish
 * transaction before primary teardown. Finish rejects any linked scope.
 * Leave does not finish, decref, or consume the caller's frame reference.
 * Return 0 on success, -1 on invalid input without a partial pop. */
PyAPI_FUNC(int) PyFrame_LeaveSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, int reason);
