/* Observation only: capture a matching actual CREATE object without entering
 * Python from a function watcher. In particular, DESTROY can arrive with an
 * exception already pending, before a ctypes callback could save that state.
 * Tests mutate a captured function later, outside the watcher callback. */

static int soac_capture_function_create(PyFunction_WatchEvent,
                                         PyFunctionObject *, PyObject *);

static void
clear_soac_function_create_watch(module_state *state)
{
    PyObject *exception = PyErr_GetRaisedException();
    int id = state->soac_create_watcher_plus_one - 1;
    state->soac_create_watcher_plus_one = 0;
    if (id >= 0 && id < FUNC_MAX_WATCHERS &&
        _PyInterpreterState_GET()->func_watchers[id] == soac_capture_function_create) {
        if (PyFunction_ClearWatcher(id) < 0) {
            PyErr_Clear();
        }
    }
    Py_CLEAR(state->soac_create_globals);
    Py_CLEAR(state->soac_create_name);
    Py_CLEAR(state->soac_create_captures);
    PyErr_SetRaisedException(exception);
}

static int
soac_capture_function_create(PyFunction_WatchEvent event,
                              PyFunctionObject *function, PyObject *new_value)
{
    if (event != PyFunction_EVENT_CREATE) {
        return 0;  /* Preserve a pending error without invoking any API. */
    }
    PyObject *exception = PyErr_GetRaisedException();
    int status = 0;
    PyObject *name = PyUnicode_FromString(MODULE_NAME);
    PyObject *module = name == NULL ? NULL : PyImport_GetModule(name);
    Py_XDECREF(name);
    if (module == NULL) {
        status = PyErr_Occurred() ? -1 : 0;
    }
    else if (PyModule_CheckExact(module) &&
             PyModule_GetDef(module) == &_testcapimodule) {
        module_state *state = get_module_state(module);
        if (state->soac_create_watcher_plus_one != 0 &&
            state->soac_create_globals == function->func_globals &&
            state->soac_create_name != NULL &&
            state->soac_create_captures != NULL &&
            _PyUnicode_Equal(state->soac_create_name, function->func_name)) {
            status = PyList_Append(state->soac_create_captures, (PyObject *)function);
        }
    }
    Py_XDECREF(module);
    if (exception != NULL) {
        if (status < 0) {
            PyErr_WriteUnraisable((PyObject *)function);
        }
        PyErr_SetRaisedException(exception);
        return 0;
    }
    return status;
}

static PyObject *
soac_function_create_watch(PyObject *self, PyObject *args)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_RuntimeError, "this function watcher fixture requires the GIL");
    return NULL;
#else
    PyObject *globals, *name, *captures;
    if (!PyArg_ParseTuple(args, "OOO:soac_function_create_watch",
                          &globals, &name, &captures)) {
        return NULL;
    }
    if (!PyDict_CheckExact(globals) || !PyUnicode_CheckExact(name) ||
        !PyList_CheckExact(captures)) {
        PyErr_SetString(PyExc_TypeError, "exact globals, name and capture list required");
        return NULL;
    }
    module_state *state = get_module_state(self);
    if (state->soac_create_watcher_plus_one != 0) {
        PyErr_SetString(PyExc_ValueError, "a function CREATE capture is already active");
        return NULL;
    }
    int id = PyFunction_AddWatcher(soac_capture_function_create);
    if (id < 0) {
        return NULL;
    }
    state->soac_create_globals = Py_NewRef(globals);
    state->soac_create_name = Py_NewRef(name);
    state->soac_create_captures = Py_NewRef(captures);
    state->soac_create_watcher_plus_one = id + 1;
    PyObject *result = PyLong_FromLong(id);
    if (result == NULL) {
        clear_soac_function_create_watch(state);
    }
    return result;
#endif
}

static PyObject *
soac_function_create_unwatch(PyObject *self, PyObject *id_object)
{
    long id = PyLong_AsLong(id_object);
    if (id == -1 && PyErr_Occurred()) {
        return NULL;
    }
    module_state *state = get_module_state(self);
    if (id < 0 || id >= FUNC_MAX_WATCHERS ||
        id != state->soac_create_watcher_plus_one - 1 ||
        _PyInterpreterState_GET()->func_watchers[id] != soac_capture_function_create) {
        PyErr_SetString(PyExc_ValueError, "not the active function CREATE capture");
        return NULL;
    }
    clear_soac_function_create_watch(state);
    Py_RETURN_NONE;
}
