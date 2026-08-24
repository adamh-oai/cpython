/* Included by funcobject.c: C-only, interpreter-owned dataclass provenance.
 * None of these records permits execution of original strict bytecode.
 */

typedef enum {
    SOAC_DC_NEW,
    SOAC_DC_FACTORY_ACTIVE,
    SOAC_DC_FACTORY_READY,
    SOAC_DC_CLASS_BOUND,
    SOAC_DC_APPLY_ACTIVE,
    SOAC_DC_APPLY_RETURNED,
    SOAC_DC_COMPLETE,
    SOAC_DC_DECLINED,
    SOAC_DC_FAILED,
    SOAC_DC_TERMINAL,
} SoacDataclassPhase;

typedef struct {
    PyObject_HEAD
    int64_t interpreter_id;
    SoacDataclassPhase phase;
    PyObject *owner;              /* Active GC-visible compiler/catalog owner. */
    PyObject *class_owner;        /* Active contract owner, not the type. */
    PyTypeObject *actual_type;    /* Comparison only; never dereference it. */
    PyObject *replacement_owner; /* Separate active owner, no type backedge. */
    PyTypeObject *replacement_type; /* Comparison only; guarded by its caller. */
    PyObject *replacement_weakref; /* Callback-free, prevents raw-address ABA. */
    int slots_started;
    int member_pending;
    PyObject *pending_type_root;  /* existing native lineage metadata only */
    int pending_scope_active;
} SoacDataclassInvocation;

typedef enum {
    SOAC_DC_RECORD_CONSTRUCTING,
    SOAC_DC_RECORD_FRESH,
    SOAC_DC_RECORD_CONSUMING,
    SOAC_DC_RECORD_INSTALLED,
    SOAC_DC_RECORD_COMPONENT,
    SOAC_DC_RECORD_TERMINAL,
} SoacDataclassRecordPhase;

typedef struct {
    PyObject_HEAD
    SoacDataclassInvocation *invocation;
    PyObject *code_weakref;       /* No original-code/co_consts lifetime edge. */
    PyObject *boundary;          /* Immutable required-check plan, if selected. */
    PyFunctionObject *function;  /* Cleared before weakrefs/decrefs; nonowning. */
    uint64_t identity;
    unsigned int role;
    SoacDataclassRecordPhase phase;
} SoacDataclassRecord;

typedef struct {
    PyObject_HEAD
    SoacDataclassInvocation *invocation;
    SoacDataclassRecord *record;
    PyObject *actual_type;
    PyObject *class_owner;
    PyObject *name;
    PyObject *function;
    /* First-check destinations may become logically stale across callbacks.
     * The mandatory second check clears/rederives them before use. Existing
     * operation operands provide support; no new owner or function/code pin. */
    uint64_t *pending_birth;
    uint64_t pending_identity;
    int finished;
} SoacDataclassMember;

struct _PySoacDataclassFrameView {
    _PyInterpreterFrame *frame;
    unsigned int role;
    PyObject *invocation;  /* Explicit borrowed callback context, not ambient. */
    _PySoacDataclassSlotsContext *slots_context; /* Only prepare_slots view. */
};

static PyTypeObject SoacDataclassInvocation_Type;
static PyTypeObject SoacDataclassRecord_Type;
static PyTypeObject SoacDataclassMember_Type;
static PyTypeObject SoacDataclassBoundary_Type;
static PyTypeObject SoacDataclassActivation_Type;
static SoacDataclassRecord *soac_dataclass_record(PyFunctionObject *);
static int soac_dataclass_original_code(SoacDataclassRecord *, PyFunctionObject *);
static unsigned int soac_dataclass_bridge_kind(PyObject *);
static int soac_dataclass_boundary_publication(PyFunctionObject *, SoacDataclassRecord *);
static PyObject *soac_dataclass_checked_value_call(
    _PyInterpreterFrame *, PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
static PyObject *soac_dataclass_dispatch_bridge(
    _PyInterpreterFrame *, PyObject *, unsigned int, PyObject *const *,
    Py_ssize_t, PyObject *);

static int
soac_dataclass_error(const char *message)
{
    if (!PyErr_Occurred()) {
        func_soac_runtime_error(message);
    }
    return -1;
}

static int
soac_dataclass_is_running(SoacDataclassInvocation *invocation)
{
    return invocation->phase == SOAC_DC_FACTORY_ACTIVE ||
           invocation->phase == SOAC_DC_APPLY_ACTIVE;
}

static void
soac_dataclass_expire(SoacDataclassInvocation *invocation,
                     SoacDataclassPhase phase)
{
    /* Publish every terminal coordinate before releasing active edges. A
     * generated function may retain this now-inert invocation indefinitely. */
    invocation->phase = phase;
    invocation->actual_type = NULL;
    invocation->replacement_type = NULL;
    invocation->slots_started = 0;
    invocation->member_pending = 0;
    PyObject *owner = invocation->owner;
    PyObject *class_owner = invocation->class_owner;
    PyObject *replacement_owner = invocation->replacement_owner;
    PyObject *replacement_weakref = invocation->replacement_weakref;
    invocation->owner = NULL;
    invocation->class_owner = NULL;
    invocation->replacement_owner = NULL;
    invocation->replacement_weakref = NULL;
    PyObject *pending_root = invocation->pending_type_root;
    invocation->pending_type_root = NULL;
    if (invocation->pending_scope_active) {
        invocation->pending_scope_active = 0;
        _PySOAC_EndPendingTypeAdapter(pending_root, phase != SOAC_DC_COMPLETE);
    }
    else if (pending_root != NULL && phase != SOAC_DC_COMPLETE) {
        /* Body error/abandonment terminalizes before releasing active owners. */
        (void)PyType_FailSoacPendingV1(pending_root);
    }
    PyObject *exception = PyErr_GetRaisedException();
    Py_XDECREF(owner);
    Py_XDECREF(class_owner);
    Py_XDECREF(replacement_owner);
    Py_XDECREF(replacement_weakref);
    Py_XDECREF(pending_root);
    PyErr_SetRaisedException(exception);
}

static int
soac_dataclass_fail(SoacDataclassInvocation *invocation, const char *message)
{
    /* Failure cannot revoke a completed contract or convert ordinary decline
     * into a new execution restriction. Both already forbid adoption. */
    if (invocation->phase != SOAC_DC_COMPLETE &&
        invocation->phase != SOAC_DC_DECLINED &&
        invocation->phase != SOAC_DC_TERMINAL) {
        soac_dataclass_expire(invocation, SOAC_DC_FAILED);
    }
    return soac_dataclass_error(message);
}

static int
soac_dataclass_current(SoacDataclassInvocation *invocation)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (invocation->interpreter_id != PyInterpreterState_GetID(interp) ||
        interp->soac.dataclass_closed ||
        interp->soac.dataclass_callbacks.abi_version != Py_SOAC_DATACLASS_ABI) {
        return soac_dataclass_error("dataclass invocation belongs to an unavailable interpreter");
    }
    return 0;
}

static SoacDataclassInvocation *
soac_dataclass_invocation(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassInvocation_Type)) {
        PyErr_SetString(PyExc_TypeError, "expected a native dataclass invocation");
        return NULL;
    }
    SoacDataclassInvocation *invocation = (SoacDataclassInvocation *)object;
    if (soac_dataclass_current(invocation) < 0) {
        return NULL;
    }
    return invocation;
}

static int
soac_dataclass_traverse(SoacDataclassInvocation *invocation,
                        visitproc visit, void *arg)
{
    Py_VISIT(invocation->owner);
    Py_VISIT(invocation->class_owner);
    Py_VISIT(invocation->replacement_owner);
    Py_VISIT(invocation->replacement_weakref);
    Py_VISIT(invocation->pending_type_root);
    return 0;
}

static int
soac_dataclass_clear(SoacDataclassInvocation *invocation)
{
    soac_dataclass_expire(invocation, SOAC_DC_TERMINAL);
    return 0;
}

static void
soac_dataclass_dealloc(SoacDataclassInvocation *invocation)
{
    PyObject_GC_UnTrack(invocation);
    soac_dataclass_clear(invocation);
    PyObject_GC_Del(invocation);
}

static PyTypeObject SoacDataclassInvocation_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.DataclassInvocation",
    .tp_basicsize = sizeof(SoacDataclassInvocation),
    .tp_dealloc = (destructor)soac_dataclass_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_dataclass_traverse,
    .tp_clear = (inquiry)soac_dataclass_clear,
};

static int
soac_dataclass_record_traverse(SoacDataclassRecord *record,
                               visitproc visit, void *arg)
{
    Py_VISIT(record->invocation);
    Py_VISIT(record->code_weakref);
    Py_VISIT(record->boundary);
    return 0;
}

static int
soac_dataclass_record_clear(SoacDataclassRecord *record)
{
    record->function = NULL;
    record->phase = SOAC_DC_RECORD_TERMINAL;
    Py_CLEAR(record->invocation);
    Py_CLEAR(record->code_weakref);
    Py_CLEAR(record->boundary);
    return 0;
}

static void
soac_dataclass_record_dealloc(SoacDataclassRecord *record)
{
    PyObject_GC_UnTrack(record);
    soac_dataclass_record_clear(record);
    PyObject_GC_Del(record);
}

static PyTypeObject SoacDataclassRecord_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.DataclassCreation",
    .tp_basicsize = sizeof(SoacDataclassRecord),
    .tp_dealloc = (destructor)soac_dataclass_record_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_dataclass_record_traverse,
    .tp_clear = (inquiry)soac_dataclass_record_clear,
};

static void
soac_dataclass_function_clear(PyFunctionObject *function)
{
    PyObject *owner = function->func_soac_strict_owner;
    if (owner != NULL && Py_IS_TYPE(owner, &SoacDataclassRecord_Type)) {
        SoacDataclassRecord *record = (SoacDataclassRecord *)owner;
        /* A copied owner never gets to clear its original function binding. */
        if (record->function == function) {
            record->function = NULL;
            record->phase = SOAC_DC_RECORD_TERMINAL;
        }
    }
}

int
PySoac_SetDataclassCallbacks(const PySoacDataclassCallbacks *callbacks)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "dataclass provenance requires the GIL build");
    return -1;
#endif
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (callbacks == NULL || callbacks->abi_version != Py_SOAC_DATACLASS_ABI ||
        callbacks->enter == NULL || callbacks->create == NULL ||
        callbacks->validate_member == NULL || callbacks->bridge == NULL ||
        callbacks->compiled == NULL || callbacks->created == NULL ||
        callbacks->bound == NULL || callbacks->value == NULL ||
        callbacks->validate_component == NULL || callbacks->init_value == NULL ||
        callbacks->prepare_slots == NULL) {
        PyErr_SetString(PyExc_TypeError, "invalid native dataclass callback table");
        return -1;
    }
    if (interp->soac.dataclass_closed || interp->soac.dataclass_installing) {
        return soac_dataclass_error("native dataclass callbacks are closed");
    }
    PySoacDataclassCallbacks *installed = &interp->soac.dataclass_callbacks;
    if (installed->abi_version != 0) {
        if (installed->abi_version == callbacks->abi_version &&
            installed->enter == callbacks->enter &&
            installed->create == callbacks->create &&
            installed->validate_member == callbacks->validate_member &&
            installed->bridge == callbacks->bridge &&
            installed->compiled == callbacks->compiled &&
            installed->created == callbacks->created &&
            installed->bound == callbacks->bound &&
            installed->value == callbacks->value &&
            installed->validate_component == callbacks->validate_component &&
            installed->init_value == callbacks->init_value &&
            installed->prepare_slots == callbacks->prepare_slots) {
            return 0;
        }
        return soac_dataclass_error("native dataclass callbacks are already installed");
    }
    interp->soac.dataclass_installing = 1;
    if (PyType_Ready(&SoacDataclassInvocation_Type) < 0 ||
        PyType_Ready(&SoacDataclassRecord_Type) < 0 ||
        PyType_Ready(&SoacDataclassMember_Type) < 0 ||
        PyType_Ready(&SoacDataclassBoundary_Type) < 0 ||
        PyType_Ready(&SoacDataclassActivation_Type) < 0) {
        interp->soac.dataclass_installing = 0;
        return -1;
    }
    if (interp->soac.dataclass_closed) {
        interp->soac.dataclass_installing = 0;
        return soac_dataclass_error("native dataclass callbacks closed during initialization");
    }
    /* Copy immutable function pointers, never a caller-owned table address. */
    *installed = *callbacks;
    interp->soac.dataclass_installing = 0;
    return 0;
}

PyObject *
PySoac_NewDataclassInvocation(PyObject *owner)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (owner == NULL || interp->soac.dataclass_closed ||
        interp->soac.dataclass_callbacks.abi_version != Py_SOAC_DATACLASS_ABI) {
        soac_dataclass_error("native dataclass runtime is not initialized");
        return NULL;
    }
    SoacDataclassInvocation *invocation = PyObject_GC_New(
        SoacDataclassInvocation, &SoacDataclassInvocation_Type);
    if (invocation == NULL) {
        return NULL;
    }
    invocation->interpreter_id = PyInterpreterState_GetID(interp);
    invocation->phase = SOAC_DC_NEW;
    invocation->owner = Py_NewRef(owner);
    invocation->class_owner = NULL;
    invocation->actual_type = NULL;
    invocation->replacement_owner = NULL;
    invocation->replacement_type = NULL;
    invocation->replacement_weakref = NULL;
    invocation->slots_started = 0;
    invocation->member_pending = 0;
    invocation->pending_type_root = NULL;
    invocation->pending_scope_active = 0;
    PyObject_GC_Track(invocation);
    return (PyObject *)invocation;
}

int
PySoac_DeclineDataclassInvocation(PyObject *object)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (invocation->phase == SOAC_DC_DECLINED) {
        return 0;
    }
    if (invocation->phase != SOAC_DC_NEW &&
        invocation->phase != SOAC_DC_FACTORY_READY) {
        return soac_dataclass_fail(invocation,
                                  "dataclass decline is only valid before class binding");
    }
    soac_dataclass_expire(invocation, SOAC_DC_DECLINED);
    return 0;
}

int
PySoac_FailDataclassInvocation(PyObject *object)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (invocation->phase != SOAC_DC_COMPLETE &&
        invocation->phase != SOAC_DC_DECLINED &&
        invocation->phase != SOAC_DC_TERMINAL) {
        soac_dataclass_expire(invocation, SOAC_DC_FAILED);
    }
    return 0;
}

int
PySoac_DataclassBindClass(PyObject *object, PyObject *actual_type,
                          PyObject *expected_owner)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if ((invocation->phase != SOAC_DC_NEW &&
         invocation->phase != SOAC_DC_FACTORY_READY) ||
        actual_type == NULL || !PyType_Check(actual_type) || expected_owner == NULL ||
        _PySOAC_GetTypeConstructionOwner(actual_type) != expected_owner) {
        return soac_dataclass_fail(invocation, "dataclass class binding is not authentic");
    }
    PyObject *pending_root = NULL;
    if (_PySOAC_BeginPendingTypeAdapter(actual_type, &pending_root) < 0) {
        return soac_dataclass_fail(invocation, "pending dataclass interval is unavailable");
    }
    invocation->pending_type_root = pending_root;
    invocation->pending_scope_active = pending_root != NULL;
    invocation->class_owner = Py_NewRef(expected_owner);
    invocation->actual_type = (PyTypeObject *)actual_type;
    invocation->phase = SOAC_DC_CLASS_BOUND;
    return 0;
}

int
PySoac_CompleteDataclassInvocation(PyObject *object)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (invocation->phase == SOAC_DC_COMPLETE) {
        return 0;
    }
    if (invocation->phase != SOAC_DC_APPLY_RETURNED || invocation->member_pending) {
        return soac_dataclass_fail(invocation, "dataclass application is not complete");
    }
    soac_dataclass_expire(invocation, SOAC_DC_COMPLETE);
    return 0;
}

static int
soac_dataclass_frame_valid(_PyInterpreterFrame *frame)
{
    if (frame == NULL || frame->owner >= FRAME_OWNED_BY_INTERPRETER ||
        PyStackRef_IsNull(frame->f_funcobj)) {
        return 0;
    }
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (!PyFunction_Check(function)) {
        return 0;
    }
    PyCodeObject *code = _PyFrame_GetCode(frame);
    return ((PyFunctionObject *)function)->func_code == (PyObject *)code &&
           ((PyFunctionObject *)function)->vectorcall == _PyFunction_Vectorcall &&
           !(code->co_flags & (CO_FUTURE_STRICT | CO_GENERATOR |
                              CO_COROUTINE | CO_ASYNC_GENERATOR)) &&
           code->_co_soac_strict_source_id == 0;
}

static int
soac_dataclass_enter(SoacDataclassInvocation *invocation, unsigned int stage,
                     _PyInterpreterFrame *parent, _PyInterpreterFrame *frame)
{
    if (!soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        soac_dataclass_current(invocation) < 0 ||
        (parent != NULL &&
         (parent->soac_dataclass_invocation != (PyObject *)invocation ||
          !soac_dataclass_frame_valid(parent)))) {
        return soac_dataclass_fail(invocation, "dataclass frame entry is not authentic");
    }
    PySoacDataclassFrameView parent_view = {
        .frame = parent,
        .role = parent == NULL ? 0 : parent->soac_dataclass_role,
        .invocation = (PyObject *)invocation,
    };
    PySoacDataclassFrameView child_view = {
        .frame = frame, .role = 0, .invocation = (PyObject *)invocation,
    };
    unsigned int role = 0;
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.enter(
        invocation->owner, stage, parent == NULL ? NULL : &parent_view,
        &child_view, &role);
    if (status < 0 || status > 1 || PyErr_Occurred() ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        (status == 1 && (role == 0 || !soac_dataclass_frame_valid(frame) ||
                        _PyInterpreterState_GET()->eval_frame != NULL))) {
        return soac_dataclass_fail(invocation, "dataclass call edge did not authenticate");
    }
    if (status == 0) {
        if (stage != 0) {
            return soac_dataclass_fail(invocation, "dataclass root entry did not authenticate");
        }
        return 0;
    }
    PyFunctionObject *function = (PyFunctionObject *)
        PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_NONE) {
        SoacDataclassRecord *record = soac_dataclass_record(function);
        if (record == NULL || record->invocation != invocation ||
            !soac_dataclass_original_code(record, function)) {
            return soac_dataclass_fail(invocation,
                                      "dataclass callee creation record did not authenticate");
        }
    }
    frame->soac_dataclass_role = role;
    frame->soac_dataclass_invocation = Py_NewRef((PyObject *)invocation);
    return 0;
}

int
_PySOAC_DataclassEnterExplicit(PyObject *object, unsigned int stage,
                              _PyInterpreterFrame *parent,
                              _PyInterpreterFrame *frame)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    return soac_dataclass_enter(invocation, stage, parent, frame);
}

int
_PySOAC_DataclassEnterFrame(_PyInterpreterFrame *frame)
{
    if (frame->soac_dataclass_invocation != NULL) {
        /* Explicit root entry already validated the bound frame. */
        return 0;
    }
    _PyInterpreterFrame *parent = frame->previous;
    if (parent == NULL || parent->owner >= FRAME_OWNED_BY_INTERPRETER ||
        parent->soac_dataclass_invocation == NULL) {
        return 0;
    }
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    if (!soac_dataclass_is_running(invocation)) {
        /* Failed/declined paths may still execute ordinary finally cleanup. */
        return 0;
    }
    if (!soac_dataclass_frame_valid(parent)) {
        return soac_dataclass_fail(invocation, "dataclass parent frame changed after admission");
    }
    if (frame->owner >= FRAME_OWNED_BY_INTERPRETER ||
        PyStackRef_IsNull(frame->f_funcobj) ||
        !PyFunction_Check(PyStackRef_AsPyObjectBorrow(frame->f_funcobj))) {
        /* Synthetic type-call trampolines do not transmit provenance. */
        return 0;
    }
    return soac_dataclass_enter(invocation, 0, parent, frame);
}

static int
soac_dataclass_direct_python_callable(PyObject *callable)
{
    if (PyMethod_Check(callable)) {
        callable = PyMethod_GET_FUNCTION(callable);
    }
    return PyFunction_Check(callable) &&
        ((PyFunctionObject *)callable)->vectorcall == _PyFunction_Vectorcall;
}

PyObject *
_PySOAC_DataclassVectorcallFromFrame(_PyInterpreterFrame *parent,
                                    PyObject *callable, PyObject *const *args,
                                    size_t nargsf, PyObject *kwnames)
{
    if (parent != NULL && parent->soac_checked_activation != NULL &&
        _PySOAC_DataclassHasValueSite(parent)) {
        return soac_dataclass_checked_value_call(
            parent, callable, args, PyVectorcall_NARGS(nargsf), kwnames);
    }
    if (parent == NULL || parent->soac_dataclass_invocation == NULL ||
        !soac_dataclass_is_running(
            (SoacDataclassInvocation *)parent->soac_dataclass_invocation)) {
        return PyObject_Vectorcall(callable, args, nargsf, kwnames);
    }
    if (_PySOAC_DataclassIsBridgeImplementation(callable)) {
        unsigned int kind = soac_dataclass_bridge_kind(callable);
        if (kind == 0) {
            soac_dataclass_fail(
                (SoacDataclassInvocation *)parent->soac_dataclass_invocation,
                "dataclass bridge is not the canonical native object");
            return NULL;
        }
        if (kind == Py_SOAC_DATACLASS_BRIDGE_VALUE) {
            /* Only its expected site in a checked frame consumes a value
             * obligation. Ordinary generated functions stay ordinary. */
            return PyObject_Vectorcall(callable, args, nargsf, kwnames);
        }
        return soac_dataclass_dispatch_bridge(
            parent, callable, kind, args, PyVectorcall_NARGS(nargsf), kwnames);
    }
    if (!soac_dataclass_direct_python_callable(callable)) {
        return PyObject_Vectorcall(callable, args, nargsf, kwnames);
    }

    /* Match ordinary method_vectorcall's borrowed receiver projection. This
     * explicit opcode edge is not installed on the method or function and
     * therefore cannot be inherited by a C proxy forwarding the same call. */
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (PyMethod_Check(callable)) {
        PyObject *self = PyMethod_GET_SELF(callable);
        PyObject *function = PyMethod_GET_FUNCTION(callable);
        if (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET) {
            PyObject **newargs = (PyObject **)args - 1;
            PyObject *saved = newargs[0];
            newargs[0] = self;
            PyObject *result = _PySOAC_DataclassVectorcallFromFrame(
                parent, function, newargs, nargs + 1, kwnames);
            newargs[0] = saved;
            return result;
        }
        Py_ssize_t nkwargs = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
        Py_ssize_t total = nargs + nkwargs;
        PyObject *small[_PY_FASTCALL_SMALL_STACK];
        PyObject **newargs = small;
        if (total >= (Py_ssize_t)Py_ARRAY_LENGTH(small)) {
            newargs = PyMem_Malloc((total + 1) * sizeof(PyObject *));
            if (newargs == NULL) {
                return PyErr_NoMemory();
            }
        }
        newargs[0] = self;
        if (total != 0) {
            memcpy(newargs + 1, args, total * sizeof(PyObject *));
        }
        PyObject *result = _PySOAC_DataclassVectorcallFromFrame(
            parent, function, newargs, nargs + 1, kwnames);
        if (newargs != small) {
            PyMem_Free(newargs);
        }
        return result;
    }

    PyFunctionObject *function = (PyFunctionObject *)callable;
    PyObject *locals = ((PyCodeObject *)function->func_code)->co_flags & CO_OPTIMIZED
        ? NULL : function->func_globals;
    PyObject *result = _PySOAC_DataclassEvalVector(
        _PyThreadState_GET(), function, locals, args, nargs, kwnames,
        parent->soac_dataclass_invocation, 0, parent);
    return _Py_CheckFunctionResult(_PyThreadState_GET(), callable, result, NULL);
}

PyObject *
_PySOAC_DataclassObjectCallFromFrame(_PyInterpreterFrame *parent,
                                    PyObject *callable, PyObject *args,
                                    PyObject *kwargs)
{
    int checked_site = parent != NULL &&
        parent->soac_checked_activation != NULL &&
        _PySOAC_DataclassHasValueSite(parent);
    if (!checked_site && (parent == NULL || parent->soac_dataclass_invocation == NULL ||
        (!soac_dataclass_direct_python_callable(callable) &&
         !_PySOAC_DataclassIsBridgeImplementation(callable)))) {
        return PyObject_Call(callable, args, kwargs);
    }
    /* Use the same unpacking and release order as PyVectorcall_Call. The
     * caller still owns the tuple/dictionary throughout this C-dispatched
     * path, unlike the ordinary inlined CALL_FUNCTION_EX frame transfer. */
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (kwargs == NULL || PyDict_GET_SIZE(kwargs) == 0) {
        return _PySOAC_DataclassVectorcallFromFrame(
            parent, callable, _PyTuple_ITEMS(args), nargs, NULL);
    }
    PyObject *kwnames;
    PyObject *const *unpacked = _PyStack_UnpackDict(
        _PyThreadState_GET(), _PyTuple_ITEMS(args), nargs, kwargs, &kwnames);
    if (unpacked == NULL) {
        return NULL;
    }
    PyObject *result = _PySOAC_DataclassVectorcallFromFrame(
        parent, callable, unpacked, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET,
        kwnames);
    _PyStack_UnpackDict_Free(unpacked, nargs, kwnames);
    return result;
}

PyObject *
PySoac_DataclassVectorcall(PyObject *object, unsigned int stage,
                           PyObject *callable, PyObject *const *args,
                           size_t nargsf, PyObject *kwnames)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return NULL;
    }
    if (stage != Py_SOAC_DATACLASS_ROOT_FACTORY &&
        stage != Py_SOAC_DATACLASS_ROOT_APPLY) {
        soac_dataclass_fail(invocation, "invalid dataclass root stage");
        return NULL;
    }
    if (invocation->phase == SOAC_DC_DECLINED) {
        return PyObject_Vectorcall(callable, args, nargsf, kwnames);
    }
    SoacDataclassPhase expected = stage == Py_SOAC_DATACLASS_ROOT_FACTORY
        ? SOAC_DC_NEW : SOAC_DC_CLASS_BOUND;
    if (invocation->phase != expected || callable == NULL ||
        !PyFunction_Check(callable) ||
        ((PyFunctionObject *)callable)->vectorcall != _PyFunction_Vectorcall ||
        _PyInterpreterState_GET()->eval_frame != NULL) {
        soac_dataclass_fail(invocation, "dataclass root invocation is not available");
        return NULL;
    }
    invocation->phase = stage == Py_SOAC_DATACLASS_ROOT_FACTORY
        ? SOAC_DC_FACTORY_ACTIVE : SOAC_DC_APPLY_ACTIVE;
    PyObject *result = _PySOAC_DataclassEvalVector(
        _PyThreadState_GET(), (PyFunctionObject *)callable, NULL,
        args, PyVectorcall_NARGS(nargsf), kwnames, object, stage, NULL);
    if (result == NULL || !soac_dataclass_is_running(invocation) ||
        invocation->member_pending ||
        (stage == Py_SOAC_DATACLASS_ROOT_APPLY &&
         (invocation->slots_started
             ? PySoac_DataclassMatchesSlotsClass(object, result,
                                                 invocation->replacement_owner) != 1
             : result != (PyObject *)invocation->actual_type))) {
        soac_dataclass_fail(invocation, "dataclass root did not finish its exact application");
        Py_XDECREF(result);
        return NULL;
    }
    invocation->phase = stage == Py_SOAC_DATACLASS_ROOT_FACTORY
        ? SOAC_DC_FACTORY_READY : SOAC_DC_APPLY_RETURNED;
    if (stage == Py_SOAC_DATACLASS_ROOT_APPLY && invocation->pending_scope_active) {
        invocation->pending_scope_active = 0;
        _PySOAC_EndPendingTypeAdapter(invocation->pending_type_root, 0);
    }
    return result;
}

PyObject *
PySoac_DataclassFrameFunction(const PySoacDataclassFrameView *view)
{
    return view == NULL ? NULL : PyStackRef_AsPyObjectBorrow(view->frame->f_funcobj);
}

PyObject *
PySoac_DataclassFrameCode(const PySoacDataclassFrameView *view)
{
    return view == NULL ? NULL : (PyObject *)_PyFrame_GetCode(view->frame);
}

PyObject *
PySoac_DataclassFrameGlobals(const PySoacDataclassFrameView *view)
{
    return view == NULL ? NULL : view->frame->f_globals;
}

PyObject *
PySoac_DataclassFrameBuiltins(const PySoacDataclassFrameView *view)
{
    return view == NULL ? NULL : view->frame->f_builtins;
}

PyObject *
PySoac_DataclassFrameInvocation(const PySoacDataclassFrameView *view)
{
    return view == NULL ? NULL : view->invocation;
}

PyObject *
PySoac_DataclassFrameLocal(const PySoacDataclassFrameView *view, Py_ssize_t index)
{
    if (view == NULL || index < 0 || index >= _PyFrame_GetCode(view->frame)->co_nlocalsplus) {
        PyErr_SetString(PyExc_IndexError, "dataclass frame local index is out of bounds");
        return NULL;
    }
    _PyStackRef value = view->frame->localsplus[index];
    return PyStackRef_IsNull(value) ? NULL : PyStackRef_AsPyObjectBorrow(value);
}

PyObject *
PySoac_DataclassFrameCellValue(const PySoacDataclassFrameView *view, Py_ssize_t index)
{
    PyObject *cell = PySoac_DataclassFrameLocal(view, index);
    if (cell == NULL) {
        return NULL;
    }
    if (!PyCell_Check(cell)) {
        PyErr_SetString(PyExc_TypeError, "dataclass frame slot is not a cell");
        return NULL;
    }
    return PyCell_GET(cell);
}

unsigned int
PySoac_DataclassFrameRole(const PySoacDataclassFrameView *view)
{
    return view == NULL ? 0 : view->role;
}

Py_ssize_t
PySoac_DataclassFrameInstruction(const PySoacDataclassFrameView *view)
{
    return view == NULL ? -1 : _PyInterpreterFrame_LASTI(view->frame);
}

static SoacDataclassRecord *
soac_dataclass_record(PyFunctionObject *function)
{
    if (function->func_soac_strict_owner_state == FUNC_SOAC_OWNER_TERMINAL) {
        soac_dataclass_error("generated function owner is terminal");
        return NULL;
    }
    PyObject *owner = function->func_soac_strict_owner;
    if (owner == NULL || !Py_IS_TYPE(owner, &SoacDataclassRecord_Type)) {
        return NULL;
    }
    SoacDataclassRecord *record = (SoacDataclassRecord *)owner;
    if (record->function != function || record->invocation == NULL ||
        record->phase == SOAC_DC_RECORD_TERMINAL ||
        soac_dataclass_current(record->invocation) < 0) {
        soac_dataclass_error("generated function record is cleared or replayed");
        return NULL;
    }
    return record;
}

int
PyFunction_HasSoacDataclassCreation(PyObject *object)
{
    if (object == NULL || !PyFunction_Check(object)) {
        PyErr_SetString(PyExc_TypeError, "dataclass creation query requires an exact function");
        return -1;
    }
    SoacDataclassRecord *record = soac_dataclass_record((PyFunctionObject *)object);
    return record != NULL ? 1 : PyErr_Occurred() ? -1 : 0;
}

static int
soac_dataclass_original_code(SoacDataclassRecord *record, PyFunctionObject *function)
{
    /* GIL-only protocol: compare a callback-free weak reference with code
     * already pinned by the actual function. Never dereference an old code
     * pointer, acquire its last reference, or run a code-extra finalizer. */
    return record->code_weakref != NULL &&
        ((PyWeakReference *)record->code_weakref)->wr_object == function->func_code;
}

int
PyFunction_MatchesSoacDataclassCreation(PyObject *object, PyObject *inv_object,
                                       unsigned int role)
{
    if (object == NULL || !PyFunction_Check(object)) {
        PyErr_SetString(PyExc_TypeError, "dataclass creation query requires an exact function");
        return -1;
    }
    SoacDataclassRecord *record = soac_dataclass_record((PyFunctionObject *)object);
    if (record == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(inv_object);
    if (invocation == NULL) {
        return -1;
    }
    if (record->invocation != invocation || record->role != role) {
        return 0;
    }
    if (invocation->owner == NULL ||
        invocation->phase >= SOAC_DC_COMPLETE ||
        !soac_dataclass_original_code(record, (PyFunctionObject *)object)) {
        return soac_dataclass_fail(invocation, "generated function adoption provenance expired");
    }
    return 1;
}

static int
soac_dataclass_check_creation(_PyInterpreterFrame *producer, PyObject *code,
                              unsigned int *role)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)producer->soac_dataclass_invocation;
    if (!soac_dataclass_is_running(invocation)) {
        return 0;
    }
    if (soac_dataclass_current(invocation) < 0 || invocation->owner == NULL ||
        !soac_dataclass_frame_valid(producer) || !PyCode_Check(code) ||
        (((PyCodeObject *)code)->co_flags & CO_FUTURE_STRICT) ||
        ((PyCodeObject *)code)->_co_soac_strict_source_id != 0) {
        return soac_dataclass_fail(invocation, "generated function producer is not authentic");
    }
    PySoacDataclassFrameView view = {
        .frame = producer, .role = producer->soac_dataclass_role,
        .invocation = (PyObject *)invocation,
    };
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.create(
        invocation->owner, &view, code, role);
    if (status < 0 || status > 1 || PyErr_Occurred() ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        (status == 1 && *role == 0)) {
        return soac_dataclass_fail(invocation, "generated function creation did not authenticate");
    }
    return status;
}

static SoacDataclassRecord *
soac_dataclass_new_record(_PyInterpreterFrame *producer, PyObject *code)
{
    if (producer == NULL || producer->soac_dataclass_invocation == NULL) {
        return NULL;
    }
    unsigned int role = 0;
    if (soac_dataclass_check_creation(producer, code, &role) != 1) {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->soac.dataclass_creation_counter == UINT64_MAX) {
        soac_dataclass_error("dataclass creation identity space is exhausted");
        return NULL;
    }
    PyObject *code_weakref = PyWeakref_NewRef(code, NULL);
    if (code_weakref == NULL) {
        return NULL;
    }
    SoacDataclassRecord *record = PyObject_GC_New(
        SoacDataclassRecord, &SoacDataclassRecord_Type);
    if (record == NULL) {
        Py_DECREF(code_weakref);
        return NULL;
    }
    if (interp->soac.dataclass_creation_counter == UINT64_MAX) {
        PyObject_GC_Del(record);
        Py_DECREF(code_weakref);
        soac_dataclass_error("dataclass creation identity space is exhausted");
        return NULL;
    }
    record->invocation = (SoacDataclassInvocation *)Py_NewRef(
        producer->soac_dataclass_invocation);
    record->code_weakref = code_weakref;
    record->boundary = NULL;
    record->function = NULL;
    record->identity = ++interp->soac.dataclass_creation_counter;
    record->role = role;
    record->phase = SOAC_DC_RECORD_CONSTRUCTING;
    PyObject_GC_Track(record);
    return record;
}

static int
soac_dataclass_attach_record(PyFunctionObject *function, SoacDataclassRecord *record,
                             _PyInterpreterFrame *producer)
{
    unsigned int role = 0;
    if (soac_dataclass_check_creation(producer, function->func_code, &role) != 1 ||
        role != record->role || producer->soac_dataclass_invocation != (PyObject *)record->invocation) {
        return soac_dataclass_fail(record->invocation,
                                  "generated function producer changed during allocation");
    }
    record->function = function;
    function->func_soac_strict_owner = Py_NewRef((PyObject *)record);
    function->func_soac_strict_owner_state = FUNC_SOAC_OWNER_ATTACHED;
    PySoacDataclassFrameView view = {
        .frame = producer, .role = producer->soac_dataclass_role,
        .invocation = (PyObject *)record->invocation,
    };
    /* Unlike the pure validators, Created may allocate. Capture its owner
     * before callbacks can expire the invocation and release its GC edge. */
    PyObject *owner = Py_NewRef(record->invocation->owner);
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.created(
        owner, (PyObject *)record->invocation, &view,
        (PyObject *)function, role);
    /* Created may allocate an immutable check plan. Publish no function until
     * its exact executing producer and all native binding coordinates survive
     * those callbacks. Constructor attributes are populated only afterward. */
    unsigned int rechecked_role = 0;
    if (status != 0 || PyErr_Occurred() ||
        soac_dataclass_check_creation(producer, function->func_code,
                                      &rechecked_role) != 1 ||
        rechecked_role != role || record->phase != SOAC_DC_RECORD_CONSTRUCTING ||
        producer->soac_dataclass_invocation != (PyObject *)record->invocation ||
        function->func_soac_strict_owner != (PyObject *)record ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        !soac_dataclass_original_code(record, function) ||
        soac_dataclass_boundary_publication(function, record) < 0) {
        /* Releasing the captured owner can run observers of an allocated
         * weak function witness. They must see a terminal actual function
         * even before the constructor performs its normal reference drop. */
        record->function = NULL;
        record->phase = SOAC_DC_RECORD_TERMINAL;
        function->func_soac_strict_owner_state = FUNC_SOAC_OWNER_TERMINAL;
        soac_dataclass_fail(record->invocation,
                            "generated function changed before publication");
        PyObject *exception = PyErr_GetRaisedException();
        Py_DECREF(owner);
        PyErr_SetRaisedException(exception);
        return -1;
    }
    assert(record->invocation->owner == owner);
    Py_DECREF(owner);  /* Active invocation still owns it; no callback. */
    record->phase = SOAC_DC_RECORD_FRESH;
    return 0;
}

static int
soac_dataclass_member_traverse(SoacDataclassMember *member, visitproc visit, void *arg)
{
    Py_VISIT(member->invocation);
    Py_VISIT(member->record);
    Py_VISIT(member->actual_type);
    Py_VISIT(member->class_owner);
    Py_VISIT(member->name);
    Py_VISIT(member->function);
    return 0;
}

static int
soac_dataclass_member_clear(SoacDataclassMember *member)
{
    member->pending_birth = NULL;
    member->pending_identity = 0;
    if (!member->finished && member->invocation != NULL) {
        member->finished = 1;
        soac_dataclass_expire(member->invocation, SOAC_DC_FAILED);
    }
    Py_CLEAR(member->invocation);
    Py_CLEAR(member->record);
    Py_CLEAR(member->actual_type);
    Py_CLEAR(member->class_owner);
    Py_CLEAR(member->name);
    Py_CLEAR(member->function);
    return 0;
}

static void
soac_dataclass_member_dealloc(SoacDataclassMember *member)
{
    PyObject_GC_UnTrack(member);
    soac_dataclass_member_clear(member);
    PyObject_GC_Del(member);
}

static PyTypeObject SoacDataclassMember_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.DataclassMemberOperation",
    .tp_basicsize = sizeof(SoacDataclassMember),
    .tp_dealloc = (destructor)soac_dataclass_member_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_dataclass_member_traverse,
    .tp_clear = (inquiry)soac_dataclass_member_clear,
};

static int
soac_dataclass_validate_member(SoacDataclassInvocation *invocation,
                              PyObject *actual_type, PyObject *class_owner,
                              PyObject *name, PyObject *function,
                              SoacDataclassRecord *record)
{
    if (soac_dataclass_current(invocation) < 0 ||
        invocation->phase != SOAC_DC_APPLY_ACTIVE || invocation->owner == NULL ||
        actual_type != (PyObject *)invocation->actual_type ||
        class_owner == NULL || class_owner != invocation->class_owner ||
        !PyType_Check(actual_type) ||
        _PySOAC_GetTypeConstructionOwner(actual_type) != class_owner ||
        name == NULL || !PyUnicode_CheckExact(name) ||
        function == NULL || !PyFunction_Check(function) ||
        record == NULL || record->function != (PyFunctionObject *)function ||
        record->invocation != invocation ||
        ((PyFunctionObject *)function)->func_soac_strict_owner != (PyObject *)record ||
        !soac_dataclass_original_code(record, (PyFunctionObject *)function) ||
        record->role < Py_SOAC_DATACLASS_MEMBER ||
        record->role > Py_SOAC_DATACLASS_FROZEN_DELATTR) {
        return soac_dataclass_fail(invocation, "dataclass member operands are not authentic");
    }
    if ((record->role == Py_SOAC_DATACLASS_FROZEN_SETATTR) !=
            _PyUnicode_Equal(name, &_Py_ID(__setattr__)) ||
        (record->role == Py_SOAC_DATACLASS_FROZEN_DELATTR) !=
            _PyUnicode_Equal(name, &_Py_ID(__delattr__))) {
        return soac_dataclass_fail(invocation, "dataclass generated hook name does not match its role");
    }
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.validate_member(
        invocation->owner, actual_type, class_owner, name, function, record->role);
    if (status != 0 || PyErr_Occurred() || invocation->phase != SOAC_DC_APPLY_ACTIVE) {
        return soac_dataclass_fail(invocation, "dataclass member validation failed");
    }
    return 0;
}

PyObject *
_PySOAC_DataclassBeginMember(PyObject *object, PyObject *actual_type,
                            PyObject *class_owner, PyObject *name,
                            PyObject *function)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return NULL;
    }
    SoacDataclassRecord *record = function != NULL && PyFunction_Check(function)
        ? soac_dataclass_record((PyFunctionObject *)function) : NULL;
    if (invocation->member_pending || record == NULL ||
        record->phase != SOAC_DC_RECORD_FRESH ||
        soac_dataclass_validate_member(invocation, actual_type, class_owner,
                                       name, function, record) < 0) {
        soac_dataclass_fail(invocation, "dataclass member record cannot be consumed");
        return NULL;
    }
    record->phase = SOAC_DC_RECORD_CONSUMING;
    invocation->member_pending = 1;
    SoacDataclassMember *member = PyObject_GC_New(
        SoacDataclassMember, &SoacDataclassMember_Type);
    if (member == NULL) {
        soac_dataclass_fail(invocation, "dataclass member allocation failed");
        return NULL;
    }
    member->invocation = (SoacDataclassInvocation *)Py_NewRef(object);
    member->record = (SoacDataclassRecord *)Py_NewRef((PyObject *)record);
    member->actual_type = Py_NewRef(actual_type);
    member->class_owner = Py_NewRef(class_owner);
    member->name = Py_NewRef(name);
    member->function = Py_NewRef(function);
    member->pending_birth = NULL;
    member->pending_identity = 0;
    member->finished = 0;
    PyObject_GC_Track(member);
    /* Metadata is permanent BEFORE a dictionary watcher can observe the
     * installation. Operation allocation may have run GC callbacks; validate
     * again BEFORE the seal, not just after it. This never sets a code
     * execution or source/JIT flag. */
    if (soac_dataclass_validate_member(invocation, actual_type, class_owner,
                                       name, function, record) < 0 ||
        PyFunction_SealSoacStrict(function, record->identity) < 0 ||
        _PySOAC_DataclassCheckMember((PyObject *)member, actual_type,
                                    class_owner, name, function) < 0) {
        soac_dataclass_fail(invocation, "dataclass member sealing failed");
        Py_DECREF(member);
        return NULL;
    }
    return (PyObject *)member;
}

int
_PySOAC_DataclassCheckMember(PyObject *object, PyObject *actual_type,
                            PyObject *class_owner, PyObject *name,
                            PyObject *function)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassMember_Type)) {
        return soac_dataclass_error("expected a native dataclass member operation");
    }
    SoacDataclassMember *member = (SoacDataclassMember *)object;
    if (member->invocation == NULL) {
        return soac_dataclass_error("dataclass member operation is terminal");
    }
    if (member->finished || !member->invocation->member_pending ||
        member->actual_type != actual_type || member->class_owner != class_owner ||
        name == NULL || !PyUnicode_CheckExact(name) ||
        !_PyUnicode_Equal(member->name, name) || member->function != function ||
        member->record->phase != SOAC_DC_RECORD_CONSUMING ||
        ((PyFunctionObject *)function)->func_soac_strict_id != member->record->identity) {
        return soac_dataclass_fail(member->invocation, "dataclass member operation was replayed or changed");
    }
    return soac_dataclass_validate_member(member->invocation, actual_type,
                                          class_owner, name, function, member->record);
}

int
_PySOAC_DataclassPendingMemberCheck(PyObject *object, PyObject *dict,
                                    PyObject *name, PyObject *function,
                                    PyObject *owner)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassMember_Type)) {
        return soac_dataclass_error("expected a native pending member operation");
    }
    SoacDataclassMember *member = (SoacDataclassMember *)object;
    /* Never dereference a target saved before an equality/watcher callback. */
    member->pending_birth = NULL;
    member->pending_identity = 0;
    if (_PySOAC_DataclassCheckMember(object, member->actual_type,
                                     owner, name, function) < 0) return -1;
    uint64_t *target = NULL;
    int selected = _PySOAC_PendingTypeMemberTarget(
        member->actual_type, owner, member->invocation->pending_type_root,
        dict, name, member->record->role, &target);
    if (selected < 0) {
        return soac_dataclass_fail(member->invocation,
                                   "pending member native state changed");
    }
    if (selected == 1) {
        member->pending_birth = target;
        member->pending_identity = member->record->identity;
    }
    return selected;
}

void
_PySOAC_DataclassPendingMemberCommit(PyObject *object)
{
    assert(object != NULL && Py_IS_TYPE(object, &SoacDataclassMember_Type));
    SoacDataclassMember *member = (SoacDataclassMember *)object;
    assert(!member->finished && member->pending_identity != 0 &&
           member->record->phase == SOAC_DC_RECORD_CONSUMING &&
           member->record->identity == member->pending_identity);
    if (member->pending_birth != NULL) {
        *member->pending_birth = member->pending_identity;
    }
    member->record->phase = SOAC_DC_RECORD_INSTALLED;
    member->pending_birth = NULL;
    member->pending_identity = 0;
}

int
_PySOAC_DataclassMatchesInstalledHook(PyObject *object, uint64_t birth,
                                      unsigned int role)
{
    if (object == NULL || !PyFunction_Check(object) || birth == 0 ||
        (role != Py_SOAC_DATACLASS_FROZEN_SETATTR &&
         role != Py_SOAC_DATACLASS_FROZEN_DELATTR)) return 0;
    PyFunctionObject *function = (PyFunctionObject *)object;
    SoacDataclassRecord *record = soac_dataclass_record(function);
    if (record == NULL) return PyErr_Occurred() ? -1 : 0;
    /* COMPLETE may retain its immutable installed record, but no active
     * invocation/type owner is recovered or granted by this comparison. */
    return record->identity == birth && record->role == role &&
        record->phase == SOAC_DC_RECORD_INSTALLED &&
        function->func_soac_strict_id == birth &&
        soac_dataclass_original_code(record, function);
}

void
_PySOAC_DataclassFinishMember(PyObject *object, int succeeded)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassMember_Type)) {
        return;
    }
    SoacDataclassMember *member = (SoacDataclassMember *)object;
    member->pending_birth = NULL;
    member->pending_identity = 0;
    if (member->finished || member->invocation == NULL) {
        return;
    }
    member->finished = 1;
    member->invocation->member_pending = 0;
    if (succeeded && member->invocation->phase == SOAC_DC_APPLY_ACTIVE) {
        member->record->phase = SOAC_DC_RECORD_INSTALLED;
    }
    else {
        /* Preserve the setter's actual exception while dropping active refs. */
        soac_dataclass_expire(member->invocation, SOAC_DC_FAILED);
    }
}

#include "soac_dataclass_boundary.inc"
#include "soac_dataclass_bridge.inc"
