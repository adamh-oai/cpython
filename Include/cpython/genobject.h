/* Generator object interface */

#ifndef Py_LIMITED_API
#ifndef Py_GENOBJECT_H
#define Py_GENOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/* --- Generators --------------------------------------------------------- */

typedef struct _PyGenObject PyGenObject;

PyAPI_DATA(PyTypeObject) PyGen_Type;

#define PyGen_Check(op) PyObject_TypeCheck((op), &PyGen_Type)
#define PyGen_CheckExact(op) Py_IS_TYPE((op), &PyGen_Type)

PyAPI_FUNC(PyObject *) PyGen_New(PyFrameObject *);
PyAPI_FUNC(PyObject *) PyGen_NewWithQualName(PyFrameObject *,
    PyObject *name, PyObject *qualname);
PyAPI_FUNC(PyCodeObject *) PyGen_GetCode(PyGenObject *gen);

#define PySoac_GENERATOR_ABI_VERSION 2

typedef enum {
    PySoac_GENERATOR_SEND = 1,
    PySoac_GENERATOR_THROW = 2,
    PySoac_GENERATOR_CLOSE = 3
} PySoacGeneratorOperation;

typedef enum {
    /* ERROR before entering/resuming the body: restore its prior state. */
    PySoac_GENERATOR_UNCHANGED = 0,
    PySoac_GENERATOR_SUSPENDED = 1,
    PySoac_GENERATOR_CLOSED = 2
} PySoacGeneratorState;

typedef enum {
    /* Required for RETURN/ERROR; never valid for NEXT. */
    PySoac_SUSPEND_NONE = 0,
    PySoac_SUSPEND_DIRECT = 1,
    PySoac_SUSPEND_DELEGATING = 2,
    /* Value is already an exact native async-generator wrapped token. */
    PySoac_SUSPEND_ASYNC_YIELD = 3
} PySoacGeneratorSuspension;

typedef struct {
    PySoacGeneratorOperation operation;
    int close_on_genexit;
    /* SEND: actual value (Py_None for next); THROW: raw typ; CLOSE: NULL. */
    PyObject *arg;
    /* THROW retains NULL versus supplied Py_None and the original traceback. */
    PyObject *value;
    PyObject *traceback;
} PySoacGeneratorInput;

typedef struct {
    /* NEXT/RETURN require a new value ref and no pending error.
       ERROR requires NULL value and a pending error. */
    PySendResult outcome;
    /* NEXT -> SUSPENDED; RETURN -> CLOSED; ERROR -> UNCHANGED or CLOSED. */
    PySoacGeneratorState state;
    /* Explicit source operation, not inferred from a name or yielded value. */
    PySoacGeneratorSuspension suspension;
    PyObject *value;
} PySoacGeneratorResult;

typedef struct {
    unsigned int abi_version;
    unsigned int reserved;  /* zero */
    /* One-way owner binding before publication; all arguments borrowed.
       Rust validates the actual active factory snapshot, not current function
       metadata that may legitimately have changed during that activation. */
    int (*bind)(PyObject *owner, PyObject *generator,
                PyObject *function, PyCodeObject *source_code);
    /* Native state is EXECUTING; native gi_exc_state remains empty/unlinked.
       The owned record is the sole handled-exception activation. Input refs
       are borrowed for this callback only. It must initialize every result. */
    void (*step)(PyObject *owner, PyObject *generator,
                 const PySoacGeneratorInput *input,
                 PySoacGeneratorResult *result);
    /* New reference to the actual delegate or Py_None; no Python dispatch. */
    PyObject *(*yield_from)(PyObject *owner);
    /* Idempotent retirement of this exact association, not a different live
       generator already bound to a reused owner. Must not fail; native makes
       this generator terminal and preserves errors around callback/decrefs. */
    void (*clear)(PyObject *owner, PyObject *generator);
} PySoacGeneratorSpec;

/* Copies the spec; takes its own owner/code/name references, never steals args.
   Binds the exact requested native type before GC tracking/publication. Bind failure makes
   any escaped object terminal before releasing callback-visible state.
   No original strict bytecode executes and no source authority is minted. */
PyAPI_FUNC(PyObject *) PyGen_NewSoacManaged(
    PyObject *function, PyCodeObject *source_code, PyObject *name,
    PyObject *qualname, PyObject *owner,
    const PySoacGeneratorSpec *spec);

/* Coroutine origin tracking with nonzero depth explicitly refuses creation:
   optimized source ancestry is not available as a faithful native frame chain. */
PyAPI_FUNC(PyObject *) PyCoro_NewSoacManaged(
    PyObject *function, PyCodeObject *source_code, PyObject *name,
    PyObject *qualname, PyObject *owner,
    const PySoacGeneratorSpec *spec);

PyAPI_FUNC(PyObject *) PyAsyncGen_NewSoacManaged(
    PyObject *function, PyCodeObject *source_code, PyObject *name,
    PyObject *qualname, PyObject *owner,
    const PySoacGeneratorSpec *spec);

/* Borrowed value -> owned native wrapped token. Invoke inside the source
   async-yield operation BEFORE suspension, so allocation failure follows the
   body's actual exception edge. The step's ASYNC_YIELD result only validates
   this token and performs no late allocation. No execution authority is added. */
PyAPI_FUNC(PyObject *) PyAsyncGen_WrapSoacYield(PyObject *value);

/* 1 exact live association (including the active bind callback), 0 unrelated
   or mismatched owner, -1 with RuntimeError for a terminal/invalid record. This is
   association evidence only, not permission to execute a source body. */
PyAPI_FUNC(int) PyGen_MatchesSoacOwner(PyObject *generator, PyObject *owner);

/* Publish terminal public state before compiler terminal cleanup can dispatch
   finalizers. Only the exact active owner may notify; repeated notification
   during the same completing step is idempotent. Success is callback-free and
   preserves any pending error. Native retains owner/spec until step returns;
   only RETURN/CLOSED or ERROR/CLOSED is then valid. No clear callback runs here.
   MatchesSoacOwner already reports terminal (-1), and public send/throw/close/
   frame observation follow the exact native family's closed behavior.
   Async-generator operation ownership (ag_running_async) remains with the
   native ASend/AThrow helper, independently of terminal frame state. */
PyAPI_FUNC(int) PyGen_MarkSoacManagedTerminal(
    PyObject *generator, PyObject *owner);

/* Common native throw normalization, called only after the callback's
   yield-from decision. Returns a new exception ref with no pending error, or
   NULL on failure. Pre-normalizing before delegation would change behavior. */
PyAPI_FUNC(PyObject *) PyGen_NormalizeSoacThrow(
    PyObject *typ, PyObject *value, PyObject *traceback);

/* Native yield-from close semantics, including unraisable attribute-lookup
   errors and direct exact-generator/coroutine handling. 0 success, -1 error. */
PyAPI_FUNC(int) PyGen_CloseSoacDelegate(PyObject *delegate);

/* Raw yield-from throw, without another public throw() deprecation warning.
   0: method missing, no error, *result=NULL.
  -1: lookup failed, pending error, *result=NULL; body remains unresumed.
   1: called; *result is a new yielded value, or NULL with a body-deliverable
      pending exception. No synthetic current_frame is installed here. */
PyAPI_FUNC(int) PyGen_ThrowSoacDelegate(
    PyObject *delegate, int close_on_genexit, PyObject *typ, PyObject *value,
    PyObject *traceback, PyObject **result);


/* --- PyCoroObject ------------------------------------------------------- */

typedef struct _PyCoroObject PyCoroObject;

PyAPI_DATA(PyTypeObject) PyCoro_Type;

#define PyCoro_CheckExact(op) Py_IS_TYPE((op), &PyCoro_Type)
PyAPI_FUNC(PyObject *) PyCoro_New(PyFrameObject *,
    PyObject *name, PyObject *qualname);


/* --- Asynchronous Generators -------------------------------------------- */

typedef struct _PyAsyncGenObject PyAsyncGenObject;

PyAPI_DATA(PyTypeObject) PyAsyncGen_Type;
PyAPI_DATA(PyTypeObject) _PyAsyncGenASend_Type;

PyAPI_FUNC(PyObject *) PyAsyncGen_New(PyFrameObject *,
    PyObject *name, PyObject *qualname);

#define PyAsyncGen_CheckExact(op) Py_IS_TYPE((op), &PyAsyncGen_Type)

#define PyAsyncGenASend_CheckExact(op) Py_IS_TYPE((op), &_PyAsyncGenASend_Type)

#undef _PyGenObject_HEAD

#ifdef __cplusplus
}
#endif
#endif /* !Py_GENOBJECT_H */
#endif /* Py_LIMITED_API */
