/* Native generated-member creation and component metadata fixture.
 * This is not source admission and contains no call-type predicates. */

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
        /* Production records this weak birth witness before publication. The
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
    PyObject *components = PyTuple_GET_ITEM(owner, 6);
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
