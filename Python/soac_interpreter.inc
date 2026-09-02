/* Interpreter enforcement uses ordinary CPython frames and value owners.
 * Included by ceval.c; no SOAC body, reference-token or suspension ABI is used.
 */
static PyTypeObject SoacInterpreterActivation_Type;

typedef struct {
    PyObject_HEAD
    int64_t interpreter_id;
    uint8_t terminal;
} SoacInterpreterExecutionGuard;

static PyTypeObject SoacInterpreterExecutionGuard_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac_interpreter_execution_guard",
    .tp_basicsize = sizeof(SoacInterpreterExecutionGuard),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_dealloc = (destructor)PyObject_Free,
};

typedef struct {
    PyObject_VAR_HEAD
    PyObject *publication;
    SoacInterpreterExecutionGuard *execution;
    PyObject *globals;               /* Comparison only; actual module owns. */
    uint64_t source_id;
    int64_t interpreter_id;
    uint8_t terminal;
    PySoacInterpreterDescriptorV1 descriptors[1];
} SoacInterpreterSourceBinding;

#define SOAC_FUNCTION_READY 1u
#define SOAC_FUNCTION_TERMINAL 2u
#define SOAC_FUNCTION_ENTERED 4u

typedef struct {
    PyObject_HEAD
    PyCodeObject *original_code; /* Comparison only; function/source own code. */
    PyObject *globals, *builtins; /* Immutable function edges, never dereferenced here. */
    uint64_t source_id;
    int64_t interpreter_id;
    uint8_t ready, terminal, entered;
    SoacInterpreterExecutionGuard *execution;
    /* Borrowed from the actual function's dynamic owner GC vector. */
    SoacInterpreterSourceBinding *binding;
    const PySoacInterpreterDescriptorV1 *descriptor;
    uint64_t birth_activation_id;
} SoacInterpreterFunctionGuard;

static void
soac_function_guard_dealloc(SoacInterpreterFunctionGuard *guard)
{
    Py_XDECREF(guard->execution);
    PyObject_Free(guard);
}

static PyTypeObject SoacInterpreterFunctionGuard_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac_interpreter_function_guard",
    .tp_basicsize = sizeof(SoacInterpreterFunctionGuard),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_dealloc = (destructor)soac_function_guard_dealloc,
};

typedef struct _PySoacInterpreterSiteV1 {
    int32_t ordinal;
    uint32_t flags;
    int32_t definition_end;
} SoacInterpreterSite;

static int
soac_interpreter_error(const char *message)
{
    if (!PyErr_Occurred()) {
        PyObject *type = PySoac_GetStrictRuntimeUnavailableError();
        if (type != NULL) {
            PyErr_SetString(type, message);
        }
    }
    return -1;
}

static int
soac_interpreter_status(int status, const char *message)
{
    if (status == 0 && !PyErr_Occurred()) {
        return 0;
    }
    return soac_interpreter_error(message);
}

static int
soac_source_binding_traverse(SoacInterpreterSourceBinding *binding,
                            visitproc visit, void *arg)
{
    Py_VISIT(binding->publication);
    Py_VISIT(binding->execution);
    return 0;
}

static int
soac_source_binding_clear(SoacInterpreterSourceBinding *binding)
{
    /* Descriptors remain stable until deallocation. No callback following a
     * decref may use a live-looking binding whose publication was cleared. */
    binding->terminal = 1;
    if (binding->execution != NULL) binding->execution->terminal = 1;
    Py_CLEAR(binding->publication);
    Py_CLEAR(binding->execution);
    return 0;
}

static void
soac_source_binding_dealloc(SoacInterpreterSourceBinding *binding)
{
    PyObject_GC_UnTrack(binding);
    soac_source_binding_clear(binding);
    PyObject_GC_Del(binding);
}

static PyTypeObject SoacInterpreterSourceBinding_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac_interpreter_source_binding",
    .tp_basicsize = offsetof(SoacInterpreterSourceBinding, descriptors),
    .tp_itemsize = sizeof(PySoacInterpreterDescriptorV1),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_source_binding_traverse,
    .tp_clear = (inquiry)soac_source_binding_clear,
    .tp_dealloc = (destructor)soac_source_binding_dealloc,
};

int
PySoac_IsInterpreterSourceBindingV1(PyObject *object)
{
    return object != NULL && Py_IS_TYPE(object, &SoacInterpreterSourceBinding_Type);
}

static int
soac_source_binding_live(const SoacInterpreterSourceBinding *binding)
{
    return binding != NULL && !binding->terminal && binding->publication != NULL &&
        binding->execution != NULL && !binding->execution->terminal &&
        binding->interpreter_id == _PyInterpreterState_GET()->id;
}

static SoacInterpreterSourceBinding *
soac_source_binding(PyObject *object)
{
    if (!PySoac_IsInterpreterSourceBindingV1(object)) {
        soac_interpreter_error("invalid interpreter source binding");
        return NULL;
    }
    SoacInterpreterSourceBinding *binding = (SoacInterpreterSourceBinding *)object;
    if (!soac_source_binding_live(binding)) {
        soac_interpreter_error("interpreter source binding is unavailable");
        return NULL;
    }
    return binding;
}

int
PySoac_SetInterpreterCodeDescriptorV1(PyObject *object, uint32_t ordinal,
                                     uint32_t requirements)
{
    if (object == NULL || !PyCode_Check(object) || ordinal == UINT32_MAX ||
        (requirements & ~Py_SOAC_DESCRIPTOR_REQUIREMENTS_MASK))
        return soac_interpreter_error("invalid prepared interpreter descriptor");
    PyCodeObject *code = (PyCodeObject *)object;
    if (!(code->co_flags & CO_FUTURE_STRICT) || code->_co_soac_strict_source_id == 0 ||
        code->_co_soac_scope_kind == UINT8_MAX)
        return soac_interpreter_error("descriptor code has no original source identity");
    if (code->_co_soac_descriptor_ordinal != UINT32_MAX &&
        (code->_co_soac_descriptor_ordinal != ordinal ||
         code->_co_soac_descriptor_requirements != requirements))
        return soac_interpreter_error("prepared interpreter descriptor is immutable");
    code->_co_soac_descriptor_ordinal = ordinal;
    code->_co_soac_descriptor_requirements = requirements;
    return 0;
}

PyObject *
PySoac_NewInterpreterSourceBindingV1(
    PyObject *publication, PyObject *execution, PyObject *globals,
    uint64_t source_id, const PySoacInterpreterDescriptorV1 *descriptors,
    size_t count, size_t descriptor_size)
{
    if (publication == NULL || execution == NULL ||
        !Py_IS_TYPE(execution, &SoacInterpreterExecutionGuard_Type) ||
        globals == NULL || !PyDict_CheckExact(globals) || source_id == 0 ||
        descriptors == NULL || descriptor_size != sizeof(*descriptors) ||
        count == 0 || count > UINT32_MAX ||
        count > (size_t)(PY_SSIZE_T_MAX - sizeof(SoacInterpreterSourceBinding)) / sizeof(*descriptors)) {
        soac_interpreter_error("invalid interpreter source publication");
        return NULL;
    }
    SoacInterpreterExecutionGuard *guard = (SoacInterpreterExecutionGuard *)execution;
    if (guard->terminal || guard->interpreter_id != _PyInterpreterState_GET()->id) {
        soac_interpreter_error("interpreter publication execution is unavailable");
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const PySoacInterpreterDescriptorV1 *descriptor = &descriptors[i];
        if (descriptor->ordinal != i || descriptor->original_code == NULL ||
            !PyCode_Check(descriptor->original_code) ||
            (descriptor->requirements & ~Py_SOAC_DESCRIPTOR_REQUIREMENTS_MASK) ||
            (descriptor->parent_ordinal != UINT32_MAX && descriptor->parent_ordinal >= count)) {
            soac_interpreter_error("invalid interpreter descriptor array");
            return NULL;
        }
        PyCodeObject *code = (PyCodeObject *)descriptor->original_code;
        if (code->_co_soac_strict_source_id != source_id ||
            code->_co_soac_descriptor_ordinal != descriptor->ordinal ||
            code->_co_soac_descriptor_requirements != descriptor->requirements ||
            code->_co_soac_scope_kind != descriptor->scope_kind ||
            !(code->co_flags & CO_FUTURE_STRICT) ||
            ((descriptor->parent_ordinal == UINT32_MAX) !=
             (descriptor->scope_kind == Py_SOAC_SCOPE_MODULE))) {
            soac_interpreter_error("interpreter descriptor differs from its original code");
            return NULL;
        }
    }
    if (PyType_Ready(&SoacInterpreterSourceBinding_Type) < 0) return NULL;
    SoacInterpreterSourceBinding *binding = PyObject_GC_NewVar(
        SoacInterpreterSourceBinding, &SoacInterpreterSourceBinding_Type, (Py_ssize_t)count);
    if (binding == NULL) return NULL;
    binding->publication = Py_NewRef(publication);
    binding->execution = (SoacInterpreterExecutionGuard *)Py_NewRef(execution);
    binding->globals = globals;
    binding->source_id = source_id;
    binding->interpreter_id = guard->interpreter_id;
    binding->terminal = 0;
    memcpy(binding->descriptors, descriptors, count * sizeof(*descriptors));
    PyObject_GC_Track(binding);
    if (!soac_source_binding_live(binding)) {
        Py_DECREF(binding);
        soac_interpreter_error("interpreter publication changed during preparation");
        return NULL;
    }
    return (PyObject *)binding;
}

PyObject *
PySoac_GetInterpreterSourcePublicationV1(PyObject *object)
{
    SoacInterpreterSourceBinding *binding = soac_source_binding(object);
    return binding == NULL ? NULL : binding->publication;
}

PyObject *
PySoac_GetInterpreterSourcePublicationForTeardownV1(PyObject *object)
{
    if (!PySoac_IsInterpreterSourceBindingV1(object)) return NULL;
    SoacInterpreterSourceBinding *binding = (SoacInterpreterSourceBinding *)object;
    return binding->interpreter_id == _PyInterpreterState_GET()->id
        ? binding->publication : NULL;
}

int
PySoac_InterpreterSourceBindingInvalidateV1(PyObject *object)
{
    if (!PySoac_IsInterpreterSourceBindingV1(object))
        return soac_interpreter_error("invalid interpreter source binding");
    SoacInterpreterSourceBinding *binding = (SoacInterpreterSourceBinding *)object;
    if (binding->interpreter_id != _PyInterpreterState_GET()->id)
        return soac_interpreter_error("interpreter source binding belongs to another interpreter");
    binding->terminal = 1;
    if (binding->execution != NULL) binding->execution->terminal = 1;
    return 0;
}

static const PySoacInterpreterDescriptorV1 *
soac_source_descriptor(SoacInterpreterSourceBinding *binding, PyCodeObject *code)
{
    uint32_t ordinal = code->_co_soac_descriptor_ordinal;
    if (code->_co_soac_strict_source_id != binding->source_id ||
        ordinal >= (uint32_t)Py_SIZE(binding)) return NULL;
    const PySoacInterpreterDescriptorV1 *descriptor = &binding->descriptors[ordinal];
    return descriptor->original_code == (PyObject *)code &&
        descriptor->requirements == code->_co_soac_descriptor_requirements &&
        descriptor->scope_kind == code->_co_soac_scope_kind ? descriptor : NULL;
}

/* The actual function's owner edge supports this pointer. Dynamic owners
 * include the same binding in their GC vector; guards never own it. */
static SoacInterpreterSourceBinding *
soac_function_source_binding(PyFunctionObject *function)
{
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED)
        return NULL;
    if (function->func_soac_interpreter_descriptor != NULL) {
        return PySoac_IsInterpreterSourceBindingV1(function->func_soac_strict_owner)
            ? (SoacInterpreterSourceBinding *)function->func_soac_strict_owner : NULL;
    }
    SoacInterpreterFunctionGuard *guard =
        (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
    return guard == NULL ? NULL : guard->binding;
}

static SoacInterpreterSourceBinding *
soac_frame_source_binding(_PyInterpreterFrame *frame)
{
    if (_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
        return (SoacInterpreterSourceBinding *)activation->source_binding;
    }
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    return PyFunction_Check(function)
        ? soac_function_source_binding((PyFunctionObject *)function) : NULL;
}

int
PySoac_GetInterpreterFunctionViewV1(
    PyObject *object, PySoacInterpreterFunctionViewV1 *out, size_t out_size)
{
    if (object == NULL || !PyFunction_Check(object) || out == NULL || out_size != sizeof(*out))
        return soac_interpreter_error("invalid interpreter function view");
    PyFunctionObject *function = (PyFunctionObject *)object;
    *out = (PySoacInterpreterFunctionViewV1){
        .abi_version = 1, .ordinal = UINT32_MAX,
        .interpreter_id = _PyInterpreterState_GET()->id,
        .terminal = function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_TERMINAL,
    };
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED)
        return 0;
    SoacInterpreterSourceBinding *binding = soac_function_source_binding(function);
    const PySoacInterpreterDescriptorV1 *descriptor = function->func_soac_interpreter_descriptor;
    if (descriptor != NULL) {
        if (binding == NULL) return soac_interpreter_error("shared function lost its source binding");
        out->owner_kind = 2;
        out->ready = !!(function->func_soac_interpreter_flags & SOAC_FUNCTION_READY);
        out->terminal |= !!(function->func_soac_interpreter_flags & SOAC_FUNCTION_TERMINAL);
        out->entered = !!(function->func_soac_interpreter_flags & SOAC_FUNCTION_ENTERED);
        out->birth_activation_id = function->func_soac_interpreter_birth_id;
    }
    else {
        SoacInterpreterFunctionGuard *guard =
            (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
        if (guard == NULL) return soac_interpreter_error("dynamic function lost its native guard");
        descriptor = guard->descriptor;
        out->owner_kind = 1;
        out->ready = guard->ready;
        out->terminal |= guard->terminal;
        out->entered = guard->entered;
        out->birth_activation_id = guard->birth_activation_id;
        if (guard->terminal) return 0;
    }
    if (binding == NULL || descriptor == NULL)
        return soac_interpreter_error("interpreter function has no prepared descriptor");
    out->binding = (PyObject *)binding;
    out->ordinal = descriptor->ordinal;
    out->requirements = descriptor->requirements;
    out->interpreter_id = binding->interpreter_id;
    out->terminal |= !soac_source_binding_live(binding);
    return 0;
}

static SoacInterpreterFunctionGuard *
soac_function_guard(PyObject *guard)
{
    if (guard == NULL || !Py_IS_TYPE(guard, &SoacInterpreterFunctionGuard_Type)) {
        soac_interpreter_error("invalid interpreter function guard");
        return NULL;
    }
    SoacInterpreterFunctionGuard *value = (SoacInterpreterFunctionGuard *)guard;
    if (value->interpreter_id != _PyInterpreterState_GET()->id) {
        soac_interpreter_error("interpreter function guard belongs to another interpreter");
        return NULL;
    }
    return value;
}

int
_PySOAC_InterpreterGuardCreate(PyFunctionObject *function)
{
    if (PyType_Ready(&SoacInterpreterFunctionGuard_Type) < 0) return -1;
    SoacInterpreterFunctionGuard *guard = PyObject_New(
        SoacInterpreterFunctionGuard, &SoacInterpreterFunctionGuard_Type);
    if (guard == NULL) return -1;
    guard->original_code = (PyCodeObject *)function->func_code;
    guard->source_id = guard->original_code->_co_soac_strict_source_id;
    guard->globals = function->func_globals;
    guard->builtins = function->func_builtins;
    guard->interpreter_id = _PyInterpreterState_GET()->id;
    guard->ready = guard->terminal = guard->entered = 0;
    guard->execution = NULL;
    guard->binding = NULL;
    guard->descriptor = NULL;
    guard->birth_activation_id = 0;
    function->func_soac_interpreter_guard = (PyObject *)guard;
    return 0;
}

PyObject *
PySoac_GetInterpreterFunctionGuardV1(PyObject *object)
{
    if (object == NULL || !PyFunction_Check(object)) {
        soac_interpreter_error("interpreter guard requires an actual function");
        return NULL;
    }
    PyObject *guard = ((PyFunctionObject *)object)->func_soac_interpreter_guard;
    return soac_function_guard(guard) == NULL ? NULL : Py_NewRef(guard);
}

PyObject *
PyFunction_GetSoacStrictOwnerForTeardownV1(PyObject *object)
{
    if (object == NULL || !PyFunction_Check(object)) return NULL;
    PyFunctionObject *function = (PyFunctionObject *)object;
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) return NULL;
    SoacInterpreterFunctionGuard *guard =
        (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
    if (guard == NULL || guard->terminal || guard->interpreter_id != _PyInterpreterState_GET()->id)
        return NULL;
    return function->func_soac_strict_owner;
}

int
PySoac_InterpreterFunctionReadyV1(PyObject *object)
{
    SoacInterpreterFunctionGuard *guard = soac_function_guard(object);
    if (guard == NULL) return -1;
    if (guard->terminal) return soac_interpreter_error("interpreter function is terminal");
    guard->ready = 1;
    return 0;
}

void
_PySOAC_InterpreterGuardInvalidate(PyObject *object)
{
    if (object != NULL) {
        SoacInterpreterFunctionGuard *guard = (SoacInterpreterFunctionGuard *)object;
        assert(Py_IS_TYPE(object, &SoacInterpreterFunctionGuard_Type));
        guard->terminal = 1;
        guard->ready = 0;
        guard->binding = NULL;
        guard->descriptor = NULL;
    }
}

int
PySoac_InterpreterFunctionInvalidateV1(PyObject *object)
{
    if (soac_function_guard(object) == NULL) return -1;
    _PySOAC_InterpreterGuardInvalidate(object);
    return 0;
}

int
PySoac_InterpreterFunctionEnteredV1(PyObject *object)
{
    SoacInterpreterFunctionGuard *guard = soac_function_guard(object);
    return guard == NULL ? -1 : guard->entered;
}

void
_PySOAC_InterpreterFunctionInvalidate(PyFunctionObject *function)
{
    function->func_soac_interpreter_flags |= SOAC_FUNCTION_TERMINAL;
    function->func_soac_interpreter_flags &= ~SOAC_FUNCTION_READY;
    _PySOAC_InterpreterGuardInvalidate(function->func_soac_interpreter_guard);
}

PyObject *
PySoac_NewInterpreterExecutionGuardV1(void)
{
    if (PyType_Ready(&SoacInterpreterExecutionGuard_Type) < 0) return NULL;
    SoacInterpreterExecutionGuard *guard = PyObject_New(
        SoacInterpreterExecutionGuard, &SoacInterpreterExecutionGuard_Type);
    if (guard == NULL) return NULL;
    guard->interpreter_id = _PyInterpreterState_GET()->id;
    guard->terminal = 0;
    return (PyObject *)guard;
}

int
PySoac_InvalidateInterpreterExecutionGuardV1(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacInterpreterExecutionGuard_Type))
        return soac_interpreter_error("invalid interpreter execution guard");
    SoacInterpreterExecutionGuard *guard = (SoacInterpreterExecutionGuard *)object;
    if (guard->interpreter_id != _PyInterpreterState_GET()->id)
        return soac_interpreter_error("execution guard belongs to another interpreter");
    guard->terminal = 1;
    return 0;
}

int
PySoac_BindInterpreterFunctionExecutionV1(PyObject *object, PyObject *execution)
{
    SoacInterpreterFunctionGuard *guard = soac_function_guard(object);
    if (guard == NULL) return -1;
    if (execution == NULL || !Py_IS_TYPE(execution, &SoacInterpreterExecutionGuard_Type))
        return soac_interpreter_error("invalid interpreter execution guard");
    SoacInterpreterExecutionGuard *value = (SoacInterpreterExecutionGuard *)execution;
    if (value->terminal || value->interpreter_id != guard->interpreter_id || guard->terminal ||
        (guard->execution != NULL && guard->execution != value))
        return soac_interpreter_error("function execution guard is unavailable or changed");
    if (guard->execution == NULL) guard->execution = (SoacInterpreterExecutionGuard *)Py_NewRef(execution);
    return 0;
}

/* Keep the exact pending exception installed while a metadata destructor runs.
 * The additional temporary owner is of the exception, never an execution value.
 */
static void
soac_interpreter_xdecref_preserving_error(PyObject *object)
{
    PyObject *saved = PyErr_GetRaisedException();
    PyErr_SetRaisedException(Py_XNewRef(saved));
    Py_XDECREF(object);
    PyErr_SetRaisedException(saved);
}

static const PySoacInterpreterCallbacksV4 *
soac_interpreter_callbacks(void)
{
#ifdef Py_GIL_DISABLED
    soac_interpreter_error("interpreter callbacks V4 require a GIL build");
    return NULL;
#else
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->soac.interpreter_closed ||
        interp->soac.interpreter_callbacks.abi_version != Py_SOAC_INTERPRETER_CALLBACKS_ABI_V4) {
        soac_interpreter_error("interpreter enforcement callbacks are unavailable");
        return NULL;
    }
    return &interp->soac.interpreter_callbacks;
#endif
}

int
PySoac_SetInterpreterCallbacksV4(
    const PySoacInterpreterCallbacksV4 *callbacks, size_t callbacks_size)
{
#ifdef Py_GIL_DISABLED
    return soac_interpreter_error("interpreter callbacks V4 require a GIL build");
#else
    if (callbacks == NULL || callbacks_size != sizeof(*callbacks) ||
        callbacks->abi_version != Py_SOAC_INTERPRETER_CALLBACKS_ABI_V4 ||
        callbacks->flags != 0 || callbacks->root_begin == NULL ||
        callbacks->root_end == NULL || callbacks->birth == NULL ||
        callbacks->function_attribute == NULL || callbacks->enter == NULL ||
        callbacks->started == NULL ||
        callbacks->call == NULL || callbacks->selected_call_finished == NULL ||
        callbacks->check_entry == NULL || callbacks->definition_end == NULL ||
        callbacks->definition_abort == NULL ||
        callbacks->leave == NULL ||
        callbacks->prepare_type == NULL || callbacks->definition_complete == NULL) {
        return soac_interpreter_error("invalid interpreter enforcement callback table");
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->soac.interpreter_closed) {
        return soac_interpreter_error("interpreter enforcement callbacks are closed");
    }
    PySoacInterpreterCallbacksV4 *installed = &interp->soac.interpreter_callbacks;
    if (installed->abi_version != 0) {
        if (installed->abi_version == callbacks->abi_version &&
            installed->flags == callbacks->flags &&
            installed->root_begin == callbacks->root_begin &&
            installed->root_end == callbacks->root_end &&
            installed->birth == callbacks->birth &&
            installed->function_attribute == callbacks->function_attribute &&
            installed->enter == callbacks->enter &&
            installed->started == callbacks->started &&
            installed->call == callbacks->call &&
            installed->selected_call_finished == callbacks->selected_call_finished &&
            installed->check_entry == callbacks->check_entry &&
            installed->definition_end == callbacks->definition_end &&
            installed->definition_abort == callbacks->definition_abort &&
            installed->leave == callbacks->leave &&
            installed->prepare_type == callbacks->prepare_type &&
            installed->definition_complete == callbacks->definition_complete) {
            return 0;
        }
        return soac_interpreter_error("interpreter enforcement callbacks are already installed");
    }
    /* Callback pointers and scalars only. No registration allocation, Python
     * references, interpreter inheritance, or callback before atomic publish.
     */
    *installed = *callbacks;
    return 0;
#endif
}

int
_PySOAC_InterpreterIsActivation(PyObject *object)
{
    return object != NULL && Py_IS_TYPE(object, &SoacInterpreterActivation_Type);
}

static int
soac_interpreter_activation_matches(
    _PySoacInterpreterActivationV1 *activation, _PyInterpreterFrame *frame)
{
    return activation->state != NULL &&
        activation->function != NULL && activation->code != NULL &&
        PyStackRef_AsPyObjectBorrow(frame->f_funcobj) == (PyObject *)activation->function &&
        _PyFrame_GetCode(frame) == activation->code;
}

/* One cold, native decoding pass validates instruction boundaries and exact
 * emitted definition regions. Runtime events perform direct array lookups;
 * ordinary calls never decode code or visit source receipt tables. */
int
PySoac_SetInterpreterCodeEventsV1(PyObject *object,
    const PySoacInterpreterCodeEventV1 *events, size_t event_count, size_t event_size)
{
    if (object == NULL || !PyCode_Check(object) || event_size != sizeof(*events) ||
        (event_count != 0 && events == NULL))
        return soac_interpreter_error("invalid interpreter code event table");
    PyCodeObject *code = (PyCodeObject *)object;
    Py_ssize_t count = _PyCode_NBYTES(code) / sizeof(_Py_CODEUNIT);
    if (!(code->co_flags & CO_FUTURE_STRICT) || code->_co_soac_strict_source_id == 0 ||
        count <= 0 || count > INT_MAX || (size_t)count > SIZE_MAX / sizeof(SoacInterpreterSite))
        return soac_interpreter_error("interpreter events require actual trusted source code");
    SoacInterpreterSite *sites = PyMem_Malloc((size_t)count * sizeof(*sites));
    if (sites == NULL) { PyErr_NoMemory(); return -1; }
    for (int i = 0; i < count; i++)
        sites[i] = (SoacInterpreterSite){.ordinal = -1, .definition_end = -1};
    int32_t ordinal = 0, begin = -1;
    size_t event = 0;
    for (int offset = 0; offset < count;) {
        _Py_CODEUNIT instruction = _Py_GetBaseCodeUnit(code, offset);
        int length = _PyInstruction_GetLength(code, offset);
        if (length <= 0 || length > count - offset) goto invalid;
        sites[offset].ordinal = ordinal;
        if (instruction.op.code != EXTENDED_ARG) {
            if (event < event_count && events[event].instruction_ordinal < (uint32_t)ordinal)
                goto invalid;
            if (event < event_count && events[event].instruction_ordinal == (uint32_t)ordinal) {
                if (events[event].flags != Py_SOAC_INTERPRETER_EVENT_CALL ||
                    (instruction.op.code != CALL && instruction.op.code != CALL_KW &&
                     instruction.op.code != CALL_FUNCTION_EX)) goto invalid;
                sites[offset].flags = events[event++].flags;
            }
            if (instruction.op.code == SOAC_BEGIN_DEFINITION) {
                sites[offset].definition_end = begin;
                begin = offset;
            }
            else if (instruction.op.code == SOAC_COMPLETE_DEFINITION) {
                if (begin < 0) goto invalid;
                int previous = sites[begin].definition_end;
                sites[begin].definition_end = offset;
                begin = previous;
            }
            ordinal++;
        }
        offset += length;
    }
    if (event != event_count || begin != -1) goto invalid;
    if (code->_co_soac_sites != NULL) {
        int same = memcmp(code->_co_soac_sites, sites, (size_t)count * sizeof(*sites)) == 0;
        PyMem_Free(sites);
        return same ? 0 : soac_interpreter_error("interpreter code events are immutable");
    }
    code->_co_soac_sites = sites;
    return 0;
invalid:
    PyMem_Free(sites);
    return soac_interpreter_error("interpreter code events or definition regions are invalid");
}

static int
soac_interpreter_site(_PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction,
                      Py_ssize_t *units, Py_ssize_t *ordinal)
{
    *units = *ordinal = -1;
    if (instruction == NULL) return 0;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    uintptr_t start = (uintptr_t)_PyFrame_GetBytecode(frame);
    uintptr_t position = (uintptr_t)instruction;
    size_t bytes = (size_t)_PyCode_NBYTES(code);
    if (position < start || position - start >= bytes ||
        (position - start) % sizeof(_Py_CODEUNIT) != 0 || code->_co_soac_sites == NULL)
        return soac_interpreter_error("interpreter event has no indexed native instruction");
    Py_ssize_t offset = (position - start) / sizeof(_Py_CODEUNIT);
    if (code->_co_soac_sites[offset].ordinal < 0)
        return soac_interpreter_error("interpreter event is not at a native opcode boundary");
    *units = offset;
    *ordinal = code->_co_soac_sites[offset].ordinal;
    return 0;
}

int
_PySOAC_InterpreterCallRelevant(_PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction)
{
    PyCodeObject *code = _PyFrame_GetCode(frame);
    if (code->_co_soac_sites == NULL) return 0;
    Py_ssize_t offset = instruction - _PyFrame_GetBytecode(frame);
    return offset >= 0 && offset < Py_SIZE(code) &&
        (code->_co_soac_sites[offset].flags & Py_SOAC_INTERPRETER_EVENT_CALL) != 0;
}

int
_PySOAC_InterpreterView(_PyInterpreterFrame *frame,
                        const _Py_CODEUNIT *this_instr, uint32_t phase,
                        PyObject *state, PySoacInterpreterFrameViewV1 *view)
{
    if (frame == NULL || view == NULL ||
        (frame->owner != FRAME_OWNED_BY_THREAD &&
         frame->owner != FRAME_OWNED_BY_GENERATOR) ||
        !PyFunction_Check(PyStackRef_AsPyObjectBorrow(frame->f_funcobj))) {
        return soac_interpreter_error("interpreter callback requires its actual ordinary frame");
    }
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        if (frame->soac_checked_activation != NULL || phase != Py_SOAC_INTERPRETER_BINDING || state != NULL)
            return soac_interpreter_error("interpreter callback has no native activation");
        *view = (PySoacInterpreterFrameViewV1){
            .frame = frame, .thread = _PyThreadState_GET(), .self = view,
            .phase = phase, .kind = Py_SOAC_INTERPRETER_FUNCTION,
            .interpreter_id = _PyInterpreterState_GET()->id,
            .instruction_units = -1, .instruction_ordinal = -1,
        };
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (activation->function !=
            (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(frame->f_funcobj) ||
        activation->code != _PyFrame_GetCode(frame) ||
        activation->phase != phase || activation->state != state ||
        activation->interpreter_id != _PyInterpreterState_GET()->id ||
        activation->activation_id == 0) {
        return soac_interpreter_error("interpreter callback activation changed");
    }
    memset(view, 0, sizeof(*view));
    view->frame = frame;
    view->thread = _PyThreadState_GET();
    view->self = view;
    view->call_state = state;
    view->phase = phase;
    view->kind = activation->kind;
    view->source_authority = activation->source_authority;
    view->interpreter_id = activation->interpreter_id;
    view->activation_id = activation->activation_id;
    return soac_interpreter_site(
        frame, this_instr, &view->instruction_units, &view->instruction_ordinal);
}

static int
soac_interpreter_check_view(const PySoacInterpreterFrameViewV1 *view)
{
    if (view == NULL || view->self != view || view->frame == NULL ||
        view->thread != _PyThreadState_GET() ||
        !((view->phase >= Py_SOAC_INTERPRETER_BINDING &&
           view->phase <= Py_SOAC_INTERPRETER_RETURNING) ||
          view->phase == Py_SOAC_INTERPRETER_FAILING)) {
        return soac_interpreter_error("invalid or expired interpreter callback view");
    }
    return 0;
}

PyObject *
PySoac_InterpreterFrameSourceBindingV1(const PySoacInterpreterFrameViewV1 *view)
{
    if (soac_interpreter_check_view(view) < 0) return NULL;
    SoacInterpreterSourceBinding *binding = soac_frame_source_binding(view->frame);
    if (!soac_source_binding_live(binding)) {
        soac_interpreter_error("interpreter frame has no live source binding");
        return NULL;
    }
    return (PyObject *)binding;
}

int
PySoac_GetInterpreterFrameInfoV2(
    const PySoacInterpreterFrameViewV1 *view,
    PySoacInterpreterFrameInfoV2 *out, size_t out_size)
{
    if (out == NULL || out_size != sizeof(*out) ||
        soac_interpreter_check_view(view) < 0) {
        return soac_interpreter_error("invalid interpreter frame information output");
    }
    _PyInterpreterFrame *frame = view->frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    *out = (PySoacInterpreterFrameInfoV2) {
        .abi_version = Py_SOAC_INTERPRETER_ABI_V2,
        .phase = view->phase,
        .kind = view->kind,
        .source_authority = view->source_authority,
        .interpreter_id = view->interpreter_id,
        .activation_id = view->activation_id,
        .function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj),
        .code = (PyObject *)code,
        .globals = frame->f_globals,
        .builtins = frame->f_builtins,
        .locals = frame->f_locals,
        .call_state = view->call_state,
        .instruction_units = view->instruction_units,
        .instruction_ordinal = view->instruction_ordinal,
        .localsplus_count = code->co_nlocalsplus,
    };
    return 0;
}

PyObject *
PySoac_InterpreterFrameLocalV1(
    const PySoacInterpreterFrameViewV1 *view, Py_ssize_t index)
{
    if (soac_interpreter_check_view(view) < 0) {
        return NULL;
    }
    if (index < 0 || index >= _PyFrame_GetCode(view->frame)->co_nlocalsplus) {
        soac_interpreter_error("interpreter frame local index is out of range");
        return NULL;
    }
    return PyStackRef_AsPyObjectBorrow(view->frame->localsplus[index]);
}

static void
soac_interpreter_retire(_PySoacInterpreterActivationV1 *activation, uint32_t reason)
{
    PyObject *state = activation->state;
    PyObject *source_binding = activation->source_binding;
    activation->state = NULL;
    activation->thread = NULL;
    activation->function = NULL;
    activation->code = NULL;
    activation->source_binding = NULL;
    activation->namespace_state_out = NULL;
    memset(&activation->incoming_call, 0, sizeof(activation->incoming_call));
    activation->source_authority = 0;
    activation->phase = Py_SOAC_INTERPRETER_RETIRED;
    if (state != NULL) {
        /* Closing an interpreter blocks NEW entry but retains this immutable
         * scalar table until its already-entered ordinary frames are cleared.
         */
        const PySoacInterpreterCallbacksV4 *callbacks =
            &_PyInterpreterState_GET()->soac.interpreter_callbacks;
        assert(callbacks->abi_version == Py_SOAC_INTERPRETER_CALLBACKS_ABI_V4);
        callbacks->leave(state, reason, activation->interpreter_id,
                         activation->activation_id); /* Metadata/scalars only. */
        soac_interpreter_xdecref_preserving_error(state);
    }
    soac_interpreter_xdecref_preserving_error(source_binding);
}

static int
soac_interpreter_activation_traverse(
    _PySoacInterpreterActivationV1 *activation, visitproc visit, void *arg)
{
    Py_VISIT(activation->state);
    Py_VISIT(activation->source_binding);
    return 0;
}

static int
soac_interpreter_activation_clear(_PySoacInterpreterActivationV1 *activation)
{
    soac_interpreter_retire(activation, Py_SOAC_INTERPRETER_FRAME_CLEARED);
    return 0;
}

static void
soac_interpreter_activation_dealloc(_PySoacInterpreterActivationV1 *activation)
{
    PyObject_GC_UnTrack(activation);
    soac_interpreter_activation_clear(activation);
    PyObject_GC_Del(activation);
}

static PyTypeObject SoacInterpreterActivation_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac_interpreter_activation",
    .tp_basicsize = sizeof(_PySoacInterpreterActivationV1),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_dealloc = (destructor)soac_interpreter_activation_dealloc,
    .tp_traverse = (traverseproc)soac_interpreter_activation_traverse,
    .tp_clear = (inquiry)soac_interpreter_activation_clear,
};

void
_PySOAC_CheckedFrameClear(_PyInterpreterFrame *frame, uint32_t reason)
{
    PyObject *object = frame->soac_checked_activation;
    frame->soac_checked_activation = NULL;
    if (_PySOAC_InterpreterIsActivation(object)) {
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)object;
        if (reason == Py_SOAC_INTERPRETER_FRAME_CLEARED) {
            if (activation->phase == Py_SOAC_INTERPRETER_RETIRED) {
                reason = Py_SOAC_INTERPRETER_RETURNED;
            }
            else if (PyErr_Occurred()) {
                reason = activation->phase == Py_SOAC_INTERPRETER_RUNNING
                    ? Py_SOAC_INTERPRETER_BODY_FAILED
                    : Py_SOAC_INTERPRETER_CHECK_FAILED;
            }
        }
        soac_interpreter_retire(activation, reason);
    }
    soac_interpreter_xdecref_preserving_error(object);
}

static int
soac_shared_function_authorize(_PyInterpreterFrame *frame, PyFunctionObject *function)
{
    SoacInterpreterSourceBinding *binding = soac_function_source_binding(function);
    const PySoacInterpreterDescriptorV1 *descriptor = function->func_soac_interpreter_descriptor;
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED ||
        descriptor == NULL || !soac_source_binding_live(binding) ||
        function->func_soac_interpreter_flags & SOAC_FUNCTION_TERMINAL ||
        function->func_soac_metadata != NULL || function->func_soac_strict_id != 0 ||
        function->func_globals != binding->globals ||
        frame->f_globals != function->func_globals || frame->f_builtins != function->func_builtins)
        return soac_interpreter_error("shared interpreter function owner is unavailable");
    PyCodeObject *code = _PyFrame_GetCode(frame);
    int original = (PyObject *)code == descriptor->original_code &&
        soac_source_descriptor(binding, code) == descriptor;
    if (original && (descriptor->scope_kind == Py_SOAC_SCOPE_MODULE ||
                     descriptor->scope_kind == Py_SOAC_SCOPE_CLASS || descriptor->requirements != 0))
        return soac_interpreter_error("shared function has a dynamic source requirement");
    if (!original && ((code->co_flags & CO_FUTURE_STRICT) || code->_co_soac_strict_source_id != 0))
        return soac_interpreter_error("shared interpreter function captured unrelated strict code");
    /* This authorizes an actual definition event. Ordinary leaf calls do not
     * need the descriptor; an incomplete closure is checked at its first read. */
    frame->soac_source_authority = original;
    if (original) function->func_soac_interpreter_flags |= SOAC_FUNCTION_ENTERED;
    return 0;
}

static int
soac_interpreter_authorize(_PyInterpreterFrame *frame)
{
    PyFunctionObject *function =
        (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (function->func_soac_interpreter_descriptor != NULL)
        return soac_shared_function_authorize(frame, function);
    SoacInterpreterFunctionGuard *guard =
        (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED ||
        function->func_soac_strict_owner == NULL || guard == NULL || guard->terminal ||
        function->func_soac_metadata != NULL ||
        guard->interpreter_id != _PyInterpreterState_GET()->id ||
        (guard->execution != NULL && guard->execution->terminal) ||
        function->func_globals != guard->globals || function->func_builtins != guard->builtins ||
        frame->f_globals != function->func_globals || frame->f_builtins != function->func_builtins)
        return soac_interpreter_error("interpreter function owner is unavailable");
    PyCodeObject *code = _PyFrame_GetCode(frame);
    int original = code == guard->original_code &&
        code->_co_soac_strict_source_id == guard->source_id && guard->source_id != 0;
    if (original && (code->_co_soac_scope_kind == Py_SOAC_SCOPE_MODULE ||
                     code->_co_soac_scope_kind == Py_SOAC_SCOPE_CLASS ||
                     code->_co_soac_scope_kind == UINT8_MAX))
        return soac_interpreter_error("namespace source code requires its explicit native construction entry");
    if (!original && ((code->co_flags & CO_FUTURE_STRICT) || code->_co_soac_strict_source_id != 0))
        return soac_interpreter_error("interpreter function captured unrelated strict code");
    /* Definition events authenticate pending births before using their
     * metadata. Completed ordinary calls never enter this path. */
    if (!guard->ready || guard->execution == NULL) {
        const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
        PySoacInterpreterFrameViewV1 view;
        if (callbacks == NULL || _PySOAC_InterpreterView(frame, NULL,
                Py_SOAC_INTERPRETER_BINDING, NULL, &view) < 0) return -1;
        int status = callbacks->check_entry(function->func_soac_strict_owner, &view);
        view.self = NULL;
        if (soac_interpreter_status(status, "interpreter function entry was refused") < 0) return -1;
        /* A cold check can run callbacks. Repeat native captured-code and
         * liveness facts before granting actual frame authority. */
        if (guard->terminal || (guard->execution != NULL && guard->execution->terminal) ||
            function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED)
            return soac_interpreter_error("interpreter owner changed during entry");
    }
    /* Read after the cold callback: it can finish or seal the metadata.
     * A real keyword-default policy still needs its independent live check. */
    if (function->func_soac_strict_id != 0 && function->func_kwdefaults != NULL &&
        PyFunction_CheckSoacStrictDefaults((PyObject *)function) < 0) return -1;
    frame->soac_source_authority = original;
    if (original) guard->entered = 1;
    return 0;
}

int
_PySOAC_InterpreterInitFrame(
    _PyInterpreterFrame *frame, const _PySoacInterpreterEntryV1 *entry)
{
    if (entry == NULL) {
        return 0;
    }
    if (entry->kind == Py_SOAC_INTERPRETER_FUNCTION) {
        if (soac_interpreter_authorize(frame) < 0) return -1;
        if (!entry->construction && entry->incoming_call == NULL) return 0;
    }
    const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL) {
        return -1;
    }
    PyFunctionObject *function =
        (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    PyCodeObject *code = _PyFrame_GetCode(frame);
    int original_source = (code->co_flags & CO_FUTURE_STRICT) != 0;
    if (frame->soac_checked_activation != NULL ||
        !PyFunction_Check(function) || entry->subject_owner == NULL ||
        entry->kind < Py_SOAC_INTERPRETER_ROOT ||
        entry->kind > Py_SOAC_INTERPRETER_CLASS_NAMESPACE ||
        (original_source && code->_co_soac_strict_source_id == 0) ||
        (!original_source && entry->kind != Py_SOAC_INTERPRETER_FUNCTION) ||
        (entry->kind == Py_SOAC_INTERPRETER_CLASS_NAMESPACE) !=
            (entry->namespace_state_out != NULL) ||
        (entry->namespace_state_out != NULL && *entry->namespace_state_out != NULL)) {
        return soac_interpreter_error("invalid checked interpreter frame entry");
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    /* The GIL-build-only ABI serializes this counter. Reserve before metadata
     * allocation can reenter; failed entries leave holes, never reused tokens.
     * The interpreter identity separates independently advancing counters. */
    if (interp->soac.interpreter_activation_counter == UINT64_MAX)
        return soac_interpreter_error("interpreter activation identity space is exhausted");
    /* Each selected context has its own pending-definition obligations. The
     * separate lazy frame token is only for allocation-free simple births;
     * reusing it here could make a later failure retire earlier definitions. */
    uint64_t activation_id = ++interp->soac.interpreter_activation_counter;
    SoacInterpreterSourceBinding *binding = entry->source_binding == NULL
        ? soac_function_source_binding(function) : soac_source_binding(entry->source_binding);
    if (!soac_source_binding_live(binding) || binding->globals != frame->f_globals ||
        (original_source && soac_source_descriptor(binding, code) == NULL))
        return soac_interpreter_error("interpreter activation has no matching source binding");
    if (PyType_Ready(&SoacInterpreterActivation_Type) < 0) {
        return -1;
    }
    _PySoacInterpreterActivationV1 *activation = PyObject_GC_New(
        _PySoacInterpreterActivationV1, &SoacInterpreterActivation_Type);
    if (activation == NULL) {
        return -1;
    }
    activation->state = NULL;
    activation->source_binding = Py_NewRef((PyObject *)binding);
    activation->thread = _PyThreadState_GET();
    activation->function = function;
    activation->code = code;
    activation->interpreter_id = interp->id;
    activation->activation_id = activation_id;
    activation->kind = entry->kind;
    activation->phase = entry->kind == Py_SOAC_INTERPRETER_FUNCTION
        ? Py_SOAC_INTERPRETER_RUNNING : Py_SOAC_INTERPRETER_BINDING;
    activation->source_authority = entry->kind == Py_SOAC_INTERPRETER_FUNCTION && original_source;
    activation->return_attempted = 0;
    activation->definition_begin = activation->definition_end = -1;
    activation->namespace_state_out = entry->namespace_state_out;
    activation->incoming_call = entry->incoming_call == NULL
        ? (_PySoacInterpreterConsumedCallV1){0} : *entry->incoming_call;
    activation->incoming_call.committed = 0;
    frame->soac_checked_activation = (PyObject *)activation;  /* MOVE sole edge. */
    PyObject_GC_Track(activation);
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, NULL, activation->phase, NULL, &view) < 0) {
        return -1;
    }
    PyObject *state = NULL;
    int status = callbacks->enter(
        entry->kind, entry->subject_owner, &view, entry->parent, &state);
    view.self = NULL;
    if (soac_interpreter_status(status, "interpreter entry callback failed") < 0 ||
        state == NULL) {
        soac_interpreter_error("interpreter entry callback supplied no state");
        soac_interpreter_xdecref_preserving_error(state);
        return -1;
    }
    activation->state = state;  /* MOVE one metadata result, never value owners. */
    /* The callback authenticated either exact original code or the documented
     * permitted ordinary replacement. The latter may already be frozen:
     * strict_id alone is neither refusal nor original-source authority. */
    activation->source_authority = original_source;
    frame->soac_source_authority = original_source;
    return 0;
}

/* CREATE precedes SET_FUNCTION_ATTRIBUTE. A watcher can call the fresh
 * function before closure installation. Validate the captured frame code,
 * not a later func_code replacement, before native COPY_FREE_VARS can read it.
 * This is a representation check, not a second cell/value ownership scheme. */
static int
soac_interpreter_closure_complete(_PyInterpreterFrame *frame)
{
    PyFunctionObject *function = (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    PyObject *closure = function->func_closure;
    Py_ssize_t count = _PyFrame_GetCode(frame)->co_nfreevars;
    if ((count != 0 && closure == NULL) ||
        (closure != NULL &&
         (!PyTuple_CheckExact(closure) || PyTuple_GET_SIZE(closure) != count))) {
        return soac_interpreter_error("interpreter function construction is incomplete");
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        if (!PyCell_Check(PyTuple_GET_ITEM(closure, i))) {
            return soac_interpreter_error("interpreter function closure has an invalid cell");
        }
    }
    return 0;
}

int
_PySOAC_CheckedFrameBound(_PyInterpreterFrame *frame)
{
    PyObject *object = frame->soac_checked_activation;
    if (!_PySOAC_InterpreterIsActivation(object)) {
        return object == NULL ? 0
            : soac_interpreter_error("interpreter binding has unknown frame metadata");
    }
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)object;
    if ((activation->phase != Py_SOAC_INTERPRETER_BINDING &&
         activation->phase != Py_SOAC_INTERPRETER_RUNNING) ||
        !soac_interpreter_activation_matches(activation, frame)) {
        return soac_interpreter_error("interpreter binding activation changed");
    }
    if (soac_interpreter_closure_complete(frame) < 0) {
        return -1;
    }
    if (activation->phase == Py_SOAC_INTERPRETER_BINDING)
        activation->phase = Py_SOAC_INTERPRETER_BOUND;
    return 0;
}

int
_PySOAC_CheckedFrameExecution(_PyInterpreterFrame *frame)
{
    PyObject *object = frame->soac_checked_activation;
    if (_PySOAC_InterpreterIsActivation(object)) {
        _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)object;
        if ((activation->phase != Py_SOAC_INTERPRETER_BOUND &&
             activation->phase != Py_SOAC_INTERPRETER_RUNNING) ||
            !soac_interpreter_activation_matches(activation, frame) ||
            soac_interpreter_callbacks() == NULL) {
            return soac_interpreter_error("interpreter source activation is unavailable");
        }
        /* Native suspended frames may legitimately resume on another thread
         * in the same interpreter. This is an active view hint, not a permit.
         */
        activation->thread = _PyThreadState_GET();
        uint32_t previous_phase = activation->phase;
        activation->phase = Py_SOAC_INTERPRETER_RUNNING;
        if (activation->source_authority && activation->kind != Py_SOAC_INTERPRETER_FUNCTION) {
            /* Native generator.throw() arrives with its actual raised
             * exception already installed. The entry evidence callback
             * must not mistake that exception for its own failure. */
            PyObject *pending = PyErr_GetRaisedException();
            PySoacInterpreterFrameViewV1 view;
            const _Py_CODEUNIT *instruction = frame->instr_ptr;
            int status = _PySOAC_InterpreterView(
                frame, instruction, activation->phase, activation->state, &view);
            if (status == 0) {
                status = _PyInterpreterState_GET()->soac.interpreter_callbacks.started(
                    activation->state, &view);
                view.self = NULL;
            }
            status = soac_interpreter_status(status, "interpreter VM entry was refused");
            if (status < 0) {
                activation->phase = previous_phase;
                if (pending != NULL) {
                    /* Refusal still stops this entry, but its secondary
                     * diagnostic may not replace the incoming exception. */
                    PyErr_WriteUnraisable(PyStackRef_AsPyObjectBorrow(frame->f_funcobj));
                }
            }
            if (pending != NULL) {
                PyErr_SetRaisedException(pending);
            }
            if (status < 0) {
                return -1;
            }
        }
        return 0;
    }
    if (frame->soac_namespace) {
        return soac_interpreter_error(
            "strict namespace execution requires its authenticated construction entry");
    }
    if (object != NULL) {
        return soac_interpreter_error("interpreter frame has unknown metadata");
    }
    if (frame->soac_dataclass_invocation != NULL ||
        (frame->previous != NULL && frame->previous->soac_dataclass_invocation != NULL)) {
        return _PySOAC_DataclassEnterFrame(frame);
    }
    return 0;
}

/* An external evaluator cannot consume an already-open construction context
 * without its native completion/cleanup. Ordinary function frames may use
 * PEP 523; they acquire source authority only at actual definition operations.
 * Tracing and monitoring continue to use ordinary CPython machinery. */
int
_PySOAC_InterpreterCheckEvalHook(_PyInterpreterFrame *frame)
{
    if (!frame->soac_namespace && frame->soac_checked_activation == NULL &&
        frame->soac_dataclass_invocation == NULL) {
        return 0;
    }
    soac_interpreter_error("external frame evaluators are unsupported for interpreter-owned code");
    if (frame->owner == FRAME_OWNED_BY_THREAD ||
        frame->owner == FRAME_OWNED_BY_GENERATOR) {
        assert(_PyThreadState_GET()->current_frame != frame);
        _PySOAC_CheckedFrameClear(frame, Py_SOAC_INTERPRETER_CHECK_FAILED);
        /* Includes native generator exception-state restoration and sets its
         * return-kind last. No callback-capable work may follow this call. */
        _PyEval_FrameClearAndPop(_PyThreadState_GET(), frame);
    }
    /* A public raw PyEval attempt with a frame-object-owned frame did not
     * transfer its locals/code owners. Refuse without consuming that object. */
    return -1;
}

static int soac_interpreter_authorize_definition(_PyInterpreterFrame *frame);

static int
soac_interpreter_ensure_context(_PyInterpreterFrame *frame)
{
    int authorized = soac_interpreter_authorize_definition(frame);
    if (authorized <= 0) return authorized;
    if (_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) return 1;
    PyFunctionObject *function = (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    _PySoacInterpreterEntryV1 entry = {
        .kind = Py_SOAC_INTERPRETER_FUNCTION, .construction = 1,
        .subject_owner = function->func_soac_strict_owner,
    };
    return _PySOAC_InterpreterInitFrame(frame, &entry) < 0 ? -1 : 1;
}

static uint64_t
soac_frame_birth_identity(_PyInterpreterFrame *frame)
{
    if (frame->soac_invocation_id == 0) {
        PyInterpreterState *interp = _PyInterpreterState_GET();
        if (interp->soac.interpreter_activation_counter == UINT64_MAX) {
            soac_interpreter_error("interpreter activation identity space is exhausted");
            return 0;
        }
        frame->soac_invocation_id = ++interp->soac.interpreter_activation_counter;
    }
    return frame->soac_invocation_id;
}

static int
soac_source_parent_live(_PyInterpreterFrame *frame)
{
    if (_PyInterpreterState_GET()->soac.interpreter_closed)
        return soac_interpreter_error("interpreter construction is closed");
    PyObject *object = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (!PyFunction_Check(object))
        return soac_interpreter_error("source parent has no actual native function");
    PyFunctionObject *function = (PyFunctionObject *)object;
    if (function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_TERMINAL)
        return soac_interpreter_error("source parent function became terminal");
    if (function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        if (function->func_soac_interpreter_descriptor != NULL) {
            if (function->func_soac_interpreter_flags & SOAC_FUNCTION_TERMINAL)
                return soac_interpreter_error("shared source parent became terminal");
        }
        else {
            SoacInterpreterFunctionGuard *guard =
                (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
            if (guard == NULL || guard->terminal || guard->execution == NULL || guard->execution->terminal)
                return soac_interpreter_error("dynamic source parent became terminal");
        }
        if (function->func_soac_metadata != NULL)
            return soac_interpreter_error("source parent acquired incompatible metadata");
    }
    if (_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
        if (activation->phase != Py_SOAC_INTERPRETER_RUNNING || !activation->source_authority ||
            activation->state == NULL || !soac_interpreter_activation_matches(activation, frame))
            return soac_interpreter_error("source parent has no active native invocation");
        if (function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_NONE &&
            activation->kind != Py_SOAC_INTERPRETER_ROOT)
            return soac_interpreter_error("source parent has no actual birth owner");
    }
    else if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        return soac_interpreter_error("source parent has no authenticated native entry");
    }
    return 0;
}

/* Authenticate when a real definition operation needs authority, including
 * after callbacks or a suspension. Frame/source flags only route this work;
 * copied code and a previously live binding cannot authorize a new birth. */
static int
soac_interpreter_authorize_definition(_PyInterpreterFrame *frame)
{
    if (frame == NULL) return 0;
    PyObject *object = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (!PyFunction_Check(object))
        return soac_interpreter_error("definition has no actual native function");
    PyFunctionObject *function = (PyFunctionObject *)object;
    if (_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
        if (!soac_source_binding_live((SoacInterpreterSourceBinding *)activation->source_binding))
            return soac_interpreter_error("definition source binding is unavailable");
        if (activation->kind == Py_SOAC_INTERPRETER_FUNCTION &&
            soac_interpreter_authorize(frame) < 0) return -1;
    }
    else if ((_PyFrame_GetCode(frame)->co_flags & CO_FUTURE_STRICT) ||
             function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        if (soac_interpreter_authorize(frame) < 0) return -1;
    }
    else {
        return 0;
    }
    if (!frame->soac_source_authority) return 0;
    return soac_source_parent_live(frame) < 0 ? -1 : 1;
}

/* No allocation, Python callback or temporary construction activation. The
 * actual native MAKE_FUNCTION supplies the newly allocated function; code
 * correspondence only routes this already authenticated birth. */
static int
soac_shared_function_birth(_PyInterpreterFrame *parent, PyFunctionObject *function)
{
    int authorized = soac_interpreter_authorize_definition(parent);
    if (authorized <= 0) return authorized;
    SoacInterpreterSourceBinding *binding = soac_frame_source_binding(parent);
    if (!soac_source_binding_live(binding))
        return soac_interpreter_error("function birth has no live source binding");
    const PySoacInterpreterDescriptorV1 *descriptor =
        soac_source_descriptor(binding, (PyCodeObject *)function->func_code);
    const PySoacInterpreterDescriptorV1 *parent_descriptor =
        soac_source_descriptor(binding, _PyFrame_GetCode(parent));
    if (descriptor == NULL || parent_descriptor == NULL ||
        descriptor->parent_ordinal != parent_descriptor->ordinal ||
        parent->f_globals != binding->globals || function->func_globals != binding->globals)
        return soac_interpreter_error("function birth differs from its prepared source parent");
    if (descriptor->requirements != 0) return 0;
    if (descriptor->scope_kind == Py_SOAC_SCOPE_MODULE || descriptor->scope_kind == Py_SOAC_SCOPE_CLASS ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_NONE ||
        function->func_soac_strict_owner != NULL || function->func_soac_interpreter_guard != NULL ||
        function->func_soac_interpreter_descriptor != NULL || function->func_soac_strict_id != 0 ||
        function->func_soac_metadata != NULL || function->vectorcall != _PyFunction_Vectorcall)
        return soac_interpreter_error("shared function birth has an invalid native shape");
    uint64_t birth = soac_frame_birth_identity(parent);
    if (birth == 0) return -1;
    function->func_soac_interpreter_descriptor = descriptor;
    function->func_soac_interpreter_birth_id = birth;
    function->func_soac_interpreter_flags = 0;
    function->func_soac_strict_owner = Py_NewRef((PyObject *)binding);
    function->func_soac_strict_owner_state = FUNC_SOAC_OWNER_INTERPRETER_ATTACHED;
    return 1;
}

static int
soac_shared_function_parent(_PyInterpreterFrame *parent, PyFunctionObject *function)
{
    if (soac_interpreter_authorize_definition(parent) <= 0)
        return soac_interpreter_error("shared function has no authenticated definition parent");
    SoacInterpreterSourceBinding *binding = soac_function_source_binding(function);
    const PySoacInterpreterDescriptorV1 *descriptor = function->func_soac_interpreter_descriptor;
    if (parent == NULL || !parent->soac_source_authority ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED ||
        !soac_source_binding_live(binding) || binding != soac_frame_source_binding(parent) ||
        descriptor == NULL || descriptor->requirements != 0 ||
        (function->func_soac_interpreter_flags & SOAC_FUNCTION_TERMINAL) ||
        function->func_soac_strict_id != 0 || function->func_soac_metadata != NULL ||
        parent->soac_invocation_id == 0 ||
        parent->soac_invocation_id != function->func_soac_interpreter_birth_id ||
        function->func_globals != binding->globals || parent->f_globals != binding->globals)
        return soac_interpreter_error("shared function has no actual birth parent");
    const PySoacInterpreterDescriptorV1 *parent_descriptor =
        soac_source_descriptor(binding, _PyFrame_GetCode(parent));
    if (parent_descriptor == NULL || descriptor->parent_ordinal != parent_descriptor->ordinal)
        return soac_interpreter_error("shared function has another source parent");
    return 0;
}

int
_PySOAC_InterpreterBirth(_PyInterpreterFrame *parent,
                         const _Py_CODEUNIT *this_instr, PyFunctionObject *function)
{
    int shared = soac_shared_function_birth(parent, function);
    if (shared != 0) return shared < 0 ? -1 : 0;
    int active = soac_interpreter_ensure_context(parent);
    if (active <= 0) return active;
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)parent->soac_checked_activation;
    if (!activation->source_authority) {
        return 0;  /* Authenticated ordinary replacement: no source child grant. */
    }
    const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL || activation->phase != Py_SOAC_INTERPRETER_RUNNING ||
        !soac_interpreter_activation_matches(activation, parent)) {
        return soac_interpreter_error("function birth has no active interpreter parent");
    }
    PyObject *code = function->func_code;
    PyObject *globals = function->func_globals;
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(parent, this_instr, activation->phase,
                                activation->state, &view) < 0) {
        return -1;
    }
    PyObject *owner = NULL;
    if (_PySOAC_InterpreterGuardCreate(function) < 0) { view.self = NULL; return -1; }
    SoacInterpreterFunctionGuard *guard =
        (SoacInterpreterFunctionGuard *)function->func_soac_interpreter_guard;
    guard->binding = (SoacInterpreterSourceBinding *)activation->source_binding;
    guard->descriptor = soac_source_descriptor(guard->binding, (PyCodeObject *)code);
    guard->birth_activation_id = activation->activation_id;
    if (guard->descriptor == NULL) {
        view.self = NULL;
        return soac_interpreter_error("dynamic function birth has no source descriptor");
    }
    int status = callbacks->birth(&view, (PyObject *)function, &owner);
    view.self = NULL;
    if (soac_interpreter_status(status, "interpreter function birth failed") < 0 ||
        owner == NULL || function->func_code != code ||
        function->func_globals != globals ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_NONE ||
        function->func_soac_strict_owner != NULL || function->func_soac_strict_id != 0 ||
        function->vectorcall != _PyFunction_Vectorcall) {
        soac_interpreter_error("interpreter birth callback changed the uncommitted function");
        soac_interpreter_xdecref_preserving_error(owner);
        return -1;
    }
    /* No callback after this MOVE and before the ordinary CREATE watcher.
     * Stock vectorcall is intentional: common native entry validates source
     * ownership while generic VM CALL still consumes its input tokens.
     */
    function->func_soac_strict_owner = owner;
    function->func_soac_strict_owner_state = FUNC_SOAC_OWNER_INTERPRETER_ATTACHED;
    return 0;
}

int
_PySOAC_InterpreterParentView(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction,
    PySoacInterpreterFrameViewV1 *view)
{
    int active = soac_interpreter_ensure_context(frame);
    if (active <= 0) return active;
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (!activation->source_authority) {
        return 0;
    }
    if (activation->phase != Py_SOAC_INTERPRETER_RUNNING ||
        !soac_interpreter_activation_matches(activation, frame) ||
        soac_interpreter_callbacks() == NULL) {
        return soac_interpreter_error("interpreter operation has no live original parent");
    }
    if (_PySOAC_InterpreterView(frame, instruction, activation->phase,
                                activation->state, view) < 0) {
        return -1;
    }
    return 1;
}

int
_PySOAC_InterpreterFunctionAttribute(
    _PyInterpreterFrame *parent, const _Py_CODEUNIT *instruction,
    PyFunctionObject *function, uint32_t attribute_flag, PyObject *installed)
{
    if (function != NULL && PyFunction_Check(function) &&
        function->func_soac_interpreter_descriptor != NULL) {
        if (soac_shared_function_parent(parent, function) < 0) return -1;
        PyObject *actual;
        switch (attribute_flag) {
            case MAKE_FUNCTION_DEFAULTS: actual = function->func_defaults; break;
            case MAKE_FUNCTION_KWDEFAULTS: actual = function->func_kwdefaults; break;
            case MAKE_FUNCTION_CLOSURE: actual = function->func_closure; break;
            default: return soac_interpreter_error("shared function requires a dynamic association");
        }
        return installed != NULL && installed == actual ? 0
            : soac_interpreter_error("shared function attribute differs from its actual field");
    }
    PySoacInterpreterFrameViewV1 view;
    int selected = _PySOAC_InterpreterParentView(parent, instruction, &view);
    if (selected <= 0) {
        return selected;
    }
    if (function == NULL || !PyFunction_Check(function) ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        view.self = NULL;
        return soac_interpreter_error("function attribute has no actual native birth owner");
    }
    int status = _PyInterpreterState_GET()->soac.interpreter_callbacks.function_attribute(
        &view, (PyObject *)function, attribute_flag, installed);
    view.self = NULL;
    /* The callback's successful association is allocation/callback-free.
     * Do not follow the installed operand after a callback/error can replace
     * the function field: no duplicate value pin is introduced here. */
    return soac_interpreter_status(status, "interpreter function attribute association failed");
}

int
_PySOAC_InterpreterCompleteDefinition(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    PyObject *value)
{
    int authorized = soac_interpreter_authorize_definition(frame);
    if (authorized <= 0) return authorized;
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return soac_interpreter_error("definition completion has no active construction");
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (!activation->source_authority) {
        return 0;
    }
    const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL || activation->phase != Py_SOAC_INTERPRETER_RUNNING ||
        !soac_interpreter_activation_matches(activation, frame) || value == NULL) {
        return soac_interpreter_error("definition completion has no active interpreter frame/value");
    }
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, this_instr, activation->phase,
                                activation->state, &view) < 0) {
        return -1;
    }
    if (activation->definition_begin < 0 || activation->definition_end != view.instruction_units) {
        view.self = NULL;
        return soac_interpreter_error("definition completion crossed its emitted region");
    }
    int status = callbacks->definition_complete(&view, value);
    view.self = NULL;
    status = soac_interpreter_status(status, "interpreter definition completion failed");
    if (status == 0) {
        activation->definition_begin = activation->definition_end = -1;
        if (activation->kind == Py_SOAC_INTERPRETER_FUNCTION && activation->incoming_call.caller == NULL)
            _PySOAC_CheckedFrameClear(frame, Py_SOAC_INTERPRETER_RETURNED);
    }
    return status;
}

int
_PySOAC_InterpreterBeginDefinition(_PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction)
{
    int active = soac_interpreter_ensure_context(frame);
    if (active <= 0) return active;
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    Py_ssize_t units, ordinal;
    if (soac_interpreter_site(frame, instruction, &units, &ordinal) < 0) return -1;
    int end = _PyFrame_GetCode(frame)->_co_soac_sites[units].definition_end;
    if (activation->definition_begin != -1 || end < units)
        return soac_interpreter_error("definition begin has no exact inactive emitted region");
    activation->definition_begin = units;
    activation->definition_end = end;
    return 0;
}

int
_PySOAC_InterpreterEndFunctionBirth(_PyInterpreterFrame *frame,
    const _Py_CODEUNIT *instruction, PyObject *function)
{
    int authorized = soac_interpreter_authorize_definition(frame);
    if (authorized <= 0) return authorized;
    if (function != NULL && PyFunction_Check(function) &&
        ((PyFunctionObject *)function)->func_soac_interpreter_descriptor != NULL) {
        PyFunctionObject *actual = (PyFunctionObject *)function;
        if (soac_shared_function_parent(frame, actual) < 0) return -1;
        actual->func_soac_interpreter_flags |= SOAC_FUNCTION_READY;
        return 0;
    }
    PySoacInterpreterFrameViewV1 view;
    int active = _PySOAC_InterpreterParentView(frame, instruction, &view);
    if (active <= 0) return active;
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    uint32_t complete = activation->kind == Py_SOAC_INTERPRETER_FUNCTION &&
        activation->definition_begin == -1 && activation->incoming_call.caller == NULL;
    int status = soac_interpreter_callbacks()->definition_end(&view, function, complete, NULL);
    view.self = NULL;
    status = soac_interpreter_status(status, "interpreter function birth completion failed");
    if (status == 0 && complete && activation->incoming_call.caller == NULL)
        _PySOAC_CheckedFrameClear(frame, Py_SOAC_INTERPRETER_RETURNED);
    return status;
}

void
_PySOAC_InterpreterAbortDefinition(_PyInterpreterFrame *frame,
    const _Py_CODEUNIT *instruction, int handler)
{
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) return;
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (activation->definition_begin == -1 && activation->kind != Py_SOAC_INTERPRETER_FUNCTION) return;
    if (handler >= 0 && activation->definition_begin >= 0 &&
        handler > activation->definition_begin && handler <= activation->definition_end) return;
    PyObject *primary = PyErr_GetRaisedException();
    PySoacInterpreterFrameViewV1 view;
    int status = _PySOAC_InterpreterView(frame, instruction, activation->phase, activation->state, &view);
    if (status == 0) {
        /* Already-open contexts must retire even after new entry is closed. */
        const PySoacInterpreterCallbacksV4 *callbacks =
            &_PyInterpreterState_GET()->soac.interpreter_callbacks;
        status = callbacks->definition_abort(&view);
        view.self = NULL;
    }
    status = soac_interpreter_status(status, "interpreter definition abort failed");
    activation->definition_begin = activation->definition_end = -1;
    if (activation->kind == Py_SOAC_INTERPRETER_FUNCTION &&
        (activation->incoming_call.caller == NULL || handler < 0))
        _PySOAC_CheckedFrameClear(frame, status == 0
            ? Py_SOAC_INTERPRETER_RETURNED : Py_SOAC_INTERPRETER_BODY_FAILED);
    if (status < 0)
        PyErr_WriteUnraisable(PyStackRef_AsPyObjectBorrow(frame->f_funcobj));
    PyErr_SetRaisedException(primary);
}

int
_PySOAC_CheckedFrameReturn(_PyInterpreterFrame *frame,
                           const _Py_CODEUNIT *this_instr, PyObject *value)
{
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (activation->phase != Py_SOAC_INTERPRETER_RUNNING ||
        activation->return_attempted ||
        !soac_interpreter_activation_matches(activation, frame)) {
        return soac_interpreter_error("interpreter return activation is unavailable");
    }
    if (!activation->source_authority) {
        return 0;
    }
    if (activation->kind == Py_SOAC_INTERPRETER_FUNCTION) {
        if (activation->incoming_call.caller == NULL)
            return soac_interpreter_error("function returned with an unfinished definition event");
        PySoacInterpreterFrameViewV1 view;
        if (_PySOAC_InterpreterView(frame, this_instr, activation->phase,
                                   activation->state, &view) < 0) return -1;
        const _PySoacInterpreterConsumedCallV1 *incoming = &activation->incoming_call;
        PySoacInterpreterFrameViewV1 parent;
        if (!incoming->committed || incoming->instruction_units < 0 ||
            incoming->caller != frame->previous ||
            _PySOAC_InterpreterParentView(incoming->caller,
                _PyFrame_GetBytecode(incoming->caller) + incoming->instruction_units, &parent) != 1) {
            view.self = NULL;
            return soac_interpreter_error("generic completion lost its exact incoming native call");
        }
        int status = soac_interpreter_callbacks()->definition_end(&view, value, 2, &parent);
        parent.self = NULL;
        view.self = NULL;
        if (soac_interpreter_status(status, "generic definition context completion failed") < 0) return -1;
    }
    activation->phase = Py_SOAC_INTERPRETER_RETURNING;
    activation->return_attempted = 1;
    return 0;
}

int
_PySOAC_CheckedFrameFailed(_PyInterpreterFrame *frame,
                           const _Py_CODEUNIT *instruction)
{
    _PySOAC_InterpreterAbortDefinition(frame, instruction, -1);
    return 0;
}

int
_PySOAC_CheckedFrameReturnCommit(_PyInterpreterFrame *frame)
{
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (activation->phase != (activation->source_authority
            ? Py_SOAC_INTERPRETER_RETURNING : Py_SOAC_INTERPRETER_RUNNING) ||
        !soac_interpreter_activation_matches(activation, frame)) {
        return soac_interpreter_error("interpreter return completion changed");
    }
    /* PY_RETURN instrumentation has succeeded. Ordinary function/root state
     * stays in its frame until the native unlink/clear point, not earlier.
     */
    if (activation->kind == Py_SOAC_INTERPRETER_CLASS_NAMESPACE) {
        PyObject **destination = activation->namespace_state_out;
        if (destination == NULL || *destination != NULL) {
            return soac_interpreter_error("class namespace has no one-use output destination");
        }
        PyObject *state = activation->state;
        activation->phase = Py_SOAC_INTERPRETER_RETIRED;
        activation->state = NULL;
        activation->namespace_state_out = NULL;
        memset(&activation->incoming_call, 0, sizeof(activation->incoming_call));
        activation->source_authority = 0;
        activation->thread = NULL;
        activation->function = NULL;
        activation->code = NULL;
        _PyInterpreterState_GET()->soac.interpreter_callbacks.leave(
            state, Py_SOAC_INTERPRETER_NAMESPACE_TRANSFERRED,
            activation->interpreter_id, activation->activation_id);
        *destination = state;  /* MOVE, no duplicate Python metadata owner. */
    }
    else {
        activation->phase = Py_SOAC_INTERPRETER_RETIRED;
    }
    return 0;
}

/* Only the explicit loader API passes a ROOT entry. The temporary native
 * wrapper itself is never assigned a reusable owner or execution permission.
 * root_begin consumes the initialization attempt before any CREATE watcher. */
PyObject *
PySoac_EvalInterpreterModuleV2(PyObject *module, PyObject *root_code,
                               PyObject *source_binding)
{
    if (PyErr_Occurred()) {
        return NULL;
    }
    const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL) {
        return NULL;
    }
    SoacInterpreterSourceBinding *binding = soac_source_binding(source_binding);
    if (binding == NULL) return NULL;
    PyObject *module_owner = binding->publication;
    if (module == NULL || !PyModule_Check(module) ||
        root_code == NULL || !PyCode_Check(root_code) || module_owner == NULL ||
        !(((PyCodeObject *)root_code)->co_flags & CO_FUTURE_STRICT) ||
        ((PyCodeObject *)root_code)->_co_soac_strict_source_id == 0 ||
        binding->globals != PyModule_GetDict(module) ||
        soac_source_descriptor(binding, (PyCodeObject *)root_code) == NULL) {
        soac_interpreter_error("interpreter root needs an authenticated module and code");
        return NULL;
    }
    /* A supported clear may release binding->publication during callbacks.
     * Keep this metadata owner alive through root_end independently of that
     * mutable edge; no function, code, globals or execution values are pinned. */
    Py_INCREF(module_owner);
    int status = callbacks->root_begin(module_owner, module, root_code);
    if (soac_interpreter_status(status, "interpreter root initialization was refused") < 0) {
        soac_interpreter_xdecref_preserving_error(module_owner);
        return NULL;
    }
    PyObject *result = NULL;
    PyObject *globals = PyModule_GetDict(module);  /* Borrowed actual module dict. */
    PyObject *builtins = _PyDict_LoadBuiltinsFromGlobals(globals);
    if (builtins != NULL) {
        PyFrameConstructor desc = {
            .fc_globals = globals,
            .fc_builtins = builtins,
            .fc_name = ((PyCodeObject *)root_code)->co_name,
            .fc_qualname = ((PyCodeObject *)root_code)->co_name,
            .fc_code = root_code,
            .fc_defaults = NULL,
            .fc_kwdefaults = NULL,
            .fc_closure = NULL,
        };
        PyFunctionObject *function = _PyFunction_FromConstructor(&desc);
        _Py_DECREF_BUILTINS(builtins);
        if (function != NULL) {
            const _PySoacInterpreterEntryV1 entry = {
                .kind = Py_SOAC_INTERPRETER_ROOT,
                .subject_owner = module_owner,
                .source_binding = source_binding,
                .parent = NULL,
                .namespace_state_out = NULL,
            };
            result = _PySOAC_InterpreterEvalVector(
                _PyThreadState_GET(), function, globals, NULL, 0, NULL, &entry);
            soac_interpreter_xdecref_preserving_error((PyObject *)function);
        }
    }
    callbacks->root_end(module_owner, result != NULL);
    soac_interpreter_xdecref_preserving_error(module_owner);
    return result;
}


/* ---- Exact native CALL operand/completion join. ---- */
static PyObject *
soac_call_stack_value(_PyStackRef value)
{
    if (PyStackRef_IsTaggedInt(value)) {
        soac_interpreter_error("call operand is not a Python value");
        return NULL;
    }
    return PyStackRef_AsPyObjectBorrow(value);
}

int
PySoac_InterpreterCallOperandV1(
    const PySoacInterpreterCallViewV1 *view, uint32_t kind, Py_ssize_t index,
    PySoacInterpreterCallOperandV1 *out, size_t out_size)
{
    if (view == NULL || view->self != view || out == NULL ||
        out_size != sizeof(*out) || view->thread != _PyThreadState_GET() ||
        view->info == NULL || view->info->abi_version != Py_SOAC_INTERPRETER_CALL_ABI_V1 ||
        (view->info->phase != Py_SOAC_INTERPRETER_CALL_SELECT &&
         view->info->phase != Py_SOAC_INTERPRETER_CALL_PREPARE_TYPE) ||
        soac_interpreter_check_view(view->info->current.frame) < 0 || index < 0)
        return soac_interpreter_error("invalid or expired interpreter call view");
    const PySoacInterpreterCallInfoV1 *info = view->info;
    PyObject *value = NULL;
    switch (kind) {
        case Py_SOAC_INTERPRETER_OPERAND_CALLABLE:
            if (index != 0) goto invalid;
            value = view->backing == 0
                ? soac_call_stack_value(view->stack_inputs[0]) : view->callable;
            break;
        case Py_SOAC_INTERPRETER_OPERAND_POSITIONAL:
            if (index >= info->current.positional_count) goto invalid;
            if (info->current.form == Py_SOAC_INTERPRETER_CALL_EXPANDED) {
                value = PyTuple_GET_ITEM(view->expanded_args, index);
            }
            else if (view->backing == 0) {
                value = soac_call_stack_value(view->stack_inputs[
                    2 - info->current.channel + index]);
            }
            else value = view->argv[index];
            break;
        case Py_SOAC_INTERPRETER_OPERAND_KEYWORD_VALUE:
            if (info->current.form != Py_SOAC_INTERPRETER_CALL_VECTOR_KW ||
                index >= info->current.keyword_count) goto invalid;
            index += info->current.positional_count;
            value = view->backing == 0
                ? soac_call_stack_value(view->stack_inputs[2 - info->current.channel + index])
                : view->argv[index];
            break;
        case Py_SOAC_INTERPRETER_OPERAND_KEYWORD_NAMES:
            if (index != 0 || info->current.form != Py_SOAC_INTERPRETER_CALL_VECTOR_KW)
                goto invalid;
            value = view->names;
            break;
        case Py_SOAC_INTERPRETER_OPERAND_EXPANDED_ARGS:
            if (index != 0 || info->current.form != Py_SOAC_INTERPRETER_CALL_EXPANDED)
                goto invalid;
            value = view->expanded_args;
            break;
        case Py_SOAC_INTERPRETER_OPERAND_EXPANDED_KWARGS:
            if (index != 0 || info->current.form != Py_SOAC_INTERPRETER_CALL_EXPANDED)
                goto invalid;
            value = view->expanded_kwargs; /* NULL is native absence, not None. */
            break;
        case Py_SOAC_INTERPRETER_OPERAND_DECORATOR:
            if (info->phase != Py_SOAC_INTERPRETER_CALL_PREPARE_TYPE ||
                view->decorator_frame == NULL || index >= info->decorator_count ||
                view->decorator_prefix < info->decorator_count) goto invalid;
            value = soac_call_stack_value(_PyFrame_Stackbase(view->decorator_frame)[
                view->decorator_prefix - info->decorator_count + index]);
            break;
        default: goto invalid;
    }
    if (value == NULL && kind != Py_SOAC_INTERPRETER_OPERAND_EXPANDED_KWARGS)
        return soac_interpreter_error("required native call operand is absent or unboxed");
    *out = (PySoacInterpreterCallOperandV1) {
        .abi_version = Py_SOAC_INTERPRETER_CALL_ABI_V1, .value = value,
    };
    _PySOAC_DataclassCreationInfo(value, &out->creation_status, &out->creation_role,
        &out->creation_identity, &out->dataclass_invocation, &out->dataclass_owner);
    return 0;
invalid:
    return soac_interpreter_error("interpreter call operand kind/index is unavailable");
}

static void
soac_call_unpublish(_PySoacInterpreterCallV1 *call)
{
    call->view.self = NULL;
    call->current_frame.self = NULL;
    call->direct_frame.self = NULL;
}

/* Publish only the ONE pre-observed consumed edge. No ancestors, inferred
 * stack subtraction, resumed frame, or still-unconsumed C caller is accepted. */
static int
soac_call_direct(_PySoacInterpreterCallV1 *call)
{
    call->info.direct_caller = NULL;
    PyObject *object = call->caller->soac_checked_activation;
    if (!_PySOAC_InterpreterIsActivation(object)) return 0;
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)object;
    const _PySoacInterpreterConsumedCallV1 *edge = &activation->incoming_call;
    if (!edge->committed) return 0;
    _PyInterpreterFrame *parent = edge->caller;
    if (parent == NULL || call->caller->previous != parent ||
        _PyFrame_GetCode(call->caller)->co_flags &
            (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR) ||
        edge->prefix_depth < 0 ||
        parent->stackpointer != _PyFrame_Stackbase(parent) + edge->prefix_depth)
        return soac_interpreter_error("generic call lost its exact consumed incoming edge");
    int selected = _PySOAC_InterpreterParentView(parent,
        _PyFrame_GetBytecode(parent) + edge->instruction_units, &call->direct_frame);
    if (selected != 1) return soac_interpreter_error("generic caller is no longer authenticated");
    if (call->direct_frame.instruction_ordinal != edge->instruction_ordinal)
        return soac_interpreter_error("generic caller changed its native site");
    call->direct_site = (PySoacInterpreterCallSiteV1) {
        .form = edge->form, .channel = edge->channel,
        .instruction_argument = edge->instruction_argument,
        .positional_count = edge->positional_count, .keyword_count = edge->keyword_count,
        .frame = &call->direct_frame,
    };
    call->info.direct_caller = &call->direct_site;
    return 0;
}

static int
soac_call_decision(_PySoacInterpreterCallV1 *call)
{
    PySoacInterpreterCallDecisionV1 *out = &call->decision;
    if (out->abi_version != Py_SOAC_INTERPRETER_CALL_ABI_V1 ||
        out->kind > Py_SOAC_INTERPRETER_CALL_GENERIC_SCOPE)
        return soac_interpreter_error("invalid interpreter call decision ABI/kind");
    if (out->kind == Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT) {
        if (out->metadata == NULL || out->expected_function_owner != NULL ||
            out->verified_code != NULL || out->decorator_count != 0 ||
            out->decorator_source != 0 ||
            (out->dataclass_stage != Py_SOAC_DATACLASS_ROOT_FACTORY &&
             out->dataclass_stage != Py_SOAC_DATACLASS_ROOT_APPLY))
            goto invalid;
    }
    else if (out->kind == Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR) {
        if (out->metadata == NULL || out->expected_function_owner == NULL ||
            out->verified_code == NULL || out->dataclass_stage != 0 ||
            out->decorator_count != 0 || out->decorator_source != 0 ||
            call->info.current.positional_count != 1 || call->info.current.keyword_count != 0)
            goto invalid;
    }
    else {
        if (out->metadata != NULL || out->expected_function_owner != NULL ||
            out->verified_code != NULL || out->dataclass_stage != 0) goto invalid;
        if (out->kind == Py_SOAC_INTERPRETER_CALL_CLASS) {
            if (out->decorator_count < 0 ||
                (out->decorator_source != Py_SOAC_INTERPRETER_DECORATORS_CURRENT &&
                 out->decorator_source != Py_SOAC_INTERPRETER_DECORATORS_DIRECT_CALLER))
                goto invalid;
            if (out->decorator_source == Py_SOAC_INTERPRETER_DECORATORS_CURRENT) {
                if (out->decorator_count > call->prefix_depth) goto invalid;
            }
            else {
                if (call->info.direct_caller == NULL) goto invalid;
                _PySoacInterpreterActivationV1 *activation =
                    (_PySoacInterpreterActivationV1 *)call->caller->soac_checked_activation;
                if (out->decorator_count > activation->incoming_call.prefix_depth) goto invalid;
            }
        }
        else if (out->decorator_count != 0 || out->decorator_source != 0) goto invalid;
    }
    return 0;
invalid:
    return soac_interpreter_error("interpreter call decision has mismatched kind-specific fields");
}

int
_PySOAC_InterpreterSelectCall(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction,
    uint32_t form, uint32_t oparg, _PySoacInterpreterCallV1 *call)
{
    memset(call, 0, sizeof(*call));
    int had_context = frame->soac_checked_activation != NULL;
    int selected = _PySOAC_InterpreterParentView(frame, instruction, &call->current_frame);
    if (selected <= 0) return selected;
    call->caller = frame;
    call->instruction = instruction;
    if (!had_context) {
        call->context_activation_id = call->current_frame.activation_id;
    }
    call->selected = 1;
    _PyStackRef *base = _PyFrame_Stackbase(frame);
    _PyStackRef *top = frame->stackpointer;
    Py_ssize_t size = form == Py_SOAC_INTERPRETER_CALL_EXPANDED ? 4 :
        (Py_ssize_t)oparg + (form == Py_SOAC_INTERPRETER_CALL_VECTOR_KW ? 3 : 2);
    if (form < Py_SOAC_INTERPRETER_CALL_VECTOR ||
        form > Py_SOAC_INTERPRETER_CALL_EXPANDED || top == NULL ||
        top < base || top > base + _PyFrame_GetCode(frame)->co_stacksize ||
        size > top - base || (form == Py_SOAC_INTERPRETER_CALL_EXPANDED && oparg != 0)) {
        soac_call_unpublish(call);
        return soac_interpreter_error("interpreter CALL operand stack is not published");
    }
    call->prefix_depth = top - base - size;
    call->input_depth = top - base;
    call->view = (PySoacInterpreterCallViewV1) {
        .self = &call->view, .thread = _PyThreadState_GET(), .info = &call->info,
        .input_frame = frame, .stack_inputs = top - size,
    };
    uint32_t channel = !PyStackRef_IsNull(call->view.stack_inputs[1]);
    Py_ssize_t positional = (Py_ssize_t)oparg + channel, keywords = 0;
    if (form == Py_SOAC_INTERPRETER_CALL_VECTOR_KW) {
        call->view.names = soac_call_stack_value(top[-1]);
        if (call->view.names == NULL || !PyTuple_CheckExact(call->view.names)) goto invalid;
        keywords = PyTuple_GET_SIZE(call->view.names);
        if (keywords > positional) goto invalid;
        positional -= keywords;
    }
    if (form == Py_SOAC_INTERPRETER_CALL_EXPANDED) {
        if (channel != 0) goto invalid;
        call->view.expanded_args = soac_call_stack_value(top[-2]);
        call->view.expanded_kwargs = soac_call_stack_value(top[-1]);
        if (call->view.expanded_args == NULL ||
            !PyTuple_CheckExact(call->view.expanded_args) ||
            (call->view.expanded_kwargs != NULL && !PyDict_CheckExact(call->view.expanded_kwargs)))
            goto invalid;
        positional = PyTuple_GET_SIZE(call->view.expanded_args);
        keywords = call->view.expanded_kwargs == NULL ? 0 : PyDict_GET_SIZE(call->view.expanded_kwargs);
    }
    call->info = (PySoacInterpreterCallInfoV1) {
        .abi_version = Py_SOAC_INTERPRETER_CALL_ABI_V1,
        .phase = Py_SOAC_INTERPRETER_CALL_SELECT,
        .current = {
            .form = form, .channel = channel, .instruction_argument = oparg,
            .positional_count = positional, .keyword_count = keywords,
            .frame = &call->current_frame,
        },
    };
    if (soac_call_direct(call) < 0) goto error;
    call->decision.abi_version = Py_SOAC_INTERPRETER_CALL_ABI_V1;
    int status = soac_interpreter_callbacks()->call(
        call->current_frame.call_state, &call->info, &call->view,
        &call->decision, sizeof(call->decision));
    if (soac_interpreter_status(status, "interpreter call selection failed") < 0 ||
        soac_call_decision(call) < 0) goto error;
    if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_ORDINARY) {
        /* Selection needed a view, but actual ordinary execution needs no
         * construction state. Retire only the context born at this CALL. */
        _PySOAC_InterpreterCallClear(call);
        return 0;
    }
    if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT) {
        PyObject *callable = soac_call_stack_value(call->view.stack_inputs[0]);
        if (_PySOAC_DataclassBeginRoot(call->decision.metadata,
                call->decision.dataclass_stage, callable) < 0) goto error;
        call->began = 1;
    }
    call->incoming = (_PySoacInterpreterConsumedCallV1) {
        .caller = frame, .instruction_units = call->current_frame.instruction_units,
        .instruction_ordinal = call->current_frame.instruction_ordinal,
        .prefix_depth = call->prefix_depth, .form = form, .channel = channel,
        .instruction_argument = oparg, .positional_count = positional,
        .keyword_count = keywords,
    };
    soac_call_unpublish(call); /* BEFORE the first native input transfer. */
    return 0;
invalid:
    soac_interpreter_error("native CALL input representation is inconsistent");
error:
    _PySOAC_InterpreterCallClear(call);
    return -1;
}

static void
soac_call_retire_context(_PyInterpreterFrame *caller, uint64_t activation_id)
{
    if (activation_id == 0 || caller == NULL ||
        !_PySOAC_InterpreterIsActivation(caller->soac_checked_activation)) return;
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)caller->soac_checked_activation;
    if (activation->activation_id == activation_id &&
        activation->kind == Py_SOAC_INTERPRETER_FUNCTION &&
        activation->definition_begin == -1 && activation->incoming_call.caller == NULL) {
        _PySOAC_CheckedFrameClear(caller, PyErr_Occurred()
            ? Py_SOAC_INTERPRETER_BODY_FAILED : Py_SOAC_INTERPRETER_RETURNED);
    }
}

void
_PySOAC_InterpreterCallClear(_PySoacInterpreterCallV1 *call)
{
    if (call == NULL) return;
    soac_call_unpublish(call);
    PyObject *metadata = call->decision.metadata;
    call->decision.metadata = NULL;
    call->decision.expected_function_owner = NULL;
    call->decision.verified_code = NULL;
    call->decision.kind = Py_SOAC_INTERPRETER_CALL_ORDINARY;
    call->selected = 0;
    uint64_t activation_id = call->context_activation_id;
    call->context_activation_id = 0;
    soac_call_retire_context(call->caller, activation_id);
    soac_interpreter_xdecref_preserving_error(metadata);
}

/* Caller is the exact waiting native frame captured by this operation. */
static int
soac_selected_finished(
    _PyInterpreterFrame *caller, const _Py_CODEUNIT *instruction,
    uint32_t kind, PyObject *metadata, PyObject *dataclass_owner,
    uint32_t stage, PyObject *result)
{
    PyObject *primary = PyErr_GetRaisedException();
    int status = -1;
    PySoacInterpreterFrameViewV1 view = {0};
    const PySoacInterpreterCallbacksV4 *callbacks = soac_interpreter_callbacks();
    if (callbacks != NULL && caller == _PyThreadState_GET()->current_frame &&
        ((kind == Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR &&
          metadata != NULL && dataclass_owner == NULL && stage == 0) ||
         (kind == Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT &&
          metadata != NULL && dataclass_owner != NULL &&
           _PySOAC_DataclassRootOwner(metadata) == dataclass_owner &&
          (stage == Py_SOAC_DATACLASS_ROOT_FACTORY || stage == Py_SOAC_DATACLASS_ROOT_APPLY))) &&
        _PySOAC_InterpreterParentView(caller, instruction, &view) == 1) {
        status = callbacks->selected_call_finished(
            view.call_state, &view, kind, metadata, dataclass_owner, stage, result);
        view.self = NULL;
    }
    if (soac_interpreter_status(status, "selected native call completion failed") < 0) {
        if (primary != NULL) PyErr_WriteUnraisable(metadata);
        status = -1;
    }
    if (primary != NULL) PyErr_SetRaisedException(primary);
    return status;
}

static int
soac_finish_dataclass(
    _PyInterpreterFrame *caller, const _Py_CODEUNIT *instruction,
    PyObject *invocation, uint32_t stage, PyObject *result)
{
    int native_status = _PySOAC_DataclassFinishRoot(invocation, stage, result);
    PyObject *owner = _PySOAC_DataclassRootOwner(invocation);
    int status = soac_selected_finished(caller, instruction,
        Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT, invocation, owner, stage,
        native_status == 0 ? result : NULL);
    if (native_status < 0 || status < 0) {
        /* A post-clear policy failure is still an invocation failure. Publish
         * terminal admission/execution before the caller closes its result. */
        PyObject *primary = PyErr_GetRaisedException();
        if (PySoac_FailDataclassInvocation(invocation) < 0)
            PyErr_WriteUnraisable(invocation);
        PyErr_SetRaisedException(primary);
    }
    _PySOAC_DataclassReleaseRoot(invocation);
    return native_status == 0 ? status : -1;
}

void
_PySOAC_InterpreterCallBindingFailed(_PySoacInterpreterCallV1 *call)
{
    if (call == NULL || !call->began || call->decision.metadata == NULL)
        return;
    /* Only the existing invocation's execution/admission state is retired here.
     * The post-clear callback still receives its same metadata owner later. */
    PyObject *primary = PyErr_GetRaisedException();
    if (PySoac_FailDataclassInvocation(call->decision.metadata) < 0)
        PyErr_WriteUnraisable(call->decision.metadata);
    PyErr_SetRaisedException(primary);
}

int
_PySOAC_InterpreterCallFailed(_PySoacInterpreterCallV1 *call)
{
    if (call == NULL) return -1;
    if (call->began && call->decision.metadata != NULL) {
        (void)soac_finish_dataclass(call->caller, call->instruction,
            call->decision.metadata, call->decision.dataclass_stage, NULL);
    }
    _PySOAC_InterpreterCallClear(call);
    return -1;
}

int
_PySOAC_InterpreterCallFinished(_PySoacInterpreterCallV1 *call, PyObject **result)
{
    if (call == NULL) return 0;
    int status = 0;
    if (call->began && call->decision.metadata != NULL) {
        status = soac_finish_dataclass(call->caller, call->instruction,
            call->decision.metadata, call->decision.dataclass_stage, *result);
    }
    _PySOAC_InterpreterCallClear(call);
    if (status < 0 && *result != NULL) {
        PyObject *error = PyErr_GetRaisedException();
        Py_CLEAR(*result); /* Actual C result owner, once. */
        PyErr_SetRaisedException(error);
    }
    return status;
}

void
_PySOAC_InterpreterTakeDataclassRoot(
    _PyInterpreterFrame *frame, _PySoacInterpreterRootFinishV1 *finish)
{
    memset(finish, 0, sizeof(*finish));
    _PySOAC_DataclassTakeRoot(frame, &finish->invocation, &finish->stage,
        &finish->caller, &finish->instruction, &finish->deferred,
        &finish->context_activation_id);
    if (finish->invocation != NULL && PyErr_Occurred()) {
        /* Mark failed before any callee value releases. The exact same
         * metadata owner remains supported until the finish callback. */
        (void)PySoac_FailDataclassInvocation(finish->invocation);
    }
}

int
_PySOAC_InterpreterFinishDataclassRoot(
    _PySoacInterpreterRootFinishV1 *finish, PyObject *result)
{
    if (finish->invocation == NULL) return 0;
    PyObject *invocation = finish->invocation;
    finish->invocation = NULL;
    if (finish->deferred != NULL) {
        /* Same actual C dispatch will finish AFTER its ordinary caller
         * containers/argv have retired; no extra frame or result primary. */
        assert(finish->deferred->decision.metadata == NULL);
        assert(finish->context_activation_id == 0);
        finish->deferred->decision.metadata = invocation;
        finish->deferred->root_detached = 1;
        finish->deferred = NULL;
        return 0;
    }
    int status = soac_finish_dataclass(
        finish->caller, finish->instruction, invocation, finish->stage, result);
    soac_call_retire_context(finish->caller, finish->context_activation_id);
    finish->context_activation_id = 0;
    finish->caller = NULL;
    finish->instruction = NULL;
    soac_interpreter_xdecref_preserving_error(invocation);
    return status;
}

int
_PySOAC_InterpreterCallCommit(_PySoacInterpreterCallV1 *call, _PyInterpreterFrame *child)
{
    if (call == NULL || !call->selected) return 0;
    if (call->caller->stackpointer != _PyFrame_Stackbase(call->caller) + call->prefix_depth) {
        soac_interpreter_error("native CALL did not commit its consumed input prefix");
        goto fail;
    }
    if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_GENERIC_SCOPE) {
        if (!_PySOAC_InterpreterIsActivation(child->soac_checked_activation) ||
            child->previous != call->caller ||
            _PyFrame_GetCode(child)->co_flags &
                (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
            return soac_interpreter_error("generic helper did not enter its exact synchronous frame");
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)child->soac_checked_activation;
        if (!activation->source_authority ||
            activation->incoming_call.caller != call->caller ||
            activation->incoming_call.prefix_depth != call->prefix_depth)
            return soac_interpreter_error("generic helper lost its pre-binding incoming observation");
        activation->incoming_call.committed = 1;
    }
    if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT) {
        if (_PySOAC_DataclassAttachRoot(&call->decision.metadata,
                call->decision.dataclass_stage, child, call->caller, call->instruction,
                NULL, call->context_activation_id) < 0)
            goto fail;
        call->context_activation_id = 0; /* MOVE retirement to root completion. */
    }
    _PySOAC_InterpreterCallClear(call);
    return 0;
fail:
    _PySOAC_InterpreterCallBindingFailed(call);
    return -1;
}

int
_PySOAC_InterpreterCallHasClass(_PySoacInterpreterCallV1 *call)
{
    return call != NULL && call->selected &&
        call->decision.kind == Py_SOAC_INTERPRETER_CALL_CLASS;
}

int
_PySOAC_InterpreterCallPrepareType(
    _PySoacInterpreterCallV1 *call, PyObject *namespace_state,
    PyObject *function, PyObject *metaclass, PyObject *name, PyObject *bases,
    PyObject *namespace_dict, PyObject *keywords, PyObject **handle)
{
    if (!_PySOAC_InterpreterCallHasClass(call) ||
        _PySOAC_InterpreterParentView(call->caller, call->instruction, &call->current_frame) != 1 ||
        soac_call_direct(call) < 0)
        return soac_interpreter_error("class preparation lost its selected native CALL");
    call->info.phase = Py_SOAC_INTERPRETER_CALL_PREPARE_TYPE;
    call->info.decorator_source = call->decision.decorator_source;
    call->info.decorator_count = call->decision.decorator_count;
    call->view.decorator_frame = call->caller;
    call->view.decorator_prefix = call->prefix_depth;
    if (call->decision.decorator_source == Py_SOAC_INTERPRETER_DECORATORS_DIRECT_CALLER) {
        if (call->info.direct_caller == NULL) goto invalid;
        _PySoacInterpreterActivationV1 *activation =
            (_PySoacInterpreterActivationV1 *)call->caller->soac_checked_activation;
        call->view.decorator_frame = activation->incoming_call.caller;
        call->view.decorator_prefix = activation->incoming_call.prefix_depth;
    }
    if (call->view.decorator_prefix < call->info.decorator_count) goto invalid;
    call->view.self = &call->view;
    call->view.thread = _PyThreadState_GET();
    int status = soac_interpreter_callbacks()->prepare_type(
        namespace_state, &call->current_frame, &call->info, &call->view,
        function, metaclass, name, bases, namespace_dict, keywords, handle);
    soac_call_unpublish(call);
    return soac_interpreter_status(status, "native class preparation failed");
invalid:
    soac_call_unpublish(call);
    return soac_interpreter_error("class preparation lost its finite decorator window");
}

PyObject *
_PySOAC_InterpreterCallVector(
    _PySoacInterpreterCallV1 *call, _PyInterpreterFrame *parent,
    const _Py_CODEUNIT *instruction, PyObject *callable, PyObject *const *args,
    size_t nargsf, PyObject *names)
{
    if (call != NULL && call->selected) {
        if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR) {
            if (!_PySOAC_DescriptorFactoryIsOriginal(callable))
                return PyObject_Vectorcall(callable, args, nargsf, names);
            PyObject *result = PySoac_NewBuiltinDescriptor(callable, args[0],
                call->decision.expected_function_owner, call->decision.verified_code,
                call->decision.metadata);
            int status = soac_selected_finished(parent, instruction,
                Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR,
                call->decision.metadata, NULL, 0, result);
            _PySOAC_InterpreterCallClear(call);
            if (status < 0 && result != NULL) {
                PyObject *error = PyErr_GetRaisedException();
                Py_DECREF(result);
                PyErr_SetRaisedException(error);
                result = NULL;
            }
            return result;
        }
        if (call->decision.kind == Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT) {
            if (!call->began) {
                if (_PySOAC_DataclassBeginRoot(call->decision.metadata,
                        call->decision.dataclass_stage, callable) < 0) return NULL;
                call->began = 1;
            }
            return _PySOAC_InterpreterCallEvalVector(call, _PyThreadState_GET(),
                (PyFunctionObject *)callable, args, PyVectorcall_NARGS(nargsf), names);
        }
        /* A fresh PREPARE_TYPE view will borrow exactly this C operation's
         * still-live argv, not a consumed caller stack alias. */
        call->view.backing = 1;
        call->view.callable = callable;
        call->view.argv = args;
        if (call->info.current.form != Py_SOAC_INTERPRETER_CALL_EXPANDED)
            call->view.names = names;
    }
    return _PySOAC_InterpreterBuildClassFromFrame(
        parent, instruction, callable, args, nargsf, names, call);
}
