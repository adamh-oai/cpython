/* Native SOAC type state. Included by typeobject.c after type_new_ctx so the
 * explicit constructor follows exactly the ordinary builtin type pipeline. */

typedef struct {
    PyObject_HEAD
    PyInterpreterState *interpreter;
    PySoacTypeConstructionSpec spec;
    int consumed;
} SoacConstructionHandle;

typedef struct {
    PyObject_HEAD
    PyInterpreterState *interpreter;
    PyObject *owner;
    PyObject *fields;
    PyObject *protected_names;
    PyObject *final_methods;
    /* Non-owning: the type owns this contract. Escaping its dictionary must
     * not create a reverse lifetime edge through the dictionary policy. The
     * type clears this address before its deallocation callbacks or reference
     * release; terminal contracts never dereference it. */
    PyTypeObject *type;
    PyObject *annotation_provider;
    uint32_t flags;
    int sealed;
    int terminal;
    int (*check_instance_write)(PyObject *, PyObject *, PyObject *, PyObject *);
    PyObject *(*new_instance_dict)(PyObject *, PyObject *);
} SoacTypeContract;

static PyTypeObject SoacConstructionHandle_Type;
static PyTypeObject SoacTypeContract_Type;
static PyObject *type_new_impl(type_new_ctx *ctx);
static int type_new_get_bases(type_new_ctx *ctx, PyObject **type);

static PyObject *
soac_type_mutation_error(void)
{
    PyObject *error = PySoac_GetStrictMutationError();
    return error != NULL ? error : PyExc_TypeError;
}

static SoacTypeContract *
soac_type_contract(PyTypeObject *type)
{
    if ((type->tp_flags & (Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_SOAC_CONTRACT)) !=
            (Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_SOAC_CONTRACT)) {
        return NULL;
    }
    return (SoacTypeContract *)((PyHeapTypeObject *)type)->ht_soac_contract;
}

/* An ordinary descendant has no own strict receiver capability, but changing
 * its ancestry or an instance's actual class still requires admission. Read
 * the actual native MRO rather than only the direct base's own contract: an
 * intermediate ordinary class must not hide installed inherited invariants.
 * This query does not grant or install a dictionary/dispatch policy. */
static int
soac_mro_has_contract(PyObject *mro)
{
    for (Py_ssize_t i = 0; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        if (soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i)) != NULL) {
            return 1;
        }
    }
    return 0;
}

static int
soac_type_has_contract_in_mro(PyTypeObject *type)
{
    return soac_type_contract(type) != NULL ||
        soac_mro_has_contract(lookup_tp_mro(type));
}

static int
soac_mro_contains_type(PyObject *mro, PyTypeObject *type)
{
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
        if (PyTuple_GET_ITEM(mro, i) == (PyObject *)type) {
            return 1;
        }
    }
    return 0;
}

/* Ordinary subclasses do not acquire a strict dispatch capability. They do
 * retain inherited physical dictionary invariants. The trusted factory sees
 * the actual instance and can compose the policies of its actual MRO. */
static SoacTypeContract *
soac_type_dictionary_contract(PyTypeObject *type)
{
    SoacTypeContract *own = soac_type_contract(type);
    if (own != NULL && own->new_instance_dict != NULL) {
        return own;
    }
    PyObject *mro = type->tp_mro;
    for (Py_ssize_t i = 1; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base != NULL && base->new_instance_dict != NULL) {
            /* A factory is installed on every actual strict class, including
             * method-only classes with no physical field prefix. Its presence
             * alone must not turn an ordinary subclass into a policy-bearing
             * receiver or forbid ordinary __dict__ replacement. Continue the
             * actual MRO search so a later nonempty prefix is still enforced.
             * Unknown/terminal state stays fail-closed, not silently ordinary. */
            if (!base->terminal && base->interpreter == _PyInterpreterState_GET() &&
                base->fields != NULL && PyTuple_GET_SIZE(base->fields) == 0) {
                continue;
            }
            return base;
        }
    }
    return NULL;
}

int
_PySOAC_UsesInstanceDictionaryPolicy(PyTypeObject *type)
{
    return soac_type_dictionary_contract(type) != NULL;
}

static int
soac_contract_live(SoacTypeContract *state)
{
    if (state->terminal || state->interpreter != _PyInterpreterState_GET()) {
        PyErr_SetString(soac_type_mutation_error(), "strict class contract is unavailable in this execution");
        return -1;
    }
    return 0;
}

static int
soac_handle_traverse(SoacConstructionHandle *handle, visitproc visit, void *arg)
{
    Py_VISIT(handle->spec.owner);
    Py_VISIT(handle->spec.namespace_function);
    Py_VISIT(handle->spec.name);
    Py_VISIT(handle->spec.bases);
    Py_VISIT(handle->spec.namespace);
    Py_VISIT(handle->spec.keywords);
    Py_VISIT(handle->spec.fields);
    Py_VISIT(handle->spec.protected_names);
    Py_VISIT(handle->spec.final_methods);
    return 0;
}

static int
soac_handle_clear(SoacConstructionHandle *handle)
{
    handle->consumed = 1;
    Py_CLEAR(handle->spec.owner);
    Py_CLEAR(handle->spec.namespace_function);
    Py_CLEAR(handle->spec.name);
    Py_CLEAR(handle->spec.bases);
    Py_CLEAR(handle->spec.namespace);
    Py_CLEAR(handle->spec.keywords);
    Py_CLEAR(handle->spec.fields);
    Py_CLEAR(handle->spec.protected_names);
    Py_CLEAR(handle->spec.final_methods);
    return 0;
}

static void
soac_handle_dealloc(SoacConstructionHandle *handle)
{
    PyObject_GC_UnTrack(handle);
    soac_handle_clear(handle);
    PyObject_GC_Del(handle);
}

static PyTypeObject SoacConstructionHandle_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_soac.NativeClassConstruction",
    .tp_basicsize = sizeof(SoacConstructionHandle),
    .tp_dealloc = (destructor)soac_handle_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_handle_traverse,
    .tp_clear = (inquiry)soac_handle_clear,
};

static int
soac_state_traverse(SoacTypeContract *state, visitproc visit, void *arg)
{
    Py_VISIT(state->owner);
    Py_VISIT(state->fields);
    Py_VISIT(state->protected_names);
    Py_VISIT(state->final_methods);
    Py_VISIT(state->annotation_provider);
    return 0;
}

static int
soac_state_clear(SoacTypeContract *state)
{
    /* Terminal GC cleanup is not unsealing. Any resurrection still sees the
     * permanent terminal marker and cannot resume optimized or ordinary use. */
    state->terminal = 1;
    state->type = NULL;
    Py_CLEAR(state->owner);
    Py_CLEAR(state->annotation_provider);
    Py_CLEAR(state->fields);
    Py_CLEAR(state->protected_names);
    Py_CLEAR(state->final_methods);
    return 0;
}

static void
soac_state_dealloc(SoacTypeContract *state)
{
    PyObject_GC_UnTrack(state);
    soac_state_clear(state);
    PyObject_GC_Del(state);
}

static PyTypeObject SoacTypeContract_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_soac.NativeTypeContract",
    .tp_basicsize = sizeof(SoacTypeContract),
    .tp_dealloc = (destructor)soac_state_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_state_traverse,
    .tp_clear = (inquiry)soac_state_clear,
};

static int
soac_valid_names(PyObject *names)
{
    if (!PyTuple_CheckExact(names)) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(names); ++i) {
        if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(names, i))) {
            return 0;
        }
        for (Py_ssize_t j = 0; j < i; ++j) {
            if (_PyUnicode_Equal(PyTuple_GET_ITEM(names, i), PyTuple_GET_ITEM(names, j))) {
                return 0;
            }
        }
    }
    return 1;
}

PyObject *
PyType_NewSoacConstructionHandle(const PySoacTypeConstructionSpec *spec)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_RuntimeError, "strict type construction does not yet support free-threaded CPython");
    return NULL;
#else
    if (spec == NULL || spec->abi_version != Py_SOAC_TYPE_CONTRACT_ABI ||
        (spec->flags & ~Py_SOAC_TYPE_FINAL) != 0 || spec->owner == NULL ||
        spec->namespace_function == NULL || !PyFunction_Check(spec->namespace_function) ||
        spec->name == NULL || !PyUnicode_CheckExact(spec->name) ||
        spec->bases == NULL || !PyTuple_CheckExact(spec->bases) ||
        spec->namespace == NULL || !PyDict_CheckExact(spec->namespace) ||
        spec->keywords == NULL || !PyDict_CheckExact(spec->keywords) ||
        spec->fields == NULL || !soac_valid_names(spec->fields) ||
        spec->protected_names == NULL || !soac_valid_names(spec->protected_names) ||
        spec->final_methods == NULL || !soac_valid_names(spec->final_methods)) {
        PyErr_SetString(soac_type_mutation_error(), "invalid native strict class construction plan");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(spec->bases); ++i) {
        PyObject *base = PyTuple_GET_ITEM(spec->bases, i);
        if (!PyType_Check(base) || Py_TYPE(base) != &PyType_Type) {
            PyErr_SetString(soac_type_mutation_error(), "strict type constructor requires verified builtin-metaclass bases");
            return NULL;
        }
        if (spec->new_instance_dict != NULL &&
            ((PyTypeObject *)base)->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
            PyErr_SetString(soac_type_mutation_error(),
                "strict dictionary construction cannot replace an inherited inline-values layout");
            return NULL;
        }
    }
    const char *unsupported_hooks[] = {"__getattribute__", "__getattr__", "__setattr__", "__delattr__", NULL};
    for (const char **hook = unsupported_hooks; *hook != NULL; ++hook) {
        PyObject *value = PyDict_GetItemString(spec->namespace, *hook);
        if (value != NULL) {
            PyErr_SetString(soac_type_mutation_error(), "custom attribute hooks require a verified adapter or dynamic construction");
            return NULL;
        }
        if (PyErr_Occurred()) {
            return NULL;
        }
    }
    if (PyType_Ready(&SoacConstructionHandle_Type) < 0 ||
        PyType_Ready(&SoacTypeContract_Type) < 0) {
        return NULL;
    }
    SoacConstructionHandle *handle = PyObject_GC_New(SoacConstructionHandle, &SoacConstructionHandle_Type);
    if (handle == NULL) {
        return NULL;
    }
    handle->interpreter = _PyInterpreterState_GET();
    handle->spec = *spec;
    handle->consumed = 0;
    Py_INCREF(spec->owner);
    Py_INCREF(spec->namespace_function);
    Py_INCREF(spec->name);
    Py_INCREF(spec->bases);
    Py_INCREF(spec->namespace);
    Py_INCREF(spec->keywords);
    Py_INCREF(spec->fields);
    Py_INCREF(spec->protected_names);
    Py_INCREF(spec->final_methods);
    PyObject_GC_Track(handle);
    return (PyObject *)handle;
#endif
}

static int
soac_names_contains(PyObject *names, PyObject *name)
{
    if (!PyUnicode_Check(name)) {
        return 0;
    }
    PyObject *exact = PyUnicode_CheckExact(name) ? Py_NewRef(name) : _PyUnicode_Copy(name);
    if (exact == NULL) {
        return -1;
    }
    int result = PySet_Contains(names, exact);
    Py_DECREF(exact);
    return result;
}

static int
soac_final_override(PyTypeObject *type, PyObject *name)
{
    PyObject *mro = lookup_tp_mro(type);
    if (mro == NULL) {
        return 0;
    }
    for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base != NULL) {
            if (soac_contract_live(base) < 0) {
                return -1;
            }
            int found = soac_names_contains(base->final_methods, name);
            if (found != 0) {
                if (found > 0) {
                    PyErr_Format(soac_type_mutation_error(), "cannot override strict final method %R", name);
                }
                return -1;
            }
        }
    }
    return 0;
}

static int
soac_check_class_write(PyTypeObject *type, PyObject *name, PyObject *value,
                       PyObject *member_operation)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (member_operation != NULL) {
        if (state == NULL || state->owner == NULL ||
            _PySOAC_DataclassCheckMember(member_operation, (PyObject *)type,
                                         state->owner, name, value) < 0) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(soac_type_mutation_error(),
                                "generated member requires its actual strict class");
            }
            return -1;
        }
    }
    if (state != NULL) {
        if (soac_contract_live(state) < 0) {
            return -1;
        }
        if (state->sealed) {
            PyErr_Format(soac_type_mutation_error(), "cannot mutate sealed strict class %.200s", type->tp_name);
            return -1;
        }
        if (PyUnicode_Check(name) &&
            (_PyUnicode_EqualToASCIIString(name, "__bases__") ||
             _PyUnicode_EqualToASCIIString(name, "__class__") ||
             _PyUnicode_EqualToASCIIString(name, "__getattribute__") ||
             _PyUnicode_EqualToASCIIString(name, "__getattr__") ||
             (member_operation == NULL &&
              (_PyUnicode_EqualToASCIIString(name, "__setattr__") ||
               _PyUnicode_EqualToASCIIString(name, "__delattr__"))))) {
            PyErr_SetString(soac_type_mutation_error(), "cannot change installed strict class lookup or identity");
            return -1;
        }
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(state->fields); ++i) {
            if (PyUnicode_Check(name) && _PyUnicode_Equal(PyTuple_GET_ITEM(state->fields, i), name)) {
                PyObject *previous = PyDict_GetItemWithError(type->tp_dict, name);
                if (previous == NULL && PyErr_Occurred()) {
                    return -1;
                }
                if (value != previous &&
                    ((value != NULL && (Py_TYPE(value)->tp_descr_get != NULL || Py_TYPE(value)->tp_descr_set != NULL)) ||
                     (previous != NULL && (Py_TYPE(previous)->tp_descr_get != NULL || Py_TYPE(previous)->tp_descr_set != NULL)))) {
                    PyErr_SetString(soac_type_mutation_error(), "cannot change an installed strict field's descriptor policy");
                    return -1;
                }
            }
        }
    }
    return soac_final_override(type, name);
}

int
_PySOAC_CheckClassWrite(PyTypeObject *type, PyObject *name, PyObject *value)
{
    return soac_check_class_write(type, name, value, NULL);
}

/* Dict policies also protect authoritative tp_dict aliases held by native
 * callers. Public dict.clear has an error channel; void PyDict_Clear follows
 * the explicitly unsupported immutable-namespace native boundary. */
static int
soac_class_dictionary_policy(PyObject *owner, PyObject *dict, PyObject *key,
                             PyObject *value, int operation, PyObject *provenance)
{
    SoacTypeContract *state = (SoacTypeContract *)owner;
    if (operation == PyDict_SOAC_TERMINAL_TEARDOWN) {
        state->terminal = 1;
        state->type = NULL;
        return 0;
    }
    if (operation == PyDict_SOAC_VALIDATE_INITIAL) {
        /* Recheck the actual copied namespace at installation, not just the
         * earlier input dict: allocations may have run unrelated GC callbacks. */
        return _PySOAC_CheckClassWrite((PyTypeObject *)state->type, key, value);
    }
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    if (operation == PyDict_SOAC_CLASS_MEMBER_INSERT ||
        operation == PyDict_SOAC_CLASS_MEMBER_REPLACE) {
        if (provenance == NULL || state->type == NULL ||
            dict != state->type->tp_dict) {
            PyErr_SetString(soac_type_mutation_error(),
                            "invalid generated-member namespace operation");
            return -1;
        }
        /* Only the exact fresh native role may install frozen setattr/delattr.
         * Sealed, terminal, final, and field-descriptor barriers still apply. */
        return soac_check_class_write(state->type, key, value, provenance);
    }
    if (operation == PyDict_SOAC_CACHE_INSERT || operation == PyDict_SOAC_CACHE_REPLACE) {
        if (!state->sealed || provenance == NULL || provenance != state->annotation_provider ||
            key == NULL || !PyUnicode_CheckExact(key)) {
            PyErr_SetString(soac_type_mutation_error(), "strict annotation cache provider mismatch");
            return -1;
        }
        if (_PyUnicode_EqualToASCIIString(key, "__annotate_func__") &&
            operation == PyDict_SOAC_CACHE_INSERT && provenance == Py_None && value == Py_None) {
            return 0;
        }
        if (_PyUnicode_EqualToASCIIString(key, "__annotations_cache__") && PyDict_Check(value)) {
            /* Only the native getter calls this private operation. Nested
             * successful class getters preserve ordinary completion order. */
            return 0;
        }
        PyErr_SetString(soac_type_mutation_error(), "invalid strict annotation cache publication");
        return -1;
    }
    if (operation == PyDict_SOAC_CLEAR) {
        PyErr_SetString(soac_type_mutation_error(), "cannot clear a strict class namespace");
        return -1;
    }
    if (key == NULL || state->type == NULL || provenance != NULL) {
        PyErr_SetString(soac_type_mutation_error(), "invalid strict class namespace mutation");
        return -1;
    }
    return _PySOAC_CheckClassWrite((PyTypeObject *)state->type, key, value);
}

int
_PySOAC_MatchesClassNamespacePolicy(PyObject *policy_owner,
                                   PyDict_SoacPolicyCallback validate,
                                   PyObject *dict, PyObject *expected_owner)
{
    /* The dictionary kernel pins policy_owner; expected_owner is only an
     * identity comparison operand and may be a stale/arbitrary address. */
    if (validate != soac_class_dictionary_policy) {
        return 0;
    }
    /* Teardown keeps the immutable sidecar role/terminal marker but releases
     * its GC owner edge. Do not inspect a released owner or turn that terminal
     * class namespace into an unrelated ordinary dictionary. */
    if (policy_owner == NULL) {
        PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
        if (error != NULL) {
            PyErr_SetString(error, "strict class namespace binding is unavailable");
        }
        return -1;
    }
    if (!Py_IS_TYPE(policy_owner, &SoacTypeContract_Type)) {
        return 0;
    }
    SoacTypeContract *state = (SoacTypeContract *)policy_owner;
    if (state->terminal || state->type == NULL || state->owner == NULL ||
        state->interpreter != _PyInterpreterState_GET()) {
        PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
        if (error != NULL) {
            PyErr_SetString(error, "strict class namespace binding is unavailable");
        }
        return -1;
    }
    if (state->owner != expected_owner) {
        return 0;
    }
    /* BeginTeardown nulls the non-owning type address before type deallocation
     * callbacks or reference release. No allocation or Python effect occurs between
     * checking that live binding and inspecting its actual dictionary. */
    PyTypeObject *type = state->type;
    return type->tp_dict == dict && soac_type_contract(type) == state &&
        PyDict_MatchesSoacPolicy(dict, policy_owner,
                                soac_class_dictionary_policy, 0);
}

static int
soac_install_type_contract(PyTypeObject *type, SoacConstructionHandle *handle)
{
    PySoacTypeConstructionSpec *spec = &handle->spec;
    SoacTypeContract *state = PyObject_GC_New(SoacTypeContract, &SoacTypeContract_Type);
    if (state == NULL) {
        return -1;
    }
    state->interpreter = handle->interpreter;
    state->owner = Py_NewRef(spec->owner);
    state->type = NULL;
    state->annotation_provider = NULL;
    state->fields = Py_NewRef(spec->fields);
    state->protected_names = PySet_New(spec->protected_names);
    state->final_methods = PySet_New(spec->final_methods);
    state->flags = spec->flags;
    state->sealed = 0;
    state->terminal = 0;
    state->check_instance_write = spec->check_instance_write;
    state->new_instance_dict = spec->new_instance_dict;
    if (state->protected_names == NULL || state->final_methods == NULL) {
        Py_DECREF(state);
        return -1;
    }
    PyObject *bases = type->tp_bases;
    for (Py_ssize_t i = 0; bases != NULL && i < PyTuple_GET_SIZE(bases); ++i) {
        PyTypeObject *base_type = (PyTypeObject *)PyTuple_GET_ITEM(bases, i);
        SoacTypeContract *base = soac_type_contract(base_type);
        if (base == NULL) {
            if (base_type != &PyBaseObject_Type) {
                Py_DECREF(state);
                PyErr_SetString(soac_type_mutation_error(), "strict construction requires protected bases; classify unsupported bases before construction");
                return -1;
            }
            continue;
        }
        if (soac_contract_live(base) < 0 || _PySet_Update(state->protected_names, base->protected_names) < 0 ||
            _PySet_Update(state->final_methods, base->final_methods) < 0) {
            Py_DECREF(state);
            return -1;
        }
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(spec->fields); ++i) {
        PyObject *name = PyTuple_GET_ITEM(spec->fields, i);
        if (PySet_Contains(state->final_methods, name) > 0) {
            PyErr_Format(soac_type_mutation_error(), "field cannot replace strict final method %R", name);
            Py_DECREF(state);
            return -1;
        }
        if (PySet_Discard(state->protected_names, name) < 0) {
            Py_DECREF(state);
            return -1;
        }
    }
    /* GC introspection may expose referents. Published policy catalogs must
     * themselves be immutable, not merely hidden behind an opaque C owner. */
    PyObject *protected_names = PyFrozenSet_New(state->protected_names);
    PyObject *final_methods = PyFrozenSet_New(state->final_methods);
    if (protected_names == NULL || final_methods == NULL) {
        Py_XDECREF(protected_names);
        Py_XDECREF(final_methods);
        Py_DECREF(state);
        return -1;
    }
    Py_SETREF(state->protected_names, protected_names);
    Py_SETREF(state->final_methods, final_methods);
    state->type = type;
    ((PyHeapTypeObject *)type)->ht_soac_contract = (PyObject *)state;
    type->tp_flags |= Py_TPFLAGS_SOAC_CONTRACT;
    PyObject_GC_Track(state);
    if (spec->bind_type != NULL && spec->bind_type(spec->owner, (PyObject *)type) < 0) {
        state->terminal = 1;
        if (!PyErr_Occurred()) {
            PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
            if (error != NULL) {
                PyErr_SetString(error, "strict type owner rejected actual type binding");
            }
        }
        return -1;
    }
    if (PyDict_SetSoacPolicy(type->tp_dict, (PyObject *)state,
                             soac_class_dictionary_policy, 0) < 0) {
        state->terminal = 1;
        return -1;
    }
    return 0;
}

PyObject *
PyType_FromSoacConstructionHandle(PyObject *object, PyObject *namespace_function)
{
    if (!Py_IS_TYPE(object, &SoacConstructionHandle_Type)) {
        PyErr_SetString(soac_type_mutation_error(), "expected an interpreter-owned strict construction handle");
        return NULL;
    }
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    if (handle->consumed || handle->interpreter != _PyInterpreterState_GET() ||
        handle->spec.namespace_function != namespace_function) {
        PyErr_SetString(soac_type_mutation_error(), "strict construction handle was consumed, transferred, or used by the wrong namespace function");
        return NULL;
    }
    /* Consume before allocation or a callback can observe the class/owner. */
    handle->consumed = 1;
    PySoacTypeConstructionSpec *spec = &handle->spec;
    PyObject *args = PyTuple_Pack(3, spec->name, spec->bases, spec->namespace);
    if (args == NULL) {
        return NULL;
    }
    type_new_ctx ctx = {
        .metatype = &PyType_Type, .args = args, .kwds = spec->keywords,
        .orig_dict = spec->namespace, .name = spec->name, .bases = spec->bases,
        .soac_handle = object,
    };
    PyObject *result = NULL;
    int status = type_new_get_bases(&ctx, &result);
    if (status == 0) {
        result = type_new_impl(&ctx);
        Py_DECREF(ctx.bases);
    }
    else if (status == 1) {
        Py_CLEAR(result);
        PyErr_SetString(soac_type_mutation_error(), "strict construction selected an unauthenticated metaclass");
    }
    if (result != NULL && PyType_Type.tp_init(result, args, spec->keywords) < 0) {
        Py_CLEAR(result);
    }
    Py_DECREF(args);
    return result;
}

int
PyType_HasSoacContract(PyObject *object)
{
    return PyType_Check(object) && soac_type_contract((PyTypeObject *)object) != NULL;
}

PyObject *
PyType_GetSoacContractOwner(PyObject *object)
{
    if (!PyType_Check(object)) {
        return NULL;
    }
    PyTypeObject *type = (PyTypeObject *)object;
    SoacTypeContract *state = soac_type_contract(type);
    if (state == NULL && !(type->tp_flags & Py_TPFLAGS_SOAC_CONTRACT)) {
        return NULL;
    }
    if (state == NULL || state->terminal || state->owner == NULL ||
        state->interpreter != _PyInterpreterState_GET()) {
        PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
        if (error != NULL) {
            PyErr_SetString(error, "strict class owner is unavailable in this execution");
        }
        return NULL;
    }
    return state->owner;  /* borrowed */
}

int
_PySoac_IsLayoutDescriptor(PyObject *name, PyObject *descriptor)
{
    if (name == NULL || descriptor == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyUnicode_CheckExact(name)) {
        return 0;
    }
    int is_dict = _PyUnicode_Equal(name, &_Py_ID(__dict__));
    int is_weakref = _PyUnicode_Equal(name, &_Py_ID(__weakref__));
    if (!is_dict && !is_weakref) {
        return 0;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
#ifdef Py_GIL_DISABLED
    PyMutex_Lock(&interp->cached_objects.descriptor_mutex);
#endif
    PyObject *cached = is_dict ? interp->cached_objects.dict_descriptor
                              : interp->cached_objects.weakref_descriptor;
    int result = cached != NULL && descriptor == cached;
#ifdef Py_GIL_DISABLED
    PyMutex_Unlock(&interp->cached_objects.descriptor_mutex);
#endif
    return result;
}

int
PyType_IsSoacSealed(PyObject *object)
{
    SoacTypeContract *state = PyType_Check(object) ? soac_type_contract((PyTypeObject *)object) : NULL;
    return state != NULL && state->sealed && !state->terminal && state->interpreter == _PyInterpreterState_GET();
}

int
PyType_SealSoacContract(PyObject *object, PyObject *owner)
{
    SoacTypeContract *state = PyType_Check(object) ? soac_type_contract((PyTypeObject *)object) : NULL;
    if (state == NULL || state->owner != owner) {
        PyErr_SetString(soac_type_mutation_error(), "strict class seal owner mismatch");
        return -1;
    }
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    if (state->sealed) {
        return 0;
    }
    PyObject *dict = ((PyTypeObject *)object)->tp_dict;
    PyObject *provider = PyDict_GetItemWithError(dict, &_Py_ID(__annotate__));
    if (provider == NULL && !PyErr_Occurred()) {
        provider = PyDict_GetItemWithError(dict, &_Py_ID(__annotate_func__));
    }
    if (provider == NULL && PyErr_Occurred()) {
        return -1;
    }
    state->annotation_provider = Py_NewRef(provider != NULL ? provider : Py_None);
    if (PyDict_SealSoacNamespace(((PyTypeObject *)object)->tp_dict) < 0) {
        Py_CLEAR(state->annotation_provider);
        return -1;
    }
    state->sealed = 1;
    return 0;
}

int
_PySOAC_PublishAnnotationCache(PyTypeObject *type, PyObject *key, PyObject *value)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (state == NULL || !state->sealed) {
        return PyDict_SetItem(type->tp_dict, key, value);
    }
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    return _PyDict_SetItemForRuntimeCache(type->tp_dict, key, value, state->annotation_provider);
}

int
_PySOAC_ProtectedName(PyTypeObject *type, PyObject *name)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (state == NULL) {
        return 0;
    }
    if (soac_contract_live(state) < 0) {
        return -1;
    }
    return soac_names_contains(state->protected_names, name);
}

int
_PySOAC_CheckInstanceWrite(PyObject *object, PyObject *name, PyObject *value)
{
    SoacTypeContract *state = soac_type_contract(Py_TYPE(object));
    if (state == NULL) {
        return 0;
    }
    int protected = _PySOAC_ProtectedName(Py_TYPE(object), name);
    if (protected != 0) {
        if (protected > 0) {
            PyErr_Format(soac_type_mutation_error(), "cannot assign protected strict class member %R on an instance", name);
        }
        return -1;
    }
    if (state->check_instance_write != NULL) {
        return state->check_instance_write(state->owner, object, name, value);
    }
    return 0;
}

int
_PySOAC_CheckClassAssignment(PyTypeObject *old_type, PyTypeObject *new_type)
{
    if (soac_type_has_contract_in_mro(old_type) ||
        soac_type_has_contract_in_mro(new_type)) {
        PyErr_SetString(soac_type_mutation_error(), "cannot reassign __class__ across an installed strict contract");
        return -1;
    }
    return 0;
}

int
_PySOAC_CheckDictionaryReplacement(PyObject *object)
{
    if (soac_type_contract(Py_TYPE(object)) != NULL ||
        _PySOAC_UsesInstanceDictionaryPolicy(Py_TYPE(object))) {
        PyErr_SetString(soac_type_mutation_error(), "cannot replace a strict instance dictionary");
        return -1;
    }
    return 0;
}

int
_PySOAC_CheckTypeBases(PyTypeObject *type, PyObject *bases)
{
    if (soac_type_contract(type) != NULL) {
        PyErr_SetString(soac_type_mutation_error(), "cannot replace bases of an installed strict class");
        return -1;
    }
    if (soac_type_has_contract_in_mro(type)) {
        PyErr_SetString(soac_type_mutation_error(), "base reassignment cannot discard installed strict inheritance restrictions");
        return -1;
    }
    if (bases != NULL && PyTuple_Check(bases)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
            PyObject *base = PyTuple_GET_ITEM(bases, i);
            if (PyType_Check(base) && soac_type_has_contract_in_mro((PyTypeObject *)base)) {
                PyErr_SetString(soac_type_mutation_error(), "base reassignment requires strict inheritance admission");
                return -1;
            }
        }
    }
    return 0;
}

int
_PySOAC_CheckTypeMro(PyTypeObject *type, PyObject *new_mro, int initial)
{
    /* mro_invoke has validated this exact tuple of types. No Python equality,
     * attribute lookup, allocation, or callback occurs at this commit guard. */
    int has_contract = soac_type_contract(type) != NULL ||
        soac_mro_has_contract(new_mro);
    if (!initial) {
        if (has_contract || soac_type_has_contract_in_mro(type)) {
            PyErr_SetString(soac_type_mutation_error(),
                            "MRO reassignment requires strict inheritance admission");
            return -1;
        }
        return 0;
    }

    /* A custom initial MRO may add ordinary ancestors, but cannot hide an
     * installed contract inherited through a real base. The physical layout
     * and inherited finality must remain represented by the actual MRO. */
    PyObject *bases = lookup_tp_bases(type);
    for (Py_ssize_t i = 0; bases != NULL && i < PyTuple_GET_SIZE(bases); ++i) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(bases, i);
        if (soac_type_contract(base) != NULL) {
            has_contract = 1;
            if (!soac_mro_contains_type(new_mro, base)) {
                goto hidden;
            }
        }
        PyObject *base_mro = lookup_tp_mro(base);
        for (Py_ssize_t j = 0; base_mro != NULL && j < PyTuple_GET_SIZE(base_mro); ++j) {
            PyTypeObject *ancestor = (PyTypeObject *)PyTuple_GET_ITEM(base_mro, j);
            if (soac_type_contract(ancestor) != NULL) {
                has_contract = 1;
                if (!soac_mro_contains_type(new_mro, ancestor)) {
                    goto hidden;
                }
            }
        }
    }
    if (has_contract && PyTuple_GET_ITEM(new_mro, 0) != (PyObject *)type) {
        PyErr_SetString(soac_type_mutation_error(),
                        "strict inheritance requires the actual class first in its MRO");
        return -1;
    }
    return 0;

hidden:
    PyErr_SetString(soac_type_mutation_error(),
                    "custom MRO cannot hide an installed strict base contract");
    return -1;
}

static int
soac_inherited_final_dictionary_policy(PyObject *owner, PyObject *dict,
                                      PyObject *key, PyObject *value,
                                      int operation, PyObject *provenance)
{
    if (operation == PyDict_SOAC_VALIDATE_INITIAL || operation == PyDict_SOAC_TERMINAL_TEARDOWN ||
        operation == PyDict_SOAC_CLEAR) {
        return 0;
    }
    if (provenance != NULL || key == NULL) {
        PyErr_SetString(soac_type_mutation_error(), "invalid inherited strict finality mutation");
        return -1;
    }
    /* An immutable set of final names is sufficient. It does not own a child
     * type, grant receiver/layout authority, or freeze unrelated bindings. */
    int final = soac_names_contains(owner, key);
    if (final != 0) {
        if (final > 0) {
            PyErr_Format(soac_type_mutation_error(), "cannot override strict final method %R", key);
        }
        return -1;
    }
    return 0;
}

int
_PySOAC_ReadyTypeInheritance(PyTypeObject *type)
{
    PyObject *mro = lookup_tp_mro(type);
    if (mro == NULL) {
        return 0;
    }
    PyObject *inherited_finals = NULL;
    for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base == NULL) {
            continue;
        }
        if (soac_contract_live(base) < 0) {
            Py_XDECREF(inherited_finals);
            return -1;
        }
        if (base->flags & Py_SOAC_TYPE_FINAL) {
            PyErr_SetString(soac_type_mutation_error(), "cannot subclass a runtime-enforced strict final class");
            Py_XDECREF(inherited_finals);
            return -1;
        }
        Py_ssize_t position = 0;
        PyObject *name, *value;
        while (PyDict_Next(type->tp_dict, &position, &name, &value)) {
            if (soac_final_override(type, name) < 0) {
                Py_XDECREF(inherited_finals);
                return -1;
            }
        }
        if (soac_type_contract(type) == NULL && PySet_GET_SIZE(base->final_methods) != 0) {
            if (inherited_finals == NULL) {
                inherited_finals = PySet_New(base->final_methods);
            }
            else if (_PySet_Update(inherited_finals, base->final_methods) < 0) {
                Py_CLEAR(inherited_finals);
            }
            if (inherited_finals == NULL) {
                return -1;
            }
        }
    }
    if (inherited_finals != NULL) {
        PyObject *names = PyFrozenSet_New(inherited_finals);
        Py_DECREF(inherited_finals);
        if (names == NULL) {
            return -1;
        }
        if (!PyDict_HasSoacPolicy(type->tp_dict) &&
            PyDict_SetSoacPolicy(type->tp_dict, names, soac_inherited_final_dictionary_policy, 0) < 0) {
            Py_DECREF(names);
            return -1;
        }
        Py_DECREF(names);
    }
    return 0;
}

int
_PySOAC_TypeContractTraverse(PyTypeObject *type, visitproc visit, void *arg)
{
    Py_VISIT(((PyHeapTypeObject *)type)->ht_soac_contract);
    return 0;
}

void
_PySOAC_TypeContractBeginTeardown(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (state != NULL) {
        /* Invalidate before callbacks; preserve the existing positions of
         * all decrefs so this does not reorder user-visible finalizers. */
        state->terminal = 1;
        state->type = NULL;
    }
}

void
_PySOAC_TypeContractClear(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_contract(type);
    if (state != NULL) {
        soac_state_clear(state);
    }
}

void
_PySOAC_TypeContractDealloc(PyTypeObject *type)
{
    _PySOAC_TypeContractBeginTeardown(type);
    Py_CLEAR(((PyHeapTypeObject *)type)->ht_soac_contract);
}

PyObject *
_PySOAC_NewInstanceDictionary(PyObject *object)
{
    SoacTypeContract *state = soac_type_dictionary_contract(Py_TYPE(object));
    if (state == NULL) {
        return PyDict_New();
    }
    if (soac_contract_live(state) < 0) {
        return NULL;
    }
    return state->new_instance_dict(state->owner, object);
}
