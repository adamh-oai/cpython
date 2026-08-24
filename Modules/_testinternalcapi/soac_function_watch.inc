/* Observation only: default capture never enters Python from a watcher.
 * Optional CREATE-only probe tests actual reentry; DESTROY remains untouched
 * because it can arrive with an exception already pending. The explicit
 * observer list/callback owners are accounted equally in ordinary controls. */

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
    Py_CLEAR(state->soac_create_callback);
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
            if (status == 0 && state->soac_create_callback != NULL &&
                state->soac_create_callback != Py_None) {
                /* The observer may unwatch reentrantly. Keep only its callable,
                 * never another subject function/code/map/value owner. */
                PyObject *callback = Py_NewRef(state->soac_create_callback);
                PyObject *result = PyObject_CallOneArg(callback, (PyObject *)function);
                status = result == NULL ? -1 : 0;
                PyObject *error = PyErr_GetRaisedException();
                Py_XDECREF(result);
                Py_DECREF(callback);
                PyErr_SetRaisedException(error);
            }
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
    PyObject *globals, *name, *captures, *callback = Py_None;
    if (!PyArg_ParseTuple(args, "OOO|O:soac_function_create_watch",
                          &globals, &name, &captures, &callback)) {
        return NULL;
    }
    if (!PyDict_CheckExact(globals) || !PyUnicode_CheckExact(name) ||
        !PyList_CheckExact(captures) ||
        (callback != Py_None && !PyCallable_Check(callback))) {
        PyErr_SetString(PyExc_TypeError,
                        "exact globals/name/capture list and optional callable required");
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
    state->soac_create_callback = Py_NewRef(callback);
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
