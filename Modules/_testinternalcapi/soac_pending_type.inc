/* Test-only actual native pending construction/admission operations.
 * No source, function, type-layout or compiler capability is fabricated.
 * The owner is a GC-visible tuple supplied by the test; reserved observations
 * contain immortal booleans. Final commit allocates/calls no Python; pending
 * early bind may invoke the explicitly supplied reentrant preparation probe. */

static int soac_pending_test_write(PyObject *, PyObject *, PyObject *, PyObject *);

static int
soac_pending_test_owner(PyObject *owner)
{
    if (!PyTuple_CheckExact(owner) || PyTuple_GET_SIZE(owner) != 4 ||
        !PyList_CheckExact(PyTuple_GET_ITEM(owner, 0)) ||
        PyList_GET_SIZE(PyTuple_GET_ITEM(owner, 0)) != 4 ||
        !PyTuple_CheckExact(PyTuple_GET_ITEM(owner, 2))) {
        PyErr_SetString(PyExc_TypeError, "invalid pending-type fixture owner");
        return -1;
    }
    PyObject *observations = PyTuple_GET_ITEM(owner, 0);
    for (int i = 0; i < 4; ++i) {
        PyObject *value = PyList_GET_ITEM(observations, i);
        if (value != Py_False && value != Py_True) {
            PyErr_SetString(PyExc_TypeError, "fixture observation is not reserved");
            return -1;
        }
    }
    PyObject *checked = PyTuple_GET_ITEM(owner, 2);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checked); ++i) {
        if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(checked, i))) {
            PyErr_SetString(PyExc_TypeError, "fixture checked field must be exact Unicode");
            return -1;
        }
    }
    PyObject *error = PyTuple_GET_ITEM(owner, 1);
    if (error != Py_None && !PyExceptionInstance_Check(error)) {
        PyErr_SetString(PyExc_TypeError, "fixture error must be an exception");
        return -1;
    }
    return 0;
}

static int
soac_pending_test_bind(PyObject *owner, PyObject *actual)
{
    PySoacTypeConstructionInfoV1 info;
    if (soac_pending_test_owner(owner) < 0 ||
        PyType_GetSoacConstructionInfoV1(actual, &info, sizeof(info)) != 1 ||
        info.phase != Py_SOAC_TYPE_STATE_PENDING ||
        info.permanent_contract_published || info.owner != owner ||
        info.root_construction == NULL ||
        PyType_HasFeature((PyTypeObject *)actual, Py_TPFLAGS_READY)) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_AssertionError, "pending bind phase mismatch");
        }
        return -1;
    }
    /* Publish only a scalar observation; never retain the actual type. */
    if (PyList_SetItem(PyTuple_GET_ITEM(owner, 0), 0, Py_NewRef(Py_True)) < 0) return -1;
    PyObject *prepare = PyTuple_GET_ITEM(owner, 3);
    if (PyCallable_Check(prepare)) {
        PyObject *result = PyObject_CallOneArg(prepare, actual);
        if (result == NULL) return -1;
        Py_DECREF(result);
    }
    return 0;
}

static int
soac_pending_test_commit(PyObject *owner, PyObject *actual,
                         const PySoacTypeContractSpecV4 *contract)
{
    PySoacTypeConstructionInfoV1 info;
    if (soac_pending_test_owner(owner) < 0 ||
        PyType_GetSoacConstructionInfoV1(actual, &info, sizeof(info)) != 1 ||
        info.phase != Py_SOAC_TYPE_STATE_ADMITTING || info.owner != owner ||
        info.root_construction == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_AssertionError, "pending final commit phase mismatch");
        }
        return -1;
    }
    PyObject *observations = PyTuple_GET_ITEM(owner, 0);
    PyList_SetItem(observations, 1, Py_NewRef(Py_True));
    PyList_SetItem(observations, 2, Py_NewRef(Py_True));
    PyList_SetItem(observations, 3, Py_NewRef(
        info.permanent_contract_published ? Py_True : Py_False));
    if (contract->dictionary_mode != Py_SOAC_INSTANCE_DICT_NONE ||
        contract->check_instance_write != soac_pending_test_write ||
        contract->new_instance_dict != NULL ||
        contract->prepare_instance_dictionary_policy != NULL) {
        PyErr_SetString(PyExc_AssertionError, "pending fixture contract mode/callback mismatch");
        return -1;
    }
    PyObject *required = PyTuple_GET_ITEM(owner, 2);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(required); ++i) {
        PyObject *name = PyTuple_GET_ITEM(required, i);
        int found = 0;
        if (contract->object_slot_fields != NULL) {
            for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(contract->object_slot_fields); ++j) {
                found |= _PyUnicode_Equal(name, PyTuple_GET_ITEM(contract->object_slot_fields, j));
            }
        }
        if (!found) {
            PyErr_SetString(PyExc_AssertionError, "pending fixture required field was omitted");
            return -1;
        }
    }
    PyObject *error = PyTuple_GET_ITEM(owner, 1);
    if (error != Py_None) {
        PyErr_SetObject((PyObject *)Py_TYPE(error), error);
        return -1;
    }
    return 0;
}

static int
soac_pending_test_write(PyObject *owner, PyObject *instance,
                        PyObject *name, PyObject *value)
{
    PyObject *checked = PyTuple_GET_ITEM(owner, 2);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checked); ++i) {
        if (PyUnicode_CheckExact(name) &&
            _PyUnicode_Equal(name, PyTuple_GET_ITEM(checked, i)) &&
            value != NULL && !PyLong_CheckExact(value)) {
            PyErr_SetString(PyExc_TypeError, "pending fixture slot requires exact int");
            return -1;
        }
    }
    return 0;
}

static PyObject *
soac_pending_type_construct(PyObject *module, PyObject *args)
{
    PyObject *name, *bases, *namespace, *function, *owner;
    if (!PyArg_ParseTuple(args, "OOOOO", &name, &bases, &namespace, &function, &owner) ||
        soac_pending_test_owner(owner) < 0) {
        return NULL;
    }
    PyObject *keywords = PyDict_New();
    if (keywords == NULL) return NULL;
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(PySoacTypeConstructionSpec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_PENDING,
        .owner = owner, .namespace_function = function, .name = name,
        .bases = bases, .namespace_dict = namespace, .keywords = keywords,
        .bind_type = soac_pending_test_bind,
        .commit_final = soac_pending_test_commit,
        .contract = {0},
    };
    PyObject *handle = PyType_NewSoacConstructionHandle(&spec);
    Py_DECREF(keywords);
    if (handle == NULL) return NULL;
    PyObject *actual = PyType_FromSoacConstructionHandle(handle, function);
    PyObject *result = actual == NULL ? NULL : PyTuple_Pack(2, actual, handle);
    Py_XDECREF(actual);
    Py_DECREF(handle);
    return result;
}

static PyObject *
soac_pending_type_admit(PyObject *module, PyObject *args)
{
    PyObject *actual, *owner, *root, *fields, *protected, *final, *slots;
    unsigned int flags = 0;
    if (!PyArg_ParseTuple(args, "OOOOOOO|I", &actual, &owner, &root,
                         &fields, &protected, &final, &slots, &flags) ||
        soac_pending_test_owner(owner) < 0) {
        return NULL;
    }
    PySoacTypeContractSpecV4 spec = {
        .flags = flags, .fields = fields, .protected_names = protected,
        .final_methods = final, .object_slot_fields = slots == Py_None ? NULL : slots,
        .check_instance_write = soac_pending_test_write,
    };
    if (PyType_AdmitSoacPendingV1(actual, owner, root, &spec, sizeof(spec),
                                 soac_pending_test_commit) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

/* A real C caller owns this one payload through the public admission call,
 * then retires it before returning to Python. This tests the first C cleanup
 * boundary, not an invented callback inside the exact-name input tuples. */
static PyObject *
soac_pending_type_admit_and_retire(PyObject *module, PyObject *args)
{
    PyObject *admission_args, *holder;
    if (!PyArg_ParseTuple(args, "OO", &admission_args, &holder) ||
        !PyTuple_CheckExact(admission_args) ||
        !PyList_CheckExact(holder) || PyList_GET_SIZE(holder) != 1) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "expected actual admission args and one release holder");
        }
        return NULL;
    }
    PyObject *retired = PyList_GET_ITEM(holder, 0);  /* MOVE from actual slot */
    PyList_SET_ITEM(holder, 0, Py_NewRef(Py_None));
    PyObject *result = soac_pending_type_admit(module, admission_args);
    PyObject *primary = PyErr_GetRaisedException();
    Py_DECREF(retired);
    PyErr_SetRaisedException(primary);
    return result;
}

/* This attempted callback is deliberately different from the one captured
 * at construction. The equality preflight must reject without invoking it. */
static int
soac_pending_test_noop(PyObject *owner, PyObject *actual,
                       const PySoacTypeContractSpecV4 *contract)
{
    PyList_SetItem(PyTuple_GET_ITEM(owner, 0), 1, Py_NewRef(Py_True));
    return 0;
}

static PyObject *
soac_pending_type_try_weaker_admission(PyObject *module, PyObject *args)
{
    PyObject *actual, *owner, *root;
    int substitute;
    if (!PyArg_ParseTuple(args, "OOOp", &actual, &owner, &root, &substitute) ||
        soac_pending_test_owner(owner) < 0) return NULL;
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) return NULL;
    PySoacTypeContractSpecV4 spec = {
        .fields = empty, .protected_names = empty, .final_methods = empty,
        .check_instance_write = soac_pending_test_write,
    };
    int result = PyType_AdmitSoacPendingV1(actual, owner, root, &spec, sizeof(spec),
        substitute ? soac_pending_test_noop : soac_pending_test_commit);
    Py_DECREF(empty);
    if (result < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
soac_pending_type_preserve_error(PyObject *module, PyObject *args)
{
    PyObject *actual, *root, *error;
    if (!PyArg_ParseTuple(args, "OOO", &actual, &root, &error) ||
        !PyExceptionInstance_Check(error)) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "expected exception");
        return NULL;
    }
    PySoacTypeConstructionInfoV1 info;
    PyErr_SetObject((PyObject *)Py_TYPE(error), error);
    int query = PyType_GetSoacConstructionInfoV1(actual, &info, sizeof(info));
    PyObject *after_query = PyErr_GetRaisedException();
    PyErr_SetRaisedException(Py_NewRef(error));
    int failed = PyType_FailSoacPendingV1(root);
    PyObject *after_fail = PyErr_GetRaisedException();
    PyObject *result = Py_BuildValue("(iiii)", query, failed,
        after_query == error, after_fail == error);
    Py_XDECREF(after_query);
    Py_XDECREF(after_fail);
    return result;
}

/* Status-returning allocation APIs are called with actual pending types.
 * Non-GC raw results must not run the pending subtype's GC destructor if a
 * broken barrier unexpectedly returns an object. Convert only that test-owned
 * unexpected allocation to the matching ordinary deallocator, then fail the
 * Python assertRaises control. This creates no pending-state mutation/grant. */
static void
soac_pending_discard_raw(PyObject *object, int gc_allocated)
{
    PyTypeObject *type = Py_TYPE(object);
    if (gc_allocated) {
        assert(type->tp_basicsize >= (Py_ssize_t)sizeof(PyListObject));
        Py_SET_TYPE(object, &PyList_Type);
        Py_SET_SIZE(object, 0);
        ((PyListObject *)object)->ob_item = NULL;
        ((PyListObject *)object)->allocated = 0;
    }
    else {
        Py_SET_TYPE(object, &PyBaseObject_Type);
    }
    Py_DECREF(type);  /* all fixture types are actual heap types */
    Py_DECREF(object);
}

static PyObject *
soac_pending_type_allocate(PyObject *module, PyObject *args)
{
    PyObject *actual;
    int operation;
    if (!PyArg_ParseTuple(args, "Oi", &actual, &operation) || !PyType_Check(actual)) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "expected actual type");
        return NULL;
    }
    PyTypeObject *type = (PyTypeObject *)actual;
    if (operation >= 2 && (type->tp_basicsize < (Py_ssize_t)sizeof(PyListObject) ||
                           !PyType_IsSubtype(type, &PyList_Type))) {
        PyErr_SetString(PyExc_TypeError, "raw allocation control needs actual list layout");
        return NULL;
    }
    PyObject *result = NULL;
    switch (operation) {
        case 0: result = PyType_GenericAlloc(type, 0); break;
        case 1: result = PyType_GenericNew(type, NULL, NULL); break;
        case 2: result = _PyObject_New(type); break;
        case 3: result = (PyObject *)_PyObject_NewVar(type, 0); break;
        case 4: result = _PyObject_GC_New(type); break;
        case 5: result = (PyObject *)_PyObject_GC_NewVar(type, 0); break;
        case 6: result = PyUnstable_Object_GC_NewWithExtraData(type, 17); break;
        default:
            PyErr_SetString(PyExc_ValueError, "unknown allocation operation");
            return NULL;
    }
    if (result == NULL) return NULL;
    if (operation < 2) Py_DECREF(result);
    else soac_pending_discard_raw(result, operation >= 4);
    Py_RETURN_NONE;
}

static PyObject *
soac_pending_type_init_buffer(PyObject *module, PyObject *args)
{
    PyObject *actual, *incoming;
    int variable;
    if (!PyArg_ParseTuple(args, "OpO", &actual, &variable, &incoming) ||
        !PyType_Check(actual) ||
        (incoming != Py_None && !PyExceptionInstance_Check(incoming))) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "invalid buffer control");
        return NULL;
    }
    PyTypeObject *type = (PyTypeObject *)actual;
    size_t size = (size_t)type->tp_basicsize + sizeof(PyVarObject) + 32;
    void *buffer = PyObject_Malloc(size);
    void *before = PyMem_Malloc(size);
    if (buffer == NULL || before == NULL) {
        PyObject_Free(buffer);
        PyMem_Free(before);
        return PyErr_NoMemory();
    }
    memset(buffer, 0xA5, size);
    memcpy(before, buffer, size);
    Py_ssize_t type_refs = Py_REFCNT(type);
    if (incoming != Py_None) PyErr_SetObject((PyObject *)Py_TYPE(incoming), incoming);
    PyObject *result = variable
        ? (PyObject *)PyObject_InitVar((PyVarObject *)buffer, type, 0)
        : PyObject_Init((PyObject *)buffer, type);
    PyObject *error = PyErr_GetRaisedException();
    int unchanged = memcmp(buffer, before, size) == 0;
    int refs_unchanged = Py_REFCNT(type) == type_refs;
    if (result != NULL) soac_pending_discard_raw(result, 0);
    else PyObject_Free(buffer);  /* rejected API never owns caller storage */
    PyMem_Free(before);
    PyObject *answer = Py_BuildValue("(Oiii)", error == NULL ? Py_None : error,
        result == NULL, unchanged, refs_unchanged);
    Py_XDECREF(error);
    return answer;
}

static PyObject *
soac_pending_type_from_spec(PyObject *module, PyObject *base)
{
    if (!PyType_Check(base)) {
        PyErr_SetString(PyExc_TypeError, "expected base type");
        return NULL;
    }
    PyType_Slot slots[] = {{0, NULL}};
    PyType_Spec spec = {
        .name = "_testinternalcapi.PendingDerived",
        .basicsize = 0, .itemsize = 0,
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, .slots = slots,
    };
    PyObject *bases = PyTuple_Pack(1, base);
    if (bases == NULL) return NULL;
    PyObject *result = PyType_FromSpecWithBases(&spec, bases);
    Py_DECREF(bases);
    return result;
}
