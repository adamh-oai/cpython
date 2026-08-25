/* Physical object-member contracts. Included by soac_type.inc after its native
 * state definitions. No caller-supplied member spelling selects a policy. */

static Py_ssize_t
soac_object_slot_name_index(SoacTypeContract *state, PyObject *name)
{
    if (state->object_slot_fields == NULL || name == NULL ||
        !PyUnicode_Check(name)) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < state->object_slot_count; ++i) {
        if (_PyUnicode_Equal(PyTuple_GET_ITEM(state->object_slot_fields, i), name)) {
            return i;
        }
    }
    return -1;
}

static int
soac_inherited_dictionary_field(PyTypeObject *type, PyObject *name)
{
    PyObject *bases = type->tp_bases;
    for (Py_ssize_t i = 0; bases != NULL && i < PyTuple_GET_SIZE(bases); ++i) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(bases, i);
        PyObject *mro = lookup_tp_mro(base);
        for (Py_ssize_t j = 0; mro != NULL && j < PyTuple_GET_SIZE(mro); ++j) {
            PyTypeObject *ancestor = (PyTypeObject *)PyTuple_GET_ITEM(mro, j);
            SoacTypeContract *inherited = soac_type_contract(ancestor);
            if (inherited == NULL) {
                continue;
            }
            if (soac_contract_live(inherited) < 0) {
                return -1;
            }
            /* The inherited field catalog is independent of optional value
             * checks or indexed allocation. ORDINARY and unchecked NONE
             * dictionaries deliberately have no new_instance_dict factory. */
            if (inherited->fields == NULL ||
                (ancestor->tp_dictoffset == 0 &&
                 !(ancestor->tp_flags & Py_TPFLAGS_MANAGED_DICT))) {
                continue;
            }
            for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(inherited->fields); ++k) {
                if (_PyUnicode_Equal(name, PyTuple_GET_ITEM(inherited->fields, k))) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int
soac_build_object_slot_catalog(SoacTypeContract *state, PyTypeObject *type)
{
    Py_ssize_t count = state->object_slot_fields == NULL ? 0
        : PyTuple_GET_SIZE(state->object_slot_fields);
    if (count == 0) {
        return 0;
    }
    if (type->tp_itemsize != 0 ||
        (size_t)count > SIZE_MAX / sizeof(SoacObjectSlot)) {
        PyErr_SetString(soac_type_mutation_error(),
                        "strict object slots require a fixed native object layout");
        return -1;
    }
    state->object_slots = PyMem_Calloc((size_t)count, sizeof(SoacObjectSlot));
    if (state->object_slots == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    state->object_slot_count = count;
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *name = PyTuple_GET_ITEM(state->object_slot_fields, i);
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(state->fields); ++j) {
            if (_PyUnicode_Equal(name, PyTuple_GET_ITEM(state->fields, j))) {
                int inherited = soac_inherited_dictionary_field(type, name);
                if (inherited <= 0) {
                    if (inherited == 0) {
                        PyErr_SetString(soac_type_mutation_error(),
                            "a field cannot select both new dictionary and native-slot storage");
                    }
                    return -1;
                }
                /* Keep the inherited dictionary field catalog intact. The
                 * native descriptor is active attribute storage; its writes
                 * never populate or mirror the separate hidden dictionary entry. */
            }
        }
        const PyMemberDef *selected = NULL;
        PyTypeObject *declaring_type = NULL;
        /* The solid base chain owns physical members. A public namespace or
         * custom MRO is not a source of offsets or representation authority. */
        for (PyTypeObject *base = type; base != NULL && selected == NULL;
             base = base->tp_base) {
            for (const PyMemberDef *member = base->tp_members;
                 member != NULL && member->name != NULL; ++member) {
                int equal = PyUnicode_EqualToUTF8(name, member->name);
                if (equal < 0) {
                    return -1;
                }
                if (equal) {
                    selected = member;
                    declaring_type = base;
                    break;
                }
            }
        }
        if (selected == NULL || selected->type != Py_T_OBJECT_EX ||
            selected->flags != 0 || selected->offset < (Py_ssize_t)sizeof(PyObject) ||
            selected->offset % (Py_ssize_t)sizeof(PyObject *) != 0 ||
            selected->offset > type->tp_basicsize - (Py_ssize_t)sizeof(PyObject *)) {
            PyErr_Format(soac_type_mutation_error(),
                         "field %R has no supported actual native object slot", name);
            return -1;
        }
        state->object_slots[i].member = selected;
        state->object_slots[i].declaring_type = declaring_type;
        state->object_slots[i].offset = selected->offset;
    }
    return 0;
}

static int
soac_object_slot_in_layout(PyTypeObject *type, const SoacObjectSlot *slot)
{
    if (slot->offset < (Py_ssize_t)sizeof(PyObject) ||
        slot->offset > type->tp_basicsize - (Py_ssize_t)sizeof(PyObject *)) {
        return 0;
    }
    for (PyTypeObject *base = type; base != NULL; base = base->tp_base) {
        for (const PyMemberDef *member = base->tp_members;
             member != NULL && member->name != NULL; ++member) {
            if (member == slot->member) {
                return base == slot->declaring_type &&
                    member->type == Py_T_OBJECT_EX && member->flags == 0 &&
                    member->offset == slot->offset;
            }
        }
    }
    return 0;
}

static SoacTypeContract *
soac_object_slot_contract_at(PyTypeObject *type, PyObject *mro, Py_ssize_t index)
{
    if (index < 0) {
        return soac_type_contract(type);
    }
    PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(mro, index);
    return base == type ? NULL : soac_type_contract(base);
}

int
_PyTypeState_BindSpec(PyTypeObject *type, const PyTypeStateSpecV1 *spec,
                      PyTypeState *storage)
{
    PyObject *mro = lookup_tp_mro(type);
    if (mro == NULL || !PyTuple_CheckExact(mro)) {
        return soac_type_state_error("storage-state preparation has no actual native MRO");
    }
    Py_INCREF(mro);
    Py_ssize_t contracts = 0, required_slots = 0;
    int dictionary = 0;
    for (Py_ssize_t i = -1; i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state == NULL) continue;
        if (soac_contract_live(state) < 0) goto error;
        if (state->dictionary_mode == Py_SOAC_INSTANCE_DICT_INDEXED) {
            soac_type_state_error("indexed storage remains an explicit legacy policy family");
            goto error;
        }
        ++contracts;
        dictionary |= state->dictionary_mode == Py_SOAC_INSTANCE_DICT_ORDINARY;
        if (state->object_slot_count > PY_SSIZE_T_MAX - required_slots) {
            PyErr_NoMemory();
            goto error;
        }
        required_slots += state->object_slot_count;
        for (Py_ssize_t field = 0; field < state->object_slot_count; ++field) {
            Py_ssize_t found = 0;
            for (Py_ssize_t row = 0; row < spec->slot_count; ++row) {
                const PyTypeStateSlotSpecV1 *item = &spec->slots[row];
                if (item->expected_class_owner != state->owner || item->field_index != field) continue;
                ++found;
                if (item->rule_owner == NULL || item->validate == NULL ||
                    item->canonical_name == NULL || !PyUnicode_CheckExact(item->canonical_name) ||
                    !_PyUnicode_Equal(item->canonical_name, PyTuple_GET_ITEM(state->object_slot_fields, field)) ||
                    item->offset != state->object_slots[field].offset ||
                    !soac_object_slot_in_layout(type, &state->object_slots[field])) {
                    soac_type_state_error("storage-state row does not match its actual native slot");
                    goto error;
                }
            }
            if (found != 1) {
                soac_type_state_error("storage-state slot catalogue is missing or duplicates a declaration");
                goto error;
            }
        }
    }
    if (required_slots != spec->slot_count ||
        dictionary != (spec->dictionary_owner != NULL)) {
        soac_type_state_error("storage-state projection does not cover its actual native storage");
        goto error;
    }
    PyObject *held = PyTuple_New(contracts);
    if (held == NULL) goto error;
    if (type->tp_mro != mro) {
        Py_DECREF(held);
        soac_type_state_error("actual MRO changed while retaining storage-state metadata");
        goto error;
    }
    Py_ssize_t next = 0;
    for (Py_ssize_t i = -1; i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state == NULL) continue;
        if (soac_contract_live(state) < 0 || next >= contracts) {
            Py_DECREF(held);
            if (!PyErr_Occurred()) soac_type_state_error("native contract catalogue changed during storage preparation");
            goto error;
        }
        PyTuple_SET_ITEM(held, next++, Py_NewRef(state));
    }
    if (next != contracts) {
        Py_DECREF(held);
        soac_type_state_error("native contract catalogue changed during storage preparation");
        goto error;
    }
    storage->class_contracts = held;
    Py_DECREF(mro);
    return 0;
error:
    {
        PyObject *error = PyErr_GetRaisedException();
        Py_DECREF(mro);
        PyErr_SetRaisedException(error);
        return -1;
    }
}

static SoacTypeContract *
soac_find_object_slot_owner(PyObject *object, PyObject *expected_owner,
                             Py_ssize_t index)
{
    if (object == NULL || !PyType_Check(object)) {
        return NULL;
    }
    PyTypeObject *type = (PyTypeObject *)object;
    if ((type->tp_flags & Py_TPFLAGS_SOAC_CONTRACT) &&
        PyType_GetSoacContractOwner(object) == NULL) {
        return NULL;
    }
    PyObject *mro = lookup_tp_mro(type);
    Py_ssize_t size = mro == NULL ? 0 : PyTuple_GET_SIZE(mro);
    for (Py_ssize_t i = -1; i < size; ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state == NULL) {
            continue;
        }
        if (state->terminal || state->owner == NULL ||
            state->interpreter != _PyInterpreterState_GET()) {
            PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                            "strict object-slot owner is unavailable");
            return NULL;
        }
        if (state->owner != expected_owner) {
            continue;
        }
        if (index < 0 || index >= state->object_slot_count) {
            PyErr_SetString(PyExc_IndexError, "strict object-slot index out of range");
            return NULL;
        }
        if (!soac_object_slot_in_layout(type, &state->object_slots[index])) {
            PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(),
                            "strict object-slot owner is absent from the actual physical layout");
            return NULL;
        }
        return state;
    }
    return NULL;
}

int
PyType_GetSoacObjectSlotOffset(PyObject *object, PyObject *expected_owner,
                              Py_ssize_t index, Py_ssize_t *offset)
{
    if (offset == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    SoacTypeContract *state = soac_find_object_slot_owner(object, expected_owner, index);
    if (state == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    *offset = state->object_slots[index].offset;
    return 1;
}

int
PyType_MatchesSoacObjectSlotDescriptor(PyObject *object, PyObject *expected_owner,
                                       Py_ssize_t index, PyObject *descriptor)
{
    SoacTypeContract *state = soac_find_object_slot_owner(object, expected_owner, index);
    if (state == NULL) {
        return PyErr_Occurred() ? -1 : 0;
    }
    return descriptor != NULL && Py_IS_TYPE(descriptor, &PyMemberDescr_Type) &&
        ((PyMemberDescrObject *)descriptor)->d_member == state->object_slots[index].member &&
        PyDescr_TYPE(descriptor) == state->object_slots[index].declaring_type;
}

static void
soac_record_legacy_slot_lookup(PyTypeObject *type)
{
    struct _PySoacStorageLookupProbe *probe =
        _PyInterpreterState_GET()->dict_state.soac_storage_lookup_probe;
    if (probe != NULL && probe->actual_type == type &&
        probe->slot_type_lookups != UINT64_MAX) {
        ++probe->slot_type_lookups;
    }
}

int
_PySOAC_UsesObjectSlotPolicy(PyTypeObject *type)
{
    soac_record_legacy_slot_lookup(type);
    PyObject *mro = lookup_tp_mro(type);
    Py_ssize_t size = mro == NULL ? 0 : PyTuple_GET_SIZE(mro);
    for (Py_ssize_t i = -1; i < size; ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state != NULL && state->object_slot_count != 0) {
            return 1;
        }
    }
    return 0;
}

static Py_ssize_t
soac_member_width(int kind)
{
    switch (kind) {
        case Py_T_BOOL: case Py_T_BYTE: case Py_T_UBYTE: case Py_T_CHAR:
            return sizeof(char);
        case Py_T_SHORT: case Py_T_USHORT: return sizeof(short);
        case Py_T_INT: case Py_T_UINT: return sizeof(int);
        case Py_T_LONG: case Py_T_ULONG: return sizeof(long);
        case Py_T_LONGLONG: case Py_T_ULONGLONG: return sizeof(long long);
        case Py_T_PYSSIZET: return sizeof(Py_ssize_t);
        case Py_T_FLOAT: return sizeof(float);
        case Py_T_DOUBLE: return sizeof(double);
        case Py_T_STRING: return sizeof(char *);
        case _Py_T_OBJECT: case Py_T_OBJECT_EX: return sizeof(PyObject *);
        case Py_T_STRING_INPLACE: return -1; /* unbounded native read */
        default: return 0; /* None / an ordinary error, with no memory access */
    }
}

int
_PySOAC_CheckObjectSlotAccess(PyObject *object, const PyMemberDef *view)
{
    if (_PyObject_HasTypeStateSlot(object)) {
        return _PyTypeState_CheckMemberAccess(object, view);
    }
    PyTypeObject *type = Py_TYPE(object);
    soac_record_legacy_slot_lookup(type);
    PyObject *mro = lookup_tp_mro(type);
    Py_ssize_t size = mro == NULL ? 0 : PyTuple_GET_SIZE(mro);
    Py_ssize_t width = soac_member_width(view->type);
    for (Py_ssize_t i = -1; i < size; ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state == NULL || state->object_slot_count == 0) {
            continue;
        }
        if (soac_contract_live(state) < 0) {
            return -1;
        }
        if ((view->flags & _Py_AFTER_ITEMS) ||
            (width > 0 && view->offset > PY_SSIZE_T_MAX - width)) {
            goto unsafe_view;
        }
        for (Py_ssize_t j = 0; j < state->object_slot_count; ++j) {
            if (!soac_object_slot_in_layout(type, &state->object_slots[j])) {
                goto unsafe_view;
            }
            Py_ssize_t offset = state->object_slots[j].offset;
            int overlaps = width != 0 &&
                view->offset < offset + (Py_ssize_t)sizeof(PyObject *) &&
                (width < 0 || view->offset + width > offset);
            if (overlaps &&
                (view->offset != offset ||
                 (view->type != _Py_T_OBJECT && view->type != Py_T_OBJECT_EX))) {
                goto unsafe_view;
            }
        }
    }
    return 0;

unsafe_view:
    PyErr_SetString(soac_type_mutation_error(),
                    "native member view is incompatible with a protected object slot");
    return -1;
}

int
_PySOAC_CheckObjectSlotWrite(PyObject *object, Py_ssize_t offset, PyObject *value)
{
    if (_PyObject_HasTypeStateSlot(object)) {
        return _PyTypeState_CheckMemberWrite(object, offset, value);
    }
    PyTypeObject *type = Py_TYPE(object);
    soac_record_legacy_slot_lookup(type);
    PyObject *mro = lookup_tp_mro(type);
    Py_ssize_t size = mro == NULL ? 0 : PyTuple_GET_SIZE(mro);
    for (Py_ssize_t i = -1; i < size; ++i) {
        SoacTypeContract *state = soac_object_slot_contract_at(type, mro, i);
        if (state == NULL || state->object_slot_count == 0) {
            continue;
        }
        if (soac_contract_live(state) < 0) {
            return -1;
        }
        for (Py_ssize_t j = 0; j < state->object_slot_count; ++j) {
            if (state->object_slots[j].offset != offset) {
                continue;
            }
            PyObject *name = PyTuple_GET_ITEM(state->object_slot_fields, j);
            if (state->check_instance_write != NULL &&
                state->check_instance_write(state->owner, object, name, value) < 0) {
                return -1;
            }
            /* The fixed callback cannot transfer the physical contract. Keep
             * the final commit fail-closed if it terminalized the owner. */
            if (Py_TYPE(object) != type || soac_contract_live(state) < 0) {
                if (!PyErr_Occurred()) {
                    PyErr_SetString(soac_type_mutation_error(),
                                    "object-slot receiver changed during validation");
                }
                return -1;
            }
        }
    }
    return 0;
}

int
_PyTypeState_CheckMemberAccess(PyObject *object, const PyMemberDef *view)
{
    PyTypeState *state = PyObject_GetTypeState(object);
    if (state == NULL) return -1;
    Py_ssize_t width = soac_member_width(view->type);
    if (state->kind != _Py_TYPE_STATE_INSTANCE) goto unsafe_view;
    if ((view->flags & _Py_AFTER_ITEMS) ||
        (width > 0 && view->offset > PY_SSIZE_T_MAX - width)) goto unsafe_view;
    /* The trailer is never a public member. A forged view must not replace
     * its state edge even when no declared native slot shares that offset. */
    PyTypeState **tail = _PyObject_TypeStateSlot(object);
    if (tail == NULL) return -1;
    Py_ssize_t tail_offset = (char *)tail - (char *)object;
    if (width != 0 && view->offset < tail_offset + (Py_ssize_t)sizeof(*tail) &&
        (width < 0 || view->offset + width > tail_offset)) goto unsafe_view;
    for (Py_ssize_t i = 0; i < state->slot_count; ++i) {
        Py_ssize_t offset = state->slots[i].offset;
        if (width != 0 && view->offset < offset + (Py_ssize_t)sizeof(PyObject *) &&
            (width < 0 || view->offset + width > offset) &&
            (view->offset != offset ||
             (view->type != _Py_T_OBJECT && view->type != Py_T_OBJECT_EX))) goto unsafe_view;
    }
    return 0;
unsafe_view:
    PyErr_SetString(soac_type_mutation_error(),
                    "native member view is incompatible with protected storage");
    return -1;
}

int
_PyTypeState_CheckMemberWrite(PyObject *object, Py_ssize_t offset, PyObject *value)
{
    PyTypeState *state = PyObject_GetTypeState(object);
    if (state == NULL) return -1;
    if (state->kind != _Py_TYPE_STATE_INSTANCE) {
        return soac_type_state_error("native slot write has another storage family");
    }
    PyTypeObject *type = Py_TYPE(object);
    Py_INCREF(state);
    int result = 0;
    for (Py_ssize_t i = 0; i < state->slot_count; ++i) {
        const _PyTypeStateSlot *slot = &state->slots[i];
        if (slot->offset != offset) continue;
        if (slot->validate(slot->owner, slot->canonical_name, value) != 0 || PyErr_Occurred()) {
            if (!PyErr_Occurred()) soac_type_state_error("native slot validator failed without an exception");
            result = -1;
            break;
        }
        if (Py_TYPE(object) != type || PyObject_GetTypeState(object) != state) {
            if (!PyErr_Occurred()) soac_type_state_error("native slot state changed during validation");
            result = -1;
            break;
        }
    }
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(state);
    PyErr_SetRaisedException(error);
    return result;
}

static int
soac_validate_existing_slot_descriptor(SoacTypeContract *state,
                                       PyObject *name, PyObject *descriptor)
{
    Py_ssize_t index = soac_object_slot_name_index(state, name);
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    if (index < 0 || descriptor == NULL ||
        !Py_IS_TYPE(descriptor, &PyMemberDescr_Type) ||
        ((PyMemberDescrObject *)descriptor)->d_member != state->object_slots[index].member ||
        PyDescr_TYPE(descriptor) != state->object_slots[index].declaring_type) {
        PyErr_SetString(soac_type_mutation_error(),
                        "native object-slot plan is shadowed by a different descriptor");
        return -1;
    }
    return 0;
}

static int
soac_validate_slot_publication(SoacTypeContract *state, PyObject *dict,
                               PyObject *name, PyObject *descriptor,
                               PyObject *provenance)
{
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    if (state->sealed || state->type == NULL || state->type->tp_dict != dict ||
        !(state->type->tp_flags & Py_TPFLAGS_READYING) ||
        (state->type->tp_flags & Py_TPFLAGS_READY) ||
        provenance != descriptor || descriptor == NULL ||
        !Py_IS_TYPE(descriptor, &PyMemberDescr_Type) ||
        PyDescr_TYPE(descriptor) != state->type ||
        soac_validate_existing_slot_descriptor(state, name, descriptor) < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(soac_type_mutation_error(),
                            "invalid canonical object-slot publication");
        }
        return -1;
    }
    return soac_final_override(state->type, name);
}

/* Called only by the actual type_ready member producer. Returns one when its
 * exact just-created native descriptor belongs to the selected own catalog. */
static int
soac_publish_object_slot_descriptor(PyTypeObject *type, PyObject *descriptor)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (state == NULL || soac_object_slot_name_index(state, PyDescr_NAME(descriptor)) < 0) {
        return 0;
    }
    if (soac_validate_slot_publication(state, type->tp_dict,
                                        PyDescr_NAME(descriptor), descriptor,
                                        descriptor) < 0 ||
        _PyDict_SetItemForSoacSlotDescriptor(type->tp_dict, PyDescr_NAME(descriptor),
                                            descriptor, state->owner) < 0) {
        return -1;
    }
    return 1;
}
