/* Exact-type generated-boundary fixture. This is not source admission. */

typedef struct {
    PyObject_HEAD
    PyObject *types;
    PyObject *sentinels;
    PyObject *payload;
    unsigned char *supplied;
    unsigned long long bound_calls;
    unsigned long long value_calls;
} SoacBoundaryTestOwner;

static int
soac_boundary_test_traverse(SoacBoundaryTestOwner *owner, visitproc visit, void *arg)
{
    Py_VISIT(owner->types);
    Py_VISIT(owner->sentinels);
    Py_VISIT(owner->payload);
    return 0;
}

static int
soac_boundary_test_clear(SoacBoundaryTestOwner *owner)
{
    Py_CLEAR(owner->types);
    Py_CLEAR(owner->sentinels);
    Py_CLEAR(owner->payload);
    return 0;
}

static void
soac_boundary_test_dealloc(SoacBoundaryTestOwner *owner)
{
    PyObject_GC_UnTrack(owner);
    soac_boundary_test_clear(owner);
    PyMem_Free(owner->supplied);
    PyObject_GC_Del(owner);
}

static PyTypeObject SoacBoundaryTestOwner_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_testinternalcapi.GeneratedBoundaryOwner",
    .tp_basicsize = sizeof(SoacBoundaryTestOwner),
    .tp_dealloc = (destructor)soac_boundary_test_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_boundary_test_traverse,
    .tp_clear = (inquiry)soac_boundary_test_clear,
};

static PyObject *
soac_dataclass_boundary_owner(PyObject *self, PyObject *args)
{
    PyObject *types, *sentinels, *payload;
    if (!PyArg_ParseTuple(args, "OOO", &types, &sentinels, &payload)) {
        return NULL;
    }
    if (!PyTuple_CheckExact(types) || !PyTuple_CheckExact(sentinels) ||
        PyTuple_GET_SIZE(types) == 0 ||
        PyTuple_GET_SIZE(types) != PyTuple_GET_SIZE(sentinels)) {
        PyErr_SetString(PyExc_TypeError, "boundary fixture needs equal nonempty type/sentinel tuples");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(types); i++) {
        PyObject *type = PyTuple_GET_ITEM(types, i);
        if (type != Py_None && !PyType_Check(type)) {
            PyErr_SetString(PyExc_TypeError, "boundary fixture predicate needs a type or None");
            return NULL;
        }
    }
    if (PyType_Ready(&SoacBoundaryTestOwner_Type) < 0) {
        return NULL;
    }
    unsigned char *supplied = PyMem_Calloc(PyTuple_GET_SIZE(types), 1);
    if (supplied == NULL) {
        return PyErr_NoMemory();
    }
    SoacBoundaryTestOwner *owner = PyObject_GC_New(
        SoacBoundaryTestOwner, &SoacBoundaryTestOwner_Type);
    if (owner == NULL) {
        PyMem_Free(supplied);
        return NULL;
    }
    owner->types = Py_NewRef(types);
    owner->sentinels = Py_NewRef(sentinels);
    owner->payload = Py_NewRef(payload);
    owner->supplied = supplied;
    owner->bound_calls = 0;
    owner->value_calls = 0;
    PyObject_GC_Track(owner);
    return (PyObject *)owner;
}

static SoacBoundaryTestOwner *
soac_boundary_test_owner(PyObject *object)
{
    if (!Py_IS_TYPE(object, &SoacBoundaryTestOwner_Type) ||
        ((SoacBoundaryTestOwner *)object)->types == NULL) {
        PyErr_SetString(PyExc_TypeError, "invalid native boundary fixture owner");
        return NULL;
    }
    return (SoacBoundaryTestOwner *)object;
}

static PyObject *
soac_dataclass_boundary_state(PyObject *self, PyObject *object)
{
    SoacBoundaryTestOwner *owner = soac_boundary_test_owner(object);
    if (owner == NULL) {
        return NULL;
    }
    PyObject *supplied = PyTuple_New(PyTuple_GET_SIZE(owner->types));
    if (supplied == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(owner->types); i++) {
        PyTuple_SET_ITEM(supplied, i, PyBool_FromLong(owner->supplied[i]));
    }
    return Py_BuildValue("{s:N,s:K,s:K}", "supplied", supplied,
                         "bound_calls", owner->bound_calls,
                         "value_calls", owner->value_calls);
}

static int
soac_boundary_test_value(SoacBoundaryTestOwner *owner, Py_ssize_t index, PyObject *value)
{
    if (index < 0 || index >= PyTuple_GET_SIZE(owner->types) || value == NULL) {
        PyErr_SetString(PyExc_TypeError, "invalid native checked parameter slot");
        return -1;
    }
    PyObject *type = PyTuple_GET_ITEM(owner->types, index);
    if (type != Py_None && !Py_IS_TYPE(value, (PyTypeObject *)type)) {
        PyErr_Format(PyExc_TypeError, "generated parameter %zd requires exact %.100s",
                     index, ((PyTypeObject *)type)->tp_name);
        return -1;
    }
    return 0;
}

static int
soac_dc_test_bound(PyObject *object, const PySoacDataclassFrameView *frame,
                    const unsigned char *supplied, unsigned char *deferred,
                    Py_ssize_t count)
{
    if (PySoac_DataclassFrameInvocation(frame) != NULL) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "required boundary received construction authority");
        return -1;
    }
    SoacBoundaryTestOwner *owner = soac_boundary_test_owner(object);
    if (owner == NULL) {
        return -1;
    }
    if (count != PyTuple_GET_SIZE(owner->types)) {
        PyErr_SetString(PyExc_TypeError, "boundary fixture parameter count mismatch");
        return -1;
    }
    owner->bound_calls++;
    memcpy(owner->supplied, supplied, count);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *value = PySoac_DataclassFrameLocal(frame, i);
        PyObject *sentinel = PyTuple_GET_ITEM(owner->sentinels, i);
        if (sentinel != Py_None && !supplied[i] && value == sentinel) {
            deferred[i] = 1;
        }
        else if (soac_boundary_test_value(owner, i, value) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
soac_dc_test_checked_value(PyObject *object, Py_ssize_t parameter, PyObject *value)
{
    SoacBoundaryTestOwner *owner = soac_boundary_test_owner(object);
    if (owner == NULL) {
        return -1;
    }
    owner->value_calls++;
    return soac_boundary_test_value(owner, parameter, value);
}

static int
soac_dc_test_configure_boundary(PyObject *invocation, PyObject *function,
                                 PyObject *code, PyObject *owner,
                                 PyObject *mask, PyObject *input_sites)
{
    if (!PyBytes_CheckExact(mask) || !PyTuple_CheckExact(input_sites)) {
        PyErr_SetString(PyExc_TypeError, "fixture boundary needs bytes mask and tuple sites");
        return -1;
    }
    Py_ssize_t count = PyTuple_GET_SIZE(input_sites);
    PySoacDataclassValueSite *sites = count == 0 ? NULL : PyMem_Calloc(count, sizeof(*sites));
    if (count != 0 && sites == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *site = PyTuple_GET_ITEM(input_sites, i);
        if (!PyTuple_CheckExact(site) || PyTuple_GET_SIZE(site) != 2 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(site, 0)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(site, 1))) {
            PyMem_Free(sites);
            PyErr_SetString(PyExc_TypeError, "fixture site needs exact offset/index pair");
            return -1;
        }
        sites[i].instruction = PyLong_AsSsize_t(PyTuple_GET_ITEM(site, 0));
        sites[i].parameter_index = PyLong_AsSsize_t(PyTuple_GET_ITEM(site, 1));
        if (PyErr_Occurred()) {
            PyMem_Free(sites);
            return -1;
        }
    }
    PySoacDataclassBoundarySpec spec = {
        .abi_version = Py_SOAC_DATACLASS_BOUNDARY_ABI,
        .parameter_count = PyBytes_GET_SIZE(mask),
        .factory_mask = (const unsigned char *)PyBytes_AS_STRING(mask),
        .value_site_count = count,
        .value_sites = sites,
    };
    int result = PyFunction_ConfigureSoacDataclassBoundary(
        invocation, function, code, owner, &spec, sizeof(spec));
    PyMem_Free(sites);
    return result;
}

static PyObject *
soac_dataclass_configure_boundary(PyObject *self, PyObject *args)
{
    PyObject *invocation, *function, *code, *owner, *mask, *sites;
    if (!PyArg_ParseTuple(args, "OOOOOO", &invocation, &function, &code,
                          &owner, &mask, &sites) ||
        soac_dc_test_configure_boundary(invocation, function, code, owner, mask, sites) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
soac_dc_test_boundaries(PyObject *input)
{
    if (input == NULL) {
        return PyTuple_New(0);
    }
    if (!PyTuple_CheckExact(input)) {
        PyErr_SetString(PyExc_TypeError, "fixture boundary catalog requires a tuple");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(input); i++) {
        PyObject *item = PyTuple_GET_ITEM(input, i);
        if (!PyTuple_CheckExact(item) || PyTuple_GET_SIZE(item) != 4 ||
            !PyCode_Check(PyTuple_GET_ITEM(item, 0)) ||
            !Py_IS_TYPE(PyTuple_GET_ITEM(item, 1), &SoacBoundaryTestOwner_Type) ||
            !PyBytes_CheckExact(PyTuple_GET_ITEM(item, 2)) ||
            !PyTuple_CheckExact(PyTuple_GET_ITEM(item, 3))) {
            PyErr_SetString(PyExc_TypeError, "fixture boundary needs code, check owner, mask, and sites");
            return NULL;
        }
    }
    return Py_NewRef(input);
}

static int
soac_dc_test_created(PyObject *owner, PyObject *invocation,
                      const PySoacDataclassFrameView *producer,
                      PyObject *function, unsigned int role)
{
    if (PySoac_DataclassFrameInvocation(producer) != invocation) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "created fixture received a different invocation");
        return -1;
    }
    PyObject *boundaries = PyTuple_GET_ITEM(owner, 6);
    PyFunctionObject *fn = (PyFunctionObject *)function;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(boundaries); i++) {
        PyObject *entry = PyTuple_GET_ITEM(boundaries, i);
        if (fn->func_code != PyTuple_GET_ITEM(entry, 0)) {
            continue;
        }
        PyObject *root = PyTuple_GET_ITEM(owner, 0);
        if (role != Py_SOAC_DATACLASS_MEMBER ||
            fn->func_globals != PyTuple_GET_ITEM(root, 2) ||
            fn->func_builtins != PyTuple_GET_ITEM(root, 3)) {
            PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                            "fixture generated check environment mismatch");
            return -1;
        }
        if (soac_dc_test_configure_boundary(invocation, function, fn->func_code,
                PyTuple_GET_ITEM(entry, 1), PyTuple_GET_ITEM(entry, 2),
                PyTuple_GET_ITEM(entry, 3)) < 0) {
            return -1;
        }
        break;
    }
    PyObject *payload = PyTuple_GET_ITEM(owner, 4);
    if (PyTuple_CheckExact(payload) && PyTuple_GET_SIZE(payload) == 5 &&
        PyUnicode_CheckExact(PyTuple_GET_ITEM(payload, 0)) &&
        PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(payload, 0), "created-weak-v1") == 0) {
        PyObject *slots = PyTuple_GET_ITEM(payload, 1);
        PyObject *retain = PyTuple_GET_ITEM(payload, 2);
        PyObject *defaults = PyTuple_GET_ITEM(payload, 3);
        if (!PyList_CheckExact(slots) || PyList_GET_SIZE(slots) != 2 ||
            !PyBool_Check(retain) || !PyTuple_CheckExact(defaults)) {
            PyErr_SetString(PyExc_TypeError, "invalid fixture weak-creation payload");
            return -1;
        }
        /* Production configures before exposing a weak birth witness. The
         * retained variant models an observer acquiring that exact weak
         * referent, never a new function/code or an authority substitute. */
        PyObject *weak = PyWeakref_NewRef(function, NULL);
        if (weak == NULL) {
            return -1;
        }
        if (PyList_SetItem(slots, 0, Py_NewRef(weak)) < 0 ||
            PyFunction_SetDefaults(function, defaults) < 0) {
            Py_DECREF(weak);
            return -1;
        }
        if (retain == Py_True) {
            PyObject *observed = NULL;
            if (PyWeakref_GetRef(weak, &observed) != 1) {
                Py_DECREF(weak);
                PyErr_SetString(PyExc_RuntimeError, "unpublished weak witness expired early");
                return -1;
            }
            if (PyList_SetItem(slots, 1, observed) < 0) {
                Py_DECREF(weak);
                return -1;
            }
        }
        Py_DECREF(weak);
        PyErr_SetString(PyExc_RuntimeError, "fixture failed after unpublished weak witness");
        return -1;
    }
    return 0;
}

static PyObject *
soac_dc_test_components(PyObject *input)
{
    if (input == NULL) {
        return PyTuple_New(0);
    }
    if (!PyTuple_CheckExact(input)) {
        PyErr_SetString(PyExc_TypeError, "fixture components require a tuple");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(input); i++) {
        PyObject *item = PyTuple_GET_ITEM(input, i);
        if (!PyTuple_CheckExact(item) || PyTuple_GET_SIZE(item) != 5 ||
            !PyCode_Check(PyTuple_GET_ITEM(item, 0)) ||
            !PyCode_Check(PyTuple_GET_ITEM(item, 1)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(item, 2)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(item, 3)) ||
            !PyDict_CheckExact(PyTuple_GET_ITEM(item, 4))) {
            PyErr_SetString(PyExc_TypeError, "fixture component needs exact code/role/slot/globals");
            return NULL;
        }
    }
    return Py_NewRef(input);
}

static int
soac_dc_test_component(PyObject *owner, PyObject *method_object,
                        PyObject *component_object, unsigned int kind, Py_ssize_t index)
{
    PyFunctionObject *method = (PyFunctionObject *)method_object;
    PyFunctionObject *component = (PyFunctionObject *)component_object;
    PyObject *components = PyTuple_GET_ITEM(owner, 7);
    PyObject *builtins = PyTuple_GET_ITEM(PyTuple_GET_ITEM(owner, 0), 3);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(components); i++) {
        PyObject *entry = PyTuple_GET_ITEM(components, i);
        if (method->func_code == PyTuple_GET_ITEM(entry, 0) &&
            component->func_code == PyTuple_GET_ITEM(entry, 1) &&
            kind == PyLong_AsUnsignedLong(PyTuple_GET_ITEM(entry, 2)) &&
            index == PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 3)) &&
            method->func_globals == PyTuple_GET_ITEM(entry, 4) &&
            component->func_globals == PyTuple_GET_ITEM(entry, 4) &&
            method->func_builtins == builtins && component->func_builtins == builtins &&
            method->func_defaults == NULL && method->func_kwdefaults == NULL &&
            component->func_defaults == NULL && component->func_kwdefaults == NULL &&
            component->func_closure == NULL) {
            return 0;
        }
    }
    if (!PyErr_Occurred()) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "fixture generated component policy mismatch");
    }
    return -1;
}

static int
soac_dc_test_init_value(PyObject *owner, const PySoacDataclassFrameView *frame,
                         PyObject *field, PyObject *expression, PyObject *locals,
                         PyObject **helper_name)
{
    *helper_name = NULL;
    PyObject *payload = PyTuple_GET_ITEM(owner, 4);
    if (!PyTuple_CheckExact(payload) || PyTuple_GET_SIZE(payload) != 8 ||
        !PyUnicode_CheckExact(PyTuple_GET_ITEM(payload, 0)) ||
        PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(payload, 0), "init-value-v1") != 0) {
        return 0;
    }
    if (!PyCode_Check(PyTuple_GET_ITEM(payload, 1)) ||
        !PyLong_CheckExact(PyTuple_GET_ITEM(payload, 2)) ||
        !PyUnicode_CheckExact(PyTuple_GET_ITEM(payload, 4)) ||
        !PyDict_CheckExact(PyTuple_GET_ITEM(payload, 5)) ||
        !PyUnicode_CheckExact(PyTuple_GET_ITEM(payload, 6)) ||
        !PyBool_Check(PyTuple_GET_ITEM(payload, 7))) {
        PyErr_SetString(PyExc_TypeError, "invalid fixture init-value catalog");
        return -1;
    }
    Py_ssize_t instruction = PyLong_AsSsize_t(PyTuple_GET_ITEM(payload, 2));
    if (PyErr_Occurred()) {
        return -1;
    }
    if (PySoac_DataclassFrameInvocation(frame) == NULL ||
        PySoac_DataclassFrameRole(frame) != 1000 ||
        PySoac_DataclassFrameCode(frame) != PyTuple_GET_ITEM(payload, 1) ||
        PySoac_DataclassFrameInstruction(frame) != instruction ||
        field != PyTuple_GET_ITEM(payload, 3) ||
        expression != PyTuple_GET_ITEM(payload, 4) ||
        locals != PyTuple_GET_ITEM(payload, 5)) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "fixture init-value edge or bound operands changed");
        return -1;
    }
    if (PyTuple_GET_ITEM(payload, 7) == Py_False) {
        return 0;
    }
    *helper_name = PyTuple_GET_ITEM(payload, 6);
    return 1;
}

static PyObject *
soac_dc_test_forward_checked(PyObject *function, PyObject *const *args,
                              size_t nargsf, PyObject *kwnames)
{
    return _PySOAC_DataclassCheckedVectorcall(function, args, nargsf, kwnames);
}

static PyObject *
soac_dataclass_forward_checked_entry(PyObject *self, PyObject *function)
{
    if (!PyFunction_Check(function) ||
        ((PyFunctionObject *)function)->vectorcall != _PySOAC_DataclassCheckedVectorcall) {
        PyErr_SetString(PyExc_TypeError, "fixture can only forward the existing native checked entry");
        return NULL;
    }
    PyFunction_SetVectorcall((PyFunctionObject *)function, soac_dc_test_forward_checked);
    Py_RETURN_NONE;
}
