/* Required generated-method checks. Included by soac_dataclass.inc.
 * A permanent function record owns immutable policy; an actual native frame
 * owns each bound activation. Neither permits original strict bytecode. */

#include "opcode_ids.h"

typedef struct {
    PyObject_HEAD
    PyObject *owner;
    Py_ssize_t parameter_count;
    unsigned char *factory_mask;
    Py_ssize_t site_count;
    PySoacDataclassValueSite *sites;
} SoacDataclassBoundary;

typedef enum {
    SOAC_DC_BINDING,
    SOAC_DC_BOUND,
    SOAC_DC_ACTIVATION_CLEARED,
} SoacDataclassActivationPhase;

typedef struct {
    PyObject_VAR_HEAD
    SoacDataclassBoundary *boundary;
    PyObject *owner;             /* Captured before callback-capable binding. */
    PyFunctionObject *function;  /* Comparison only; the native frame pins it. */
    PyObject *code;              /* Comparison only; the native frame pins it. */
    SoacDataclassRecord *record; /* Comparison only; actual function owns it. */
    SoacDataclassActivationPhase phase;
    unsigned char masks[1];      /* supplied[n], deferred[n] */
} SoacDataclassActivation;

static int
soac_dataclass_boundary_traverse(SoacDataclassBoundary *boundary,
                                  visitproc visit, void *arg)
{
    Py_VISIT(boundary->owner);
    return 0;
}

static int
soac_dataclass_boundary_clear(SoacDataclassBoundary *boundary)
{
    Py_CLEAR(boundary->owner);
    return 0;
}

static void
soac_dataclass_boundary_dealloc(SoacDataclassBoundary *boundary)
{
    PyObject_GC_UnTrack(boundary);
    soac_dataclass_boundary_clear(boundary);
    PyMem_Free(boundary->factory_mask);
    PyMem_Free(boundary->sites);
    PyObject_GC_Del(boundary);
}

static PyTypeObject SoacDataclassBoundary_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.GeneratedBoundary",
    .tp_basicsize = sizeof(SoacDataclassBoundary),
    .tp_dealloc = (destructor)soac_dataclass_boundary_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_dataclass_boundary_traverse,
    .tp_clear = (inquiry)soac_dataclass_boundary_clear,
};

static int
soac_dataclass_activation_traverse(SoacDataclassActivation *activation,
                                    visitproc visit, void *arg)
{
    Py_VISIT(activation->boundary);
    Py_VISIT(activation->owner);
    return 0;
}

static int
soac_dataclass_activation_clear(SoacDataclassActivation *activation)
{
    activation->phase = SOAC_DC_ACTIVATION_CLEARED;
    activation->function = NULL;
    activation->code = NULL;
    activation->record = NULL;
    Py_CLEAR(activation->owner);
    Py_CLEAR(activation->boundary);
    return 0;
}

static void
soac_dataclass_activation_dealloc(SoacDataclassActivation *activation)
{
    PyObject_GC_UnTrack(activation);
    soac_dataclass_activation_clear(activation);
    PyObject_GC_Del(activation);
}

static PyTypeObject SoacDataclassActivation_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.GeneratedCall",
    .tp_basicsize = offsetof(SoacDataclassActivation, masks),
    .tp_itemsize = 1,
    .tp_dealloc = (destructor)soac_dataclass_activation_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_dataclass_activation_traverse,
    .tp_clear = (inquiry)soac_dataclass_activation_clear,
};

static int
soac_dataclass_boundary_shape(PyCodeObject *code, Py_ssize_t count)
{
    return !(code->co_flags & (CO_FUTURE_STRICT | CO_GENERATOR | CO_COROUTINE |
                               CO_ASYNC_GENERATOR | CO_VARARGS | CO_VARKEYWORDS)) &&
        (code->co_flags & CO_OPTIMIZED) && code->_co_soac_strict_source_id == 0 &&
        count == (Py_ssize_t)code->co_argcount + code->co_kwonlyargcount &&
        count > 0 && count <= PY_SSIZE_T_MAX / 2;
}

static int
soac_dataclass_configuration_live(SoacDataclassInvocation *invocation,
                                   PyFunctionObject *function,
                                   SoacDataclassRecord *record, PyObject *code)
{
    if (soac_dataclass_current(invocation) < 0 ||
        invocation->phase != SOAC_DC_APPLY_ACTIVE || invocation->owner == NULL ||
        invocation->class_owner == NULL || invocation->actual_type == NULL ||
        record->invocation != invocation || record->function != function ||
        record->phase != SOAC_DC_RECORD_CONSTRUCTING || record->boundary != NULL ||
        record->role != Py_SOAC_DATACLASS_MEMBER ||
        function->func_code != code || !soac_dataclass_original_code(record, function) ||
        function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        function->func_soac_strict_owner != (PyObject *)record ||
        function->func_soac_strict_id != 0 || function->func_soac_required_boundary ||
        function->vectorcall != _PyFunction_Vectorcall) {
        return soac_dataclass_fail(invocation,
                                  "generated boundary needs its exact unpublished function");
    }
    return 0;
}

int
PyFunction_ConfigureSoacDataclassBoundary(
    PyObject *object, PyObject *function, PyObject *verified_code,
    PyObject *check_owner, const PySoacDataclassBoundarySpec *spec, size_t spec_size)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (function == NULL || !PyFunction_Check(function) ||
        verified_code == NULL || !PyCode_Check(verified_code) || check_owner == NULL ||
        spec == NULL || spec_size != sizeof(*spec) ||
        spec->abi_version != Py_SOAC_DATACLASS_BOUNDARY_ABI ||
        !soac_dataclass_boundary_shape((PyCodeObject *)verified_code, spec->parameter_count) ||
        spec->factory_mask == NULL || spec->value_site_count < 0 ||
        spec->value_site_count > spec->parameter_count ||
        (spec->value_site_count != 0 && spec->value_sites == NULL)) {
        return soac_dataclass_fail(invocation, "invalid generated boundary specification");
    }
    PyFunctionObject *fn = (PyFunctionObject *)function;
    SoacDataclassRecord *record = soac_dataclass_record(fn);
    if (record == NULL ||
        soac_dataclass_configuration_live(invocation, fn, record, verified_code) < 0) {
        return soac_dataclass_fail(invocation, "generated boundary cannot be configured");
    }
    /* Copy caller-owned arrays before any callback-capable allocation. They
     * are thereafter immutable native metadata, never Python attributes. */
    unsigned char *factory_mask = PyMem_Malloc(spec->parameter_count);
    PySoacDataclassValueSite *sites = spec->value_site_count == 0 ? NULL
        : PyMem_Malloc(sizeof(*sites) * spec->value_site_count);
    if (factory_mask == NULL || (spec->value_site_count != 0 && sites == NULL)) {
        PyMem_Free(factory_mask);
        PyMem_Free(sites);
        PyErr_NoMemory();
        return soac_dataclass_fail(invocation, "generated boundary allocation failed");
    }
    Py_ssize_t count = spec->parameter_count;
    Py_ssize_t site_count = spec->value_site_count;
    memcpy(factory_mask, spec->factory_mask, count);
    if (site_count != 0) {
        memcpy(sites, spec->value_sites, sizeof(*sites) * site_count);
    }
    Py_ssize_t factories = 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        if (factory_mask[i] > 1) {
            goto invalid;
        }
        factories += factory_mask[i];
    }
    if (factories != site_count) {
        goto invalid;
    }
    /* Cold materialization is allowed during Created. Only an actual CALL
     * instruction may be a value site, never a cache or arbitrary offset. */
    PyObject *bytecode = PyCode_GetCode((PyCodeObject *)verified_code);
    if (bytecode == NULL) {
        PyMem_Free(factory_mask);
        PyMem_Free(sites);
        return soac_dataclass_fail(invocation, "generated boundary code is unavailable");
    }
    Py_ssize_t units = PyBytes_GET_SIZE(bytecode) / sizeof(_Py_CODEUNIT);
    const unsigned char *bytes = (const unsigned char *)PyBytes_AS_STRING(bytecode);
    for (Py_ssize_t i = 0; i < site_count; i++) {
        Py_ssize_t offset = sites[i].instruction;
        Py_ssize_t parameter = sites[i].parameter_index;
        if (offset < 0 || offset >= units || parameter < 0 || parameter >= count ||
            !factory_mask[parameter] || (i > 0 && sites[i - 1].instruction >= offset) ||
            bytes[offset * sizeof(_Py_CODEUNIT)] != CALL) {
            Py_DECREF(bytecode);
            goto invalid;
        }
        for (Py_ssize_t j = 0; j < i; j++) {
            if (sites[j].parameter_index == parameter) {
                Py_DECREF(bytecode);
                goto invalid;
            }
        }
    }
    Py_DECREF(bytecode);
    SoacDataclassBoundary *boundary = PyObject_GC_New(
        SoacDataclassBoundary, &SoacDataclassBoundary_Type);
    if (boundary == NULL) {
        PyMem_Free(factory_mask);
        PyMem_Free(sites);
        return soac_dataclass_fail(invocation, "generated boundary allocation failed");
    }
    boundary->owner = Py_NewRef(check_owner);
    boundary->parameter_count = count;
    boundary->factory_mask = factory_mask;
    boundary->site_count = site_count;
    boundary->sites = sites;
    PyObject_GC_Track(boundary);
    if (soac_dataclass_configuration_live(invocation, fn, record, verified_code) < 0) {
        Py_DECREF(boundary);
        return -1;
    }
    /* No callback or allocation follows this permanent native publication. */
    record->boundary = (PyObject *)boundary;
    fn->func_soac_required_boundary = 1;
    PyFunction_SetVectorcall(fn, _PySOAC_DataclassCheckedVectorcall);
    return 0;

invalid:
    PyMem_Free(factory_mask);
    PyMem_Free(sites);
    return soac_dataclass_fail(invocation, "invalid generated factory mask or call site");
}

static int
soac_dataclass_boundary_publication(PyFunctionObject *function,
                                     SoacDataclassRecord *record)
{
    if (function->func_soac_strict_id != 0) {
        return soac_dataclass_error("generated metadata was sealed before constructor completion");
    }
    if (record->boundary == NULL) {
        return function->func_soac_required_boundary == 0 &&
            function->vectorcall == _PyFunction_Vectorcall ? 0
            : soac_dataclass_error("generated entry changed before publication");
    }
    SoacDataclassBoundary *boundary = (SoacDataclassBoundary *)record->boundary;
    return Py_IS_TYPE(boundary, &SoacDataclassBoundary_Type) &&
        boundary->owner != NULL && function->func_soac_required_boundary &&
        function->vectorcall == _PySOAC_DataclassCheckedVectorcall &&
        soac_dataclass_boundary_shape((PyCodeObject *)function->func_code,
                                      boundary->parameter_count) ? 0
        : soac_dataclass_error("required generated boundary changed before publication");
}

int
PyFunction_MatchesSoacDataclassBoundary(PyObject *object, PyObject *expected_owner)
{
    if (object == NULL || !PyFunction_Check(object) || expected_owner == NULL) {
        PyErr_SetString(PyExc_TypeError, "generated boundary match needs an exact function and owner");
        return -1;
    }
    PyFunctionObject *function = (PyFunctionObject *)object;
    SoacDataclassRecord *record = soac_dataclass_record(function);
    if (record == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    if (record->boundary == NULL) {
        return function->func_soac_required_boundary
            ? soac_dataclass_error("required generated record has no check delegate") : 0;
    }
    if (!Py_IS_TYPE(record->boundary, &SoacDataclassBoundary_Type) ||
        !function->func_soac_required_boundary ||
        !soac_dataclass_original_code(record, function)) {
        return soac_dataclass_error("generated check delegate is no longer authentic");
    }
    SoacDataclassBoundary *boundary = (SoacDataclassBoundary *)record->boundary;
    if (boundary->owner == NULL ||
        !soac_dataclass_boundary_shape((PyCodeObject *)function->func_code,
                                       boundary->parameter_count)) {
        return soac_dataclass_error("generated check delegate is unavailable");
    }
    return boundary->owner == expected_owner;
}

static int
soac_dataclass_closure_complete(PyFunctionObject *function)
{
    PyCodeObject *code = (PyCodeObject *)function->func_code;
    if ((code->co_nfreevars != 0 && function->func_closure == NULL) ||
        (function->func_closure != NULL &&
         (!PyTuple_CheckExact(function->func_closure) ||
          PyTuple_GET_SIZE(function->func_closure) != code->co_nfreevars))) {
        return soac_dataclass_error("required generated function construction is incomplete");
    }
    for (int i = 0; i < code->co_nfreevars; i++) {
        if (!PyCell_Check(PyTuple_GET_ITEM(function->func_closure, i))) {
            return soac_dataclass_error("required generated closure has an invalid cell");
        }
    }
    return 0;
}

static SoacDataclassActivation *
soac_dataclass_new_activation(PyFunctionObject *function)
{
    SoacDataclassRecord *record = soac_dataclass_record(function);
    if (record == NULL || record->boundary == NULL ||
        !Py_IS_TYPE(record->boundary, &SoacDataclassBoundary_Type) ||
        !function->func_soac_required_boundary ||
        !soac_dataclass_original_code(record, function)) {
        soac_dataclass_error("required generated entry has no live native boundary");
        return NULL;
    }
    SoacDataclassBoundary *boundary = (SoacDataclassBoundary *)record->boundary;
    PyCodeObject *code = (PyCodeObject *)function->func_code;
    if (boundary->owner == NULL || !soac_dataclass_boundary_shape(code, boundary->parameter_count)) {
        soac_dataclass_error("required generated boundary layout is unavailable");
        return NULL;
    }
    /* MAKE_FUNCTION publishes CREATE before SET_FUNCTION_ATTRIBUTE installs
     * closure/defaults. Calling an incomplete closure must not dereference
     * missing cells. Later unsealed closure contents retain ordinary behavior. */
    if (soac_dataclass_closure_complete(function) < 0) {
        return NULL;
    }
    Py_INCREF(boundary);
    PyObject *owner = Py_NewRef(boundary->owner);
    SoacDataclassActivation *activation = PyObject_GC_NewVar(
        SoacDataclassActivation, &SoacDataclassActivation_Type,
        2 * boundary->parameter_count);
    if (activation == NULL) {
        Py_DECREF(owner);
        Py_DECREF(boundary);
        return NULL;
    }
    activation->boundary = boundary;
    activation->owner = owner;
    activation->function = function;
    activation->code = (PyObject *)code;
    activation->record = record;
    activation->phase = SOAC_DC_BINDING;
    memset(activation->masks, 0, 2 * boundary->parameter_count);
    PyObject_GC_Track(activation);
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        function->func_soac_strict_owner != (PyObject *)record ||
        record->boundary != (PyObject *)boundary || record->function != function ||
        function->func_code != (PyObject *)code ||
        !function->func_soac_required_boundary ||
        soac_dataclass_closure_complete(function) < 0 ||
        PyFunction_CheckSoacStrictDefaults((PyObject *)function) < 0) {
        soac_dataclass_error("required generated entry changed during allocation");
        Py_DECREF(activation);
        return NULL;
    }
    return activation;
}

PyObject *
_PySOAC_DataclassCheckedVectorcall(PyObject *object, PyObject *const *args,
                                   size_t nargsf, PyObject *kwnames)
{
    if (object == NULL || !PyFunction_Check(object)) {
        PyErr_SetString(PyExc_TypeError, "required generated entry needs an exact function");
        return NULL;
    }
    SoacDataclassActivation *activation = soac_dataclass_new_activation(
        (PyFunctionObject *)object);
    if (activation == NULL) {
        return NULL;
    }
    PyObject *result = _PySOAC_DataclassEvalCheckedVector(
        _PyThreadState_GET(), (PyFunctionObject *)object, args,
        PyVectorcall_NARGS(nargsf), kwnames, (PyObject *)activation);
    Py_DECREF(activation);
    return result;
}

unsigned char *
_PySOAC_DataclassSuppliedMask(PyObject *object)
{
    assert(Py_IS_TYPE(object, &SoacDataclassActivation_Type));
    SoacDataclassActivation *activation = (SoacDataclassActivation *)object;
    assert(activation->phase == SOAC_DC_BINDING);
    return activation->masks;
}

static int
soac_dataclass_activation_matches(SoacDataclassActivation *activation,
                                   _PyInterpreterFrame *frame)
{
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    return activation->boundary != NULL && activation->owner != NULL &&
        function == (PyObject *)activation->function && function != NULL &&
        PyFunction_Check(function) &&
        (PyObject *)_PyFrame_GetCode(frame) == activation->code &&
        ((PyFunctionObject *)function)->func_soac_strict_owner_state == FUNC_SOAC_OWNER_ATTACHED &&
        ((PyFunctionObject *)function)->func_soac_strict_owner == (PyObject *)activation->record &&
        ((PyFunctionObject *)function)->func_soac_required_boundary;
}

int
_PySOAC_DataclassCheckBound(_PyInterpreterFrame *frame)
{
    PyObject *object = frame->soac_checked_activation;
    if (object == NULL || !Py_IS_TYPE(object, &SoacDataclassActivation_Type)) {
        return soac_dataclass_error("generated binding has no captured activation");
    }
    SoacDataclassActivation *activation = (SoacDataclassActivation *)object;
    if (activation->phase != SOAC_DC_BINDING ||
        !soac_dataclass_activation_matches(activation, frame) ||
        soac_dataclass_closure_complete(activation->function) < 0) {
        return soac_dataclass_error("generated binding changed before checks");
    }
    SoacDataclassBoundary *boundary = activation->boundary;
    Py_ssize_t count = boundary->parameter_count;
    unsigned char *deferred = activation->masks + count;
    PySoacDataclassFrameView view = {.frame = frame, .role = 0, .invocation = NULL};
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.bound(
        activation->owner, &view, activation->masks, deferred, count);
    if (status != 0 || PyErr_Occurred() ||
        !soac_dataclass_activation_matches(activation, frame) ||
        soac_dataclass_closure_complete(activation->function) < 0) {
        return soac_dataclass_error("required generated argument check failed");
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        if (deferred[i] > 1 || (deferred[i] &&
            (activation->masks[i] || !boundary->factory_mask[i]))) {
            return soac_dataclass_error("invalid generated factory deferral");
        }
    }
    activation->phase = SOAC_DC_BOUND;
    return 0;
}

int
_PySOAC_DataclassCheckRequiredFrame(_PyInterpreterFrame *frame)
{
    PyObject *function = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    PyObject *object = frame->soac_checked_activation;
    if (object == NULL) {
        if (function == NULL || !PyFunction_Check(function)) {
            return 0;
        }
        PyFunctionObject *fn = (PyFunctionObject *)function;
        if (fn->func_soac_required_boundary &&
            fn->func_soac_strict_owner_state == FUNC_SOAC_OWNER_TERMINAL) {
            return soac_dataclass_error("required generated function owner is terminal");
        }
        if (!fn->func_soac_required_boundary || fn->func_soac_strict_owner == NULL ||
            !Py_IS_TYPE(fn->func_soac_strict_owner, &SoacDataclassRecord_Type)) {
            return 0;
        }
        return soac_dataclass_error("required generated body needs its checked native entry");
    }
    if (!Py_IS_TYPE(object, &SoacDataclassActivation_Type)) {
        return soac_dataclass_error("invalid generated frame activation");
    }
    SoacDataclassActivation *activation = (SoacDataclassActivation *)object;
    return activation->phase == SOAC_DC_BOUND &&
        soac_dataclass_activation_matches(activation, frame) ? 0
        : soac_dataclass_error("generated frame has no completed argument checks");
}

static const PySoacDataclassValueSite *
soac_dataclass_value_site(SoacDataclassActivation *activation,
                           _PyInterpreterFrame *frame)
{
    Py_ssize_t offset = _PyInterpreterFrame_LASTI(frame);
    Py_ssize_t low = 0, high = activation->boundary->site_count;
    while (low < high) {
        Py_ssize_t mid = low + (high - low) / 2;
        const PySoacDataclassValueSite *site = &activation->boundary->sites[mid];
        if (site->instruction < offset) {
            low = mid + 1;
        }
        else if (site->instruction > offset) {
            high = mid;
        }
        else {
            return site;
        }
    }
    return NULL;
}

int
_PySOAC_DataclassHasValueSite(_PyInterpreterFrame *frame)
{
    PyObject *object = frame->soac_checked_activation;
    if (object == NULL || _PySOAC_InterpreterIsActivation(object)) {
        return 0;
    }
    if (!Py_IS_TYPE(object, &SoacDataclassActivation_Type)) {
        return 1;  /* Deopt to explicit failure, never bypass on invalid state. */
    }
    SoacDataclassActivation *activation = (SoacDataclassActivation *)object;
    return activation->phase != SOAC_DC_BOUND ||
        !soac_dataclass_activation_matches(activation, frame) ||
        soac_dataclass_value_site(activation, frame) != NULL;
}

static PyObject *
soac_dataclass_checked_value_call(_PyInterpreterFrame *frame, PyObject *callable,
                                   PyObject *const *args, Py_ssize_t nargs,
                                   PyObject *kwnames)
{
    if (_PySOAC_DataclassCheckRequiredFrame(frame) < 0) {
        return NULL;
    }
    SoacDataclassActivation *activation =
        (SoacDataclassActivation *)frame->soac_checked_activation;
    const PySoacDataclassValueSite *site = soac_dataclass_value_site(activation, frame);
    PyObject *canonical = PySoac_GetDataclassBuiltin(Py_SOAC_DATACLASS_BRIDGE_VALUE);
    if (site == NULL || canonical == NULL || callable != canonical || nargs != 1 ||
        (kwnames != NULL && PyTuple_GET_SIZE(kwnames) != 0)) {
        soac_dataclass_error("required generated value site did not call its exact native bridge");
        return NULL;
    }
    Py_ssize_t parameter = site->parameter_index;
    unsigned char *deferred = activation->masks + activation->boundary->parameter_count;
    if (deferred[parameter]) {
        int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.value(
            activation->owner, parameter, args[0]);
        if (status != 0 || PyErr_Occurred()) {
            soac_dataclass_error("required generated factory result check failed");
            return NULL;
        }
    }
    /* Deferred remains an immutable classification: a traced repeated line
     * checks its newly evaluated value too. Supplied parameters are not
     * rechecked after ordinary body/tracing changes to their local values. */
    return Py_NewRef(args[0]);
}

static int
soac_dataclass_component_valid(SoacDataclassInvocation *invocation,
                                PyFunctionObject *method, SoacDataclassRecord *method_record,
                                PyFunctionObject *component, SoacDataclassRecord *component_record,
                                unsigned int kind, Py_ssize_t index,
                                SoacDataclassRecordPhase expected_phase)
{
    unsigned int expected_role = kind == Py_SOAC_DATACLASS_COMPONENT_ANNOTATE
        ? Py_SOAC_DATACLASS_ANNOTATION_PROVIDER : Py_SOAC_DATACLASS_REPR_IMPLEMENTATION;
    if (invocation->phase != SOAC_DC_APPLY_ACTIVE || invocation->owner == NULL ||
        method == component || method_record == NULL || component_record == NULL ||
        method_record->invocation != invocation || component_record->invocation != invocation ||
        method_record->function != method || component_record->function != component ||
        method->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        component->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        method->func_soac_strict_owner != (PyObject *)method_record ||
        component->func_soac_strict_owner != (PyObject *)component_record ||
        method_record->phase != SOAC_DC_RECORD_FRESH ||
        component_record->phase != expected_phase ||
        method_record->role != Py_SOAC_DATACLASS_MEMBER || component_record->role != expected_role ||
        !soac_dataclass_original_code(method_record, method) ||
        !soac_dataclass_original_code(component_record, component) ||
        (kind != Py_SOAC_DATACLASS_COMPONENT_ANNOTATE && kind != Py_SOAC_DATACLASS_COMPONENT_REPR)) {
        return soac_dataclass_fail(invocation, "generated component roles are not authentic");
    }
    if (kind == Py_SOAC_DATACLASS_COMPONENT_ANNOTATE) {
        if (index != -1 || method->func_annotate != (PyObject *)component) {
            return soac_dataclass_fail(invocation, "generated annotation component is not attached");
        }
    }
    else if (method->func_closure == NULL || !PyTuple_CheckExact(method->func_closure) ||
             index < 0 || index >= PyTuple_GET_SIZE(method->func_closure) ||
             !PyCell_Check(PyTuple_GET_ITEM(method->func_closure, index)) ||
             PyCell_GET(PyTuple_GET_ITEM(method->func_closure, index)) != (PyObject *)component) {
        return soac_dataclass_fail(invocation, "generated repr component is not the exact closure value");
    }
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.validate_component(
        invocation->owner, (PyObject *)method, (PyObject *)component, kind, index);
    if (status != 0 || PyErr_Occurred() || invocation->phase != SOAC_DC_APPLY_ACTIVE) {
        return soac_dataclass_fail(invocation, "generated component policy did not authenticate");
    }
    return 0;
}

int
PyFunction_AdoptSoacDataclassComponent(PyObject *object, PyObject *method_object,
                                      PyObject *component_object, unsigned int kind,
                                      Py_ssize_t closure_index)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (method_object == NULL || !PyFunction_Check(method_object) ||
        component_object == NULL || !PyFunction_Check(component_object)) {
        return soac_dataclass_fail(invocation, "generated component adoption needs exact functions");
    }
    PyFunctionObject *method = (PyFunctionObject *)method_object;
    PyFunctionObject *component = (PyFunctionObject *)component_object;
    SoacDataclassRecord *method_record = soac_dataclass_record(method);
    SoacDataclassRecord *component_record = soac_dataclass_record(component);
    if (soac_dataclass_component_valid(invocation, method, method_record,
                                      component, component_record, kind, closure_index,
                                      SOAC_DC_RECORD_FRESH) < 0) {
        return -1;
    }
    component_record->phase = SOAC_DC_RECORD_CONSUMING;
    if (PyFunction_SealSoacStrict(component_object, component_record->identity) < 0 ||
        soac_dataclass_component_valid(invocation, method, method_record,
                                      component, component_record, kind, closure_index,
                                      SOAC_DC_RECORD_CONSUMING) < 0) {
        return soac_dataclass_fail(invocation, "generated component changed during adoption");
    }
    component_record->phase = SOAC_DC_RECORD_COMPONENT;
    return 0;
}
