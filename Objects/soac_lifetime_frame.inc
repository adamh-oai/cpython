/* A lazy, exact PyFrameObject that owns source lifetimes, not an execution
 * activation. ACTIVE slots are deliberately empty and are never exposed as
 * Python locals. The source activation owns its sole primary frame reference.
 * At terminal retirement, retained tracebacks take the then-current owners.
 */
#include "pycore_gc.h"  // _PyGC_PREV_MASK_COLLECTING
#include "pycore_traceback.h"  // _PyTraceBack_HereSoacLifetime

static int
soac_frame_unavailable(const char *operation)
{
    PyErr_Format(PyExc_NotImplementedError,
                 "%s is unavailable for an optimized source-lifetime frame",
                 operation);
    return -1;
}

static int
soac_frame_check_inspection(PyFrameObject *frame, const char *operation)
{
    if (_PyFrame_IsSoacLifetime(frame->f_frame)) {
        return soac_frame_unavailable(operation);
    }
    return 0;
}

static int
soac_frame_setattro(PyObject *object, PyObject *name, PyObject *value)
{
    if (soac_frame_check_inspection((PyFrameObject *)object,
                                   "frame mutation/tracing") < 0) {
        return -1;
    }
    return PyObject_GenericSetAttr(object, name, value);
}

PyFrameObject *
_PyFrame_GetSoacLifetimeObject(_PyInterpreterFrame *frame)
{
    assert(_PyFrame_IsSoacLifetime(frame));
    PyFrameObject *object = (PyFrameObject *)
        ((char *)frame - offsetof(PyFrameObject, _f_frame_data));
    assert(object->f_frame == frame);
    return object;
}

PyFrameObject *
PyFrame_NewSoacLifetime(PyCodeObject *source_code)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime frames require the selected GIL build");
    return NULL;
#else
    if (source_code == NULL || !PyCode_Check(source_code)) {
        PyErr_SetString(PyExc_TypeError,
                        "source-lifetime frame requires an actual code object");
        return NULL;
    }
    /* Like _PyFrame_MakeAndSetFrameObject, frame allocation cannot call Python
     * or synchronously collect. Preserve an already-raised source exception. */
    PyObject *error = PyErr_GetRaisedException();
    PyFrameObject *object = _PyFrame_New_NoTrack(source_code);
    if (object == NULL) {
        PyObject *allocation_error = PyErr_GetRaisedException();
        Py_XDECREF(error);
        PyErr_SetRaisedException(allocation_error);
        return NULL;
    }
    _PyInterpreterFrame *frame = (_PyInterpreterFrame *)object->_f_frame_data;
    memset(frame, 0, offsetof(_PyInterpreterFrame, localsplus));
    frame->f_executable = PyStackRef_FromPyObjectNew((PyObject *)source_code);
    frame->f_funcobj = PyStackRef_NULL;
    frame->owner = FRAME_OWNED_BY_SOAC_ACTIVE;
    frame->stackpointer = frame->localsplus;
    int slots = source_code->co_nlocalsplus + source_code->co_stacksize;
    for (int i = 0; i < slots; i++) {
        frame->localsplus[i] = PyStackRef_NULL;
    }
    object->f_frame = frame;
    _PyObject_GC_TRACK(object);
    PyErr_SetRaisedException(error);
    return object;
#endif
}

int
PyFrame_AddSoacTraceback(PyFrameObject *object,
                         int instruction_offset, int source_lineno)
{
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE) {
        PyErr_SetString(PyExc_RuntimeError,
                        "traceback attachment requires an active source-lifetime frame");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    if ((source_lineno <= 0 &&
         (source_lineno != -1 || instruction_offset != -1)) ||
        instruction_offset < -1 ||
        (instruction_offset >= 0 &&
         (instruction_offset % sizeof(_Py_CODEUNIT) != 0 ||
          instruction_offset >= _PyCode_NBYTES(code) ||
          PyBytes_GET_SIZE(code->co_linetable) == 0 ||
          PyCode_Addr2Line(code, instruction_offset) != source_lineno))) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid source-lifetime traceback position");
        return -1;
    }
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime traceback requires a raised exception");
        return -1;
    }
    /* The site belongs to this traceback, never a mutable pseudo-PC on
     * the lifetime frame. Generic FromFrame requests remain unknown. */
    return _PyTraceBack_HereSoacLifetime(object, instruction_offset, source_lineno);
}

int
PyFrame_FinishSoacLifetimeWithEnvironment(
    PyFrameObject *object, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins,
    PyObject *source_namespace, PyObject *const *source_owners, Py_ssize_t count)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime frames require the selected GIL build");
    return -1;
#else
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        source_function == NULL || !PyFunction_Check(source_function)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime finish requires an active frame and function");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    PyFunctionObject *function = (PyFunctionObject *)source_function;
    if (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime finish requires leaving its parent scope");
        return -1;
    }
    if ((code->co_flags & CO_OPTIMIZED) ? source_namespace != NULL
        : source_namespace == NULL || !PyMapping_Check(source_namespace)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime namespace does not match the original code scope");
        return -1;
    }
    if (count != code->co_nlocalsplus || (count != 0 && source_owners == NULL)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime owners do not match the original code layout");
        return -1;
    }
    if (captured_globals == NULL || captured_builtins == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime finish requires the captured function environment");
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        _PyLocals_Kind kind = _PyLocals_GetKind(code->co_localspluskinds, (int)i);
        if ((kind & (CO_FAST_CELL | CO_FAST_FREE)) &&
            source_owners[i] != NULL && !PyCell_Check(source_owners[i])) {
            PyErr_SetString(PyExc_TypeError,
                            "source-lifetime cell slots require original cell objects");
            return -1;
        }
    }
    /* Nothing below can allocate, dispatch Python, decref, or fail. Decide
     * before adding any of the parked references, including a self-local. */
    int retained = !_PyObject_IsUniquelyReferenced((PyObject *)object);
    assert(frame->soac_lifetime_owned_environment == 0 ||
           frame->soac_lifetime_owned_environment == SOAC_LIFETIME_SCOPE_TERMINAL);
    frame->soac_lifetime_owned_environment = 0;
    frame->owner = retained ? FRAME_OWNED_BY_SOAC_FINISHED
                            : FRAME_OWNED_BY_SOAC_CLEARED;
    if (!retained) {
        return 0;
    }
    frame->f_funcobj = PyStackRef_FromPyObjectNew(source_function);
    frame->f_globals = captured_globals;
    frame->f_builtins = captured_builtins;
    /* Cyclic GC may already have cleared this still-allocated function, or it
     * may still be queued for tp_clear after this owner. delete_garbage does not
     * recheck reachability after Finish adds the function reference. Retain the
     * explicit environment in either case. This is the function's own pending
     * collection state, not an ambient "GC is running" test: reachable ordinary
     * functions still add no map edges or refcount changes. */
    assert(frame->soac_lifetime_owned_environment == 0);
    int function_collecting =
        (_Py_AS_GC(source_function)->_gc_prev & _PyGC_PREV_MASK_COLLECTING) != 0;
    if (function_collecting || function->func_globals != captured_globals) {
        Py_INCREF(captured_globals);
        frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_OWNS_GLOBALS;
    }
    if (function_collecting || function->func_builtins != captured_builtins) {
        Py_INCREF(captured_builtins);
        frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_OWNS_BUILTINS;
    }
    frame->f_locals = Py_XNewRef(source_namespace);
    for (Py_ssize_t i = 0; i < count; i++) {
        frame->localsplus[i] = source_owners[i] == NULL ? PyStackRef_NULL
            : PyStackRef_FromPyObjectNew(source_owners[i]);
    }
    frame->stackpointer = frame->localsplus + count;
    return 1;
#endif
}

/* REVIEW DRAFT ONLY: appended inside Objects/soac_lifetime_frame.inc. */
typedef struct {
    uintptr_t version;
    uintptr_t state;
    uintptr_t storage;
    uintptr_t thread;
    uintptr_t frame;
    uintptr_t previous;
} SoacLifetimeScopeStateV1;

enum {
    SOAC_SCOPE_V1_LINKED = 1,
    SOAC_SCOPE_V1_SUSPENDED = 2,
    SOAC_SCOPE_V1_TERMINAL = 3,
};

_Static_assert(sizeof(SoacLifetimeScopeStateV1) == sizeof(PySoacLifetimeScopeV1),
               "source-parent scope word count must match its public ABI");
_Static_assert(_Alignof(SoacLifetimeScopeStateV1) == _Alignof(PySoacLifetimeScopeV1),
               "source-parent scope alignment must match its public ABI");

static int
soac_scope_storage_valid(PySoacLifetimeScopeV1 *scope, size_t size)
{
    if (scope == NULL || size != sizeof(*scope) ||
        (uintptr_t)scope % _Alignof(PySoacLifetimeScopeV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid source-parent scope size or alignment");
        return -1;
    }
    return 0;
}

int
PyFrame_GetSoacLifetimeScopeLayoutV1(
    uint32_t version, size_t layout_size, PySoacLifetimeScopeLayoutV1 *layout)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (version != PySoac_LIFETIME_SCOPE_ABI_VERSION || layout == NULL ||
        layout_size != sizeof(*layout) ||
        (uintptr_t)layout % _Alignof(PySoacLifetimeScopeLayoutV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "unsupported source-parent scope layout version or size");
        return -1;
    }
    *layout = (PySoacLifetimeScopeLayoutV1) {
        .version = PySoac_LIFETIME_SCOPE_ABI_VERSION,
        .word_count = PySoac_LIFETIME_SCOPE_V1_WORDS,
        .size = sizeof(PySoacLifetimeScopeV1),
        .alignment = _Alignof(PySoacLifetimeScopeV1),
    };
    return 0;
#endif
}

int
PyFrame_EnterSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, PyFrameObject *object,
    PyObject *actual_function, PyObject *captured_globals,
    PyObject *captured_builtins)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (soac_scope_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    for (size_t i = 0; i < PySoac_LIFETIME_SCOPE_V1_WORDS; i++) {
        if (scope->_opaque[i] != 0) {
            PyErr_SetString(PyExc_RuntimeError,
                            "source-parent scope storage is one-use");
            return -1;
        }
    }
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        actual_function == NULL || !PyFunction_Check(actual_function)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent entry requires an active frame and function");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    if (captured_globals == NULL || captured_builtins == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "source-parent entry requires the captured environment");
        return -1;
    }
    if (frame->soac_lifetime_owned_environment != 0 ||
        frame->previous != NULL || object->f_back != NULL ||
        !PyStackRef_IsNull(frame->f_funcobj) ||
        frame->f_globals != NULL || frame->f_builtins != NULL ||
        frame->f_locals != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent frame is already linked or terminal");
        return -1;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    SoacLifetimeScopeStateV1 state = {
        .version = PySoac_LIFETIME_SCOPE_ABI_VERSION,
        .state = SOAC_SCOPE_V1_LINKED,
        .storage = (uintptr_t)scope,
        .thread = (uintptr_t)tstate,
        .frame = (uintptr_t)frame,
        .previous = (uintptr_t)tstate->current_frame,
    };
    /* No Python ownership edge. In STACKREF_DEBUG this creates the native
     * borrowed handle, which must be closed before the external owner dies. */
    frame->f_funcobj = PyStackRef_FromPyObjectBorrow(actual_function);
    frame->f_globals = captured_globals;
    frame->f_builtins = captured_builtins;
    frame->previous = tstate->current_frame;
    frame->soac_lifetime_owned_environment = SOAC_LIFETIME_SCOPE_LINKED;
    memcpy(scope, &state, sizeof(state));
    /* Publish only after every field visible to asynchronous readers is ready. */
    tstate->current_frame = frame;
    return 0;
#endif
}

int
PyFrame_LeaveSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, int reason)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (soac_scope_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    if (reason != PySoac_LIFETIME_SCOPE_SUSPEND &&
        reason != PySoac_LIFETIME_SCOPE_TERMINAL) {
        PyErr_SetString(PyExc_ValueError, "invalid source-parent leave reason");
        return -1;
    }
    SoacLifetimeScopeStateV1 state;
    /* memcpy avoids accessing public uintptr storage through an aliased struct. */
    memcpy(&state, scope, sizeof(state));
    if (state.version != PySoac_LIFETIME_SCOPE_ABI_VERSION ||
        state.state != SOAC_SCOPE_V1_LINKED || state.storage != (uintptr_t)scope) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scope is inactive, copied, or already left");
        return -1;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (state.thread != (uintptr_t)tstate) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scope belongs to another thread state");
        return -1;
    }
    /* Compare current identity before dereferencing the saved frame pointer. */
    if (state.frame != (uintptr_t)tstate->current_frame) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scopes must leave in stack order");
        return -1;
    }
    _PyInterpreterFrame *frame = tstate->current_frame;
    if (frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        frame->soac_lifetime_owned_environment != SOAC_LIFETIME_SCOPE_LINKED ||
        (uintptr_t)frame->previous != state.previous) {
        PyErr_SetString(PyExc_RuntimeError, "invalid linked source-parent frame");
        return -1;
    }
    PyFrameObject *object = _PyFrame_GetSoacLifetimeObject(frame);
    _PyInterpreterFrame *previous = frame->previous;
    int terminal = reason == PySoac_LIFETIME_SCOPE_TERMINAL;

    /* Valid leave cannot fail after this point. Detach before allocation or
     * any caller-owned primary cleanup, including reentrant finalizers. */
    tstate->current_frame = previous;
    frame->previous = NULL;
    frame->soac_lifetime_owned_environment = terminal
        ? SOAC_LIFETIME_SCOPE_TERMINAL : 0;
    if (tstate->last_profiled_frame == frame) {
        tstate->last_profiled_frame = previous;
    }
    state.state = terminal ? SOAC_SCOPE_V1_TERMINAL : SOAC_SCOPE_V1_SUSPENDED;
    state.thread = state.frame = state.previous = 0;
    memcpy(scope, &state, sizeof(state));

    _PyStackRef function = frame->f_funcobj;
    frame->f_funcobj = PyStackRef_NULL;
    frame->f_globals = NULL;
    frame->f_builtins = NULL;
    PyStackRef_CLOSE(function);  /* borrowed: closes debug handle, no DECREF */

    /* Suspension must not retain the resume caller. At terminal, mirror the
     * ordinary take_ownership predecessor link, including its OOM policy. */
    if (terminal && !_PyObject_IsUniquelyReferenced((PyObject *)object)) {
        assert(object->f_back == NULL);
        _PyInterpreterFrame *parent = _PyFrame_GetFirstComplete(previous);
        if (parent != NULL) {
            assert(parent->owner < FRAME_OWNED_BY_INTERPRETER ||
                   (parent->owner == FRAME_OWNED_BY_SOAC_ACTIVE &&
                    (parent->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED)));
            PyObject *error = PyErr_GetRaisedException();
            PyFrameObject *back = _PyFrame_GetFrameObject(parent);
            if (back == NULL) {
                assert(PyErr_ExceptionMatches(PyExc_MemoryError));
                PyErr_Clear();
            }
            else {
                object->f_back = (PyFrameObject *)Py_NewRef(back);
            }
            PyErr_SetRaisedException(error);
        }
    }
    return 0;
#endif
}
