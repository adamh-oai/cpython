/* Test-only immutable call-edge catalog with one preallocated weak-code slot.
 * This deliberately does not authenticate production stdlib/source bytes. */

static int
soac_dc_test_bridge_error(const char *message)
{
    if (!PyErr_Occurred()) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(), message);
    }
    return -1;
}

static int
soac_dc_test_bridge_tag(PyObject *payload)
{
    return PyTuple_CheckExact(payload) && PyTuple_GET_SIZE(payload) != 0 &&
        PyUnicode_CheckExact(PyTuple_GET_ITEM(payload, 0)) &&
        PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(payload, 0), "bridges-v1") == 0;
}

static PyObject *
soac_dc_test_bridge_payload(PyObject *owner)
{
    PyObject *payload = PyTuple_GET_ITEM(owner, 4);
    return soac_dc_test_bridge_tag(payload) ? payload : NULL;
}

static int
soac_dc_test_code_path(PyObject *path)
{
    if (!PyTuple_CheckExact(path)) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(path); i++) {
        PyObject *index = PyTuple_GET_ITEM(path, i);
        if (!PyLong_CheckExact(index) || PyLong_AsSsize_t(index) < 0) {
            return -1;
        }
    }
    return PyErr_Occurred() ? -1 : 0;
}

static int
soac_dc_test_check_bridge_payload(PyObject *payload)
{
    if (!soac_dc_test_bridge_tag(payload)) {
        return 0;
    }
    if (PyTuple_GET_SIZE(payload) != 4) {
        goto bad;
    }
    PyObject *bridges = PyTuple_GET_ITEM(payload, 1);
    PyObject *plan = PyTuple_GET_ITEM(payload, 2);
    PyObject *reached = PyTuple_GET_ITEM(payload, 3);
    if (!PyTuple_CheckExact(bridges) || !PyList_CheckExact(reached) ||
        PyList_GET_SIZE(reached) != 1 || PyList_GET_ITEM(reached, 0) != Py_None) {
        goto bad;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bridges); i++) {
        PyObject *edge = PyTuple_GET_ITEM(bridges, i);
        if (!PyTuple_CheckExact(edge) || PyTuple_GET_SIZE(edge) != 3 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 0)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 1)) ||
            !PyTuple_CheckExact(PyTuple_GET_ITEM(edge, 2))) {
            goto bad;
        }
        unsigned long kind = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 1));
        if (PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 0)) < 0 ||
            kind < Py_SOAC_DATACLASS_BRIDGE_SOURCE ||
            kind > Py_SOAC_DATACLASS_BRIDGE_MEMBER) {
            goto bad;
        }
    }
    if (plan == Py_None) {
        return 0;
    }
    if (!PyTuple_CheckExact(plan) || PyTuple_GET_SIZE(plan) != 6 ||
        !PyUnicode_CheckExact(PyTuple_GET_ITEM(plan, 0)) ||
        !PyDict_CheckExact(PyTuple_GET_ITEM(plan, 1)) ||
        !PyDict_CheckExact(PyTuple_GET_ITEM(plan, 2))) {
        goto bad;
    }
    PyObject *slot = PyTuple_GET_ITEM(plan, 3);
    PyObject *frames = PyTuple_GET_ITEM(plan, 4);
    PyObject *creations = PyTuple_GET_ITEM(plan, 5);
    if (!PyList_CheckExact(slot) || PyList_GET_SIZE(slot) != 1 ||
        PyList_GET_ITEM(slot, 0) != Py_None || !PyTuple_CheckExact(frames) ||
        !PyTuple_CheckExact(creations)) {
        goto bad;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(frames); i++) {
        PyObject *edge = PyTuple_GET_ITEM(frames, i);
        if (!PyTuple_CheckExact(edge) || PyTuple_GET_SIZE(edge) != 4 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 0)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 2)) ||
            PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 0)) < 0 ||
            PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 2)) == 0 ||
            soac_dc_test_code_path(PyTuple_GET_ITEM(edge, 1)) < 0 ||
            soac_dc_test_check_bindings(PyTuple_GET_ITEM(edge, 3)) < 0) {
            goto bad;
        }
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(creations); i++) {
        PyObject *edge = PyTuple_GET_ITEM(creations, i);
        if (!PyTuple_CheckExact(edge) || PyTuple_GET_SIZE(edge) != 4 ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 1)) ||
            !PyLong_CheckExact(PyTuple_GET_ITEM(edge, 3)) ||
            PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 1)) < 0 ||
            PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 3)) == 0 ||
            soac_dc_test_code_path(PyTuple_GET_ITEM(edge, 0)) < 0 ||
            soac_dc_test_code_path(PyTuple_GET_ITEM(edge, 2)) < 0) {
            goto bad;
        }
    }
    return PyErr_Occurred() ? -1 : 0;
bad:
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "invalid dataclass bridge fixture metadata");
    }
    return -1;
}

static PyObject *
soac_dc_test_weak_code(PyObject *tree, PyObject *path)
{
    if (tree == Py_None) {
        return NULL;
    }
    for (Py_ssize_t i = 0; path != NULL && i < PyTuple_GET_SIZE(path); i++) {
        if (!PyTuple_CheckExact(tree) || PyTuple_GET_SIZE(tree) != 2) {
            return NULL;
        }
        PyObject *children = PyTuple_GET_ITEM(tree, 1);
        if (!PyTuple_CheckExact(children)) {
            return NULL;
        }
        Py_ssize_t wanted = PyLong_AsSsize_t(PyTuple_GET_ITEM(path, i));
        PyObject *child = NULL;
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(children); j++) {
            PyObject *entry = PyTuple_GET_ITEM(children, j);
            if (!PyTuple_CheckExact(entry) || PyTuple_GET_SIZE(entry) != 2 ||
                !PyLong_CheckExact(PyTuple_GET_ITEM(entry, 0))) {
                return NULL;
            }
            if (PyLong_AsSsize_t(PyTuple_GET_ITEM(entry, 0)) == wanted) {
                child = PyTuple_GET_ITEM(entry, 1);
                break;
            }
        }
        if (child == NULL) {
            return NULL;
        }
        tree = child;
    }
    if (!PyTuple_CheckExact(tree) || PyTuple_GET_SIZE(tree) != 2 ||
        !PyWeakref_CheckRef(PyTuple_GET_ITEM(tree, 0))) {
        return NULL;
    }
    PyObject *code = ((PyWeakReference *)PyTuple_GET_ITEM(tree, 0))->wr_object;
    return code == Py_None ? NULL : code;
}

static int
soac_dc_test_generated_enter(PyObject *owner, unsigned int stage,
                             const PySoacDataclassFrameView *parent,
                             const PySoacDataclassFrameView *child,
                             unsigned int *role)
{
    PyObject *payload = soac_dc_test_bridge_payload(owner);
    PyObject *plan = payload == NULL ? Py_None : PyTuple_GET_ITEM(payload, 2);
    if (plan == Py_None || (stage != 0 && stage != Py_SOAC_DATACLASS_GENERATED_EXEC)) {
        return 2;  /* Not a generated-code fixture edge. */
    }
    PyObject *tree = PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 3), 0);
    if (stage == Py_SOAC_DATACLASS_GENERATED_EXEC) {
        if (soac_dc_test_matches(PyTuple_GET_ITEM(owner, 0), parent) &&
            PySoac_DataclassFrameCode(child) == soac_dc_test_weak_code(tree, NULL) &&
            PySoac_DataclassFrameGlobals(child) == PyTuple_GET_ITEM(plan, 1) &&
            PySoac_DataclassFrameBuiltins(child) == PySoac_DataclassFrameBuiltins(parent)) {
            *role = 1002;
            return 1;
        }
        return soac_dc_test_bridge_error("fixture generated exec frame mismatch");
    }
    PyObject *frames = PyTuple_GET_ITEM(plan, 4);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(frames); i++) {
        PyObject *edge = PyTuple_GET_ITEM(frames, i);
        if (soac_dc_test_matches(PyTuple_GET_ITEM(owner, 0), parent) &&
            PySoac_DataclassFrameInstruction(parent) ==
                PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 0))) {
            if (PySoac_DataclassFrameCode(child) !=
                    soac_dc_test_weak_code(tree, PyTuple_GET_ITEM(edge, 1)) ||
                PySoac_DataclassFrameGlobals(child) != PyTuple_GET_ITEM(plan, 1) ||
                PySoac_DataclassFrameBuiltins(child) != PySoac_DataclassFrameBuiltins(parent) ||
                PyFunction_HasSoacDataclassCreation(PySoac_DataclassFrameFunction(child)) != 1 ||
                soac_dc_test_values(PyTuple_GET_ITEM(edge, 3), child, 0) < 0) {
                return soac_dc_test_bridge_error("fixture generated factory has no exact creation witness");
            }
            *role = (unsigned int)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 2));
            return PyErr_Occurred() ? -1 : 1;
        }
    }
    return 2;
}

static int
soac_dc_test_generated_create(PyObject *owner,
                              const PySoacDataclassFrameView *producer,
                              PyObject *code, unsigned int *role)
{
    PyObject *payload = soac_dc_test_bridge_payload(owner);
    PyObject *plan = payload == NULL ? Py_None : PyTuple_GET_ITEM(payload, 2);
    if (plan == Py_None) {
        return 0;
    }
    PyObject *tree = PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 3), 0);
    PyObject *creations = PyTuple_GET_ITEM(plan, 5);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(creations); i++) {
        PyObject *edge = PyTuple_GET_ITEM(creations, i);
        if (PySoac_DataclassFrameCode(producer) ==
                soac_dc_test_weak_code(tree, PyTuple_GET_ITEM(edge, 0)) &&
            PySoac_DataclassFrameInstruction(producer) ==
                PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 1)) &&
            code == soac_dc_test_weak_code(tree, PyTuple_GET_ITEM(edge, 2))) {
            *role = (unsigned int)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 3));
            return PyErr_Occurred() ? -1 : 1;
        }
    }
    return PyErr_Occurred() ? -1 : 0;
}

static int
soac_dc_test_bridge(PyObject *owner, const PySoacDataclassFrameView *parent,
                    PyObject *builtin, unsigned int kind,
                    PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *payload = soac_dc_test_bridge_payload(owner);
    if (payload == NULL || !soac_dc_test_matches(PyTuple_GET_ITEM(owner, 0), parent) ||
        PySoac_GetDataclassBuiltin(kind) != builtin) {
        return soac_dc_test_bridge_error("fixture bridge parent or native target mismatch");
    }
    PyObject *bridges = PyTuple_GET_ITEM(payload, 1);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bridges); i++) {
        PyObject *edge = PyTuple_GET_ITEM(bridges, i);
        PyObject *expected = PyTuple_GET_ITEM(edge, 2);
        if (PySoac_DataclassFrameInstruction(parent) != PyLong_AsSsize_t(PyTuple_GET_ITEM(edge, 0)) ||
            kind != PyLong_AsUnsignedLong(PyTuple_GET_ITEM(edge, 1)) ||
            nargs != PyTuple_GET_SIZE(expected)) {
            continue;
        }
        for (Py_ssize_t j = 0; j < nargs; j++) {
            if (kind == Py_SOAC_DATACLASS_BRIDGE_MEMBER && j == 3 &&
                PyTuple_GET_ITEM(expected, j) == Py_None) {
                if (!PyFunction_Check(args[j]) ||
                    PyFunction_HasSoacDataclassCreation(args[j]) != 1) {
                    return soac_dc_test_bridge_error("fixture member has no actual creation record");
                }
                continue;
            }
            if (args[j] != PyTuple_GET_ITEM(expected, j)) {
                return soac_dc_test_bridge_error("fixture bridge actual operand mismatch");
            }
        }
        PyObject *reached = PyTuple_GET_ITEM(payload, 3);
        PyObject *previous = PyList_GET_ITEM(reached, 0);
        if (previous != Py_None && previous != Py_True) {
            return soac_dc_test_bridge_error("fixture bridge event slot changed");
        }
        PyList_SET_ITEM(reached, 0, Py_NewRef(Py_True));
        Py_DECREF(previous);
        return 0;
    }
    return soac_dc_test_bridge_error("fixture bridge callsite mismatch");
}

static int
soac_dc_test_compiled(PyObject *owner, const PySoacDataclassFrameView *parent,
                      PyObject *source, PyObject *code, PyObject *weak_tree)
{
    PyObject *payload = soac_dc_test_bridge_payload(owner);
    PyObject *plan = payload == NULL ? Py_None : PyTuple_GET_ITEM(payload, 2);
    if (plan == Py_None || source != PyTuple_GET_ITEM(plan, 0) ||
        !soac_dc_test_matches(PyTuple_GET_ITEM(owner, 0), parent) ||
        code != soac_dc_test_weak_code(weak_tree, NULL)) {
        return soac_dc_test_bridge_error("fixture compilation source mismatch");
    }
    PyObject *slot = PyTuple_GET_ITEM(plan, 3);
    if (PyList_GET_ITEM(slot, 0) != Py_None) {
        return soac_dc_test_bridge_error("fixture compilation was already published");
    }
    PyList_SET_ITEM(slot, 0, Py_NewRef(weak_tree));
    Py_DECREF(Py_None);
    return 0;
}

static PyObject *
soac_dataclass_fixture_c_forward(PyObject *self, PyObject *const *args,
                                 Py_ssize_t nargs)
{
    if (nargs == 0) {
        PyErr_SetString(PyExc_TypeError, "fixture C proxy requires a callable");
        return NULL;
    }
    return PyObject_Vectorcall(args[0], args + 1, nargs - 1, NULL);
}

static PyObject *
soac_dataclass_fixture_changed_entry(PyObject *callable,
                                     PyObject *const *args, size_t nargsf,
                                     PyObject *kwnames)
{
    Py_RETURN_NONE;
}

static PyObject *
soac_dataclass_fixture_copy_builtin(PyObject *self, PyObject *args)
{
    PyObject *object;
    PyObject *bound_self = NULL;
    int changed_entry = 0;
    if (!PyArg_ParseTuple(args, "O|Op", &object, &bound_self, &changed_entry)) {
        return NULL;
    }
    if (!PyCFunction_CheckExact(object) ||
        (PyCFunction_GET_FLAGS(object) & METH_METHOD)) {
        PyErr_SetString(PyExc_TypeError, "fixture copy requires a nonmethod builtin");
        return NULL;
    }
    PyCFunctionObject *function = (PyCFunctionObject *)object;
    PyObject *copy = PyCFunction_NewEx(
        function->m_ml, bound_self == NULL ? function->m_self : bound_self,
        function->m_module);
    if (copy != NULL && changed_entry) {
        ((PyCFunctionObject *)copy)->vectorcall = soac_dataclass_fixture_changed_entry;
    }
    return copy;
}

static PyObject *
soac_code_view(PyObject *self, PyObject *args)
{
    PyObject *code;
    int compatible = 1;
    if (!PyArg_ParseTuple(args, "O|p", &code, &compatible)) {
        return NULL;
    }
    PySoacCodeView view;
    if (PySoac_GetCodeView(code, &view,
                          compatible ? sizeof(view) : sizeof(view) - 1) < 0) {
        return NULL;
    }
    return Py_BuildValue(
        "{s:I,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:n,s:K,"
        "s:O,s:O,s:O,s:O,s:O,s:O,s:O,s:O,s:O,s:n}",
        "abi_version", view.abi_version,
        "flags", view.flags,
        "argcount", view.argcount,
        "posonlyargcount", view.posonlyargcount,
        "kwonlyargcount", view.kwonlyargcount,
        "stacksize", view.stacksize,
        "firstlineno", view.firstlineno,
        "nlocalsplus", view.nlocalsplus,
        "framesize", view.framesize,
        "nlocals", view.nlocals,
        "ncellvars", view.ncellvars,
        "nfreevars", view.nfreevars,
        "code_units", view.code_units,
        "strict_source_id", (unsigned long long)view.strict_source_id,
        "consts", view.consts,
        "names", view.names,
        "localsplusnames", view.localsplusnames,
        "localspluskinds", view.localspluskinds,
        "filename", view.filename,
        "name", view.name,
        "qualname", view.qualname,
        "linetable", view.linetable,
        "exceptiontable", view.exceptiontable,
        "view_size", (Py_ssize_t)sizeof(view));
}
