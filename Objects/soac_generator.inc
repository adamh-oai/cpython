/* Managed native generators, coroutines, and async generators. This metadata
 * owns no execution frame: suspended roots and the sole handled-exception item
 * belong to owner. The common explicit pointer never reuses origin/finalizer. */

enum soac_gen_phase {
    SOAC_GEN_BINDING,
    SOAC_GEN_LIVE,
    SOAC_GEN_COMPLETING,
    SOAC_GEN_TERMINAL
};

struct _PySoacManagedGenerator {
    PySoacGeneratorSpec spec;
    PyObject *owner;
    enum soac_gen_phase phase;
};

static void
soac_gen_assert_no_native_activation(PyGenObject *gen)
{
    assert(_PyGen_IsSoacManaged(gen));
    assert(gen->gi_exc_state.exc_value == NULL);
    assert(gen->gi_exc_state.previous_item == NULL);
    assert(gen->gi_iframe.previous == NULL);
    assert(PyStackRef_IsNull(gen->gi_iframe.f_funcobj));
    assert(gen->gi_iframe.frame_obj == NULL);
}

static void
soac_gen_decref_preserving_error(PyObject *object)
{
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(object);
    PyErr_SetRaisedException(error);
}

static void
soac_gen_retire(PyGenObject *gen)
{
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    soac_gen_assert_no_native_activation(gen);
    /* Terminalize before any callback or decref can expose a retained gen. */
    gen->gi_frame_state = FRAME_CLEARED;
    managed->phase = SOAC_GEN_TERMINAL;
    PyObject *owner = managed->owner;
    managed->owner = NULL;
    if (owner != NULL) {
        PyObject *error = PyErr_GetRaisedException();
        managed->spec.clear(owner, (PyObject *)gen);
        Py_DECREF(owner);
        PyErr_SetRaisedException(error);
    }
    soac_gen_assert_no_native_activation(gen);
}

int
PyGen_MatchesSoacOwner(PyObject *generator, PyObject *owner)
{
    if (generator == NULL || !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        return 0;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->phase == SOAC_GEN_TERMINAL || managed->owner == NULL ||
        FRAME_STATE_FINISHED(gen->gi_frame_state)) {
        PyErr_SetString(PyExc_RuntimeError, "managed generator association is terminal");
        return -1;
    }
    return managed->owner == owner;
}

int
PyGen_MarkSoacManagedTerminal(PyObject *generator, PyObject *owner)
{
    if (generator == NULL || owner == NULL ||
        !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification requires a managed generator");
        return -1;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->owner != owner) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification does not own this generator");
        return -1;
    }
    if (managed->phase == SOAC_GEN_COMPLETING &&
        gen->gi_frame_state == FRAME_CLEARED) {
        return 0;
    }
    if (managed->phase != SOAC_GEN_LIVE ||
        gen->gi_frame_state != FRAME_EXECUTING) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification requires an active native step");
        return -1;
    }
    /* The compiler must do this before detaching its handled item or releasing
     * any terminal local/cell: either can run a Python finalizer. No callback,
     * allocation, or decref occurs here. The active step keeps its owner until
     * the final CLOSED result validates and performs one-way retirement. */
    managed->phase = SOAC_GEN_COMPLETING;
    gen->gi_frame_state = FRAME_CLEARED;
    return 0;
}

static PySendResult
soac_gen_step(PyGenObject *gen, int8_t previous_state,
              const PySoacGeneratorInput *input, PyObject **value)
{
    soac_gen_assert_no_native_activation(gen);
    assert(gen->gi_frame_state == FRAME_EXECUTING);
    assert(previous_state == FRAME_CREATED || FRAME_STATE_SUSPENDED(previous_state));
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    *value = NULL;
    if (managed->phase != SOAC_GEN_LIVE || managed->owner == NULL) {
        gen->gi_frame_state = previous_state;
        PyErr_SetString(PyExc_RuntimeError, "managed generator owner is not bound");
        return PYGEN_ERROR;
    }
    PyObject *owner = Py_NewRef(managed->owner);
    PyThreadState *tstate = _PyThreadState_GET();
    _PyErr_StackItem *caller_item = tstate->exc_info;
    PySoacGeneratorResult result = {
        .outcome = PYGEN_ERROR,
        .state = PySoac_GENERATOR_UNCHANGED,
        .suspension = PySoac_SUSPEND_NONE,
        .value = NULL,
    };
    managed->spec.step(owner, (PyObject *)gen, input, &result);
    /* The callback owns any materialized execution/exception activation and
     * must detach it before returning. Native never installs gi_exc_state. */
    assert(tstate->exc_info == caller_item);
    (void)caller_item;
    soac_gen_assert_no_native_activation(gen);
    int executing = managed->phase == SOAC_GEN_LIVE &&
                    gen->gi_frame_state == FRAME_EXECUTING;
    int completing = managed->phase == SOAC_GEN_COMPLETING &&
                     gen->gi_frame_state == FRAME_CLEARED;
    int valid = managed->owner == owner && (executing || completing);
    if (result.outcome == PYGEN_NEXT) {
        valid = valid && executing && result.state == PySoac_GENERATOR_SUSPENDED &&
                result.value != NULL && !PyErr_Occurred();
        if (result.suspension == PySoac_SUSPEND_DIRECT) {
            valid = valid && PyGen_CheckExact((PyObject *)gen);
        }
        else if (result.suspension == PySoac_SUSPEND_DELEGATING) {
            /* Every native family may suspend through its actual delegate. */
        }
        else if (result.suspension == PySoac_SUSPEND_ASYNC_YIELD) {
            valid = valid && PyAsyncGen_CheckExact((PyObject *)gen) &&
                    result.value != NULL &&
                    Py_IS_TYPE(result.value, &_PyAsyncGenWrappedValue_Type);
        }
        else {
            valid = 0;
        }
    }
    else if (result.outcome == PYGEN_RETURN) {
        valid = valid && result.state == PySoac_GENERATOR_CLOSED &&
                result.suspension == PySoac_SUSPEND_NONE &&
                result.value != NULL && !PyErr_Occurred() &&
                (!PyAsyncGen_CheckExact((PyObject *)gen) || result.value == Py_None);
    }
    else if (result.outcome == PYGEN_ERROR) {
        valid = valid && ((executing && result.state == PySoac_GENERATOR_UNCHANGED) ||
                           result.state == PySoac_GENERATOR_CLOSED) &&
                result.suspension == PySoac_SUSPEND_NONE &&
                result.value == NULL && PyErr_Occurred();
    }
    else {
        valid = 0;
    }
    if (!valid) {
        PyObject *error = PyErr_GetRaisedException();
        soac_gen_retire(gen);
        Py_XDECREF(result.value);
        Py_DECREF(owner);
        Py_XDECREF(error);
        PyErr_SetString(PyExc_SystemError, "invalid managed generator callback result");
        return PYGEN_ERROR;
    }
    if (result.state == PySoac_GENERATOR_CLOSED) {
        soac_gen_retire(gen);
    }
    else if (result.state == PySoac_GENERATOR_SUSPENDED) {
        gen->gi_frame_state = result.suspension == PySoac_SUSPEND_DELEGATING
                            ? FRAME_SUSPENDED_YIELD_FROM : FRAME_SUSPENDED;
    }
    else {
        gen->gi_frame_state = previous_state;
    }
    soac_gen_decref_preserving_error(owner);
    *value = result.value;
    return result.outcome;
}

static PyObject *
soac_gen_close(PyGenObject *gen, int8_t previous_state)
{
    const PySoacGeneratorInput input = {
        .operation = PySoac_GENERATOR_CLOSE,
        .close_on_genexit = 1,
    };
    PyObject *value;
    PySendResult result = soac_gen_step(gen, previous_state, &input, &value);
    if (result == PYGEN_RETURN) {
        return value;
    }
    if (result == PYGEN_NEXT) {
        Py_DECREF(value);
        PyErr_SetString(PyExc_RuntimeError, "generator ignored GeneratorExit");
        return NULL;
    }
    if (PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return NULL;
}

static PyObject *
soac_gen_yield_from(PyGenObject *gen)
{
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->phase != SOAC_GEN_LIVE || managed->owner == NULL ||
        gen->gi_frame_state != FRAME_SUSPENDED_YIELD_FROM) {
        Py_RETURN_NONE;
    }
    /* The fixed callback only reads its already-owned delegate; it must not
     * dispatch Python or introduce a second execution/exception activation. */
    PyObject *result = managed->spec.yield_from(managed->owner);
    soac_gen_assert_no_native_activation(gen);
    if (result == NULL && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_SystemError, "managed generator delegate callback returned NULL");
    }
    return result;
}

static PyObject *
soac_gen_new_managed(PyTypeObject *type, int code_kind,
                     PyObject *function, PyCodeObject *source_code,
                     PyObject *name, PyObject *qualname, PyObject *owner,
                     const PySoacGeneratorSpec *spec)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "managed generators require the selected GIL build");
    return NULL;
#else
    if (function == NULL || !PyFunction_Check(function) || source_code == NULL ||
        !PyCode_Check(source_code) || owner == NULL || spec == NULL ||
        spec->abi_version != PySoac_GENERATOR_ABI_VERSION || spec->reserved != 0 ||
        spec->bind == NULL || spec->step == NULL ||
        spec->yield_from == NULL || spec->clear == NULL ||
        (name != NULL && !PyUnicode_Check(name)) ||
        (qualname != NULL && !PyUnicode_Check(qualname))) {
        PyErr_SetString(PyExc_TypeError, "invalid managed generator construction specification");
        return NULL;
    }
    if ((source_code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
        != code_kind) {
        PyErr_SetString(PyExc_TypeError,
                        "managed generator code does not match the native family");
        return NULL;
    }
    if (type == &PyCoro_Type &&
        _PyThreadState_GET()->coroutine_origin_tracking_depth != 0) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "optimized coroutine origin tracking is not supported");
        return NULL;
    }
    PyFunctionObject *func = (PyFunctionObject *)function;
    PyObject *public_name = name == NULL ? func->func_name : name;
    PyObject *public_qualname = qualname == NULL ? func->func_qualname : qualname;
    if (public_name == NULL || public_qualname == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "managed generator function metadata is terminal");
        return NULL;
    }
    /* Copy the caller's ABI before allocation, never retaining its address. */
    PySoacGeneratorSpec copied_spec = *spec;
    struct _PySoacManagedGenerator *managed = PyMem_Calloc(1, sizeof(*managed));
    if (managed == NULL) {
        return PyErr_NoMemory();
    }
    /* No locals/operand slots: the fixed frame header holds only the code edge
     * required by gi_code. It is never an evaluation or introspection frame. */
    PyGenObject *gen = PyObject_GC_NewVar(PyGenObject, type, 0);
    if (gen == NULL) {
        PyMem_Free(managed);
        return NULL;
    }
    gen->gi_soac_managed = managed;
    gen->gi_origin_or_finalizer = NULL;
    gen->gi_weakreflist = NULL;
    gen->gi_name = Py_NewRef(public_name);
    gen->gi_qualname = Py_NewRef(public_qualname);
    gen->gi_exc_state.exc_value = NULL;
    gen->gi_exc_state.previous_item = NULL;
    gen->gi_hooks_inited = 0;
    gen->gi_closed = 0;
    gen->gi_running_async = 0;
    gen->gi_frame_state = FRAME_CREATED;
    memset(&gen->gi_iframe, 0, offsetof(_PyInterpreterFrame, localsplus));
    gen->gi_iframe.f_executable = PyStackRef_FromPyObjectNew((PyObject *)source_code);
    _PyFrame_SetExecutableView(&gen->gi_iframe, (PyObject *)source_code);
    gen->gi_iframe.f_funcobj = PyStackRef_NULL;
    gen->gi_iframe.owner = FRAME_OWNED_BY_GENERATOR;
    managed->spec = copied_spec;
    managed->owner = Py_NewRef(owner);
    managed->phase = SOAC_GEN_BINDING;
    PyObject *binding_owner = Py_NewRef(owner);
    int result = copied_spec.bind(binding_owner, (PyObject *)gen, function, source_code);
    soac_gen_assert_no_native_activation(gen);
    if (result != 0 || PyErr_Occurred() || managed->phase != SOAC_GEN_BINDING ||
        managed->owner != binding_owner || gen->gi_frame_state != FRAME_CREATED) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "managed generator binding failed without an error");
        }
        soac_gen_retire(gen);
        soac_gen_decref_preserving_error(binding_owner);
        /* A bind observer may have created a weak or strong reference. Use the
         * normal, fully initialized destructor, including weakref clearing. */
        _PyObject_GC_TRACK(gen);
        soac_gen_decref_preserving_error((PyObject *)gen);
        return NULL;
    }
    managed->phase = SOAC_GEN_LIVE;
    Py_DECREF(binding_owner);
    _PyObject_GC_TRACK(gen);
    return (PyObject *)gen;
#endif
}

PyObject *
PyGen_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                    PyObject *name, PyObject *qualname, PyObject *owner,
                    const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyGen_Type, CO_GENERATOR, function, source_code,
                                name, qualname, owner, spec);
}

PyObject *
PyCoro_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                     PyObject *name, PyObject *qualname, PyObject *owner,
                     const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyCoro_Type, CO_COROUTINE, function, source_code,
                                name, qualname, owner, spec);
}

PyObject *
PyAsyncGen_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                         PyObject *name, PyObject *qualname, PyObject *owner,
                         const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyAsyncGen_Type, CO_ASYNC_GENERATOR, function,
                                source_code, name, qualname, owner, spec);
}

PyObject *
PyAsyncGen_WrapSoacYield(PyObject *value)
{
    if (value == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyAsyncGenValueWrapperNew(_PyThreadState_GET(), value);
}
