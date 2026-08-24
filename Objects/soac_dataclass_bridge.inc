/* Exact opcode-dispatched bridges. Their public/C entries are ordinary calls;
 * neither the implementation pointer nor a Python module binding is authority.
 * Included by soac_dataclass.inc, sharing its one interpreter-owned protocol. */

static PyObject *
soac_dataclass_builtin(PyInterpreterState *interp, unsigned int kind)
{
    assert(kind >= 1 && kind <= Py_ARRAY_LENGTH(interp->soac.dataclass_builtins));
    PyObject *weak = interp->soac.dataclass_builtins[kind - 1];
    if (weak == NULL) {
        return NULL;
    }
    PyObject *object = ((PyWeakReference *)weak)->wr_object;
    return object == Py_None ? NULL : object;
}

PyObject *
PySoac_GetDataclassBuiltin(unsigned int kind)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (kind < 1 || kind > Py_ARRAY_LENGTH(interp->soac.dataclass_builtins)) {
        PyErr_SetString(PyExc_ValueError, "invalid native dataclass builtin kind");
        return NULL;
    }
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "dataclass provenance requires the GIL build");
    return NULL;
#endif
    if (interp->soac.dataclass_closed) {
        soac_dataclass_error("native dataclass builtin witnesses are closed");
        return NULL;
    }
    return soac_dataclass_builtin(interp, kind);
}

int
_PySOAC_DataclassCaptureBuiltin(PyInterpreterState *interp, unsigned int kind,
                               PyObject *builtin)
{
#ifdef Py_GIL_DISABLED
    return 0;
#endif
    assert(kind >= 1 && kind <= Py_ARRAY_LENGTH(interp->soac.dataclass_builtins));
    assert(PyCFunction_CheckExact(builtin));
    if (interp->soac.dataclass_closed) {
        return soac_dataclass_error("native dataclass builtin creation is closed");
    }
    unsigned int bit = 1U << (kind - 1);
    if (interp->soac.dataclass_builtin_seen & bit) {
        /* A dead original is not replaced by a reload, even after failure. */
        return 0;
    }
    interp->soac.dataclass_builtin_seen |= bit;
    PyObject *weak = PyWeakref_NewRef(builtin, NULL);
    if (weak == NULL) {
        return -1;
    }
    if (interp->soac.dataclass_closed) {
        Py_DECREF(weak);
        return soac_dataclass_error("native dataclass builtin creation was closed");
    }
    interp->soac.dataclass_builtins[kind - 1] = weak;
    return 0;
}

static PyObject *
soac_dataclass_bridge_default(PyObject *self, PyObject *const *args,
                              Py_ssize_t nargs)
{
    /* Even if a privileged Python frame is currently running, an ordinary
     * C call through this entry point cannot inherit its invocation. */
    if (!PyLong_CheckExact(self)) {
        PyErr_SetString(PyExc_TypeError, "invalid dataclass builtin receiver");
        return NULL;
    }
    long kind = PyLong_AsLong(self);
    if ((kind == Py_SOAC_DATACLASS_BRIDGE_SOURCE ||
         kind == Py_SOAC_DATACLASS_BRIDGE_VALUE) && nargs == 1) {
        return Py_NewRef(args[0]);
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_INIT_VALUE && nargs == 3) {
        return Py_NewRef(args[1]);
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS && nargs == 5) {
        return PyObject_Vectorcall(args[0], args + 1, 3, NULL);
    }
    if ((kind == Py_SOAC_DATACLASS_BRIDGE_EXEC ||
         kind == Py_SOAC_DATACLASS_BRIDGE_MEMBER) && nargs == 4) {
        return PyObject_Vectorcall(args[0], args + 1, 3, NULL);
    }
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "invalid dataclass builtin arguments");
    }
    return NULL;
}

static PyMethodDef soac_dataclass_bridge_methods[] = {
    {"_dataclass_record_source", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
    {"_dataclass_exec", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
    {"_dataclass_set_member", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
    {"_dataclass_check_value", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
    {"_dataclass_init_value", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
    {"_dataclass_new_slots", _PyCFunction_CAST(soac_dataclass_bridge_default),
     METH_FASTCALL, NULL},
};

static const unsigned int soac_dataclass_bridge_kinds[] = {
    Py_SOAC_DATACLASS_BRIDGE_SOURCE,
    Py_SOAC_DATACLASS_BRIDGE_EXEC,
    Py_SOAC_DATACLASS_BRIDGE_MEMBER,
    Py_SOAC_DATACLASS_BRIDGE_VALUE,
    Py_SOAC_DATACLASS_BRIDGE_INIT_VALUE,
    Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS,
};

int
_PySOAC_DataclassIsBridgeImplementation(PyObject *callable)
{
    /* This is only a dispatch/deoptimization predicate, never authority. */
    return PyCFunction_CheckExact(callable) &&
        PyCFunction_GET_FUNCTION(callable) ==
            _PyCFunction_CAST(soac_dataclass_bridge_default);
}

static unsigned int
soac_dataclass_bridge_kind(PyObject *callable)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (!_PySOAC_DataclassIsBridgeImplementation(callable) ||
        interp->soac.dataclass_closed) {
        return 0;
    }
    for (size_t i = 0; i < Py_ARRAY_LENGTH(soac_dataclass_bridge_kinds); i++) {
        unsigned int kind = soac_dataclass_bridge_kinds[i];
        if (soac_dataclass_builtin(interp, kind) == callable) {
            return kind;
        }
    }
    return 0;
}

int
_PySOAC_DataclassAddHelpers(PyObject *module)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    PyObject *module_name = PyUnicode_FromString("_types");
    if (module_name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < Py_ARRAY_LENGTH(soac_dataclass_bridge_kinds); i++) {
        unsigned int kind = soac_dataclass_bridge_kinds[i];
        PyObject *helper = NULL;
#ifndef Py_GIL_DISABLED
        helper = soac_dataclass_builtin(interp, kind);
        Py_XINCREF(helper);
#endif
        if (helper == NULL) {
            PyObject *receiver = PyLong_FromUnsignedLong(kind);
            if (receiver == NULL) {
                Py_DECREF(module_name);
                return -1;
            }
            helper = PyCFunction_NewEx(&soac_dataclass_bridge_methods[i],
                                       receiver, module_name);
            Py_DECREF(receiver);
            if (helper == NULL ||
                _PySOAC_DataclassCaptureBuiltin(interp, kind, helper) < 0) {
                Py_XDECREF(helper);
                Py_DECREF(module_name);
                return -1;
            }
        }
        int result = PyModule_AddObjectRef(
            module, soac_dataclass_bridge_methods[i].ml_name, helper);
        Py_DECREF(helper);
        if (result < 0) {
            Py_DECREF(module_name);
            return -1;
        }
    }
    Py_DECREF(module_name);
    return 0;
}

static int
soac_dataclass_validate_bridge(_PyInterpreterFrame *parent, PyObject *builtin,
                               unsigned int kind, PyObject *const *args,
                               Py_ssize_t nargs)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    if (invocation == NULL || soac_dataclass_current(invocation) < 0 ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        !soac_dataclass_frame_valid(parent) ||
        soac_dataclass_bridge_kind(builtin) != kind) {
        return invocation == NULL
            ? soac_dataclass_error("dataclass bridge has no explicit invocation")
            : soac_dataclass_fail(invocation, "dataclass bridge parent is not authentic");
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if ((kind == Py_SOAC_DATACLASS_BRIDGE_SOURCE &&
         (nargs != 1 || !PyUnicode_CheckExact(args[0]))) ||
        (kind == Py_SOAC_DATACLASS_BRIDGE_EXEC &&
         (nargs != 4 || args[0] != soac_dataclass_builtin(
              interp, Py_SOAC_DATACLASS_BUILTIN_EXEC) ||
          !PyUnicode_CheckExact(args[1]) || !PyDict_CheckExact(args[2]) ||
          !PyDict_CheckExact(args[3]))) ||
        (kind == Py_SOAC_DATACLASS_BRIDGE_MEMBER &&
         (nargs != 4 || args[0] != soac_dataclass_builtin(
              interp, Py_SOAC_DATACLASS_BUILTIN_SETATTR) ||
          !PyType_Check(args[1]) || !PyUnicode_CheckExact(args[2]) ||
          args[1] != (PyObject *)invocation->actual_type)) ||
        (kind == Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS &&
         (nargs != 5 || invocation->phase != SOAC_DC_APPLY_ACTIVE ||
          args[0] != (PyObject *)&PyType_Type ||
          !PyUnicode_CheckExact(args[1]) || !PyTuple_CheckExact(args[2]) ||
          !PyDict_CheckExact(args[3]) || !PyType_Check(args[4]) ||
          args[4] != (PyObject *)invocation->actual_type ||
          invocation->class_owner == NULL ||
          _PySOAC_GetTypeConstructionOwner(args[4]) != invocation->class_owner))) {
        return soac_dataclass_fail(invocation, "dataclass bridge operands are not authentic");
    }
    PySoacDataclassFrameView view = {
        .frame = parent, .role = parent->soac_dataclass_role,
        .invocation = (PyObject *)invocation,
    };
    int result = interp->soac.dataclass_callbacks.bridge(
        invocation->owner, &view, builtin, kind, args, nargs);
    if (result != 0 || PyErr_Occurred() ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL) {
        return soac_dataclass_fail(invocation, "dataclass bridge edge did not authenticate");
    }
    return 0;
}

static PyObject *
soac_dataclass_weak_code_tree(PyCodeObject *code)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_Py_EnterRecursiveCallTstate(tstate, " while recording dataclass code")) {
        return NULL;
    }
    PyObject *result = NULL, *weak = NULL, *children = NULL;
    if ((code->co_flags & CO_FUTURE_STRICT) || code->_co_soac_strict_source_id != 0) {
        soac_dataclass_error("generated dataclass code cannot carry strict source authority");
        goto done;
    }
    /* The later Compiled callback is allocation-free. Its exact source-span
     * projection may read canonical bytecode from this now-warm code cache. */
    PyObject *bytecode = PyCode_GetCode(code);
    if (bytecode == NULL) {
        goto done;
    }
    Py_DECREF(bytecode);
    Py_ssize_t count = 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(code->co_consts); i++) {
        count += PyCode_Check(PyTuple_GET_ITEM(code->co_consts, i));
    }
    weak = PyWeakref_NewRef((PyObject *)code, NULL);
    if (weak == NULL) {
        goto done;
    }
    children = PyTuple_New(count);
    if (children == NULL) {
        goto done;
    }
    Py_ssize_t position = 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(code->co_consts); i++) {
        PyObject *constant = PyTuple_GET_ITEM(code->co_consts, i);
        if (!PyCode_Check(constant)) {
            continue;
        }
        PyObject *child = soac_dataclass_weak_code_tree((PyCodeObject *)constant);
        if (child == NULL) {
            goto done;
        }
        PyObject *index = PyLong_FromSsize_t(i);
        PyObject *entry = index == NULL ? NULL : PyTuple_Pack(2, index, child);
        Py_XDECREF(index);
        Py_DECREF(child);
        if (entry == NULL) {
            goto done;
        }
        PyTuple_SET_ITEM(children, position++, entry);
    }
    result = PyTuple_Pack(2, weak, children);
done:
    Py_XDECREF(weak);
    Py_XDECREF(children);
    _Py_LeaveRecursiveCallTstate(tstate);
    return result;
}

static PyObject *
soac_dataclass_exec_bridge(_PyInterpreterFrame *parent, PyObject *builtin,
                           PyObject *const *args)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *globals = Py_NewRef(args[2]);
    PyObject *locals = Py_NewRef(args[3]);
    PyObject *code = NULL, *weak_tree = NULL, *function = NULL, *result = NULL;
    if (_PyEval_EnsureBuiltins(tstate, globals, NULL) < 0) {
        goto error;
    }
    Py_ssize_t source_size;
    const char *source = PyUnicode_AsUTF8AndSize(args[1], &source_size);
    if (source == NULL) {
        goto error;
    }
    if (memchr(source, '\0', source_size) != NULL) {
        PyErr_SetString(PyExc_SyntaxError, "source code string cannot contain null bytes");
        goto error;
    }
    PyObject *filename = PyUnicode_FromString("<string>");
    if (filename == NULL) {
        goto error;
    }
    if (soac_dataclass_validate_bridge(parent, builtin,
            Py_SOAC_DATACLASS_BRIDGE_EXEC, args, 4) < 0) {
        Py_DECREF(filename);
        goto error;
    }
    PyCompilerFlags flags = _PyCompilerFlags_INIT;
    flags.cf_flags = PyCF_SOURCE_IS_UTF8 |
        (_PyFrame_GetCode(parent)->co_flags & PyCF_MASK);
    code = Py_CompileStringObject(source, filename, Py_file_input, &flags, -1);
    Py_DECREF(filename);
    if (code == NULL) {
        goto error;
    }
    weak_tree = soac_dataclass_weak_code_tree((PyCodeObject *)code);
    if (weak_tree == NULL || soac_dataclass_validate_bridge(parent, builtin,
            Py_SOAC_DATACLASS_BRIDGE_EXEC, args, 4) < 0) {
        goto error;
    }
    PySoacDataclassFrameView view = {
        .frame = parent, .role = parent->soac_dataclass_role,
        .invocation = (PyObject *)invocation,
    };
    int status = tstate->interp->soac.dataclass_callbacks.compiled(
        invocation->owner, &view, args[1], code, weak_tree);
    if (status != 0 || PyErr_Occurred() ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL) {
        soac_dataclass_fail(invocation, "dataclass compiled code did not authenticate");
        goto error;
    }
    Py_CLEAR(weak_tree);
    if (PySys_Audit("exec", "O", code) < 0 ||
        soac_dataclass_validate_bridge(parent, builtin,
            Py_SOAC_DATACLASS_BRIDGE_EXEC, args, 4) < 0) {
        goto error;
    }
    PyObject *builtins = _PyDict_LoadBuiltinsFromGlobals(globals);
    if (builtins == NULL) {
        goto error;
    }
    PyFrameConstructor constructor = {
        .fc_globals = globals,
        .fc_builtins = builtins,
        .fc_name = ((PyCodeObject *)code)->co_name,
        .fc_qualname = ((PyCodeObject *)code)->co_name,
        .fc_code = code,
        .fc_defaults = NULL,
        .fc_kwdefaults = NULL,
        .fc_closure = NULL,
    };
    function = (PyObject *)_PyFunction_FromConstructor(&constructor);
    _Py_DECREF_BUILTINS(builtins);
    if (function == NULL || soac_dataclass_validate_bridge(parent, builtin,
            Py_SOAC_DATACLASS_BRIDGE_EXEC, args, 4) < 0) {
        goto error;
    }
    result = _PySOAC_DataclassEvalVector(
        tstate, (PyFunctionObject *)function, locals, NULL, 0, NULL,
        (PyObject *)invocation, Py_SOAC_DATACLASS_GENERATED_EXEC, parent);
    if (result == NULL) {
        goto error;
    }
    /* Keep the ordinary exec release order, including transient function/code
     * watcher effects, before returning from the bridge. */
    Py_CLEAR(function);
    Py_CLEAR(code);
    Py_DECREF(globals);
    Py_DECREF(locals);
    Py_DECREF(result);
    if (!soac_dataclass_is_running(invocation) || PyErr_Occurred()) {
        soac_dataclass_fail(invocation, "dataclass execution ended an active invocation");
        return NULL;
    }
    Py_RETURN_NONE;
error:
    soac_dataclass_fail(invocation, "dataclass generated execution failed");
    Py_XDECREF(weak_tree);
    Py_XDECREF(function);
    Py_XDECREF(code);
    Py_DECREF(globals);
    Py_DECREF(locals);
    Py_XDECREF(result);
    return NULL;
}

static int
soac_dataclass_validate_init_value(_PyInterpreterFrame *parent, PyObject *builtin,
                                    PyObject *const *args, Py_ssize_t nargs,
                                    PyObject **helper_name)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    *helper_name = NULL;
    if (invocation == NULL || soac_dataclass_current(invocation) < 0 ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        !soac_dataclass_frame_valid(parent) ||
        soac_dataclass_bridge_kind(builtin) != Py_SOAC_DATACLASS_BRIDGE_INIT_VALUE ||
        nargs != 3 || !PyUnicode_CheckExact(args[1]) || !PyDict_CheckExact(args[2])) {
        return invocation == NULL
            ? soac_dataclass_error("generated init expression has no invocation")
            : soac_dataclass_fail(invocation, "generated init expression is not authentic");
    }
    PySoacDataclassFrameView view = {
        .frame = parent, .role = parent->soac_dataclass_role,
        .invocation = (PyObject *)invocation,
    };
    int selected = _PyInterpreterState_GET()->soac.dataclass_callbacks.init_value(
        invocation->owner, &view, args[0], args[1], args[2], helper_name);
    if (selected < 0 || selected > 1 || PyErr_Occurred() ||
        !soac_dataclass_is_running(invocation) || invocation->owner == NULL ||
        (selected == 1 && (*helper_name == NULL ||
                          !PyUnicode_CheckExact(*helper_name) ||
                          !PyUnicode_IsIdentifier(*helper_name)))) {
        return soac_dataclass_fail(invocation, "generated init expression selection failed");
    }
    return selected;
}

static PyObject *
soac_dataclass_init_value_bridge(_PyInterpreterFrame *parent, PyObject *builtin,
                                  PyObject *const *args, Py_ssize_t nargs)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    PyObject *name = NULL;
    int selected = soac_dataclass_validate_init_value(parent, builtin, args, nargs, &name);
    if (selected < 0) {
        return NULL;
    }
    if (!selected) {
        return Py_NewRef(args[1]);
    }
    Py_INCREF(name);
    PyObject *helper = PySoac_GetDataclassBuiltin(Py_SOAC_DATACLASS_BRIDGE_VALUE);
    if (helper == NULL) {
        soac_dataclass_fail(invocation, "generated value bridge is unavailable");
        Py_DECREF(name);
        return NULL;
    }
    Py_INCREF(helper);
    PyObject *result = PyUnicode_FromFormat("%U(%U)", name, args[1]);
    if (result == NULL) {
        goto error;
    }
    PyObject *rechecked_name = NULL;
    if (soac_dataclass_validate_init_value(parent, builtin, args, nargs, &rechecked_name) != 1 ||
        rechecked_name != name) {
        goto error;
    }
    PyObject *existing = NULL;
    if (PyDict_GetItemRef(args[2], name, &existing) < 0) {
        goto error;
    }
    if (existing != NULL && existing != helper) {
        soac_dataclass_fail(invocation, "generated value helper capture collides with an existing binding");
        Py_DECREF(existing);
        goto error;
    }
    if (soac_dataclass_validate_init_value(parent, builtin, args, nargs, &rechecked_name) != 1 ||
        rechecked_name != name ||
        PySoac_GetDataclassBuiltin(Py_SOAC_DATACLASS_BRIDGE_VALUE) != helper) {
        Py_XDECREF(existing);  /* NULL or the separately pinned canonical helper. */
        goto error;
    }
    if (existing == NULL && PyDict_SetItem(args[2], name, helper) < 0) {
        goto error;
    }
    Py_XDECREF(existing);
    if (soac_dataclass_validate_init_value(parent, builtin, args, nargs, &rechecked_name) != 1 ||
        rechecked_name != name ||
        PySoac_GetDataclassBuiltin(Py_SOAC_DATACLASS_BRIDGE_VALUE) != helper) {
        goto error;
    }
    /* Final capture identity check is callback-free, even if hostile keys
     * were inserted during an earlier allocation/lookup. The policy callback
     * separately validates the actual entire builder-locals schema. */
    Py_ssize_t position = 0;
    PyObject *key, *value;
    int captured = 0;
    while (PyDict_Next(args[2], &position, &key, &value)) {
        if (PyUnicode_CheckExact(key) && _PyUnicode_Equal(key, name)) {
            captured = value == helper;
            break;
        }
    }
    if (!captured) {
        soac_dataclass_fail(invocation, "generated value helper capture changed during publication");
        goto error;
    }
    Py_DECREF(helper);
    Py_DECREF(name);
    return result;
error:
    soac_dataclass_fail(invocation, "generated init expression could not be published");
    Py_XDECREF(result);
    Py_DECREF(helper);
    Py_DECREF(name);
    return NULL;
}

#include "soac_dataclass_slots.inc"

static PyObject *
soac_dataclass_dispatch_bridge(_PyInterpreterFrame *parent, PyObject *builtin,
                               unsigned int kind, PyObject *const *args,
                               Py_ssize_t nargs, PyObject *kwnames)
{
    SoacDataclassInvocation *invocation =
        (SoacDataclassInvocation *)parent->soac_dataclass_invocation;
    if (kwnames != NULL && PyTuple_GET_SIZE(kwnames) != 0) {
        soac_dataclass_fail(invocation, "dataclass native bridges take positional operands");
        return NULL;
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_INIT_VALUE) {
        return soac_dataclass_init_value_bridge(parent, builtin, args, nargs);
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS) {
        return soac_dataclass_new_slots_bridge(parent, builtin, args, nargs);
    }
    if (soac_dataclass_validate_bridge(parent, builtin, kind, args, nargs) < 0) {
        return NULL;
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_SOURCE) {
        return Py_NewRef(args[0]);
    }
    if (kind == Py_SOAC_DATACLASS_BRIDGE_EXEC) {
        return soac_dataclass_exec_bridge(parent, builtin, args);
    }
    assert(kind == Py_SOAC_DATACLASS_BRIDGE_MEMBER);
    int recorded = PyFunction_Check(args[3])
        ? PyFunction_HasSoacDataclassCreation(args[3]) : 0;
    if (recorded < 0) {
        soac_dataclass_fail(invocation, "dataclass member creation is unavailable");
        return NULL;
    }
    if (recorded == 1) {
        if (PyType_SetSoacDataclassMember(
                (PyObject *)invocation, args[1], args[2], args[3]) < 0) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    /* The explicit catalog can approve existing shared helpers or ordinary
     * metadata. They keep their normal setter path and acquire no ownership,
     * seal, generated-member override, or function/body capability. */
    PyObject *result = PyObject_Vectorcall(args[0], args + 1, 3, NULL);
    if (result == NULL || !soac_dataclass_is_running(invocation)) {
        soac_dataclass_fail(invocation, "dataclass ordinary member installation failed");
        Py_XDECREF(result);
        return NULL;
    }
    return result;
}
