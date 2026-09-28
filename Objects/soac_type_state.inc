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
    Py_VISIT(state->receiver_birth_receipt);
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
    Py_CLEAR(state->receiver_birth_receipt);
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
    state->kind = kind;
    state->terminal = 1;  /* private, incomplete metadata cannot validate */
    _PyObject_Init((PyObject *)state, &_PyTypeState_Type);
    return state;
}

int
_PyTypeState_CheckLive(PyTypeState *state)
{
    if (state == NULL || state->terminal) {
        return type_state_unavailable("native storage type state is unavailable");
    }
    /* Only verified state constructors can publish an attachment. The type
     * and storage family cannot change while that attachment is live. */
    assert(Py_IS_TYPE(state, &_PyTypeState_Type));
    if (state->kind == _Py_TYPE_STATE_INSTANCE) {
        return _PyTypeState_CheckInstanceContracts(state);
    }
    assert(state->kind == _Py_TYPE_STATE_DICTIONARY);
    assert(state->dictionary_policy.owner != NULL);
    return 0;
}

static PyTypeState *
type_state_new_for_spec(PyObject *actual_type, const PyTypeStateSpecV1 *spec,
                        size_t spec_size, int cache_name_provenance,
                        PyObject *receiver_birth_receipt)
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
    state->receiver_birth_receipt = Py_XNewRef(receiver_birth_receipt);
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
        dictionary->dictionary_policy.flags = PyDict_SOAC_ALLOW_NONSTRING_KEYS |
            (cache_name_provenance ? PyDict_SOAC_CACHE_NAME_PROVENANCE : 0);
        dictionary->dictionary_policy.dictionary_mode = Py_SOAC_INSTANCE_DICT_ORDINARY;
        dictionary->dictionary_policy.direct = 1;
        dictionary->validate_inline = spec->validate_inline;
        dictionary->terminal = 0;
        PyObject_GC_Track(dictionary);
        state->dictionary = dictionary;
    }
    if (receiver_birth_receipt != NULL &&
        _PySOAC_MatchesReceiverPolicy(type, receiver_birth_receipt,
            spec->dictionary_owner, spec->validate_dictionary, spec->validate_inline) < 0)
        goto error;
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

PyTypeState *
PyTypeState_NewV1(PyObject *actual_type, const PyTypeStateSpecV1 *spec,
                 size_t spec_size)
{
    return type_state_new_for_spec(actual_type, spec, spec_size, 0, NULL);
}

PyTypeState *
PyTypeState_NewV2(PyObject *actual_type, const PyTypeStateSpecV1 *spec,
                 size_t spec_size)
{
    return type_state_new_for_spec(actual_type, spec, spec_size, 1, NULL);
}

PyTypeState *
PyTypeState_NewV3(PyObject *actual_type, const PyTypeStateSpecV2 *spec,
                 size_t spec_size)
{
    if (spec == NULL || spec_size != sizeof(*spec) ||
        spec->abi_version != 2 || spec->struct_size != sizeof(*spec) ||
        spec->receiver_birth_receipt == NULL || actual_type == NULL ||
        !PyType_Check(actual_type)) {
        PyErr_SetString(PyExc_ValueError, "invalid birth-aware type-state specification");
        return NULL;
    }
    if (_PySOAC_MatchesReceiverPolicy((PyTypeObject *)actual_type,
            spec->receiver_birth_receipt, spec->dictionary_owner,
            spec->validate_dictionary, spec->validate_inline) < 0) return NULL;
    PyTypeStateSpecV1 legacy = {
        .abi_version = Py_TYPE_STATE_ABI_V1, .struct_size = sizeof(legacy),
        .dictionary_owner = spec->dictionary_owner,
        .validate_dictionary = spec->validate_dictionary,
        .validate_inline = spec->validate_inline,
        .slot_count = spec->slot_count, .slots = spec->slots,
    };
    return type_state_new_for_spec(actual_type, &legacy, sizeof(legacy), 1,
                                   spec->receiver_birth_receipt);
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
        assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
        assert(type->tp_flags & Py_TPFLAGS_HAVE_GC);
        assert(type->tp_itemsize == 0);
        assert(type->tp_basicsize >= (Py_ssize_t)sizeof(PyObject));
        /* Only the audited allocator sets this marker; class reassignment
         * across an installed contract cannot transfer it. No MRO walk belongs
         * on this direct storage access path. */
        ordinary = _PyObject_VAR_SIZE(type, 1);
        if (type->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
            PyDictValues *values = _PyObject_InlineValues(object);
            size_t capacity = values->soac_allocated_capacity;
            assert(capacity > 0 && capacity <= SHARED_KEYS_MAX_SIZE);
            assert(capacity >= values->capacity);
            size_t inline_size = _Py_SIZE_ROUND_UP(capacity, sizeof(PyObject *)) +
                                 (capacity + 1) * sizeof(PyObject *);
            assert(ordinary <= (size_t)PY_SSIZE_T_MAX - inline_size);
            ordinary += inline_size;
        }
    }
    /* Allocation checked this extent before setting the marker. Supported
     * type and member mutations cannot move or overwrite the trailer. */
    const size_t alignment = sizeof(PyTypeState *);
    assert(ordinary <= (size_t)PY_SSIZE_T_MAX - 2 * alignment);
    return (PyTypeState **)((char *)object + _Py_SIZE_ROUND_UP(ordinary, alignment));
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

/* Allocation consumes state, initializes the actual owner before trusted
 * dictionary-policy callbacks, and publishes CREATE only after its escaped
 * dictionary can enforce writes. No ordinary read installs metadata. */
int
_PyObject_InitWithInstanceWritePolicy(PyObject *object, PyTypeObject *type,
                                     Py_ssize_t nitems, PyTypeState *state,
                                     freefunc free_allocation)
{
    PyObject *previous_error = PyErr_GetRaisedException();
    Py_SET_TYPE(object, type);
    uint16_t flags = 0;
    if (state != NULL) {
#if _Py_TYPE_STATE_SUPPORTED
        flags = _Py_HAS_TYPE_STATE_SLOT_FLAG;
        object->ob_flags = flags;
        PyTypeState **slot = _PyObject_TypeStateSlot(object);
        assert(slot != NULL);
        *slot = state;
#else
        Py_FatalError("unsupported optional type-state allocation");
#endif
    }
    if (type->tp_itemsize != 0) Py_SET_SIZE((PyVarObject *)object, nitems);
    _Py_INCREF_TYPE(type);
#ifdef Py_REF_DEBUG
    _Py_IncRefTotal(_PyThreadState_GET());
#endif
    init_reference(object, flags);
    if (_PyDict_InitInstanceWritePolicy(object, state) < 0) {
        PyObject *error = PyErr_GetRaisedException();
        assert(Py_REFCNT(object) == 1);
        /* This allocation never published CREATE. Retire its private reference
         * bookkeeping without DESTROY or a user tp_dealloc/__del__ callback. */
        _Py_DECREF_STAT_INC();
#ifdef Py_REF_DEBUG
        _Py_DecRefTotal(_PyThreadState_GET());
#endif
        Py_SET_REFCNT(object, 0);
#ifdef Py_TRACE_REFS
        _Py_ForgetReference(object);
#endif
        _PyObject_ClearTypeState(object);
        free_allocation(object);
        _Py_DECREF_TYPE(type);
        Py_XDECREF(previous_error);
        PyErr_SetRaisedException(error);
        return -1;
    }
    _PyReftracerTrack(object, PyRefTracer_CREATE);
    PyErr_SetRaisedException(previous_error);
    return 0;
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
