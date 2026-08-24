/* A lazy, exact PyFrameObject that owns source lifetimes, not an execution
 * activation. ACTIVE slots are deliberately empty and are never exposed as
 * Python locals. The source activation owns its sole primary frame reference.
 * At terminal retirement, retained tracebacks take the then-current owners.
 */
#include "pycore_gc.h"  // _PyGC_PREV_MASK_COLLECTING

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
    /* These fields describe the last recorded traceback site only. Dynamic
     * frame position APIs explicitly refuse, since execution may continue. */
    object->f_lineno = source_lineno;
    frame->instr_ptr = instruction_offset == -1 ? NULL
        : _PyCode_CODE(code) + instruction_offset / sizeof(_Py_CODEUNIT);
    return PyTraceBack_Here(object);
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
