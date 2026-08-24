/* Test-only exact catalog. This is not production stdlib/source authority. */

static int soac_dc_test_generated_enter(
    PyObject *, unsigned int, const PySoacDataclassFrameView *,
    const PySoacDataclassFrameView *, unsigned int *);
static int soac_dc_test_generated_create(
    PyObject *, const PySoacDataclassFrameView *, PyObject *, unsigned int *);

static PyObject *
soac_dc_test_snapshot(PyObject *function)
{
    if (!PyFunction_Check(function)) {
        PyErr_SetString(PyExc_TypeError, "fixture requires an exact function");
        return NULL;
    }
    PyFunctionObject *fn = (PyFunctionObject *)function;
    return PyTuple_Pack(6, function, fn->func_code, fn->func_globals,
                        fn->func_builtins,
                        fn->func_closure == NULL ? Py_None : fn->func_closure,
                        fn->func_defaults == NULL ? Py_None : fn->func_defaults);
}

static int
soac_dc_test_matches(PyObject *snapshot, const PySoacDataclassFrameView *view)
{
    PyObject *function = PySoac_DataclassFrameFunction(view);
    if (function != PyTuple_GET_ITEM(snapshot, 0) || !PyFunction_Check(function)) {
        return 0;
    }
    PyFunctionObject *fn = (PyFunctionObject *)function;
    return PySoac_DataclassFrameCode(view) == PyTuple_GET_ITEM(snapshot, 1) &&
           fn->func_code == PyTuple_GET_ITEM(snapshot, 1) &&
           PySoac_DataclassFrameGlobals(view) == PyTuple_GET_ITEM(snapshot, 2) &&
           PySoac_DataclassFrameBuiltins(view) == PyTuple_GET_ITEM(snapshot, 3) &&
           (fn->func_closure == NULL ? Py_None : fn->func_closure) == PyTuple_GET_ITEM(snapshot, 4) &&
           (fn->func_defaults == NULL ? Py_None : fn->func_defaults) == PyTuple_GET_ITEM(snapshot, 5);
}

static int
soac_dc_test_values(PyObject *bindings, const PySoacDataclassFrameView *view,
                    int cells)
{
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bindings); i++) {
        PyObject *binding = PyTuple_GET_ITEM(bindings, i);
        Py_ssize_t index = PyLong_AsSsize_t(PyTuple_GET_ITEM(binding, 0));
        if (index == -1 && PyErr_Occurred()) {
            return -1;
        }
        PyObject *actual = cells ? PySoac_DataclassFrameCellValue(view, index)
                                : PySoac_DataclassFrameLocal(view, index);
        if (actual != PyTuple_GET_ITEM(binding, 1)) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                                "fixture actual bound frame value mismatch");
            }
            return -1;
        }
    }
    return 0;
}

static int
soac_dc_test_enter(PyObject *owner, unsigned int stage,
                   const PySoacDataclassFrameView *parent,
                   const PySoacDataclassFrameView *child, unsigned int *role)
{
    if (!PyTuple_CheckExact(owner) || PyTuple_GET_SIZE(owner) != 8) {
        PyErr_SetString(PyExc_TypeError, "invalid native dataclass fixture owner");
        return -1;
    }
    PyObject *invocation = PySoac_DataclassFrameInvocation(child);
    if (invocation == NULL ||
        (parent != NULL && PySoac_DataclassFrameInvocation(parent) != invocation)) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "fixture did not receive one explicit invocation");
        return -1;
    }
    int generated = soac_dc_test_generated_enter(owner, stage, parent, child, role);
    if (generated != 2) {
        return generated;
    }
    if (stage != 0) {
        if (!soac_dc_test_matches(PyTuple_GET_ITEM(owner, 0), child)) {
            PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                            "fixture root function mismatch");
            return -1;
        }
        if (soac_dc_test_values(PyTuple_GET_ITEM(owner, 1), child, 0) < 0) {
            return -1;
        }
        *role = 1000;
        return 1;
    }
    PyObject *edges = PyTuple_GET_ITEM(owner, 2);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(edges); i++) {
        PyObject *edge = PyTuple_GET_ITEM(edges, i);
        Py_ssize_t offset = PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 1));
        if (soac_dc_test_matches(PyTuple_GET_ITEM(edge, 0), parent) &&
            PySoac_DataclassFrameInstruction(parent) == offset &&
            soac_dc_test_matches(PyTuple_GET_ITEM(edge, 2), child)) {
            if (soac_dc_test_values(PyTuple_GET_ITEM(edge, 4), child, 0) < 0) {
                return -1;
            }
            *role = (unsigned int)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 3));
            return PyErr_Occurred() ? -1 : 1;
        }
    }
    return 0;
}

static int
soac_dc_test_create(PyObject *owner, const PySoacDataclassFrameView *producer,
                    PyObject *code, unsigned int *role)
{
    if (PySoac_DataclassFrameInvocation(producer) == NULL) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "fixture creation has no explicit invocation");
        return -1;
    }
    PyObject *sites = PyTuple_GET_ITEM(owner, 3);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(sites); i++) {
        PyObject *site = PyTuple_GET_ITEM(sites, i);
        Py_ssize_t offset = PyLong_AsSsize_t(PyTuple_GET_ITEM(site, 1));
        if (soac_dc_test_matches(PyTuple_GET_ITEM(site, 0), producer) &&
            PySoac_DataclassFrameInstruction(producer) == offset &&
            code == PyTuple_GET_ITEM(site, 2)) {
            if (soac_dc_test_values(PyTuple_GET_ITEM(site, 4), producer, 1) < 0) {
                return -1;
            }
            *role = (unsigned int)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(site, 3));
            return PyErr_Occurred() ? -1 : 1;
        }
    }
    return soac_dc_test_generated_create(owner, producer, code, role);
}

static int
soac_dc_test_member(PyObject *owner, PyObject *type, PyObject *class_owner,
                    PyObject *name, PyObject *function, unsigned int role)
{
    PyObject *members = PyTuple_GET_ITEM(owner, 5);
    PyFunctionObject *fn = (PyFunctionObject *)function;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(members); i++) {
        PyObject *member = PyTuple_GET_ITEM(members, i);
        unsigned long expected_role = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(member, 1));
        if (role != expected_role || !_PyUnicode_Equal(name, PyTuple_GET_ITEM(member, 0))) {
            continue;
        }
        if (fn->func_code == PyTuple_GET_ITEM(member, 2) &&
            fn->func_globals == PyTuple_GET_ITEM(member, 3) &&
            (fn->func_defaults == NULL ? Py_None : fn->func_defaults) == PyTuple_GET_ITEM(member, 4) &&
            (fn->func_closure == NULL ? Py_None : fn->func_closure) == PyTuple_GET_ITEM(member, 5) &&
            fn->func_kwdefaults == NULL) {
            return 0;
        }
    }
    PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                    "fixture generated-member catalog mismatch");
    return -1;
}

static PyObject *
soac_dc_test_members(PyObject *input)
{
    if (input == NULL) {
        return PyTuple_New(0);
    }
    if (!PyTuple_CheckExact(input)) {
        PyErr_SetString(PyExc_TypeError, "fixture members require an exact tuple");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(input); i++) {
        PyObject *member = PyTuple_GET_ITEM(input, i);
        if (!PyTuple_CheckExact(member) || PyTuple_GET_SIZE(member) != 6 ||
            !PyUnicode_CheckExact(PyTuple_GET_ITEM(member, 0)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(member, 1)) ||
            !PyCode_Check(PyTuple_GET_ITEM(member, 2)) ||
            !PyDict_CheckExact(PyTuple_GET_ITEM(member, 3)) ||
            (PyTuple_GET_ITEM(member, 4) != Py_None &&
             !PyTuple_CheckExact(PyTuple_GET_ITEM(member, 4))) ||
            (PyTuple_GET_ITEM(member, 5) != Py_None &&
             !PyTuple_CheckExact(PyTuple_GET_ITEM(member, 5)))) {
            PyErr_SetString(PyExc_TypeError,
                            "fixture member requires (name, role, code, globals, defaults, closure)");
            return NULL;
        }
        unsigned long role = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(member, 1));
        if (PyErr_Occurred() || role < Py_SOAC_DATACLASS_MEMBER ||
            role > Py_SOAC_DATACLASS_FROZEN_DELATTR) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_ValueError, "invalid fixture member role");
            }
            return NULL;
        }
    }
    return Py_NewRef(input);
}

static int
soac_dc_test_check_bindings(PyObject *bindings)
{
    if (!PyTuple_CheckExact(bindings)) {
        PyErr_SetString(PyExc_TypeError, "fixture bindings require an exact tuple");
        return -1;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bindings); i++) {
        PyObject *binding = PyTuple_GET_ITEM(bindings, i);
        if (!PyTuple_CheckExact(binding) || PyTuple_GET_SIZE(binding) != 2 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(binding, 0))) {
            PyErr_SetString(PyExc_TypeError, "fixture binding requires (index, value)");
            return -1;
        }
        if (PyLong_AsSsize_t(PyTuple_GET_ITEM(binding, 0)) == -1 && PyErr_Occurred()) {
            return -1;
        }
    }
    return 0;
}

static PyObject *
soac_dc_test_edges(PyObject *input, int creation)
{
    if (!PyTuple_CheckExact(input)) {
        PyErr_SetString(PyExc_TypeError, "fixture edges require an exact tuple");
        return NULL;
    }
    PyObject *result = PyTuple_New(PyTuple_GET_SIZE(input));
    if (result == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(input); i++) {
        PyObject *edge = PyTuple_GET_ITEM(input, i);
        if (!PyTuple_CheckExact(edge) || PyTuple_GET_SIZE(edge) != 5) {
            PyErr_SetString(PyExc_TypeError, "fixture edge requires five operands");
            goto error;
        }
        PyObject *parent = soac_dc_test_snapshot(PyTuple_GET_ITEM(edge, 0));
        if (parent == NULL) {
            goto error;
        }
        PyObject *target = PyTuple_GET_ITEM(edge, 2);
        PyObject *child = creation ? Py_NewRef(target) : soac_dc_test_snapshot(target);
        if (child == NULL) {
            Py_DECREF(parent);
            goto error;
        }
        PyObject *offset = PyTuple_GET_ITEM(edge, 1);
        PyObject *role = PyTuple_GET_ITEM(edge, 3);
        PyObject *bindings = PyTuple_GET_ITEM(edge, 4);
        if ((creation && !PyCode_Check(child)) || !PyLong_CheckExact(offset) ||
            !PyLong_CheckExact(role) || soac_dc_test_check_bindings(bindings) < 0) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_TypeError, "invalid fixture edge metadata");
            }
            Py_DECREF(parent);
            Py_DECREF(child);
            goto error;
        }
        Py_ssize_t offset_value = PyLong_AsSsize_t(offset);
        unsigned long role_value = PyLong_AsUnsignedLong(role);
        if (PyErr_Occurred() || offset_value < 0 || role_value == 0 || role_value > UINT_MAX) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_ValueError, "invalid fixture edge coordinate");
            }
            Py_DECREF(parent);
            Py_DECREF(child);
            goto error;
        }
        PyObject *normalized = PyTuple_Pack(5, parent, offset, child, role, bindings);
        Py_DECREF(parent);
        Py_DECREF(child);
        if (normalized == NULL) {
            goto error;
        }
        PyTuple_SET_ITEM(result, i, normalized);
    }
    return result;
error:
    Py_DECREF(result);
    return NULL;
}

#include "soac_dataclass_bridges.inc"
#include "soac_dataclass_boundary.inc"

static PyObject *
soac_dataclass_fixture(PyObject *self, PyObject *args)
{
    PyObject *root, *bindings, *edges, *sites, *payload, *members = NULL;
    PyObject *boundaries = NULL, *components = NULL;
    if (!PyArg_ParseTuple(args, "OOOOO|OOO", &root, &bindings, &edges, &sites, &payload,
                          &members, &boundaries, &components) ||
        soac_dc_test_check_bindings(bindings) < 0 ||
        soac_dc_test_check_bridge_payload(payload) < 0) {
        return NULL;
    }
    PySoacDataclassCallbacks callbacks = {
        .abi_version = Py_SOAC_DATACLASS_ABI,
        .enter = soac_dc_test_enter,
        .create = soac_dc_test_create,
        .validate_member = soac_dc_test_member,
        .bridge = soac_dc_test_bridge,
        .compiled = soac_dc_test_compiled,
        .created = soac_dc_test_created,
        .bound = soac_dc_test_bound,
        .value = soac_dc_test_checked_value,
        .validate_component = soac_dc_test_component,
        .init_value = soac_dc_test_init_value,
    };
    if (PySoac_SetDataclassCallbacks(&callbacks) < 0) {
        return NULL;
    }
    /* Installation must own the values, not this temporary stack address. */
    memset(&callbacks, 0, sizeof(callbacks));
    PyObject *snapshot = soac_dc_test_snapshot(root);
    if (snapshot == NULL) {
        return NULL;
    }
    PyObject *normalized_edges = soac_dc_test_edges(edges, 0);
    if (normalized_edges == NULL) {
        Py_DECREF(snapshot);
        return NULL;
    }
    PyObject *normalized_sites = soac_dc_test_edges(sites, 1);
    if (normalized_sites == NULL) {
        Py_DECREF(snapshot);
        Py_DECREF(normalized_edges);
        return NULL;
    }
    PyObject *normalized_members = soac_dc_test_members(members);
    if (normalized_members == NULL) {
        Py_DECREF(snapshot);
        Py_DECREF(normalized_edges);
        Py_DECREF(normalized_sites);
        return NULL;
    }
    PyObject *normalized_boundaries = soac_dc_test_boundaries(boundaries);
    PyObject *normalized_components = normalized_boundaries == NULL ? NULL
        : soac_dc_test_components(components);
    PyObject *owner = normalized_components == NULL ? NULL : PyTuple_Pack(8, snapshot, bindings,
                                  normalized_edges, normalized_sites, payload,
                                  normalized_members, normalized_boundaries,
                                  normalized_components);
    Py_DECREF(snapshot);
    Py_DECREF(normalized_edges);
    Py_DECREF(normalized_sites);
    Py_DECREF(normalized_members);
    Py_XDECREF(normalized_boundaries);
    Py_XDECREF(normalized_components);
    if (owner == NULL) {
        return NULL;
    }
    PyObject *invocation = PySoac_NewDataclassInvocation(owner);
    Py_DECREF(owner);
    return invocation;
}

static PyObject *
soac_dataclass_fixture_call(PyObject *self, PyObject *args)
{
    PyObject *invocation, *function, *positional, *keywords;
    unsigned int stage;
    if (!PyArg_ParseTuple(args, "OIOOO", &invocation, &stage, &function,
                          &positional, &keywords)) {
        return NULL;
    }
    if (!PyTuple_CheckExact(positional) || !PyDict_CheckExact(keywords)) {
        PyErr_SetString(PyExc_TypeError, "fixture call requires exact args tuple/keyword dict");
        return NULL;
    }
    Py_ssize_t nargs = PyTuple_GET_SIZE(positional);
    if (PyDict_GET_SIZE(keywords) == 0) {
        return PySoac_DataclassVectorcall(invocation, stage, function,
                                         _PyTuple_ITEMS(positional), nargs, NULL);
    }
    PyObject *kwnames;
    PyObject *const *values = _PyStack_UnpackDict(
        _PyThreadState_GET(), _PyTuple_ITEMS(positional), nargs, keywords, &kwnames);
    if (values == NULL) {
        return NULL;
    }
    PyObject *result = PySoac_DataclassVectorcall(invocation, stage, function,
                                                values, nargs, kwnames);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
        Py_DECREF(values[nargs + i]);
    }
    _PyStack_UnpackDict_FreeNoDecRef(values, kwnames);
    return result;
}

static PyObject *
soac_dataclass_fixture_c_proxy(PyObject *self, PyObject *function)
{
    return PyObject_CallNoArgs(function);
}

static PyObject *
soac_dataclass_frame_offsets(PyObject *self, PyObject *unused)
{
    return Py_BuildValue("{s:n,s:n,s:n,s:n,s:n,s:n,s:n,s:n,s:n}",
                         "frame_size", sizeof(_PyInterpreterFrame),
                         "role", offsetof(_PyInterpreterFrame, soac_dataclass_role),
                         "invocation", offsetof(_PyInterpreterFrame, soac_dataclass_invocation),
                         "checked_activation", offsetof(_PyInterpreterFrame, soac_dataclass_checked_activation),
                         "localsplus", offsetof(_PyInterpreterFrame, localsplus),
                         "thread_base_frame_pointer", offsetof(PyThreadState, base_frame),
                         "thread_soft_limit", offsetof(_PyThreadStateImpl, c_stack_soft_limit),
                         "base_frame", offsetof(_PyThreadStateImpl, base_frame),
                         "thread_soft_limit_from_base_frame",
                         offsetof(_PyThreadStateImpl, c_stack_soft_limit) -
                             offsetof(_PyThreadStateImpl, base_frame));
}
