/* Pending final-admission protocol. Included by soac_type.inc.
 * One actual type owns one state throughout construction/admission. Its
 * canonical consumed handle carries lineage metadata, never a type backedge. */

int
PyType_GetSoacConstructionInfoV1(PyObject *object,
                                PySoacTypeConstructionInfoV1 *out, size_t size)
{
    if (out != NULL && size != 0) memset(out, 0, Py_MIN(size, sizeof(*out)));
    if (object == NULL || !PyType_Check(object) || out == NULL || size != sizeof(*out)) {
        return soac_type_state_error("invalid native type construction query");
    }
    SoacTypeContract *state = soac_type_state((PyTypeObject *)object);
    if (state == NULL) return 0;
    if (state->interpreter != _PyInterpreterState_GET()) {
        return soac_type_state_error("foreign native type construction state");
    }
    out->abi_version = Py_SOAC_TYPE_CONSTRUCTION_INFO_ABI_V1;
    out->struct_size = sizeof(*out);
    out->phase = soac_type_phase(state);
    out->permanent_contract_published = state->permanent_contract_published;
    if (!state->terminal && out->phase != Py_SOAC_TYPE_STATE_DYNAMIC) {
        out->owner = state->owner;
        if (state->construction != NULL) {
            out->root_construction = (PyObject *)soac_construction_root(state->construction);
        }
    }
    return 1;
}

int
PyType_FailSoacPendingV1(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacConstructionHandle_Type)) {
        return soac_type_state_error("expected a canonical pending construction handle");
    }
    SoacConstructionHandle *root = (SoacConstructionHandle *)object;
    if (root->root != NULL || !root->consumed ||
        root->spec.construction_mode != Py_SOAC_TYPE_CONSTRUCT_PENDING ||
        root->interpreter != _PyInterpreterState_GET()) {
        return soac_type_state_error("pending failure handle is not this canonical lineage");
    }
    /* No releases/callbacks; this never revokes a successfully enforced type. */
    if (root->lineage_phase != SOAC_LINEAGE_RESOLVED) {
        root->lineage_phase = SOAC_LINEAGE_FAILED;
    }
    return 0;
}

static int
soac_admission_live(PyTypeObject *type, SoacTypeContract *state,
                    SoacConstructionHandle *root, PyObject *owner)
{
    if (soac_type_state(type) != state || state->type != type ||
        state->owner != owner || state->terminal ||
        state->interpreter != _PyInterpreterState_GET() ||
        state->construction == NULL || soac_construction_root(state->construction) != root ||
        state->phase != Py_SOAC_TYPE_STATE_ADMITTING ||
        root->lineage_phase != SOAC_LINEAGE_ADMITTING ||
        root->active_constructions || root->active_adapters) {
        return soac_type_state_error("pending final admission was invalidated");
    }
    return 0;
}

static void
soac_move_prepared_policy(SoacTypeContract *state, SoacTypeContract *prepared)
{
    /* Pending state has no previous own policy; its inherited-only dictionary
     * owner remains exactly this state and is never removed/replaced. */
    assert(state->fields == NULL && state->protected_names == NULL &&
           state->final_methods == NULL && state->object_slots == NULL);
    state->fields = prepared->fields; prepared->fields = NULL;
    state->object_slot_fields = prepared->object_slot_fields; prepared->object_slot_fields = NULL;
    state->protected_names = prepared->protected_names; prepared->protected_names = NULL;
    state->final_methods = prepared->final_methods; prepared->final_methods = NULL;
    state->object_slots = prepared->object_slots; prepared->object_slots = NULL;
    state->object_slot_count = prepared->object_slot_count; prepared->object_slot_count = 0;
    state->flags = prepared->flags;
    state->dictionary_mode = prepared->dictionary_mode;
    state->prepare_instance_dictionary_policy = prepared->prepare_instance_dictionary_policy;
    state->check_instance_write = prepared->check_instance_write;
    state->new_instance_dict = prepared->new_instance_dict;
}

static int
soac_validate_final_slot_views(SoacTypeContract *state, PyTypeObject *type)
{
    /* Preparation uses the true physical members; admission must additionally
     * authenticate each currently visible descriptor, including missing and
     * shadowed entries after decorators. Never invoke descriptor lookup. */
    for (Py_ssize_t i = 0; i < state->object_slot_count; ++i) {
        PyObject *name = PyTuple_GET_ITEM(state->object_slot_fields, i);
        PyObject *descriptor = NULL;
        PyObject *mro = type->tp_mro;
        for (Py_ssize_t j = 0; mro != NULL && j < PyTuple_GET_SIZE(mro); ++j) {
            PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(mro, j);
            descriptor = PyDict_GetItemWithError(base->tp_dict, name);
            if (descriptor != NULL || PyErr_Occurred()) break;
        }
        if (PyErr_Occurred() ||
            soac_validate_existing_slot_descriptor(state, name, descriptor) < 0) return -1;
    }
    return 0;
}

static void
soac_release_contract_inputs(PySoacTypeContractSpecV4 *spec)
{
    Py_DECREF(spec->fields);
    Py_DECREF(spec->protected_names);
    Py_DECREF(spec->final_methods);
    Py_XDECREF(spec->object_slot_fields);
}

int
PyType_AdmitSoacPendingV1(PyObject *object, PyObject *owner, PyObject *expected_root,
                         const PySoacTypeContractSpecV4 *spec, size_t size,
                         PySoacTypeFinalCommitV4 expected_commit_final)
{
    /* Invalid/foreign requests do not terminalize a valid unrelated type.
     * A real admission starts only after callback-free exact preflight. */
    if (PyErr_Occurred()) return -1;
    if (object == NULL || !PyType_Check(object) || Py_TYPE(object) != &PyType_Type ||
        owner == NULL || expected_root == NULL || size != sizeof(*spec) ||
        !soac_valid_contract_spec(spec) || expected_commit_final == NULL) {
        return soac_type_state_error("invalid pending final contract request");
    }
    PyTypeObject *type = (PyTypeObject *)object;
    SoacTypeContract *state = soac_type_state(type);
    if (state == NULL || state->construction == NULL ||
        state->owner != owner || state->type != type || state->terminal ||
        state->permanent_contract_published ||
        state->interpreter != _PyInterpreterState_GET()) {
        return soac_type_state_error("pending final type/owner does not match");
    }
    if (state->commit_final == NULL || state->commit_final != expected_commit_final) {
        return soac_type_state_error("pending final commit callback cannot be substituted");
    }
    SoacConstructionHandle *root = soac_construction_root(state->construction);
    if ((PyObject *)root != expected_root || !root->consumed ||
        root->lineage_phase != SOAC_LINEAGE_OPEN ||
        soac_type_phase(state) != Py_SOAC_TYPE_STATE_PENDING ||
        root->active_constructions || root->active_adapters ||
        !(type->tp_flags & Py_TPFLAGS_READY) || (type->tp_flags & Py_TPFLAGS_READYING)) {
        return soac_type_state_error("pending final lineage is unavailable or still constructing");
    }
    PySoacTypeContractSpecV4 inputs = *spec;
    Py_INCREF(inputs.fields);
    Py_INCREF(inputs.protected_names);
    Py_INCREF(inputs.final_methods);
    Py_XINCREF(inputs.object_slot_fields);
    root->lineage_phase = SOAC_LINEAGE_ADMITTING;
    state->phase = Py_SOAC_TYPE_STATE_ADMITTING;
    SoacTypeContract *prepared = NULL;
    PyObject *seal_provider = NULL;  /* future state edge, not another owner */
    /* An ordinary layout is never changed late to fit an indexed factory.
     * The separately reviewed ordinary-storage mode supplies its own join. */
    if (inputs.dictionary_mode == Py_SOAC_INSTANCE_DICT_INDEXED &&
        (type->tp_flags & Py_TPFLAGS_INLINE_VALUES)) {
        soac_type_state_error("final indexed policy cannot change an existing inline-values layout");
        goto failed;
    }
    if (_PySOAC_CheckPendingBases(type->tp_bases) < 0) goto failed;
    prepared = soac_prepare_type_contract(type, owner, &inputs);
    if (prepared == NULL || soac_admission_live(type, state, root, owner) < 0) goto failed;
    soac_move_prepared_policy(state, prepared);
    Py_CLEAR(prepared);
    if (soac_admission_live(type, state, root, owner) < 0 ||
        soac_validate_final_slot_views(state, type) < 0) goto failed;

    if (state->class_dictionary_policy_installed) {
        if (!PyDict_MatchesSoacPolicy(type->tp_dict, (PyObject *)state,
                                      soac_class_dictionary_policy, 0)) {
            soac_type_state_error("inherited type dictionary policy changed");
            goto failed;
        }
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        while (PyDict_Next(type->tp_dict, &pos, &key, &value)) {
            if (soac_class_dictionary_policy((PyObject *)state, type->tp_dict,
                    key, value, PyDict_SOAC_VALIDATE_INITIAL, NULL) < 0) goto failed;
        }
    }
    else {
        if (PyDict_SetSoacPolicy(type->tp_dict, (PyObject *)state,
                                 soac_class_dictionary_policy, 0) < 0) goto failed;
        state->class_dictionary_policy_installed = 1;
    }
    /* The permanent native namespace constraints are now published. Failure
     * may tighten/terminalize them, never remove them. Instances remain barred. */
    state->permanent_contract_published = 1;
    type->tp_flags |= Py_TPFLAGS_SOAC_CONTRACT;
    PyType_Modified(type);
    if (PyErr_Occurred() || soac_admission_live(type, state, root, owner) < 0 ||
        soac_validate_final_slot_views(state, type) < 0) goto failed;

    if (soac_prepare_type_seal(type, &seal_provider) < 0 ||
        soac_admission_live(type, state, root, owner) < 0) goto failed;

    /* All allocations, cache notifications, namespace sealing and provider
     * preparation precede this final hook. Success must be allocation/Python/
     * release-free. No later caller seal can stand in for this publication. */
    int status = state->commit_final(owner, object, &inputs);
    if (status != 0 || PyErr_Occurred() ||
        soac_admission_live(type, state, root, owner) < 0) goto failed;
    soac_commit_type_seal(state, seal_provider);
    seal_provider = NULL;  /* MOVE committed before opening allocation */
    root->selected = state;  /* comparison-only; no strong type backedge */
    root->lineage_phase = SOAC_LINEAGE_RESOLVED;
    state->phase = Py_SOAC_TYPE_STATE_ENFORCED;
    /* Only now may temporary metadata references retire. */
    soac_release_contract_inputs(&inputs);
    return 0;

failed:
    root->lineage_phase = SOAC_LINEAGE_FAILED;
    state->phase = Py_SOAC_TYPE_STATE_FAILED;
    state->terminal = 1;
    soac_type_state_error("pending final admission failed");
    PyObject *error = PyErr_GetRaisedException();
    Py_XDECREF(prepared);
    Py_XDECREF(seal_provider);
    soac_release_contract_inputs(&inputs);
    PyErr_SetRaisedException(error);
    return -1;
}

int
PyType_DisposeSoacProvisionalV1(PyObject *object, PyObject *owner,
                                PyObject *expected_root)
{
    if (object == NULL || !PyType_Check(object) || owner == NULL || expected_root == NULL) {
        return soac_type_state_error("invalid provisional type disposition");
    }
    SoacTypeContract *state = soac_type_state((PyTypeObject *)object);
    if (state == NULL || state->owner != owner || state->construction == NULL ||
        state->type != (PyTypeObject *)object || state->terminal ||
        state->permanent_contract_published ||
        state->interpreter != _PyInterpreterState_GET()) {
        return soac_type_state_error("provisional type is not this uncommitted native state");
    }
    SoacConstructionHandle *root = soac_construction_root(state->construction);
    if ((PyObject *)root != expected_root || root->lineage_phase != SOAC_LINEAGE_RESOLVED ||
        root->selected == state || state->phase != Py_SOAC_TYPE_STATE_PENDING ||
        root->active_constructions || root->active_adapters) {
        return soac_type_state_error("only an unselected provisional after final success can become dynamic");
    }
    /* Ordinary storage, inherited policies and already sealed function owners
     * are untouched. Publish dynamic before the first callback-capable release. */
    state->phase = Py_SOAC_TYPE_STATE_DYNAMIC;
    PyObject *error = PyErr_GetRaisedException();
    Py_CLEAR(state->owner);
    Py_CLEAR(state->construction);
    PyErr_SetRaisedException(error);
    return 0;
}

/* Only the existing exact dataclass slots context calls this after authenticating
 * all five actual operands and the distinct one-use replacement handle. */
int
_PySOAC_LinkPendingTypeHandle(PyObject *object, PyObject *original, PyObject *owner)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacConstructionHandle_Type) ||
        original == NULL || !PyType_Check(original)) {
        return soac_type_state_error("invalid pending replacement linkage");
    }
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    SoacTypeContract *state = soac_type_state((PyTypeObject *)original);
    int original_pending = state != NULL &&
        soac_type_phase(state) == Py_SOAC_TYPE_STATE_PENDING && !state->permanent_contract_published;
    int replacement_pending = handle->spec.construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING;
    if (!original_pending && !replacement_pending) return 0; /* unchanged ENFORCED path */
    if (!original_pending || !replacement_pending || state->owner != owner ||
        state->construction == NULL || handle->root != NULL || handle->consumed ||
        !handle->dataclass_slots || handle->reserved_state == NULL ||
        handle->interpreter != _PyInterpreterState_GET() ||
        state->interpreter != handle->interpreter) {
        return soac_type_state_error("replacement must inherit its actual pending lineage");
    }
    SoacConstructionHandle *root = soac_construction_root(state->construction);
    if (root == handle || root->lineage_phase != SOAC_LINEAGE_OPEN ||
        root->active_adapters == 0 || !root->consumed) {
        return soac_type_state_error("replacement has no active native pending adapter");
    }
    handle->root = (SoacConstructionHandle *)Py_NewRef(root);
    return 0;
}

/* This interval describes only the actual native adapter invocation. Final
 * caller-operand cleanup/completion remains the explicit outer call protocol;
 * the trusted final owner callback must authenticate that frontier separately. */
int
_PySOAC_BeginPendingTypeAdapter(PyObject *actual, PyObject **out)
{
    *out = NULL;
    SoacTypeContract *state = soac_type_state((PyTypeObject *)actual);
    if (state == NULL || state->permanent_contract_published ||
        state->phase == Py_SOAC_TYPE_STATE_DYNAMIC) return 0;
    if (soac_type_phase(state) != Py_SOAC_TYPE_STATE_PENDING ||
        state->construction == NULL || state->terminal ||
        state->interpreter != _PyInterpreterState_GET()) {
        return soac_type_state_error("pending adapter type is unavailable");
    }
    SoacConstructionHandle *root = soac_construction_root(state->construction);
    if (root->lineage_phase != SOAC_LINEAGE_OPEN ||
        root->active_adapters == UINT_MAX) {
        return soac_type_state_error("pending adapter lineage is unavailable");
    }
    if (root->active_constructions &&
        ((((PyTypeObject *)actual)->tp_flags & (Py_TPFLAGS_READY | Py_TPFLAGS_READYING)) ||
         soac_pending_constructor_live((PyTypeObject *)actual) < 0)) {
        return soac_type_state_error("pending early adapter binding is not this live constructor");
    }
    /* The early pending owner hook may bind the selected dataclass invocation
     * after tp_dict/ht_slots publication and before set_attrs/Ready. This records
     * only the actual owner/lineage; no MRO/member layout or admission is granted. */
    root->active_adapters++;
    *out = Py_NewRef(root);  /* metadata only, no actual type/root-function pin */
    return 0;
}

void
_PySOAC_EndPendingTypeAdapter(PyObject *object, int failed)
{
    if (object == NULL) return;
    assert(Py_IS_TYPE(object, &SoacConstructionHandle_Type));
    SoacConstructionHandle *root = (SoacConstructionHandle *)object;
    assert(root->root == NULL && root->active_adapters > 0);
    if (failed) soac_fail_construction(root);
    root->active_adapters--;
}
