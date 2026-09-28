/* Actual concrete receiver storage, published once at birth. Included only by
 * soac_type.inc. This is independent of strict-class and callable authority. */
struct SoacReceiverReceipt {
    PyObject_HEAD
    PyTypeObject *type;             /* comparison-only; type retires first */
    PyObject *storage_owner;
    PyObject *reservation_names;
    PyObject *dictionary_owner;
    PyDict_SoacPolicyCallback validate_dictionary;
    PyTypeStateFieldCheckV1 validate_inline;
    PySoacReceiverDictionaryAttachmentV1 prepare_dictionary_attachment;
    uint32_t coverage;
    uint32_t dictionary_mode;
    int terminal;
    int cleared;
};

static int
soac_receiver_traverse(SoacReceiverReceipt *receipt, visitproc visit, void *arg)
{
    Py_VISIT(receipt->storage_owner);
    Py_VISIT(receipt->reservation_names);
    Py_VISIT(receipt->dictionary_owner);
    return 0;
}

static void
soac_receiver_retire(SoacReceiverReceipt *receipt)
{
    if (receipt != NULL) {
        receipt->terminal = 1;
        receipt->type = NULL;
    }
}

static int
soac_receiver_clear(SoacReceiverReceipt *receipt)
{
    receipt->cleared = 1;
    soac_receiver_retire(receipt);
    Py_CLEAR(receipt->storage_owner);
    Py_CLEAR(receipt->reservation_names);
    Py_CLEAR(receipt->dictionary_owner);
    return 0;
}

static void
soac_receiver_dealloc(SoacReceiverReceipt *receipt)
{
    PyObject_GC_UnTrack(receipt);
    soac_receiver_clear(receipt);
    PyObject_GC_Del(receipt);
}

static PyTypeObject SoacReceiverReceipt_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_soac.ReceiverBirthReceipt",
    .tp_basicsize = sizeof(SoacReceiverReceipt),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_receiver_traverse,
    .tp_clear = (inquiry)soac_receiver_clear,
    .tp_dealloc = (destructor)soac_receiver_dealloc,
};

static SoacReceiverReceipt *
soac_receiver_new(void)
{
    if (PyType_Ready(&SoacReceiverReceipt_Type) < 0) return NULL;
    size_t presize = _PyType_PreHeaderSize(&SoacReceiverReceipt_Type);
    char *memory = _PyObject_MallocWithType(&SoacReceiverReceipt_Type,
                                           presize + sizeof(SoacReceiverReceipt));
    if (memory == NULL) return (SoacReceiverReceipt *)PyErr_NoMemory();
    memset(memory, 0, presize + sizeof(SoacReceiverReceipt));
    SoacReceiverReceipt *receipt = (SoacReceiverReceipt *)(memory + presize);
    receipt->terminal = 1;
    _PyObject_GC_Link((PyObject *)receipt);
    /* Reftracers see initialized, unavailable metadata, never random pointers.
     * Explicit retirement from a callback cannot be undone by publication. */
    _PyObject_Init((PyObject *)receipt, &SoacReceiverReceipt_Type);
    if (receipt->cleared) {
        Py_DECREF(receipt);
        soac_type_state_error("receiver receipt was retired during allocation");
        return NULL;
    }
    return receipt;
}

static int
soac_receiver_live(PyTypeObject *type, SoacReceiverReceipt *receipt)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state == NULL || state->terminal || receipt == NULL ||
        state->receiver_policy != receipt || receipt->type != type ||
        receipt->terminal || receipt->storage_owner == NULL ||
        receipt->reservation_names == NULL) {
        return soac_type_state_error("receiver birth policy is unavailable");
    }
    return 0;
}

int
_PySOAC_CheckReceiverReceipt(PyObject *object, PyTypeObject *allocation_type)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacReceiverReceipt_Type))
        return soac_type_state_error("instance lost its native birth receipt");
    SoacReceiverReceipt *receipt = (SoacReceiverReceipt *)object;
    /* allocation_type is comparison-only and may have survived type death in
     * an escaped storage shell. Retirement of this owned receipt precedes
     * type destruction, so inspect it before following any type address. */
    if (receipt->terminal || receipt->type == NULL || receipt->type != allocation_type)
        return soac_type_state_error("instance birth receipt is retired or foreign");
    return soac_receiver_live(receipt->type, receipt);
}

int
PyType_GetSoacReceiverPolicyV1(PyObject *object, PySoacReceiverReceiptViewV1 *out)
{
    if (out == NULL || out->abi_version != 1 || out->struct_size != sizeof(*out) ||
        object == NULL || !PyType_Check(object)) {
        return soac_type_state_error("invalid receiver birth policy query");
    }
    out->receipt = out->storage_owner = out->dictionary_owner = NULL;
    PyTypeObject *type = (PyTypeObject *)object;
    SoacTypeContract *state = soac_type_state(type);
    if (state == NULL || state->receiver_policy == NULL) return 0;
    SoacReceiverReceipt *receipt = state->receiver_policy;
    if (soac_receiver_live(type, receipt) < 0) return -1;
    out->receipt = (PyObject *)receipt;
    out->storage_owner = receipt->storage_owner;
    out->dictionary_owner = receipt->dictionary_owner;
    return 1;
}

int
PyType_TryGetSoacReceiverPolicyV1(PyObject *object, PySoacReceiverReceiptViewV1 *out)
{
    if (out == NULL || out->abi_version != 1 || out->struct_size != sizeof(*out) ||
        object == NULL || !PyType_Check(object)) {
        return soac_type_state_error("invalid receiver birth policy query");
    }
    out->receipt = out->storage_owner = out->dictionary_owner = NULL;
    SoacTypeContract *state = soac_type_state((PyTypeObject *)object);
    if (state == NULL || state->receiver_policy == NULL) return 0;
    /* Scalar retirement is a positive optional miss. All remaining identity
     * and owner checks retain the enforcing query's error semantics. */
    if (state->terminal || state->receiver_policy->terminal) return 0;
    return PyType_GetSoacReceiverPolicyV1(object, out);
}

int
_PySOAC_MatchesReceiverPolicy(PyTypeObject *type, PyObject *receipt_object,
                             PyObject *dictionary_owner,
                             PyDict_SoacPolicyCallback validate_dictionary,
                             PyTypeStateFieldCheckV1 validate_inline)
{
    if (receipt_object == NULL ||
        !Py_IS_TYPE(receipt_object, &SoacReceiverReceipt_Type)) {
        return soac_type_state_error("expected an actual receiver birth receipt");
    }
    SoacReceiverReceipt *receipt = (SoacReceiverReceipt *)receipt_object;
    if (soac_receiver_live(type, receipt) < 0) return -1;
    if (dictionary_owner != receipt->dictionary_owner ||
        validate_dictionary != receipt->validate_dictionary ||
        validate_inline != receipt->validate_inline) {
        return soac_type_state_error("storage projection differs from its birth receipt");
    }
    return 0;
}

int
PyType_SetSoacReceiverPolicyFactoryV1(PyObject *object, PyObject *owner,
                                     PySoacReceiverBirthFactoryV1 factory)
{
    SoacTypeContract *state = object != NULL && PyType_Check(object)
        ? soac_type_state((PyTypeObject *)object) : NULL;
    if (state == NULL || owner == NULL || factory == NULL || state->owner != owner ||
        state->type != (PyTypeObject *)object || state->terminal ||
        !soac_has_pending_construction(state) ||
        soac_type_phase(state) != Py_SOAC_TYPE_STATE_PENDING ||
        !state->binding_storage_factory || state->permanent_contract_published ||
        state->receiver_policy_factory != NULL || state->receiver_policy != NULL ||
        (((PyTypeObject *)object)->tp_flags & (Py_TPFLAGS_READY | Py_TPFLAGS_READYING))) {
        return soac_type_state_error("receiver factory requires the original pending bind");
    }
    state->receiver_policy_factory = factory;
    return 0;
}

/* Consumed by the existing allocation guard's MRO walk. There is no second
 * hierarchy traversal, recursive base graph or Python lookup on ordinary
 * allocations. The custom-mro interval uses completed direct-base MROs. */
static int
soac_receiver_type_selected(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    return state != NULL && (state->receiver_policy_factory != NULL ||
                             state->receiver_policy != NULL);
}

static int
soac_receiver_allocation_barrier(PyTypeObject *type, int selected)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL && state->preparing_receiver_policy) {
        return soac_type_state_error("receiver policy is still being prepared");
    }
    if (state != NULL && state->receiver_policy != NULL)
        return soac_receiver_live(type, state->receiver_policy);
    if ((type->tp_flags & Py_TPFLAGS_HEAPTYPE) && selected) {
        return soac_type_state_error("receiver storage was not authenticated at type birth");
    }
    return 0;
}

typedef struct {
    PyObject *dictionary;
    PyObject **keys;
    PyObject **values;
    PyTypeObject **value_types;
    descrgetfunc *getters;
    descrsetfunc *setters;
    Py_ssize_t count;
    SoacTypeContract *contract;
    PyObject *fields;
    PyObject *slots;
} SoacReceiverRowSnapshot;

typedef struct {
    PySoacReceiverBirthInputV1 input;
    PyObject *mro;
    PySoacReceiverMroRowV1 *rows;
    SoacReceiverRowSnapshot *snapshots;
    Py_ssize_t dictoffset;
    getattrofunc getattro;
    unsigned long storage_flags;
} SoacReceiverSnapshot;

static uint32_t
soac_receiver_binding_kind(PyObject *value)
{
    if (PyFunction_Check(value)) return 1;
    if (Py_TYPE(value)->tp_descr_set != NULL) return 2;
    if (Py_TYPE(value)->tp_descr_get != NULL) return 3;
    return 4;
}

static void
soac_receiver_snapshot_clear(SoacReceiverSnapshot *snapshot)
{
    /* Only after publication/failure state is fixed may decrefs call Python. */
    for (size_t i = 0; i < snapshot->input.row_count; ++i) {
        SoacReceiverRowSnapshot *row = &snapshot->snapshots[i];
        for (Py_ssize_t j = 0; j < row->count; ++j) {
            Py_XDECREF(row->keys[j]);
            Py_XDECREF(row->values[j]);
            Py_XDECREF(row->value_types[j]);
        }
        PyMem_Free(row->keys);
        PyMem_Free(row->values);
        PyMem_Free(row->value_types);
        PyMem_Free(row->getters);
        PyMem_Free(row->setters);
        Py_XDECREF(row->dictionary);
        Py_XDECREF(row->contract);
        Py_XDECREF(row->fields);
        Py_XDECREF(row->slots);
        PyMem_Free((void *)snapshot->rows[i].fields);
        PyMem_Free((void *)snapshot->rows[i].bindings);
    }
    PyMem_Free(snapshot->snapshots);
    PyMem_Free(snapshot->rows);
    Py_CLEAR(snapshot->mro);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int
soac_receiver_snapshot(PyTypeObject *type, SoacReceiverSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (type->tp_mro == NULL || !PyTuple_CheckExact(type->tp_mro))
        return soac_type_state_error("receiver birth requires its actual completed MRO");
    snapshot->mro = Py_NewRef(type->tp_mro);
    snapshot->input = (PySoacReceiverBirthInputV1){
        .abi_version = 1, .struct_size = sizeof(snapshot->input),
        .actual_type = (PyObject *)type,
        .dictionary_bearing = type->tp_dictoffset != 0 ||
            (type->tp_flags & Py_TPFLAGS_MANAGED_DICT) != 0,
        .stock_lookup = type->tp_getattro == PyObject_GenericGetAttr,
    };
    snapshot->dictoffset = type->tp_dictoffset;
    snapshot->getattro = type->tp_getattro;
    snapshot->storage_flags = type->tp_flags &
        (Py_TPFLAGS_MANAGED_DICT | Py_TPFLAGS_INLINE_VALUES);
    size_t count = (size_t)PyTuple_GET_SIZE(snapshot->mro);
    snapshot->rows = PyMem_Calloc(count, sizeof(*snapshot->rows));
    snapshot->snapshots = PyMem_Calloc(count, sizeof(*snapshot->snapshots));
    if (snapshot->rows == NULL || snapshot->snapshots == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    snapshot->input.rows = snapshot->rows;
    snapshot->input.row_count = count;
    for (size_t i = 0; i < count; ++i) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(snapshot->mro, i);
        PySoacReceiverMroRowV1 *row = &snapshot->rows[i];
        SoacReceiverRowSnapshot *held = &snapshot->snapshots[i];
        row->actual_type = (PyObject *)base;
        /* Static builtins store their namespace in interpreter-owned state.
         * This accessor is a callback-free borrow for both storage families. */
        PyObject *dictionary = _PyType_GetDict(base);
        row->namespace_complete = dictionary != NULL && PyDict_CheckExact(dictionary);
        SoacTypeContract *contract = soac_type_contract(base);
        if (contract != NULL && soac_contract_not_terminal(contract) < 0) return -1;
        /* object has no instance dictionary, declared fields or object slots.
         * Its native definition is fixed; it is a complete empty field row. */
        row->field_catalog_complete = contract != NULL || base == &PyBaseObject_Type;
        held->contract = (SoacTypeContract *)Py_XNewRef(contract);
        held->fields = contract == NULL ? NULL : Py_XNewRef(contract->fields);
        held->slots = contract == NULL ? NULL : Py_XNewRef(contract->object_slot_fields);
        size_t fields = held->fields == NULL ? 0 : (size_t)PyTuple_GET_SIZE(held->fields);
        size_t slots = held->slots == NULL ? 0 : (size_t)PyTuple_GET_SIZE(held->slots);
        PySoacReceiverFieldV1 *catalog = PyMem_Calloc(fields + slots, sizeof(*catalog));
        if (fields + slots && catalog == NULL) { PyErr_NoMemory(); return -1; }
        row->fields = catalog;
        row->field_count = fields + slots;
        for (size_t j = 0; j < fields; ++j)
            catalog[j] = (PySoacReceiverFieldV1){PyTuple_GET_ITEM(held->fields, j), 0, 0};
        for (size_t j = 0; j < slots; ++j)
            catalog[fields + j] = (PySoacReceiverFieldV1){
                PyTuple_GET_ITEM(held->slots, j), 1, contract->object_slots[j].offset};
        if (!row->namespace_complete) continue;
        held->dictionary = Py_NewRef(dictionary);
        Py_ssize_t entries = PyDict_GET_SIZE(dictionary);
        held->keys = PyMem_Calloc(entries, sizeof(*held->keys));
        held->values = PyMem_Calloc(entries, sizeof(*held->values));
        held->value_types = PyMem_Calloc(entries, sizeof(*held->value_types));
        held->getters = PyMem_Calloc(entries, sizeof(*held->getters));
        held->setters = PyMem_Calloc(entries, sizeof(*held->setters));
        PySoacReceiverBindingV1 *bindings = PyMem_Calloc(entries, sizeof(*bindings));
        row->bindings = bindings;
        if (entries && (held->keys == NULL || held->values == NULL ||
                held->value_types == NULL || held->getters == NULL ||
                held->setters == NULL || bindings == NULL)) {
            PyErr_NoMemory(); return -1;
        }
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        while (PyDict_Next(dictionary, &pos, &key, &value)) {
            Py_ssize_t j = held->count++;
            held->keys[j] = Py_NewRef(key);
            held->values[j] = Py_NewRef(value);
            held->value_types[j] = (PyTypeObject *)Py_NewRef(Py_TYPE(value));
            held->getters[j] = Py_TYPE(value)->tp_descr_get;
            held->setters[j] = Py_TYPE(value)->tp_descr_set;
            if (!PyUnicode_CheckExact(key)) { row->namespace_complete = 0; continue; }
            bindings[row->binding_count++] = (PySoacReceiverBindingV1){
                key, soac_receiver_binding_kind(value)};
        }
    }
    return 0;
}

static int
soac_receiver_snapshot_live(PyTypeObject *type, const SoacReceiverSnapshot *snapshot)
{
    if (type->tp_mro != snapshot->mro || type->tp_dictoffset != snapshot->dictoffset ||
        type->tp_getattro != snapshot->getattro ||
        (type->tp_flags & (Py_TPFLAGS_MANAGED_DICT | Py_TPFLAGS_INLINE_VALUES)) !=
            snapshot->storage_flags)
        return soac_type_state_error("receiver layout changed during birth policy preparation");
    for (size_t i = 0; i < snapshot->input.row_count; ++i) {
        PyTypeObject *base = (PyTypeObject *)snapshot->rows[i].actual_type;
        const SoacReceiverRowSnapshot *held = &snapshot->snapshots[i];
        PyObject *dictionary = _PyType_GetDict(base);
        if (held->contract != soac_type_contract(base) ||
            (held->contract != NULL && (soac_contract_not_terminal(held->contract) < 0 ||
                held->contract->fields != held->fields ||
                held->contract->object_slot_fields != held->slots)))
            return soac_type_state_error("receiver field contract changed during preparation");
        if (held->dictionary == NULL) {
            if (dictionary != NULL && PyDict_CheckExact(dictionary))
                return soac_type_state_error("receiver namespace appeared during preparation");
            continue;
        }
        if (dictionary != held->dictionary || PyDict_GET_SIZE(dictionary) != held->count)
            return soac_type_state_error("receiver namespace changed during preparation");
        Py_ssize_t pos = 0, j = 0;
        PyObject *key, *value;
        while (PyDict_Next(dictionary, &pos, &key, &value)) {
            if (j >= held->count || key != held->keys[j] || value != held->values[j] ||
                Py_TYPE(value) != held->value_types[j] ||
                Py_TYPE(value)->tp_descr_get != held->getters[j] ||
                Py_TYPE(value)->tp_descr_set != held->setters[j])
                return soac_type_state_error("receiver binding changed during preparation");
            ++j;
        }
        if (j != held->count) return soac_type_state_error("receiver namespace count changed");
    }
    return 0;
}

static int
soac_receiver_validate_output(const SoacReceiverSnapshot *snapshot,
                             const PySoacReceiverBirthOutputV1 *out)
{
    if (out->abi_version != 1 || out->struct_size != sizeof(*out) ||
        out->storage_owner == NULL || out->reservation_names == NULL ||
        !soac_valid_names(out->reservation_names) || out->coverage > 2 ||
        (out->dictionary_mode != Py_SOAC_INSTANCE_DICT_NONE &&
         out->dictionary_mode != Py_SOAC_INSTANCE_DICT_ORDINARY) ||
        (out->dictionary_mode == Py_SOAC_INSTANCE_DICT_NONE
            ? out->dictionary_owner != NULL || out->validate_dictionary != NULL ||
              out->validate_inline != NULL || out->prepare_dictionary_attachment != NULL
            : !snapshot->input.dictionary_bearing || out->dictionary_owner == NULL ||
              out->validate_dictionary == NULL || out->validate_inline == NULL ||
              out->prepare_dictionary_attachment == NULL) ||
        (out->coverage == 1 ? PyTuple_GET_SIZE(out->reservation_names) == 0
                            : PyTuple_GET_SIZE(out->reservation_names) != 0))
        return soac_type_state_error("invalid receiver birth factory output");
    for (Py_ssize_t n = 0; n < PyTuple_GET_SIZE(out->reservation_names); ++n) {
        PyObject *name = PyTuple_GET_ITEM(out->reservation_names, n);
        int candidate = 0, resolved = 0;
        if (!snapshot->input.stock_lookup ||
            (snapshot->input.dictionary_bearing && out->dictionary_mode != Py_SOAC_INSTANCE_DICT_ORDINARY))
            return soac_type_state_error("method reservation lacks an ordinary storage policy");
        for (size_t i = 0; i < snapshot->input.row_count; ++i) {
            const PySoacReceiverMroRowV1 *row = &snapshot->rows[i];
            SoacTypeContract *contract = snapshot->snapshots[i].contract;
            if (!row->namespace_complete || !row->field_catalog_complete)
                return soac_type_state_error("method reservation has incomplete receiver facts");
            if (contract != NULL && contract->method_reservation_names != NULL) {
                for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(contract->method_reservation_names); ++j)
                    candidate |= _PyUnicode_Equal(name, PyTuple_GET_ITEM(contract->method_reservation_names, j));
            }
            for (size_t j = 0; j < row->field_count; ++j)
                if (_PyUnicode_Equal(name, row->fields[j].name))
                    return soac_type_state_error("method reservation conflicts with a declared field");
            if (!resolved) {
                for (size_t j = 0; j < row->binding_count; ++j) {
                    if (!_PyUnicode_Equal(name, row->bindings[j].name)) continue;
                    if (row->bindings[j].kind != 1)
                        return soac_type_state_error("method reservation is not an effective plain method");
                    resolved = 1;
                    break;
                }
            }
        }
        if (!candidate || !resolved)
            return soac_type_state_error("method reservation has no actual declaration and binding");
    }
    return 0;
}

static int
soac_prepare_receiver_policy(PyTypeObject *type)
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) return 0;
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL && state->receiver_policy != NULL)
        return soac_receiver_live(type, state->receiver_policy);
    PyObject *mro = type->tp_mro;
    if (mro == NULL) return 0;
    SoacTypeContract *selected = NULL;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_state((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base != NULL && base->receiver_policy_factory != NULL) {
            if (selected != NULL && selected->receiver_policy_factory != base->receiver_policy_factory)
                return soac_type_state_error("receiver family has incompatible policy factories");
            if (selected == NULL) selected = base;
        }
    }
    if (selected == NULL) return 0;
    Py_INCREF(selected);
    PyObject *owner = Py_XNewRef(selected->owner);
    PySoacReceiverBirthFactoryV1 factory = selected->receiver_policy_factory;
    SoacReceiverSnapshot snapshot = {0};
    PySoacReceiverBirthOutputV1 out = {0};
    SoacReceiverReceipt *receipt = NULL;
    int status = -1;
    if (owner == NULL || selected->terminal) goto failed;
    if (state == NULL) {
        state = soac_new_type_state(NULL, SOAC_TYPE_STATE_RECEIVER_ONLY);
        if (state == NULL) goto failed;
        if (soac_type_state(type) != NULL) {
            Py_DECREF(state);
            state = NULL;
            soac_type_state_error("receiver state appeared during native allocation");
            goto failed;
        }
        state->type = type;
        ((PyHeapTypeObject *)type)->ht_soac_contract = (PyObject *)state;
        PyObject_GC_Track(state);
    }
    if (state->terminal || state->preparing_receiver_policy) goto failed;
    state->preparing_receiver_policy = 1;
    Py_INCREF(state);
    if (soac_receiver_snapshot(type, &snapshot) < 0) goto done;
    receipt = soac_receiver_new();
    if (receipt == NULL) goto done;
    if (soac_receiver_snapshot_live(type, &snapshot) < 0 ||
        soac_type_state(type) != state || state->terminal ||
        selected->owner != owner || selected->receiver_policy_factory != factory || selected->terminal)
        goto done;
    if (factory(owner, &snapshot.input, &out) != 0 || PyErr_Occurred()) goto done;
    if (soac_type_state(type) != state || state->terminal || state->receiver_policy != NULL ||
        selected->owner != owner || selected->receiver_policy_factory != factory || selected->terminal ||
        receipt->cleared || soac_receiver_snapshot_live(type, &snapshot) < 0 ||
        soac_receiver_validate_output(&snapshot, &out) < 0) goto done;
    receipt->type = type;
    receipt->storage_owner = out.storage_owner; out.storage_owner = NULL;
    receipt->reservation_names = out.reservation_names; out.reservation_names = NULL;
    receipt->dictionary_owner = out.dictionary_owner; out.dictionary_owner = NULL;
    receipt->validate_dictionary = out.validate_dictionary;
    receipt->validate_inline = out.validate_inline;
    receipt->prepare_dictionary_attachment = out.prepare_dictionary_attachment;
    receipt->coverage = out.coverage;
    receipt->dictionary_mode = out.dictionary_mode;
    receipt->terminal = 0;
    state->receiver_policy = receipt; receipt = NULL;
    PyObject_GC_Track(state->receiver_policy);
    status = 0;
done:
    /* A failed real birth never reopens allocation without the obligation. */
    if (status < 0) state->terminal = 1;
    state->preparing_receiver_policy = 0;
    {
        PyObject *error = PyErr_GetRaisedException();
        Py_XDECREF(receipt);
        Py_XDECREF(out.storage_owner);
        Py_XDECREF(out.reservation_names);
        Py_XDECREF(out.dictionary_owner);
        soac_receiver_snapshot_clear(&snapshot);
        Py_DECREF(state);
        Py_XDECREF(owner);
        Py_DECREF(selected);
        PyErr_SetRaisedException(error);
    }
    SoacTypeContract *current = soac_type_state(type);
    if (status == 0 && current != NULL &&
        soac_receiver_live(type, current->receiver_policy) == 0) return 0;
    return soac_type_state_error("receiver birth policy preparation failed");
failed:
    {
        PyObject *error = PyErr_GetRaisedException();
        Py_XDECREF(owner);
        Py_DECREF(selected);
        PyErr_SetRaisedException(error);
    }
    return soac_type_state_error("receiver birth policy preparation failed");
}

int
_PySOAC_ReadyReceiverPolicy(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL && soac_has_pending_construction(state) &&
        !state->permanent_contract_published) return 0;
    return soac_prepare_receiver_policy(type);
}

static int
soac_receiver_dictionary_mode(SoacTypeContract *state)
{
    /* Terminal policies remain selected, so the destination fails closed. */
    return state != NULL && state->receiver_policy != NULL &&
        state->receiver_policy->dictionary_mode == Py_SOAC_INSTANCE_DICT_ORDINARY;
}

int
_PySOAC_HasReceiverDictionaryPolicy(PyTypeObject *type)
{
    return soac_receiver_dictionary_mode(soac_type_state(type));
}

static int
soac_receiver_inline_write(PyObject *object, PyObject *name, PyObject *value)
{
    PyTypeObject *type = Py_TYPE(object);
    SoacReceiverReceipt *receipt = soac_type_state(type)->receiver_policy;
    if (soac_receiver_live(type, receipt) < 0) return -1;
    Py_INCREF(receipt);
    PyObject *owner = Py_NewRef(receipt->dictionary_owner);
    PyTypeStateFieldCheckV1 validate = receipt->validate_inline;
    int result = validate(owner, name, value);
    if (result != 0 || PyErr_Occurred() || Py_TYPE(object) != type ||
        soac_receiver_live(type, receipt) < 0 || receipt->dictionary_owner != owner ||
        receipt->validate_inline != validate) result = -1;
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(owner);
    Py_DECREF(receipt);
    PyErr_SetRaisedException(error);
    if (result < 0) return soac_type_state_error("receiver inline write failed");
    return 0;
}

static int
soac_receiver_prepare_dictionary(PyObject *object, PyObject *dict,
                                 const PySoacInstanceDictPolicy *existing,
                                 PySoacInstanceDictPolicy *out)
{
    PyTypeObject *type = Py_TYPE(object);
    SoacReceiverReceipt *receipt = soac_type_state(type)->receiver_policy;
    if (dict == NULL || !PyDict_Check(dict) || soac_receiver_live(type, receipt) < 0)
        return soac_type_state_error("receiver dictionary preparation is unavailable");
    if (existing == NULL) {
        if (_PyDict_PrepareSoacInstanceCachePolicy(dict, receipt->validate_dictionary) < 0)
            return -1;
        out->owner = Py_NewRef(receipt->dictionary_owner);
        out->validate = receipt->validate_dictionary;
        return 1;
    }
    if (existing->owner == receipt->dictionary_owner &&
        existing->validate == receipt->validate_dictionary) {
        out->owner = Py_NewRef(existing->owner);
        out->validate = existing->validate;
        return 1;
    }
    /* Shared stronger policies remain permanent. This concrete-policy callback
     * also exists for method-only policies, independently of class field mode.
     * It checks predicate owners and reservations and returns the same owner. */
    Py_INCREF(receipt);
    PyObject *owner = Py_NewRef(receipt->storage_owner);
    PySoacReceiverDictionaryAttachmentV1 prepare = receipt->prepare_dictionary_attachment;
    int result = prepare(owner, object, dict, existing, out);
    if (result != 0 || PyErr_Occurred() || Py_TYPE(object) != type ||
        soac_receiver_live(type, receipt) < 0 || receipt->storage_owner != owner ||
        receipt->prepare_dictionary_attachment != prepare ||
        out->owner != existing->owner || out->validate != existing->validate) {
        soac_type_state_error("existing receiver dictionary policy is incompatible");
        result = -1;
    }
    PyObject *error = PyErr_GetRaisedException();
    if (result < 0) {
        Py_CLEAR(out->owner);
        out->validate = NULL;
    }
    Py_DECREF(owner);
    Py_DECREF(receipt);
    PyErr_SetRaisedException(error);
    return result < 0 ? -1 : 1;
}
