/* Interpreter enforcement uses ordinary CPython frames and value owners.
 * Included by ceval.c; no SOAC body, reference-token or suspension ABI is used.
 */
static PyTypeObject SoacInterpreterActivation_Type;

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

static const PySoacInterpreterCallbacksV1 *
soac_interpreter_callbacks(void)
{
#ifdef Py_GIL_DISABLED
    soac_interpreter_error("interpreter enforcement V1 requires a GIL build");
    return NULL;
#else
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->soac.interpreter_closed ||
        interp->soac.interpreter_callbacks.abi_version != Py_SOAC_INTERPRETER_ABI_V1) {
        soac_interpreter_error("interpreter enforcement callbacks are unavailable");
        return NULL;
    }
    return &interp->soac.interpreter_callbacks;
#endif
}

int
PySoac_SetInterpreterCallbacksV1(
    const PySoacInterpreterCallbacksV1 *callbacks, size_t callbacks_size)
{
#ifdef Py_GIL_DISABLED
    return soac_interpreter_error("interpreter enforcement V1 requires a GIL build");
#else
    if (callbacks == NULL || callbacks_size != sizeof(*callbacks) ||
        callbacks->abi_version != Py_SOAC_INTERPRETER_ABI_V1 ||
        callbacks->flags != 0 || callbacks->root_begin == NULL ||
        callbacks->root_end == NULL || callbacks->birth == NULL ||
        callbacks->function_attribute == NULL || callbacks->enter == NULL ||
        callbacks->bound == NULL || callbacks->started == NULL ||
        callbacks->returned == NULL || callbacks->failed == NULL ||
        callbacks->leave == NULL ||
        callbacks->prepare_type == NULL || callbacks->definition_store == NULL) {
        return soac_interpreter_error("invalid interpreter enforcement callback table");
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->soac.interpreter_closed) {
        return soac_interpreter_error("interpreter enforcement callbacks are closed");
    }
    PySoacInterpreterCallbacksV1 *installed = &interp->soac.interpreter_callbacks;
    if (installed->abi_version != 0) {
        if (installed->abi_version == callbacks->abi_version &&
            installed->flags == callbacks->flags &&
            installed->root_begin == callbacks->root_begin &&
            installed->root_end == callbacks->root_end &&
            installed->birth == callbacks->birth &&
            installed->function_attribute == callbacks->function_attribute &&
            installed->enter == callbacks->enter &&
            installed->bound == callbacks->bound &&
            installed->started == callbacks->started &&
            installed->returned == callbacks->returned &&
            installed->failed == callbacks->failed &&
            installed->leave == callbacks->leave &&
            installed->prepare_type == callbacks->prepare_type &&
            installed->definition_store == callbacks->definition_store) {
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

/* this_instr is supplied by the actual opcode before the first callback.
 * The frame is real, not a lifetime facade. Never parse co_code bytes or infer
 * an event from names/positions. Trusted native traversal folds EXTENDED_ARG
 * into one ordinal and steps over the real base opcode's caches.
 */
static int
soac_interpreter_site(_PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
                      Py_ssize_t *units, Py_ssize_t *ordinal)
{
    *units = -1;
    *ordinal = -1;
    if (this_instr == NULL) {
        return 0;  /* Binding is before any source instruction. */
    }
    PyCodeObject *code = _PyFrame_GetCode(frame);
    _Py_CODEUNIT *base = _PyFrame_GetBytecode(frame);
    uintptr_t start = (uintptr_t)base;
    uintptr_t position = (uintptr_t)this_instr;
    size_t bytes = (size_t)_PyCode_NBYTES(code);
    if (position < start || position - start >= bytes ||
        (position - start) % sizeof(_Py_CODEUNIT) != 0) {
        return soac_interpreter_error("interpreter callback has no actual native instruction");
    }
    Py_ssize_t target = (Py_ssize_t)((position - start) / sizeof(_Py_CODEUNIT));
    Py_ssize_t count = (Py_ssize_t)(bytes / sizeof(_Py_CODEUNIT));
    if (count > INT_MAX) {
        return soac_interpreter_error("interpreter native code length is not representable");
    }
    Py_ssize_t current_ordinal = 0;
    for (int offset = 0; offset < count;) {
        _Py_CODEUNIT instruction = _Py_GetBaseCodeUnit(code, offset);
        int length = _PyInstruction_GetLength(code, offset);
        if (length <= 0 || length > count - offset) {
            return soac_interpreter_error("interpreter callback instruction traversal is invalid");
        }
        if (offset == target) {
            /* An initial VM entry may point at EXTENDED_ARG itself. Keep
             * that exact unit while folding the prefix into the following
             * logical instruction's ordinal; cache units never match. */
            *units = target;
            *ordinal = current_ordinal;
            return 0;
        }
        if (instruction.op.code != EXTENDED_ARG) {
            current_ordinal++;
        }
        if (offset > target) {
            break;
        }
        offset += length;
    }
    return soac_interpreter_error("interpreter callback site is not an actual opcode boundary");
}

int
_PySOAC_InterpreterView(_PyInterpreterFrame *frame,
                        const _Py_CODEUNIT *this_instr, uint32_t phase,
                        PyObject *state, PySoacInterpreterFrameViewV1 *view)
{
    if (frame == NULL || view == NULL ||
        (frame->owner != FRAME_OWNED_BY_THREAD &&
         frame->owner != FRAME_OWNED_BY_GENERATOR) ||
        _PyFrame_IsSoacLifetime(frame) ||
        !PyFunction_Check(PyStackRef_AsPyObjectBorrow(frame->f_funcobj))) {
        return soac_interpreter_error("interpreter callback requires its actual ordinary frame");
    }
    memset(view, 0, sizeof(*view));
    view->frame = frame;
    view->thread = _PyThreadState_GET();
    view->self = view;
    view->call_state = state;
    view->phase = phase;
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

int
PySoac_GetInterpreterFrameInfoV1(
    const PySoacInterpreterFrameViewV1 *view,
    PySoacInterpreterFrameInfoV1 *out, size_t out_size)
{
    if (out == NULL || out_size != sizeof(*out) ||
        soac_interpreter_check_view(view) < 0) {
        return soac_interpreter_error("invalid interpreter frame information output");
    }
    _PyInterpreterFrame *frame = view->frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    *out = (PySoacInterpreterFrameInfoV1) {
        .abi_version = Py_SOAC_INTERPRETER_ABI_V1,
        .phase = view->phase,
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
    activation->state = NULL;
    activation->thread = NULL;
    activation->function = NULL;
    activation->code = NULL;
    activation->namespace_state_out = NULL;
    activation->source_authority = 0;
    activation->phase = Py_SOAC_INTERPRETER_RETIRED;
    if (state != NULL) {
        /* Closing an interpreter blocks NEW entry but retains this immutable
         * scalar table until its already-entered ordinary frames are cleared.
         */
        const PySoacInterpreterCallbacksV1 *callbacks =
            &_PyInterpreterState_GET()->soac.interpreter_callbacks;
        assert(callbacks->abi_version == Py_SOAC_INTERPRETER_ABI_V1);
        callbacks->leave(state, reason);  /* No Python/allocation/error mutation. */
        soac_interpreter_xdecref_preserving_error(state);
    }
}

static int
soac_interpreter_activation_traverse(
    _PySoacInterpreterActivationV1 *activation, visitproc visit, void *arg)
{
    Py_VISIT(activation->state);
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
                reason = (activation->failure_attempted ||
                          activation->phase == Py_SOAC_INTERPRETER_RUNNING)
                    ? Py_SOAC_INTERPRETER_BODY_FAILED
                    : Py_SOAC_INTERPRETER_CHECK_FAILED;
            }
        }
        soac_interpreter_retire(activation, reason);
    }
    soac_interpreter_xdecref_preserving_error(object);
}

/* Copy only the pre-binding scalar decision. The actual function already
 * supports its one-shot owner; the ordinary frame subsequently captures code.
 * In particular a pre-seal code replacement must not erase birth provenance.
 * Explicit ROOT/CLASS entries are supplied only by their native C operation. */
static const _PySoacInterpreterEntryV1 *
soac_interpreter_capture_entry(
    PyFunctionObject *function, const _PySoacInterpreterEntryV1 *explicit_entry,
    _PySoacInterpreterEntryV1 *snapshot)
{
    if (explicit_entry != NULL) {
        return explicit_entry;
    }
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        return NULL;
    }
    *snapshot = (_PySoacInterpreterEntryV1) {
        .kind = Py_SOAC_INTERPRETER_FUNCTION,
        .boundary_snapshot = function->func_soac_required_boundary != 0,
        .subject_owner = function->func_soac_strict_owner,
        .parent = NULL,
        .namespace_state_out = NULL,
    };
    return snapshot;
}

int
_PySOAC_InterpreterInitFrame(
    _PyInterpreterFrame *frame, const _PySoacInterpreterEntryV1 *entry)
{
    if (entry == NULL) {
        return 0;
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
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
        entry->boundary_snapshot > 1 ||
        (original_source && code->_co_soac_strict_source_id == 0) ||
        (!original_source &&
         (entry->kind != Py_SOAC_INTERPRETER_FUNCTION || entry->boundary_snapshot != 0)) ||
        (entry->kind == Py_SOAC_INTERPRETER_CLASS_NAMESPACE) !=
            (entry->namespace_state_out != NULL) ||
        (entry->namespace_state_out != NULL && *entry->namespace_state_out != NULL)) {
        return soac_interpreter_error("invalid checked interpreter frame entry");
    }
    if (PyType_Ready(&SoacInterpreterActivation_Type) < 0) {
        return -1;
    }
    _PySoacInterpreterActivationV1 *activation = PyObject_GC_New(
        _PySoacInterpreterActivationV1, &SoacInterpreterActivation_Type);
    if (activation == NULL) {
        return -1;
    }
    activation->state = NULL;
    activation->thread = _PyThreadState_GET();
    activation->function = function;
    activation->code = code;
    activation->kind = entry->kind;
    activation->phase = Py_SOAC_INTERPRETER_BINDING;
    activation->boundary_snapshot = entry->boundary_snapshot;
    activation->source_authority = 0;
    activation->return_attempted = 0;
    activation->failure_attempted = 0;
    activation->namespace_state_out = entry->namespace_state_out;
    frame->soac_checked_activation = (PyObject *)activation;  /* MOVE sole edge. */
    PyObject_GC_Track(activation);
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, NULL, activation->phase, NULL, &view) < 0) {
        return -1;
    }
    PyObject *state = NULL;
    int status = callbacks->enter(
        entry->kind, entry->subject_owner, &view, entry->parent,
        entry->boundary_snapshot, &state);
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
    return 0;
}

/* CREATE precedes SET_FUNCTION_ATTRIBUTE. A watcher can call the fresh
 * function before closure installation. Validate the captured frame code,
 * not a later func_code replacement, before native COPY_FREE_VARS can read it.
 * This is a representation check, not a second cell/value ownership scheme. */
static int
soac_interpreter_closure_complete(_PySoacInterpreterActivationV1 *activation)
{
    PyObject *closure = activation->function->func_closure;
    Py_ssize_t count = activation->code->co_nfreevars;
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
        return object == NULL ? 0 : _PySOAC_DataclassCheckBound(frame);
    }
    _PySoacInterpreterActivationV1 *activation = (_PySoacInterpreterActivationV1 *)object;
    if (activation->phase != Py_SOAC_INTERPRETER_BINDING ||
        !soac_interpreter_activation_matches(activation, frame)) {
        return soac_interpreter_error("interpreter binding activation changed");
    }
    if (soac_interpreter_closure_complete(activation) < 0) {
        return -1;
    }
    activation->phase = Py_SOAC_INTERPRETER_BOUND;
    if (!activation->boundary_snapshot ||
        (activation->code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))) {
        return 0;
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL) {
        return -1;
    }
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, NULL, activation->phase, activation->state, &view) < 0) {
        return -1;
    }
    int status = callbacks->bound(activation->state, &view);
    view.self = NULL;
    if (soac_interpreter_status(status, "interpreter parameter check failed") < 0) {
        return -1;
    }
    if (!soac_interpreter_activation_matches(activation, frame) ||
        soac_interpreter_closure_complete(activation) < 0) {
        return soac_interpreter_error("interpreter parameter check changed its frame");
    }
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
        if (activation->source_authority) {
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
    if (_PyFrame_GetCode(frame)->co_flags & CO_FUTURE_STRICT) {
        return soac_interpreter_error(
            "strict code execution requires an authenticated interpreter activation");
    }
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (function != NULL && PyFunction_Check(function) &&
        ((PyFunctionObject *)function)->func_soac_strict_owner_state ==
            FUNC_SOAC_OWNER_INTERPRETER_ATTACHED) {
        return soac_interpreter_error(
            "interpreter-owned function needs its initialized native activation");
    }
    if (object != NULL ||
        (function != NULL && PyFunction_Check(function) &&
         ((PyFunctionObject *)function)->func_soac_required_boundary)) {
        if (_PySOAC_DataclassCheckRequiredFrame(frame) < 0) {
            return -1;
        }
    }
    if (frame->soac_dataclass_invocation != NULL ||
        (frame->previous != NULL && frame->previous->soac_dataclass_invocation != NULL)) {
        return _PySOAC_DataclassEnterFrame(frame);
    }
    return 0;
}

/* An arbitrary PEP 523 evaluator can consume the frame and manufacture a
 * result without any native RETURN_VALUE. V1 refuses that actual dispatch,
 * rather than silently omitting its boundary or adding a second state owner.
 * Generic VM CALL's already-committed default-evaluator choice bypasses this
 * dispatch exactly as ordinary CPython does. Tracing/monitoring are unchanged. */
int
_PySOAC_InterpreterCheckEvalHook(_PyInterpreterFrame *frame)
{
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (!(_PyFrame_GetCode(frame)->co_flags & CO_FUTURE_STRICT) &&
        !_PySOAC_InterpreterIsActivation(frame->soac_checked_activation) &&
        !(function != NULL && PyFunction_Check(function) &&
          ((PyFunctionObject *)function)->func_soac_strict_owner_state ==
              FUNC_SOAC_OWNER_INTERPRETER_ATTACHED)) {
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

int
_PySOAC_InterpreterBirth(_PyInterpreterFrame *parent,
                         const _Py_CODEUNIT *this_instr, PyFunctionObject *function)
{
    if (parent == NULL || !_PySOAC_InterpreterIsActivation(parent->soac_checked_activation)) {
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)parent->soac_checked_activation;
    if (!activation->source_authority) {
        return 0;  /* Authenticated ordinary replacement: no source child grant. */
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
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
    uint32_t required = 0;
    int status = callbacks->birth(&view, (PyObject *)function, &owner, &required);
    view.self = NULL;
    if (soac_interpreter_status(status, "interpreter function birth failed") < 0 ||
        owner == NULL || required > 1 || function->func_code != code ||
        function->func_globals != globals ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_NONE ||
        function->func_soac_strict_owner != NULL || function->func_soac_strict_id != 0 ||
        function->vectorcall != _PyFunction_Vectorcall) {
        soac_interpreter_error("interpreter birth callback changed the uncommitted function");
        soac_interpreter_xdecref_preserving_error(owner);
        return -1;
    }
    /* No callback after this MOVE and before the ordinary CREATE watcher.
     * Stock vectorcall is intentional: common native binding checks owned
     * source functions while generic VM CALL still consumes its input tokens.
     */
    function->func_soac_strict_owner = owner;
    function->func_soac_strict_owner_state = FUNC_SOAC_OWNER_INTERPRETER_ATTACHED;
    function->func_soac_required_boundary = (unsigned char)required;
    return 0;
}

int
_PySOAC_InterpreterParentView(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *instruction,
    PySoacInterpreterFrameViewV1 *view)
{
    if (frame == NULL || !_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return 0;
    }
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
_PySOAC_InterpreterDefinitionStore(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    uint32_t lane, PyObject *value)
{
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (!activation->source_authority) {
        return 0;
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL || activation->phase != Py_SOAC_INTERPRETER_RUNNING ||
        !soac_interpreter_activation_matches(activation, frame) || lane > 1) {
        return soac_interpreter_error("definition Store has no active interpreter frame");
    }
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, this_instr, activation->phase,
                                activation->state, &view) < 0) {
        return -1;
    }
    int status = callbacks->definition_store(&view, lane, value);
    view.self = NULL;
    return soac_interpreter_status(status, "interpreter definition Store check failed");
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
    activation->phase = Py_SOAC_INTERPRETER_RETURNING;
    activation->return_attempted = 1;  /* Before any checker/error reentry. */
    if (activation->kind != Py_SOAC_INTERPRETER_FUNCTION ||
        (activation->code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))) {
        return 0;
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL) {
        return -1;
    }
    PySoacInterpreterFrameViewV1 view;
    if (_PySOAC_InterpreterView(frame, this_instr, activation->phase,
                                activation->state, &view) < 0) {
        return -1;
    }
    int status = callbacks->returned(activation->state, &view, value);
    view.self = NULL;
    return soac_interpreter_status(status, "interpreter return check failed");
}

int
_PySOAC_CheckedFrameFailed(_PyInterpreterFrame *frame,
                           const _Py_CODEUNIT *instruction)
{
    if (!_PySOAC_InterpreterIsActivation(frame->soac_checked_activation)) {
        return 0;
    }
    _PySoacInterpreterActivationV1 *activation =
        (_PySoacInterpreterActivationV1 *)frame->soac_checked_activation;
    if (!activation->source_authority || activation->kind != Py_SOAC_INTERPRETER_FUNCTION ||
        activation->failure_attempted) {
        return 0;
    }
    assert(PyErr_Occurred());
    PyObject *primary = PyErr_GetRaisedException();
    activation->failure_attempted = 1;
    activation->phase = Py_SOAC_INTERPRETER_FAILING;
    int status = -1;
    PySoacInterpreterFrameViewV1 view;
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
    if (callbacks != NULL &&
        soac_interpreter_activation_matches(activation, frame) &&
        _PySOAC_InterpreterView(frame, instruction, activation->phase,
                                activation->state, &view) == 0) {
        status = callbacks->failed(activation->state, &view);
        view.self = NULL;
    }
    if (soac_interpreter_status(status, "interpreter pending-definition completion failed") < 0) {
        /* Normal unraisable reporting preserves evidence of the secondary
         * error; it must not substitute for the already-raised body error.
         * The real native frame still supports this borrowed function. */
        PyErr_WriteUnraisable(PyStackRef_AsPyObjectBorrow(frame->f_funcobj));
        status = -1;
    }
    PyErr_SetRaisedException(primary);
    return status;
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
        activation->source_authority = 0;
        activation->thread = NULL;
        activation->function = NULL;
        activation->code = NULL;
        _PyInterpreterState_GET()->soac.interpreter_callbacks.leave(
            state, Py_SOAC_INTERPRETER_NAMESPACE_TRANSFERRED);
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
PySoac_EvalInterpreterModuleV1(PyObject *module, PyObject *root_code,
                               PyObject *module_owner)
{
    if (PyErr_Occurred()) {
        return NULL;
    }
    const PySoacInterpreterCallbacksV1 *callbacks = soac_interpreter_callbacks();
    if (callbacks == NULL) {
        return NULL;
    }
    if (module == NULL || !PyModule_Check(module) ||
        root_code == NULL || !PyCode_Check(root_code) || module_owner == NULL ||
        !(((PyCodeObject *)root_code)->co_flags & CO_FUTURE_STRICT) ||
        ((PyCodeObject *)root_code)->_co_soac_strict_source_id == 0) {
        soac_interpreter_error("interpreter root needs an authenticated module and code");
        return NULL;
    }
    int status = callbacks->root_begin(module_owner, module, root_code);
    if (soac_interpreter_status(status, "interpreter root initialization was refused") < 0) {
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
                .boundary_snapshot = 0,
                .subject_owner = module_owner,
                .parent = NULL,
                .namespace_state_out = NULL,
            };
            result = _PySOAC_InterpreterEvalVector(
                _PyThreadState_GET(), function, globals, NULL, 0, NULL, &entry);
            soac_interpreter_xdecref_preserving_error((PyObject *)function);
        }
    }
    callbacks->root_end(module_owner, result != NULL);
    return result;
}
