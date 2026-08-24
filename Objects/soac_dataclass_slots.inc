/* Explicit replacement construction, included after bridge validation. The
 * opaque context exists on this C call's stack, not in TLS or Python state. */

struct _PySoacDataclassSlotsContext {
    PySoacDataclassFrameView view;
    SoacDataclassInvocation *invocation;
    PyObject *builtin;               /* Borrowed from this pinned CALL. */
    PyObject *const *arguments;      /* Five already-evaluated CALL operands. */
    PyObject *handle;                /* Owned, prevents identity reuse. */
    PyObject *expected_owner;        /* Owned only during construction. */
    int preparing;
    int handle_requested;
    int consuming;
};

int
_PySOAC_DataclassFailSlots(_PySoacDataclassSlotsContext *context,
                          const char *message)
{
    return context == NULL ? soac_dataclass_error(message)
        : soac_dataclass_fail(context->invocation, message);
}

static int
soac_dataclass_slots_context_live(_PySoacDataclassSlotsContext *context)
{
    if (context == NULL || context->view.slots_context != context ||
        context->view.invocation != (PyObject *)context->invocation ||
        context->view.frame->soac_dataclass_invocation != context->view.invocation ||
        context->invocation->phase != SOAC_DC_APPLY_ACTIVE ||
        !context->invocation->slots_started) {
        return _PySOAC_DataclassFailSlots(context, "replacement context is unavailable");
    }
    return soac_dataclass_validate_bridge(context->view.frame, context->builtin,
        Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS, context->arguments, 5);
}

static int
soac_dataclass_slots_spec_matches(_PySoacDataclassSlotsContext *context,
                                   const PySoacTypeConstructionSpec *spec)
{
    PyObject *const *arguments = context->arguments;
    if (spec == NULL || spec->abi_version != Py_SOAC_TYPE_CONTRACT_ABI ||
        spec->owner == NULL || spec->owner == context->invocation->class_owner ||
        (context->expected_owner != NULL && spec->owner != context->expected_owner) ||
        spec->namespace_function != NULL || spec->name != arguments[1] ||
        spec->bases != arguments[2] || spec->namespace_dict != arguments[3] ||
        spec->keywords == NULL || !PyDict_CheckExact(spec->keywords) ||
        PyDict_GET_SIZE(spec->keywords) != 0 ||
        spec->object_slot_fields == NULL || !PyTuple_CheckExact(spec->object_slot_fields)) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement handle plan does not match its exact operands");
    }
    return 0;
}

int
PySoac_DataclassMatchesSlotsClass(PyObject *object, PyObject *actual,
                                  PyObject *expected_owner)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassInvocation_Type)) {
        return 0;
    }
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (invocation->phase >= SOAC_DC_COMPLETE) {
        return soac_dataclass_error("dataclass replacement association has expired");
    }
    if (!invocation->slots_started || invocation->replacement_weakref == NULL ||
        actual == NULL || !PyType_Check(actual) ||
        invocation->replacement_type != (PyTypeObject *)actual ||
        invocation->replacement_owner != expected_owner) {
        return 0;
    }
    /* The weak reference owns no class and does not invoke callbacks. Compare
     * it only against the independently pinned actual operand, never by
     * dereferencing the stored raw address after that class could have died. */
    if (((PyWeakReference *)invocation->replacement_weakref)->wr_object != actual) {
        return soac_dataclass_error("dataclass replacement class is no longer live");
    }
    PyObject *owner = PyType_GetSoacContractOwner(actual);
    if (owner == NULL && PyErr_Occurred()) {
        return -1;
    }
    return owner != NULL && owner == expected_owner;
}

int
_PySOAC_DataclassValidateCopiedHook(_PySoacDataclassSlotsContext *context,
                                    PyObject *name, PyObject *function)
{
    if (context == NULL || soac_dataclass_slots_context_live(context) < 0 ||
        name == NULL || !PyUnicode_CheckExact(name) ||
        function == NULL || !PyFunction_Check(function)) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement frozen hook has no native producer");
    }
    unsigned int role;
    if (_PyUnicode_Equal(name, &_Py_ID(__setattr__))) {
        role = Py_SOAC_DATACLASS_FROZEN_SETATTR;
    }
    else if (_PyUnicode_Equal(name, &_Py_ID(__delattr__))) {
        role = Py_SOAC_DATACLASS_FROZEN_DELATTR;
    }
    else {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement cannot copy an unsupported attribute hook");
    }
    PyTypeObject *original = (PyTypeObject *)context->arguments[4];
    PyObject *installed = PyDict_GetItemWithError(original->tp_dict, name);
    SoacDataclassRecord *record = soac_dataclass_record((PyFunctionObject *)function);
    if (installed != function || record == NULL ||
        record->invocation != context->invocation || record->role != role ||
        record->phase != SOAC_DC_RECORD_INSTALLED ||
        ((PyFunctionObject *)function)->func_soac_strict_id == 0 ||
        !soac_dataclass_original_code(record, (PyFunctionObject *)function)) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement hook is not this invocation's installed frozen member");
    }
    return 0;
}

_PySoacDataclassSlotsContext *
_PySOAC_DataclassBeginSlotsHandle(const PySoacDataclassFrameView *producer,
                                  const PySoacTypeConstructionSpec *spec)
{
    _PySoacDataclassSlotsContext *context = producer == NULL ? NULL
        : producer->slots_context;
    if (context == NULL || producer != &context->view || !context->preparing ||
        context->handle_requested || context->handle != NULL ||
        soac_dataclass_slots_context_live(context) < 0 ||
        soac_dataclass_slots_spec_matches(context, spec) < 0) {
        _PySOAC_DataclassFailSlots(context,
                                  "replacement handle needs its one explicit preparation view");
        return NULL;
    }
    /* Reserve before allocation or GC can reenter the fixed callback. */
    context->handle_requested = 1;
    context->expected_owner = Py_NewRef(spec->owner);
    return context;
}

int
_PySOAC_DataclassRecordSlotsHandle(_PySoacDataclassSlotsContext *context,
                                   PyObject *handle,
                                   const PySoacTypeConstructionSpec *spec)
{
    if (context == NULL || !context->preparing || !context->handle_requested ||
        context->handle != NULL || handle == NULL ||
        soac_dataclass_slots_context_live(context) < 0 ||
        soac_dataclass_slots_spec_matches(context, spec) < 0) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement handle birth did not authenticate");
    }
    context->handle = Py_NewRef(handle);
    return 0;
}

int
_PySOAC_DataclassValidateSlotsHandle(_PySoacDataclassSlotsContext *context,
                                     PyObject *handle,
                                     const PySoacTypeConstructionSpec *spec)
{
    if (context == NULL || context->preparing || context->consuming ||
        context->handle != handle || context->expected_owner == NULL ||
        soac_dataclass_slots_context_live(context) < 0 ||
        soac_dataclass_slots_spec_matches(context, spec) < 0) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement handle consumption did not authenticate");
    }
    context->consuming = 1;
    return 0;
}

int
_PySOAC_DataclassBindSlotsType(_PySoacDataclassSlotsContext *context,
                               PyObject *actual, PyObject *owner)
{
    if (context == NULL || !context->consuming || context->preparing ||
        context->handle == NULL || owner != context->expected_owner ||
        actual == NULL || !PyType_Check(actual) ||
        Py_TYPE(actual) != &PyType_Type || actual == context->arguments[4] ||
        PyType_GetSoacContractOwner(actual) != owner ||
        soac_dataclass_slots_context_live(context) < 0) {
        return _PySOAC_DataclassFailSlots(context,
                                         "replacement type pre-Ready binding did not authenticate");
    }
    if (context->invocation->replacement_owner != NULL ||
        context->invocation->replacement_type != NULL ||
        context->invocation->replacement_weakref != NULL) {
        /* Later calls only revalidate this exact binding after callbacks. */
        if (PySoac_DataclassMatchesSlotsClass((PyObject *)context->invocation,
                                              actual, owner) != 1) {
            return _PySOAC_DataclassFailSlots(context,
                                             "replacement type binding cannot be replaced");
        }
        return 0;
    }
    /* Pinned GIL build: the exact builtin weakref allocator only schedules
     * GC via _PyObject_GC_TRACK -> _Py_TriggerGC; it does not run Python or
     * collect synchronously. NULL callback and native list insertion keep
     * the actual type unexposed until its allocation-free owner bind. */
    PyObject *weak = PyWeakref_NewRef(actual, NULL);
    if (weak == NULL || soac_dataclass_slots_context_live(context) < 0 ||
        PyType_GetSoacContractOwner(actual) != owner) {
        _PySOAC_DataclassFailSlots(context,
                                   "replacement type changed during weak binding");
        PyObject *exception = PyErr_GetRaisedException();
        Py_XDECREF(weak);
        PyErr_SetRaisedException(exception);
        return -1;
    }
    context->invocation->replacement_owner = Py_NewRef(owner);
    context->invocation->replacement_type = (PyTypeObject *)actual;
    context->invocation->replacement_weakref = weak;
    return 0;
}

static PyObject *
soac_dataclass_new_slots_bridge(_PyInterpreterFrame *parent, PyObject *builtin,
                                 PyObject *const *arguments, Py_ssize_t nargs)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    if (soac_dataclass_validate_bridge(parent, builtin,
            Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS, arguments, nargs) < 0) {
        return NULL;
    }
    if (invocation->slots_started || invocation->replacement_owner != NULL ||
        invocation->replacement_type != NULL || invocation->replacement_weakref != NULL ||
        invocation->member_pending) {
        soac_dataclass_fail(invocation, "dataclass replacement construction was already started");
        return NULL;
    }
    invocation->slots_started = 1;
    _PySoacDataclassSlotsContext context = {
        .view = {.frame = parent, .role = parent->soac_dataclass_role,
                 .invocation = (PyObject *)invocation},
        .invocation = invocation, .builtin = builtin, .arguments = arguments,
        .preparing = 1,
    };
    context.view.slots_context = &context;
    /* Preparation can allocate and expire the invocation. Pin the callback's
     * active GC owner before entering it, as for unpublished function Created. */
    PyObject *callback_owner = Py_NewRef(invocation->owner);
    PyObject *handle = NULL;
    PyObject *result = NULL;
    int prepared = _PyInterpreterState_GET()->soac.dataclass_callbacks.prepare_slots(
        callback_owner, &context.view, arguments[0], arguments[1], arguments[2],
        arguments[3], arguments[4], &handle);
    context.preparing = 0;
    if (prepared != 0 || PyErr_Occurred() || handle == NULL ||
        handle != context.handle || context.expected_owner == NULL ||
        soac_dataclass_slots_context_live(&context) < 0) {
        goto error;
    }
    result = _PySOAC_TypeFromDataclassSlotsHandle(&context, handle);
    if (result == NULL ||
        result != (PyObject *)invocation->replacement_type ||
        PyType_GetSoacContractOwner(result) != context.expected_owner ||
        soac_dataclass_slots_context_live(&context) < 0) {
        goto error;
    }
    /* Even a deliberately escaped consumed handle becomes inert. Its old
     * namespace/owner edges do not extend original class/helper lifetimes. */
    _PySOAC_ClearDataclassSlotsHandle(context.handle);
    if (soac_dataclass_slots_context_live(&context) < 0) {
        goto error;
    }
    Py_DECREF(handle);
    Py_DECREF(context.handle);
    Py_DECREF(context.expected_owner);
    Py_DECREF(callback_owner);
    return result;

error:
    /* Terminalize before releasing a prepared/escaped class, handle or owner;
     * preserve the original exception across every possible finalizer. */
    soac_dataclass_fail(invocation, "dataclass replacement construction failed");
    if (context.handle != NULL) {
        _PySOAC_ClearDataclassSlotsHandle(context.handle);
    }
    Py_XDECREF(result);
    Py_XDECREF(handle);
    Py_XDECREF(context.handle);
    Py_XDECREF(context.expected_owner);
    Py_DECREF(callback_owner);
    return NULL;
}
