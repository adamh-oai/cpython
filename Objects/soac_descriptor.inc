/* Included by funcobject.c after the exact builtin descriptor definitions.
 * A birth is a mechanical native witness, not source or optimizer authority. */

int
_PySOAC_DescriptorFactoryIsOriginal(PyObject *factory)
{
    if (factory == (PyObject *)&PyProperty_Type)
        return _PySOAC_PropertyFactoryIsOriginal();
    if (factory != (PyObject *)&PyStaticMethod_Type &&
        factory != (PyObject *)&PyClassMethod_Type)
        return 0;
    PyTypeObject *type = (PyTypeObject *)factory;
    int is_static = type == &PyStaticMethod_Type;
    return Py_TYPE(type) == &PyType_Type && _PySOAC_TypeCallIsOriginal() &&
        type->tp_vectorcall == NULL &&
        type->tp_basicsize == (is_static ? sizeof(staticmethod) : sizeof(classmethod)) &&
        type->tp_itemsize == 0 && type->tp_alloc == PyType_GenericAlloc &&
        type->tp_free == PyObject_GC_Del &&
        type->tp_new == (is_static ? sm_new : cm_new) &&
        type->tp_init == (is_static ? sm_init : cm_init);
}

enum {
    SOAC_DESCRIPTOR_CONSTRUCTING,
    SOAC_DESCRIPTOR_PENDING,
    SOAC_DESCRIPTOR_ADOPTED,
    SOAC_DESCRIPTOR_INVALID,
};

typedef struct {
    PyObject_HEAD
    PyObject *namespace_owner;
    PyObject *code_ref;
    int64_t interpreter_id;
    uint64_t identity;
    uintptr_t descriptor_identity;
    uintptr_t function_identity;
    uintptr_t function_owner_identity;
    unsigned char state;
} SoacDescriptorBirth;

static PyTypeObject SoacDescriptorBirth_Type;

void
_PySoac_DescriptorBirth_Bind(PyObject *object, PyObject *descriptor)
{
    assert(Py_IS_TYPE(object, &SoacDescriptorBirth_Type));
    SoacDescriptorBirth *birth = (SoacDescriptorBirth *)object;
    assert(birth->state == SOAC_DESCRIPTOR_CONSTRUCTING);
    assert(birth->descriptor_identity == 0);
    birth->descriptor_identity = (uintptr_t)descriptor;
}

void
_PySoac_DescriptorBirth_Invalidate(PyObject *object)
{
    if (object == NULL) {
        return;
    }
    assert(Py_IS_TYPE(object, &SoacDescriptorBirth_Type));
    SoacDescriptorBirth *birth = (SoacDescriptorBirth *)object;
    birth->state = SOAC_DESCRIPTOR_INVALID;
    birth->descriptor_identity = 0;
    birth->function_identity = 0;
    birth->function_owner_identity = 0;
}

static int
soac_descriptor_birth_clear(PyObject *object)
{
    SoacDescriptorBirth *birth = (SoacDescriptorBirth *)object;
    _PySoac_DescriptorBirth_Invalidate(object);
    Py_CLEAR(birth->namespace_owner);
    Py_CLEAR(birth->code_ref);
    return 0;
}

void
_PySoac_DescriptorBirth_Clear(PyObject **address)
{
    PyObject *object = *address;
    *address = NULL;
    if (object != NULL) {
        assert(Py_IS_TYPE(object, &SoacDescriptorBirth_Type));
        /* The record may have escaped through GC introspection. A dead
         * descriptor must not leave even the inert namespace payload rooted. */
        soac_descriptor_birth_clear(object);
        Py_DECREF(object);
    }
}

static int
soac_descriptor_birth_traverse(PyObject *object, visitproc visit, void *arg)
{
    SoacDescriptorBirth *birth = (SoacDescriptorBirth *)object;
    Py_VISIT(birth->namespace_owner);
    Py_VISIT(birth->code_ref);
    return 0;
}

static void
soac_descriptor_birth_dealloc(PyObject *object)
{
    _PyObject_GC_UNTRACK(object);
    soac_descriptor_birth_clear(object);
    PyObject_GC_Del(object);
}

static PyTypeObject SoacDescriptorBirth_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_SoacDescriptorBirth",
    .tp_basicsize = sizeof(SoacDescriptorBirth),
    .tp_dealloc = soac_descriptor_birth_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = soac_descriptor_birth_traverse,
    .tp_clear = soac_descriptor_birth_clear,
};

static SoacDescriptorBirth *
soac_descriptor_birth(PyObject *descriptor, PyObject **function)
{
    PyObject *record;
    if (Py_IS_TYPE(descriptor, &PyStaticMethod_Type)) {
        staticmethod *method = (staticmethod *)descriptor;
        record = method->sm_soac_birth;
        *function = method->sm_callable;
    }
    else if (Py_IS_TYPE(descriptor, &PyClassMethod_Type)) {
        classmethod *method = (classmethod *)descriptor;
        record = method->cm_soac_birth;
        *function = method->cm_callable;
    }
    else if (Py_IS_TYPE(descriptor, &PyProperty_Type)) {
        propertyobject *property = (propertyobject *)descriptor;
        if (property->prop_set != NULL || property->prop_del != NULL) {
            return NULL;
        }
        record = property->prop_soac_birth;
        *function = property->prop_get;
    }
    else {
        return NULL;
    }
    if (record == NULL) {
        return NULL;
    }
    assert(Py_IS_TYPE(record, &SoacDescriptorBirth_Type));
    return (SoacDescriptorBirth *)record;
}

static int
soac_descriptor_birth_interpreter(SoacDescriptorBirth *birth)
{
    if (birth->interpreter_id !=
        PyInterpreterState_GetID(_PyInterpreterState_GET())) {
        func_soac_runtime_error("builtin descriptor birth belongs to an unavailable interpreter");
        return -1;
    }
    return 0;
}

static int
soac_descriptor_birth_current(SoacDescriptorBirth *birth,
                              PyObject *descriptor, PyObject *function)
{
    /* Reject a foreign birth before accessing the function's native owner.
     * Numeric birth IDs are interpreter-local, not cross-interpreter grants. */
    if (soac_descriptor_birth_interpreter(birth) < 0) {
        return -1;
    }
    if ((birth->state != SOAC_DESCRIPTOR_PENDING &&
         birth->state != SOAC_DESCRIPTOR_ADOPTED) ||
        birth->descriptor_identity != (uintptr_t)descriptor ||
        birth->namespace_owner == NULL || birth->code_ref == NULL ||
        function == NULL || !PyFunction_Check(function) ||
        birth->function_identity != (uintptr_t)function) {
        return 0;
    }
    /* The descriptor owns this exact function throughout this callback-free
     * check. Its attached owner and current code are therefore independently
     * live; the stored raw coordinates are never dereferenced. */
    PyObject *owner = PyFunction_GetSoacStrictOwner(function);
    if (owner == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    if (birth->function_owner_identity != (uintptr_t)owner) {
        return 0;
    }
    PyObject *code = (PyObject *)((PyFunctionObject *)function)->func_code;
    /* Compare only against the independently pinned current code. The weak
     * reference is cleared before its target can be freed/reused; there is no
     * weak upgrade/decref that could run a callback on an unrelated old code. */
    PyWeakReference *reference = (PyWeakReference *)birth->code_ref;
    return FT_ATOMIC_LOAD_PTR(reference->wr_object) == code;
}

PyObject *
PySoac_GetDescriptorBirthOwner(PyObject *descriptor)
{
    if (descriptor == NULL) {
        PyErr_SetString(PyExc_TypeError, "descriptor birth needs a non-NULL object");
        return NULL;
    }
    PyObject *function = NULL;
    SoacDescriptorBirth *birth = soac_descriptor_birth(descriptor, &function);
    if (birth == NULL ||
        soac_descriptor_birth_current(birth, descriptor, function) != 1) {
        return NULL;
    }
    return birth->namespace_owner;  /* borrowed */
}

uint64_t
PySoac_GetDescriptorBirthId(PyObject *descriptor)
{
    if (descriptor == NULL) {
        PyErr_SetString(PyExc_TypeError, "descriptor birth needs a non-NULL object");
        return 0;
    }
    PyObject *function = NULL;
    SoacDescriptorBirth *birth = soac_descriptor_birth(descriptor, &function);
    if (birth == NULL ||
        soac_descriptor_birth_current(birth, descriptor, function) != 1) {
        return 0;
    }
    assert(birth->identity != 0);
    return birth->identity;
}

int
PySoac_MatchesDescriptorBirth(PyObject *descriptor, PyObject *namespace_witness,
                              PyObject *function, PyObject *expected_owner,
                              PyObject *verified_code)
{
    if (descriptor == NULL || namespace_witness == NULL || function == NULL ||
        expected_owner == NULL || verified_code == NULL) {
        PyErr_SetString(PyExc_TypeError, "descriptor birth needs non-NULL operands");
        return -1;
    }
    PyObject *component = NULL;
    SoacDescriptorBirth *birth = soac_descriptor_birth(descriptor, &component);
    if (birth != NULL && soac_descriptor_birth_interpreter(birth) < 0) {
        return -1;
    }
    if (birth == NULL || component != function ||
        birth->namespace_owner != namespace_witness ||
        birth->function_owner_identity != (uintptr_t)expected_owner ||
        !PyFunction_Check(function) || !PyCode_Check(verified_code) ||
        (PyObject *)((PyFunctionObject *)function)->func_code != verified_code) {
        return 0;
    }
    return soac_descriptor_birth_current(birth, descriptor, function);
}

int
PySoac_AdoptBuiltinDescriptor(PyObject *descriptor, PyObject *namespace_witness,
                             PyObject *function, PyObject *expected_owner,
                             PyObject *verified_code)
{
    int matches = PySoac_MatchesDescriptorBirth(
        descriptor, namespace_witness, function, expected_owner, verified_code);
    if (matches != 1) {
        if (matches == 0) {
            func_soac_mutation_error("builtin descriptor has no matching live birth");
        }
        return -1;
    }
    int result;
    if (Py_IS_TYPE(descriptor, &PyStaticMethod_Type)) {
        result = _PySoac_SealStaticMethod(descriptor, function);
    }
    else if (Py_IS_TYPE(descriptor, &PyClassMethod_Type)) {
        result = _PySoac_SealClassMethod(descriptor, function);
    }
    else {
        assert(Py_IS_TYPE(descriptor, &PyProperty_Type));
        result = _PySoac_SealProperty(descriptor, function, Py_None, Py_None);
    }
    if (result < 0) {
        return -1;
    }
    PyObject *component = NULL;
    SoacDescriptorBirth *birth = soac_descriptor_birth(descriptor, &component);
    assert(birth != NULL);
    /* No allocation, decref, or callback separates validation and the seal. */
    birth->state = SOAC_DESCRIPTOR_ADOPTED;
    return 0;
}

static int
soac_descriptor_check_function(PyObject *function, PyObject *expected_owner,
                               PyObject *verified_code)
{
    PyObject *owner = PyFunction_GetSoacStrictOwner(function);
    if (owner == NULL && PyErr_Occurred()) {
        return -1;
    }
    if (owner != expected_owner ||
        (PyObject *)((PyFunctionObject *)function)->func_code != verified_code) {
        func_soac_mutation_error("builtin descriptor function owner or code changed");
        return -1;
    }
    return 0;
}

static PyObject *
soac_descriptor_new_method(PyTypeObject *factory, PyObject *function,
                           PyObject *record)
{
    PyObject *object = _PyType_AllocNoTrack(factory, 0);
    if (object == NULL) {
        return NULL;
    }
    if (factory == &PyStaticMethod_Type) {
        staticmethod *method = (staticmethod *)object;
        method->sm_soac_birth = Py_NewRef(record);
        method->sm_callable = Py_NewRef(function);
        _PyObject_SetDeferredRefcount(object);
    }
    else {
        assert(factory == &PyClassMethod_Type);
        classmethod *method = (classmethod *)object;
        method->cm_soac_birth = Py_NewRef(record);
        method->cm_callable = Py_NewRef(function);
    }
    _PySoac_DescriptorBirth_Bind(record, object);
    _PyObject_GC_TRACK(object);
    if (!_PySOAC_DescriptorFactoryIsOriginal((PyObject *)factory)) {
        func_soac_mutation_error("builtin descriptor init changed during allocation");
        _PySoac_DescriptorBirth_Invalidate(record);
        Py_DECREF(object);
        return NULL;
    }
    if (functools_wraps(object, function) < 0) {
        _PySoac_DescriptorBirth_Invalidate(record);
        Py_DECREF(object);
        return NULL;
    }
    return object;
}

PyObject *
PySoac_NewBuiltinDescriptor(PyObject *factory, PyObject *function,
                            PyObject *expected_owner, PyObject *verified_code,
                            PyObject *namespace_witness)
{
    if (factory == NULL || function == NULL || expected_owner == NULL ||
        verified_code == NULL || namespace_witness == NULL) {
        PyErr_SetString(PyExc_TypeError, "descriptor construction needs non-NULL operands");
        return NULL;
    }
    if (factory != (PyObject *)&PyStaticMethod_Type &&
        factory != (PyObject *)&PyClassMethod_Type &&
        factory != (PyObject *)&PyProperty_Type) {
        PyErr_SetString(PyExc_TypeError, "expected an exact supported builtin descriptor factory");
        return NULL;
    }
    if (!PyFunction_Check(function) ||
        (PyObject *)((PyFunctionObject *)function)->func_code != verified_code ||
        !PyCode_Check((PyObject *)((PyFunctionObject *)function)->func_code) ||
        namespace_witness == Py_None || expected_owner == Py_None) {
        PyErr_SetString(PyExc_TypeError, "descriptor construction needs a function, code, and live owners");
        return NULL;
    }
    if (!_PySOAC_DescriptorFactoryIsOriginal(factory)) {
        func_soac_mutation_error("builtin descriptor constructor dispatch changed");
        return NULL;
    }
    if (soac_descriptor_check_function(function, expected_owner, verified_code) < 0) {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    int64_t interpreter_id = PyInterpreterState_GetID(interp);
    if (interpreter_id < 0) {
        return NULL;
    }
    /* Reserve before any callback-capable construction allocation. Nested
     * constructors cannot use this ID, even when this construction fails.
     * Burning failed reservations also avoids rollback/address-reuse hazards. */
    uint64_t identity = 0;
    FT_MUTEX_LOCK(&interp->func_state.mutex);
    if (interp->soac.descriptor_birth_counter != UINT64_MAX) {
        identity = ++interp->soac.descriptor_birth_counter;
    }
    FT_MUTEX_UNLOCK(&interp->func_state.mutex);
    if (identity == 0) {
        func_soac_runtime_error("builtin descriptor birth identity space is exhausted");
        return NULL;
    }
    if (PyType_Ready(&SoacDescriptorBirth_Type) < 0) {
        return NULL;
    }
    PyObject *code_ref = PyWeakref_NewRef(verified_code, NULL);
    if (code_ref == NULL) {
        return NULL;
    }
    SoacDescriptorBirth *birth = PyObject_GC_New(
        SoacDescriptorBirth, &SoacDescriptorBirth_Type);
    if (birth == NULL) {
        Py_DECREF(code_ref);
        return NULL;
    }
    birth->namespace_owner = Py_NewRef(namespace_witness);
    birth->code_ref = code_ref;
    birth->interpreter_id = interpreter_id;
    birth->identity = identity;
    birth->descriptor_identity = 0;
    birth->function_identity = (uintptr_t)function;
    birth->function_owner_identity = (uintptr_t)expected_owner;
    birth->state = SOAC_DESCRIPTOR_CONSTRUCTING;
    _PyObject_GC_TRACK(birth);

    PyObject *descriptor = NULL;
    /* The metadata allocation above may collect objects. Do not bypass a
     * constructor slot changed by a callback since the source CALL selection. */
    if (!_PySOAC_DescriptorFactoryIsOriginal(factory) ||
        soac_descriptor_check_function(function, expected_owner, verified_code) < 0) {
        if (!PyErr_Occurred())
            func_soac_mutation_error("builtin descriptor constructor changed before allocation");
        goto error;
    }
    if (factory == (PyObject *)&PyProperty_Type) {
        descriptor = _PySoac_NewProperty(function, (PyObject *)birth);
    }
    else {
        descriptor = soac_descriptor_new_method(
            (PyTypeObject *)factory, function, (PyObject *)birth);
    }
    if (descriptor == NULL) {
        PyObject *exception = PyErr_GetRaisedException();
        soac_descriptor_birth_clear((PyObject *)birth);
        Py_DECREF(birth);
        PyErr_SetRaisedException(exception);
        return NULL;
    }
    /* Allocation/metadata copying can run GC callbacks. A reinitialized or
     * cleared record cannot become pending again, even if its component was
     * restored to the original function before the callback returned. */
    if (birth->state != SOAC_DESCRIPTOR_CONSTRUCTING) {
        func_soac_mutation_error("builtin descriptor changed during construction");
        goto error;
    }
    birth->state = SOAC_DESCRIPTOR_PENDING;
    int matches = PySoac_MatchesDescriptorBirth(
        descriptor, namespace_witness, function, expected_owner, verified_code);
    if (matches != 1) {
        if (matches == 0) {
            func_soac_mutation_error("builtin descriptor function changed during construction");
        }
        goto error;
    }
    Py_DECREF(birth);  /* The descriptor owns the record; cannot run a callback. */
    return descriptor;

error:
    _PySoac_DescriptorBirth_Invalidate((PyObject *)birth);
    PyObject *exception = PyErr_GetRaisedException();
    Py_XDECREF(descriptor);
    soac_descriptor_birth_clear((PyObject *)birth);
    Py_DECREF(birth);
    PyErr_SetRaisedException(exception);
    return NULL;
}
