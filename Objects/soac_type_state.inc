/* Optional native storage state. Included by object.c so reference creation
 * keeps the ordinary debug/reftracer bookkeeping in one implementation. */

#if _Py_TYPE_STATE_SUPPORTED
_Static_assert(offsetof(PyDictValues, values) == 8,
               "type-state inline extent must occupy existing LP64 padding");
_Static_assert(sizeof(PyDictValues) == 16,
               "ordinary inline-values header must not grow");
#endif

static int
type_state_unavailable(const char *message)
{
    PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
    PyErr_SetString(error == NULL ? PyExc_RuntimeError : error, message);
    return -1;
}

static int
type_state_traverse(PyTypeState *state, visitproc visit, void *arg)
{
    Py_VISIT(state->dictionary);
    Py_VISIT(state->dictionary_policy.owner);
    Py_VISIT(state->class_contracts);
    for (Py_ssize_t i = 0; i < state->slot_count; ++i) {
        Py_VISIT(state->slots[i].canonical_name);
        Py_VISIT(state->slots[i].owner);
    }
    return 0;
}

static int
type_state_clear(PyTypeState *state)
{
    /* GC retirement of unreachable metadata is not a per-attachment policy
     * mutation. A reachable attached storage keeps this state reachable. */
    state->terminal = 1;
    state->allocation_type = NULL;
    Py_CLEAR(state->dictionary);
    Py_CLEAR(state->dictionary_policy.owner);
    Py_CLEAR(state->class_contracts);
    for (Py_ssize_t i = 0; i < state->slot_count; ++i) {
        Py_CLEAR(state->slots[i].canonical_name);
        Py_CLEAR(state->slots[i].owner);
    }
    return 0;
}

static void
type_state_dealloc(PyTypeState *state)
{
    PyObject_GC_UnTrack(state);
    type_state_clear(state);
    PyMem_Free(state->slots);
    PyObject_GC_Del(state);
}

PyTypeObject _PyTypeState_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_soac.PyTypeState",
    .tp_basicsize = sizeof(PyTypeState),
    .tp_dealloc = (destructor)type_state_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)type_state_traverse,
    .tp_clear = (inquiry)type_state_clear,
};

static PyTypeState *
type_state_new(unsigned char kind)
{
    if (PyType_Ready(&_PyTypeState_Type) < 0) return NULL;
    size_t presize = _PyType_PreHeaderSize(&_PyTypeState_Type);
    char *mem = _PyObject_MallocWithType(&_PyTypeState_Type,
                                       presize + sizeof(PyTypeState));
    if (mem == NULL) return (PyTypeState *)PyErr_NoMemory();
    memset(mem, 0, presize + sizeof(PyTypeState));
    PyTypeState *state = (PyTypeState *)(mem + presize);
    _PyObject_GC_Link((PyObject *)state);
    state->interpreter = _PyInterpreterState_GET();
    state->kind = kind;
    state->terminal = 1;  /* private, incomplete metadata cannot validate */
    _PyObject_Init((PyObject *)state, &_PyTypeState_Type);
    return state;
}

int
_PyTypeState_CheckLive(PyTypeState *state)
{
    if (state == NULL || !Py_IS_TYPE(state, &_PyTypeState_Type) ||
        state->terminal || state->interpreter != _PyInterpreterState_GET()) {
        return type_state_unavailable("native storage type state is unavailable");
    }
    if (state->kind == _Py_TYPE_STATE_INSTANCE) {
        return _PyTypeState_CheckInstanceContracts(state);
    }
    if (state->kind != _Py_TYPE_STATE_DICTIONARY ||
        state->dictionary_policy.owner == NULL) {
        return type_state_unavailable("native dictionary rules are unavailable");
    }
    return 0;
}

PyTypeState *
PyTypeState_NewV1(PyObject *actual_type, const PyTypeStateSpecV1 *spec,
                 size_t spec_size)
{
    if (!_Py_TYPE_STATE_SUPPORTED) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "optional type-state allocation requires the audited LP64 GIL layout");
        return NULL;
    }
    if (actual_type == NULL || !PyType_Check(actual_type) || spec == NULL ||
        spec_size != sizeof(*spec) || spec->struct_size != sizeof(*spec) ||
        spec->abi_version != Py_TYPE_STATE_ABI_V1 || spec->slot_count < 0 ||
        (spec->slot_count != 0 && spec->slots == NULL) ||
        (spec->dictionary_owner == NULL
            ? spec->validate_dictionary != NULL || spec->validate_inline != NULL
            : spec->validate_dictionary == NULL || spec->validate_inline == NULL) ||
        (spec->dictionary_owner == NULL && spec->slot_count == 0)) {
        PyErr_SetString(PyExc_ValueError, "invalid native type-state specification");
        return NULL;
    }
    PyTypeObject *type = (PyTypeObject *)actual_type;
    if (!_PyTypeState_SupportedInstanceType(type) ||
        _PySOAC_CheckTypeAllocation(type) < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError,
                            "type has no supported fresh storage-state allocation");
        }
        return NULL;
    }
    if (!PyUnstable_Type_AssignVersionTag(type)) {
        PyErr_SetString(PyExc_TypeError, "native storage state requires a supported type-version receipt");
        return NULL;
    }
    PyTypeState *state = type_state_new(_Py_TYPE_STATE_INSTANCE);
    if (state == NULL) return NULL;
    state->allocation_type = type;
    state->type_version = type->tp_version_tag;
    if (_PyTypeState_BindSpec(type, spec, state) < 0) goto error;
    if (spec->slot_count > 0) {
        if ((size_t)spec->slot_count > SIZE_MAX / sizeof(*state->slots)) {
            PyErr_NoMemory();
            goto error;
        }
        state->slots = PyMem_Calloc((size_t)spec->slot_count, sizeof(*state->slots));
        if (state->slots == NULL) {
            PyErr_NoMemory();
            goto error;
        }
        state->slot_count = spec->slot_count;
        for (Py_ssize_t i = 0; i < spec->slot_count; ++i) {
            const PyTypeStateSlotSpecV1 *row = &spec->slots[i];
            state->slots[i] = (_PyTypeStateSlot){
                .offset = row->offset,
                .canonical_name = Py_NewRef(row->canonical_name),
                .owner = Py_NewRef(row->rule_owner),
                .validate = row->validate,
            };
        }
    }
    if (spec->dictionary_owner != NULL) {
        PyTypeState *dictionary = type_state_new(_Py_TYPE_STATE_DICTIONARY);
        if (dictionary == NULL) goto error;
        dictionary->dictionary_policy.owner = Py_NewRef(spec->dictionary_owner);
        dictionary->dictionary_policy.validate = spec->validate_dictionary;
        dictionary->dictionary_policy.flags = PyDict_SOAC_ALLOW_NONSTRING_KEYS;
        dictionary->dictionary_policy.dictionary_mode = Py_SOAC_INSTANCE_DICT_ORDINARY;
        dictionary->dictionary_policy.direct = 1;
        dictionary->validate_inline = spec->validate_inline;
        dictionary->terminal = 0;
        PyObject_GC_Track(dictionary);
        state->dictionary = dictionary;
    }
    state->terminal = 0;
    PyObject_GC_Track(state);
    return state;
error:
    {
        PyObject *error = PyErr_GetRaisedException();
        Py_DECREF(state);
        PyErr_SetRaisedException(error);
        return NULL;
    }
}

int
_PyTypeState_AllocationSize(PyTypeObject *type, size_t ordinary, size_t *size)
{
    (void)type;
    const size_t alignment = sizeof(PyTypeState *);
    if (ordinary > (size_t)PY_SSIZE_T_MAX - 2 * alignment) {
        PyErr_NoMemory();
        return -1;
    }
    *size = _Py_SIZE_ROUND_UP(ordinary, alignment) + sizeof(PyTypeState *);
    return 0;
}

PyTypeState **
_PyObject_TypeStateSlot(PyObject *object)
{
    if (!_PyObject_HasTypeStateSlot(object)) return NULL;
#if _Py_TYPE_STATE_SUPPORTED
    PyTypeObject *type = Py_TYPE(object);
    size_t ordinary;
    if (PyDict_CheckExact(object)) {
        ordinary = sizeof(PyDictObject);
    }
    else {
        /* Access checks immutable layout, not current constructor eligibility:
         * an allowed later __new__ mutation cannot move an old object's tail. */
        if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
            !(type->tp_flags & Py_TPFLAGS_HAVE_GC) || type->tp_itemsize != 0 ||
            type->tp_basicsize < (Py_ssize_t)sizeof(PyObject)) {
            type_state_unavailable("type-state marker has an unsupported allocation family");
            return NULL;
        }
        /* Only the audited allocator sets this marker; class reassignment
         * across an installed contract cannot transfer it. No MRO walk belongs
         * on this direct storage access path. */
        ordinary = _PyObject_VAR_SIZE(type, 1);
        if (type->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
            PyDictValues *values = _PyObject_InlineValues(object);
            size_t capacity = values->soac_allocated_capacity;
            if (capacity == 0 || capacity > SHARED_KEYS_MAX_SIZE ||
                capacity < values->capacity) {
                type_state_unavailable("type-state inline allocation extent is invalid");
                return NULL;
            }
            size_t inline_size = _Py_SIZE_ROUND_UP(capacity, sizeof(PyObject *)) +
                                 (capacity + 1) * sizeof(PyObject *);
            if (ordinary > (size_t)PY_SSIZE_T_MAX - inline_size) {
                type_state_unavailable("type-state inline allocation size overflows");
                return NULL;
            }
            ordinary += inline_size;
        }
    }
    size_t size;
    if (_PyTypeState_AllocationSize(type, ordinary, &size) < 0) return NULL;
    return (PyTypeState **)((char *)object + size - sizeof(PyTypeState *));
#else
    return NULL;
#endif
}

PyTypeState *
PyObject_GetTypeState(PyObject *object)
{
    if (object == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!_PyObject_HasTypeStateSlot(object)) return NULL;
    if (PyDict_CheckExact(object) &&
        (((PyDictObject *)object)->_ma_watcher_tag & _Py_DICT_TYPE_STATE_TERMINAL)) {
        type_state_unavailable("native dictionary attachment has entered terminal teardown");
        return NULL;
    }
    PyTypeState **slot = _PyObject_TypeStateSlot(object);
    if (slot == NULL) return NULL;
    PyTypeState *state = *slot;
    if (_PyTypeState_CheckLive(state) < 0) return NULL;
    if (PyDict_CheckExact(object)
            ? state->kind != _Py_TYPE_STATE_DICTIONARY
            : state->kind != _Py_TYPE_STATE_INSTANCE ||
              state->allocation_type != Py_TYPE(object)) {
        type_state_unavailable("native type state does not match its storage family");
        return NULL;
    }
    return state;
}

void
_PyObject_InitWithTypeState(PyObject *object, PyTypeObject *type, PyTypeState *state)
{
#if _Py_TYPE_STATE_SUPPORTED
    /* Caller has fully initialized ordinary payload and frozen inline extent.
     * Consume its owned state before the CREATE tracer can observe this object. */
    Py_SET_TYPE(object, type);
    object->ob_flags = _Py_HAS_TYPE_STATE_SLOT_FLAG;
    PyTypeState **slot = _PyObject_TypeStateSlot(object);
    assert(slot != NULL && state != NULL);
    *slot = state;
    _Py_INCREF_TYPE(type);
#ifdef Py_REF_DEBUG
    _Py_IncRefTotal(_PyThreadState_GET());
#endif
    new_reference(object, _Py_HAS_TYPE_STATE_SLOT_FLAG);
#else
    (void)object;
    (void)type;
    (void)state;
    Py_FatalError("unsupported optional type-state allocation");
#endif
}

int
_PyObject_TypeStateTraverse(PyObject *object, visitproc visit, void *arg)
{
    if (!_PyObject_HasTypeStateSlot(object)) return 0;
    PyTypeState **slot = _PyObject_TypeStateSlot(object);
    if (slot == NULL) return -1;
    Py_VISIT(*slot);
    return 0;
}

void
_PyObject_ClearTypeState(PyObject *object)
{
    if (!_PyObject_HasTypeStateSlot(object)) return;
    PyTypeState **slot = _PyObject_TypeStateSlot(object);
    if (slot == NULL) {
        Py_FatalError("invalid type-state allocation during terminal cleanup");
    }
    PyObject *error = PyErr_GetRaisedException();
    Py_CLEAR(*slot);
    /* HAS_TYPE_STATE_SLOT survives through final free. Null means terminal,
     * never an ordinary allocation or permission for an unchecked write. */
    PyErr_SetRaisedException(error);
}
