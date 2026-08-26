/* C-only physical-slot test policy. No artifact/source/JIT authority. The
 * ordinary tuple owner makes every test payload edge visible to cyclic GC. */

static int
soac_slot_test_write(PyObject *owner, PyObject *instance,
                     PyObject *name, PyObject *value)
{
    PyObject *checked = PyTuple_GET_ITEM(owner, 1);
    for (Py_ssize_t i = 0; value != NULL && i < PyTuple_GET_SIZE(checked); ++i) {
        if (_PyUnicode_Equal(name, PyTuple_GET_ITEM(checked, i)) &&
            !PyLong_CheckExact(value)) {
            PyErr_SetString(PyExc_TypeError, "native object slot requires exact int");
            return -1;
        }
    }
    return 0;
}

static int
soac_slot_test_bind(PyObject *owner, PyObject *actual)
{
    PyObject *observations = PyTuple_GET_ITEM(owner, 2);
    if (!PyList_CheckExact(observations) || PyList_GET_SIZE(observations) != 2 ||
        PyList_GET_ITEM(observations, 0) != Py_False ||
        PyList_GET_ITEM(observations, 1) != Py_False) {
        PyErr_SetString(PyExc_AssertionError, "slot fixture observation storage changed");
        return -1;
    }
    if (((PyTypeObject *)actual)->tp_flags & Py_TPFLAGS_READY) {
        PyErr_SetString(PyExc_AssertionError, "slot fixture bound after Ready");
        return -1;
    }
    PyObject *fields = PyTuple_GET_ITEM(owner, 0);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(fields); ++i) {
        Py_ssize_t offset;
        if (PyType_GetSoacObjectSlotOffset(actual, owner, i, &offset) != 1) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_AssertionError, "slot catalog missing before Ready");
            }
            return -1;
        }
    }
    int watcher = (int)PyLong_AsLong(PyTuple_GET_ITEM(owner, 4));
    if (watcher != -1 && PyDict_Watch(watcher, ((PyTypeObject *)actual)->tp_dict) < 0) {
        return -1;
    }
    /* The caller reserved these two immortal bool entries before construction.
     * Neither replacement allocates nor releases an observable Python value. */
    PyList_SetItem(observations, 0, Py_NewRef(Py_True));
    PyList_SetItem(observations, 1, Py_NewRef(Py_True));
    return 0;
}

static PyObject *
soac_slot_type(PyObject *self, PyObject *args)
{
    PyObject *name, *bases, *namespace, *fields, *checked;
    PyObject *namespace_function, *observations, *payload;
    PyObject *dictionary_fields = NULL;
    int watcher = -1;
    if (!PyArg_ParseTuple(args, "OOOOOOOO|iO:soac_slot_type", &name, &bases,
                          &namespace, &fields, &checked, &namespace_function,
                          &observations, &payload, &watcher, &dictionary_fields)) {
        return NULL;
    }
    if (!PyTuple_CheckExact(fields) || !PyTuple_CheckExact(checked) ||
        !PyList_CheckExact(observations) || PyList_GET_SIZE(observations) != 2 ||
        PyList_GET_ITEM(observations, 0) != Py_False ||
        PyList_GET_ITEM(observations, 1) != Py_False) {
        PyErr_SetString(PyExc_TypeError, "invalid native slot fixture inputs");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checked); ++i) {
        if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(checked, i))) {
            PyErr_SetString(PyExc_TypeError, "exact string slot checks required");
            return NULL;
        }
    }
    PyObject *watcher_id = PyLong_FromLong(watcher);
    if (watcher_id == NULL) {
        return NULL;
    }
    PyObject *owner = PyTuple_Pack(5, fields, checked, observations, payload, watcher_id);
    Py_DECREF(watcher_id);
    PyObject *empty = PyTuple_New(0);
    PyObject *keywords = PyDict_New();
    if (owner == NULL || empty == NULL || keywords == NULL) {
        Py_XDECREF(owner);
        Py_XDECREF(empty);
        Py_XDECREF(keywords);
        return NULL;
    }
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(PySoacTypeConstructionSpec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_ENFORCED,
        .owner = owner,
        .namespace_function = namespace_function,
        .name = name,
        .bases = bases,
        .namespace_dict = namespace,
        .keywords = keywords,
        .bind_type = soac_slot_test_bind,
        .contract = {
            .fields = dictionary_fields == NULL ? empty : dictionary_fields,
            .protected_names = empty,
            .final_methods = empty,
            .check_instance_write = soac_slot_test_write,
            .object_slot_fields = fields,
        },
    };
    PyObject *handle = PyType_NewSoacConstructionHandle(&spec);
    PyObject *actual = handle == NULL ? NULL
        : PyType_FromSoacConstructionHandle(handle, namespace_function);
    Py_XDECREF(handle);
    Py_DECREF(owner);
    Py_DECREF(empty);
    Py_DECREF(keywords);
    return actual;
}

static int
soac_slot_test_view(PyObject *descriptor, PyObject *receiver, PyObject *name,
                    int kind, Py_ssize_t delta, int flags, PyMemberDef *view)
{
    if (!Py_IS_TYPE(descriptor, &PyMemberDescr_Type) ||
        !PyObject_TypeCheck(receiver, PyDescr_TYPE(descriptor))) {
        PyErr_SetString(PyExc_TypeError, "expected actual member descriptor and receiver");
        return -1;
    }
    *view = *((PyMemberDescrObject *)descriptor)->d_member;
    if (name != Py_None) {
        if (!PyUnicode_CheckExact(name)) {
            PyErr_SetString(PyExc_TypeError, "exact member-view name required");
            return -1;
        }
        view->name = PyUnicode_AsUTF8(name);
        if (view->name == NULL) {
            return -1;
        }
    }
    if (kind != -1) {
        view->type = kind;
    }
    if ((delta > 0 && view->offset > PY_SSIZE_T_MAX - delta) ||
        (delta < 0 && view->offset < PY_SSIZE_T_MIN - delta)) {
        PyErr_SetString(PyExc_OverflowError, "member-view offset overflow");
        return -1;
    }
    view->offset += delta;
    if (flags != -1) {
        view->flags = flags;
    }
    return 0;
}

static PyObject *
soac_slot_view_get(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *names[] = {"descriptor", "receiver", "name", "member_type",
                            "offset_delta", "flags", NULL};
    PyObject *descriptor, *receiver, *name = Py_None;
    int kind = -1, flags = -1;
    Py_ssize_t delta = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|Oini:soac_slot_view_get",
                                     names, &descriptor, &receiver, &name,
                                     &kind, &delta, &flags)) {
        return NULL;
    }
    PyMemberDef view;
    if (soac_slot_test_view(descriptor, receiver, name, kind, delta, flags, &view) < 0) {
        return NULL;
    }
    return PyMember_GetOne((const char *)receiver, &view);
}

static PyObject *
soac_slot_view_set(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *names[] = {"descriptor", "receiver", "value", "name", "member_type",
                            "offset_delta", "flags", "delete", NULL};
    PyObject *descriptor, *receiver, *value, *name = Py_None;
    int kind = -1, flags = -1, deletion = 0;
    Py_ssize_t delta = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO|Oinip:soac_slot_view_set",
                                     names, &descriptor, &receiver, &value, &name,
                                     &kind, &delta, &flags, &deletion)) {
        return NULL;
    }
    PyMemberDef view;
    if (soac_slot_test_view(descriptor, receiver, name, kind, delta, flags, &view) < 0 ||
        PyMember_SetOne((char *)receiver, &view, deletion ? NULL : value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
soac_slot_definition(PyObject *self, PyObject *descriptor)
{
    if (!Py_IS_TYPE(descriptor, &PyMemberDescr_Type)) {
        PyErr_SetString(PyExc_TypeError, "expected exact native member descriptor");
        return NULL;
    }
    PyMemberDef *member = ((PyMemberDescrObject *)descriptor)->d_member;
    return Py_BuildValue("(ini)", member->type, member->offset, member->flags);
}

static PyObject *
soac_type_construction_layout(PyObject *self, PyObject *Py_UNUSED(args))
{
    return Py_BuildValue("{s:i,s:n,s:n,s:n,s:n,s:n,s:i,s:n,s:n}",
        "abi_version", Py_SOAC_TYPE_CONTRACT_ABI,
        "spec_size", (Py_ssize_t)sizeof(PySoacTypeConstructionSpec),
        "contract_size", (Py_ssize_t)sizeof(PySoacTypeContractSpecV4),
        "contract", (Py_ssize_t)offsetof(PySoacTypeConstructionSpec, contract),
        "commit_final", (Py_ssize_t)offsetof(PySoacTypeConstructionSpec, commit_final),
        "object_slot_fields", (Py_ssize_t)(offsetof(PySoacTypeConstructionSpec, contract) +
            offsetof(PySoacTypeContractSpecV4, object_slot_fields)),
        "dataclass_abi_version", Py_SOAC_DATACLASS_ABI,
        "dataclass_callbacks_size", (Py_ssize_t)sizeof(PySoacDataclassCallbacks),
        "prepare_slots", (Py_ssize_t)offsetof(PySoacDataclassCallbacks, prepare_slots));
}

static int
soac_slot_test_check_plan(PyObject *plan)
{
    if (plan == Py_None) {
        return 0;
    }
    if (!PyTuple_CheckExact(plan) || PyTuple_GET_SIZE(plan) != 6 ||
        !PyTuple_CheckExact(PyTuple_GET_ITEM(plan, 0)) ||
        !PyTuple_CheckExact(PyTuple_GET_ITEM(plan, 1)) ||
        !PyList_CheckExact(PyTuple_GET_ITEM(plan, 2)) ||
        (PyList_GET_SIZE(PyTuple_GET_ITEM(plan, 2)) != 2 &&
         PyList_GET_SIZE(PyTuple_GET_ITEM(plan, 2)) != 4) ||
        PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 2), 0) != Py_False ||
        PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 2), 1) != Py_False ||
        !PyList_CheckExact(PyTuple_GET_ITEM(plan, 3)) ||
        PyList_GET_SIZE(PyTuple_GET_ITEM(plan, 3)) != 1 ||
        PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 3), 0) != Py_None ||
        !PyLong_CheckExact(PyTuple_GET_ITEM(plan, 5))) {
        goto bad;
    }
    for (int field = 0; field < 2; field++) {
        PyObject *names = PyTuple_GET_ITEM(plan, field);
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(names); ++i) {
            if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(names, i))) {
                goto bad;
            }
        }
    }
    long mode = PyLong_AsLong(PyTuple_GET_ITEM(plan, 5));
    if (mode < 0 || mode > 3 ||
        PyList_GET_SIZE(PyTuple_GET_ITEM(plan, 2)) != (mode == 3 ? 4 : 2)) {
        goto bad;
    }
    if (mode == 3 &&
        (PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 2), 2) != Py_False ||
         PyList_GET_ITEM(PyTuple_GET_ITEM(plan, 2), 3) != Py_False)) goto bad;
    return 0;
bad:
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "invalid native replacement fixture plan");
    }
    return -1;
}

static int
soac_dc_test_prepare_slots(PyObject *owner, const PySoacDataclassFrameView *producer,
                            PyObject *metaclass, PyObject *name, PyObject *bases,
                            PyObject *namespace, PyObject *original, PyObject **out)
{
    *out = NULL;
    PyObject *plan = PyTuple_GET_ITEM(owner, 7);
    if (plan == Py_None) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                        "fixture has no selected replacement plan");
        return -1;
    }
    long mode = PyLong_AsLong(PyTuple_GET_ITEM(plan, 5));
    PyObject *slot_owner;
    if (mode == 3) {
        /* The same actual slots bridge creates a distinct pending owner;
         * the fixture adds no source or native-creation authority. */
        slot_owner = PyTuple_Pack(4, PyTuple_GET_ITEM(plan, 2), Py_None,
            PyTuple_GET_ITEM(plan, 1), PyTuple_GET_ITEM(plan, 4));
    }
    else {
        PyObject *minus_one = PyLong_FromLong(-1);
        if (minus_one == NULL) {
            return -1;
        }
        slot_owner = PyTuple_Pack(5, PyTuple_GET_ITEM(plan, 0),
            PyTuple_GET_ITEM(plan, 1), PyTuple_GET_ITEM(plan, 2),
            PyTuple_GET_ITEM(plan, 4), minus_one);
        Py_DECREF(minus_one);
    }
    PyObject *empty = PyTuple_New(0), *keywords = PyDict_New();
    if (slot_owner == NULL || empty == NULL || keywords == NULL) {
        Py_XDECREF(slot_owner);
        Py_XDECREF(empty);
        Py_XDECREF(keywords);
        return -1;
    }
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(PySoacTypeConstructionSpec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_ENFORCED,
        .owner = mode == 2 ? PyType_GetSoacContractOwner(original) : slot_owner,
        .namespace_function = NULL,
        .name = name,
        .bases = bases,
        .namespace_dict = namespace,
        .keywords = keywords,
        .bind_type = soac_slot_test_bind,
        .contract = {
            .fields = empty,
            .protected_names = empty,
            .final_methods = empty,
            .check_instance_write = soac_slot_test_write,
            .object_slot_fields = PyTuple_GET_ITEM(plan, 0),
        },
    };
    if (mode == 3) {
        spec.construction_mode = Py_SOAC_TYPE_CONSTRUCT_PENDING;
        spec.bind_type = soac_pending_test_bind;
        spec.commit_final = soac_pending_test_commit;
        memset(&spec.contract, 0, sizeof(spec.contract));
    }
    PyObject *handle = PyType_NewSoacDataclassSlotsHandle(producer, &spec);
    if (handle != NULL) {
        PyObject *handles = PyTuple_GET_ITEM(plan, 3);
        if (!PyList_CheckExact(handles) || PyList_GET_SIZE(handles) != 1 ||
            PyList_GET_ITEM(handles, 0) != Py_None) {
            PyErr_SetString(PyExc_AssertionError, "slot fixture handle storage changed");
            Py_CLEAR(handle);
        }
        else {
            PyList_SetItem(handles, 0, Py_NewRef(handle));
        }
        if (handle != NULL && mode == 1) {
            PyObject *duplicate = PyType_NewSoacDataclassSlotsHandle(producer, &spec);
            Py_XDECREF(duplicate);
            Py_CLEAR(handle);
        }
    }
    Py_DECREF(slot_owner);
    Py_DECREF(empty);
    Py_DECREF(keywords);
    *out = handle;
    return handle == NULL ? -1 : 0;
}
