/* Native SOAC type state. Included by typeobject.c after type_new_ctx so the
 * explicit constructor follows exactly the ordinary builtin type pipeline. */

typedef struct SoacTypeContract SoacTypeContract;
typedef struct SoacConstructionHandle SoacConstructionHandle;

enum {
    SOAC_LINEAGE_OPEN = 0,
    SOAC_LINEAGE_ADMITTING,
    SOAC_LINEAGE_RESOLVED,
    SOAC_LINEAGE_FAILED,
};

struct SoacConstructionHandle {
    PyObject_HEAD
    PyInterpreterState *interpreter;
    PySoacTypeConstructionSpec spec;
    int consumed;
    int dataclass_slots;
    /* Only the private native __build_class__ consume interval may borrow
     * these inputs from its actual C/argument owners. Never a public mode. */
    unsigned int borrowed_inputs;
    /* The reserved sidecar has no backedge until it MOVE-attaches to the new
     * type. No allocation is allowed between type allocation and attachment. */
    SoacTypeContract *reserved_state;
    SoacConstructionHandle *root;  /* owned metadata; NULL means this is root */
    unsigned int lineage_phase;   /* authoritative only on canonical root */
    unsigned int active_constructions;
    unsigned int active_adapters;
    SoacTypeContract *selected;    /* comparison only, never dereferenced */
};

enum {
    SOAC_HANDLE_BORROWS_FUNCTION = 1u,
    SOAC_HANDLE_BORROWS_NAME = 2u,
    SOAC_HANDLE_BORROWS_BASES = 4u,
    SOAC_HANDLE_BORROWS_NAMESPACE = 8u,
    SOAC_HANDLE_BORROWS_KEYWORDS = 16u,
};

typedef struct {
    /* Comparison-only native definition. The live type/base chain owns it. */
    const PyMemberDef *member;
    PyTypeObject *declaring_type;
    Py_ssize_t offset;
} SoacObjectSlot;

struct SoacTypeContract {
    PyObject_HEAD
    PyInterpreterState *interpreter;
    PyObject *owner;
    PyObject *fields;
    PyObject *object_slot_fields;
    SoacObjectSlot *object_slots;
    Py_ssize_t object_slot_count;
    PyObject *protected_names;
    PyObject *final_methods;
    /* Non-owning: the type owns this contract. Escaping its dictionary must
     * not create a reverse lifetime edge through the dictionary policy. The
     * type clears this address before its deallocation callbacks or reference
     * release; terminal contracts never dereference it. */
    PyTypeObject *type;
    PyObject *annotation_provider;
    /* Comparison-only during initial dictionary validation. The explicit
     * replacement call independently pins the original class, whose installed
     * frozen hooks cannot change. Cleared before Ready or any failure release. */
    PyObject *initial_frozen_setattr;
    PyObject *initial_frozen_delattr;
    uint32_t flags;
    uint32_t dictionary_mode;
    int sealed;
    int terminal;
    unsigned int phase;
    unsigned int permanent_contract_published;
    SoacConstructionHandle *construction; /* owned metadata, not a type pin */
    PyObject *inherited_final_names;  /* immutable inherited-only dict policy */
    int class_dictionary_policy_installed;
    PySoacTypeFinalCommitV4 commit_final; /* immutable construction authority */
    int (*check_instance_write)(PyObject *, PyObject *, PyObject *, PyObject *);
    PyObject *(*new_instance_dict)(PyObject *, PyObject *);
    int (*prepare_instance_dictionary_policy)(
        PyObject *, PyObject *, PyObject *,
        const PySoacInstanceDictPolicy *, PySoacInstanceDictPolicy *);
};

static PyTypeObject SoacConstructionHandle_Type;
static PyTypeObject SoacTypeContract_Type;
static PyObject *type_new_impl(type_new_ctx *ctx);
static int type_new_get_bases(type_new_ctx *ctx, PyObject **type);
static int soac_final_override(PyTypeObject *, PyObject *);

static PyObject *
soac_type_mutation_error(void)
{
    PyObject *error = PySoac_GetStrictMutationError();
    return error != NULL ? error : PyExc_TypeError;
}

static SoacTypeContract *
soac_type_state(PyTypeObject *type)
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) return NULL;
    return (SoacTypeContract *)((PyHeapTypeObject *)type)->ht_soac_contract;
}

static SoacTypeContract *
soac_type_contract(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    return state != NULL && state->permanent_contract_published ? state : NULL;
}

static SoacConstructionHandle *
soac_construction_root(SoacConstructionHandle *handle)
{
    return handle->root == NULL ? handle : handle->root;
}

static unsigned int
soac_type_phase(SoacTypeContract *state)
{
    if (state->phase == Py_SOAC_TYPE_STATE_DYNAMIC) return state->phase;
    if (state->terminal) return Py_SOAC_TYPE_STATE_FAILED;
    if (state->construction != NULL &&
        (state->phase == Py_SOAC_TYPE_STATE_PENDING ||
         state->phase == Py_SOAC_TYPE_STATE_ADMITTING) &&
        soac_construction_root(state->construction)->lineage_phase == SOAC_LINEAGE_FAILED) {
        return Py_SOAC_TYPE_STATE_FAILED;
    }
    return state->phase;
}

static int
soac_type_state_error(const char *message)
{
    if (!PyErr_Occurred()) PyErr_SetString(soac_type_mutation_error(), message);
    return -1;
}

static void
soac_fail_construction(SoacConstructionHandle *handle)
{
    if (handle != NULL &&
        handle->spec.construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING) {
        SoacConstructionHandle *root = soac_construction_root(handle);
        if (root->lineage_phase != SOAC_LINEAGE_RESOLVED) {
            root->lineage_phase = SOAC_LINEAGE_FAILED;
        }
    }
}

static int
soac_pending_constructor_live(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state == NULL || state->construction == NULL ||
        state->phase == Py_SOAC_TYPE_STATE_DYNAMIC) return 0;
    SoacConstructionHandle *root = soac_construction_root(state->construction);
    if (soac_type_phase(state) != Py_SOAC_TYPE_STATE_PENDING ||
        root->lineage_phase != SOAC_LINEAGE_OPEN ||
        root->active_constructions == 0 || state->type != type ||
        state->owner == NULL || state->interpreter != _PyInterpreterState_GET()) {
        soac_fail_construction(state->construction);
        return soac_type_state_error("pending native construction was invalidated");
    }
    return 0;
}

/* A pending/failed type is denied before any allocation or header write.
 * Actual MRO and solid-base links also cover inherited C allocation paths.
 * Neither a public function pointer nor source metadata grants admission. */
static int
soac_type_allocation_barrier(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state == NULL || soac_type_phase(state) == Py_SOAC_TYPE_STATE_DYNAMIC) return 0;
    if (state->interpreter != _PyInterpreterState_GET() ||
        soac_type_phase(state) != Py_SOAC_TYPE_STATE_ENFORCED) {
        return soac_type_state_error("instances of a pending or failed strict type are unavailable");
    }
    return 0;
}

int
_PySOAC_CheckTypeAllocation(PyTypeObject *type)
{
    if (soac_type_allocation_barrier(type) < 0) return -1;
    PyObject *mro = type->tp_mro;
    for (Py_ssize_t i = 1; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        if (soac_type_allocation_barrier((PyTypeObject *)PyTuple_GET_ITEM(mro, i)) < 0) return -1;
    }
    for (PyTypeObject *base = type->tp_base; base != NULL; base = base->tp_base) {
        if (soac_type_allocation_barrier(base) < 0) return -1;
    }
    return 0;
}

int
_PySOAC_CheckPendingBases(PyObject *bases)
{
    if (bases != NULL && PyTuple_Check(bases)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); ++i) {
            PyObject *base = PyTuple_GET_ITEM(bases, i);
            if (PyType_Check(base) && _PySOAC_CheckTypeAllocation((PyTypeObject *)base) < 0) return -1;
        }
    }
    return 0;
}

int
_PySOAC_CheckPendingTypeWrite(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL) {
        unsigned int phase = soac_type_phase(state);
        if (phase == Py_SOAC_TYPE_STATE_ADMITTING || phase == Py_SOAC_TYPE_STATE_FAILED) {
            return soac_type_state_error("pending type admission or failure forbids mutation");
        }
    }
    return 0;
}

void
_PySOAC_FailPendingType(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL && state->construction != NULL &&
        state->phase != Py_SOAC_TYPE_STATE_ENFORCED &&
        state->phase != Py_SOAC_TYPE_STATE_DYNAMIC) {
        soac_fail_construction(state->construction);
        state->phase = Py_SOAC_TYPE_STATE_FAILED;
    }
}

PyObject *
_PySOAC_GetTypeConstructionOwner(PyObject *object)
{
    if (object == NULL || !PyType_Check(object)) return NULL;
    SoacTypeContract *state = soac_type_state((PyTypeObject *)object);
    if (state == NULL || soac_type_phase(state) == Py_SOAC_TYPE_STATE_DYNAMIC) return NULL;
    if (state->owner == NULL || state->terminal ||
        soac_type_phase(state) == Py_SOAC_TYPE_STATE_FAILED ||
        state->interpreter != _PyInterpreterState_GET()) {
        soac_type_state_error("native type construction owner is unavailable");
        return NULL;
    }
    return state->owner;
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
    if (own != NULL && own->dictionary_mode == Py_SOAC_INSTANCE_DICT_INDEXED) {
        return own;
    }
    PyObject *mro = type->tp_mro;
    for (Py_ssize_t i = 1; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base != NULL && base->dictionary_mode == Py_SOAC_INSTANCE_DICT_INDEXED) {
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

#include "soac_slots.inc"

static int
soac_handle_traverse(SoacConstructionHandle *handle, visitproc visit, void *arg)
{
    Py_VISIT(handle->reserved_state);
    Py_VISIT(handle->root);
    Py_VISIT(handle->spec.owner);
    if (!(handle->borrowed_inputs & SOAC_HANDLE_BORROWS_FUNCTION)) {
        Py_VISIT(handle->spec.namespace_function);
    }
    if (!(handle->borrowed_inputs & SOAC_HANDLE_BORROWS_NAME)) {
        Py_VISIT(handle->spec.name);
    }
    if (!(handle->borrowed_inputs & SOAC_HANDLE_BORROWS_BASES)) {
        Py_VISIT(handle->spec.bases);
    }
    if (!(handle->borrowed_inputs & SOAC_HANDLE_BORROWS_NAMESPACE)) {
        Py_VISIT(handle->spec.namespace_dict);
    }
    if (!(handle->borrowed_inputs & SOAC_HANDLE_BORROWS_KEYWORDS)) {
        Py_VISIT(handle->spec.keywords);
    }
    Py_VISIT(handle->spec.contract.fields);
    Py_VISIT(handle->spec.contract.object_slot_fields);
    Py_VISIT(handle->spec.contract.protected_names);
    Py_VISIT(handle->spec.contract.final_methods);
    return 0;
}

/* Unpublish all C-supported views before any metadata release can reenter.
 * This does not own/close those inputs. All other spec fields retain their
 * existing owning GC/clear semantics, including an owned empty keyword dict. */
static void
soac_handle_unpublish_borrowed_inputs(SoacConstructionHandle *handle)
{
    unsigned int borrowed = handle->borrowed_inputs;
    handle->borrowed_inputs = 0;
    if (borrowed & SOAC_HANDLE_BORROWS_FUNCTION) handle->spec.namespace_function = NULL;
    if (borrowed & SOAC_HANDLE_BORROWS_NAME) handle->spec.name = NULL;
    if (borrowed & SOAC_HANDLE_BORROWS_BASES) handle->spec.bases = NULL;
    if (borrowed & SOAC_HANDLE_BORROWS_NAMESPACE) handle->spec.namespace_dict = NULL;
    if (borrowed & SOAC_HANDLE_BORROWS_KEYWORDS) handle->spec.keywords = NULL;
}

static int
soac_handle_clear(SoacConstructionHandle *handle)
{
    soac_fail_construction(handle);
    handle->consumed = 1;
    handle->selected = NULL;
    soac_handle_unpublish_borrowed_inputs(handle);
    Py_CLEAR(handle->spec.owner);
    Py_CLEAR(handle->spec.namespace_function);
    Py_CLEAR(handle->spec.name);
    Py_CLEAR(handle->spec.bases);
    Py_CLEAR(handle->spec.namespace_dict);
    Py_CLEAR(handle->spec.keywords);
    Py_CLEAR(handle->spec.contract.fields);
    Py_CLEAR(handle->spec.contract.object_slot_fields);
    Py_CLEAR(handle->spec.contract.protected_names);
    Py_CLEAR(handle->spec.contract.final_methods);
    Py_CLEAR(handle->reserved_state);
    Py_CLEAR(handle->root);
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
    Py_VISIT(state->object_slot_fields);
    Py_VISIT(state->protected_names);
    Py_VISIT(state->final_methods);
    Py_VISIT(state->annotation_provider);
    Py_VISIT(state->construction);
    Py_VISIT(state->inherited_final_names);
    return 0;
}

static int
soac_state_clear(SoacTypeContract *state)
{
    /* Terminal GC cleanup is not unsealing. Any resurrection still sees the
     * permanent terminal marker and cannot resume optimized or ordinary use. */
    state->terminal = 1;
    if (state->phase != Py_SOAC_TYPE_STATE_DYNAMIC) {
        state->phase = Py_SOAC_TYPE_STATE_FAILED;
    }
    state->type = NULL;
    state->initial_frozen_setattr = NULL;
    state->initial_frozen_delattr = NULL;
    Py_CLEAR(state->owner);
    Py_CLEAR(state->annotation_provider);
    Py_CLEAR(state->fields);
    Py_CLEAR(state->object_slot_fields);
    Py_CLEAR(state->protected_names);
    Py_CLEAR(state->final_methods);
    Py_CLEAR(state->construction);
    /* An escaped inherited-only namespace keeps its immutable final names
     * after type death, without a class/owner backedge. */
    if (state->permanent_contract_published) Py_CLEAR(state->inherited_final_names);
    return 0;
}

static void
soac_state_dealloc(SoacTypeContract *state)
{
    PyObject_GC_UnTrack(state);
    soac_state_clear(state);
    Py_CLEAR(state->inherited_final_names);
    PyMem_Free(state->object_slots);
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

static SoacTypeContract *
soac_new_type_state(PyObject *owner, unsigned int phase)
{
    SoacTypeContract *state = PyObject_GC_New(SoacTypeContract, &SoacTypeContract_Type);
    if (state == NULL) return NULL;
    memset((char *)state + sizeof(PyObject), 0, sizeof(*state) - sizeof(PyObject));
    state->interpreter = _PyInterpreterState_GET();
    state->owner = Py_NewRef(owner);
    state->phase = phase;
    return state;
}

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

static int
soac_valid_dictionary_spec(const PySoacTypeContractSpecV4 *spec)
{
    switch (spec->dictionary_mode) {
        case Py_SOAC_INSTANCE_DICT_NONE:
            return spec->new_instance_dict == NULL &&
                spec->prepare_instance_dictionary_policy == NULL;
        case Py_SOAC_INSTANCE_DICT_INDEXED:
            return spec->new_instance_dict != NULL &&
                spec->prepare_instance_dictionary_policy == NULL;
        case Py_SOAC_INSTANCE_DICT_ORDINARY:
            return spec->new_instance_dict == NULL &&
                spec->prepare_instance_dictionary_policy != NULL &&
                spec->check_instance_write != NULL;
        default:
            return 0;
    }
}

static int
soac_valid_contract_spec(const PySoacTypeContractSpecV4 *spec)
{
    return spec != NULL && !(spec->flags & ~Py_SOAC_TYPE_FINAL) &&
        soac_valid_dictionary_spec(spec) && spec->fields != NULL && soac_valid_names(spec->fields) &&
        spec->protected_names != NULL && soac_valid_names(spec->protected_names) &&
        spec->final_methods != NULL && soac_valid_names(spec->final_methods) &&
        (spec->object_slot_fields == NULL || soac_valid_names(spec->object_slot_fields));
}

static int
soac_empty_contract_spec(const PySoacTypeContractSpecV4 *spec)
{
    return spec->flags == 0 && spec->dictionary_mode == Py_SOAC_INSTANCE_DICT_NONE &&
        spec->fields == NULL &&
        spec->protected_names == NULL && spec->final_methods == NULL &&
        spec->object_slot_fields == NULL && spec->check_instance_write == NULL &&
        spec->new_instance_dict == NULL && spec->prepare_instance_dictionary_policy == NULL;
}

static PyObject *
soac_new_construction_handle(const PySoacTypeConstructionSpec *spec,
                             _PySoacDataclassSlotsContext *slots_context)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_RuntimeError, "strict type construction does not yet support free-threaded CPython");
    return NULL;
#else
    int dataclass_slots = slots_context != NULL;
    if (spec == NULL || spec->abi_version != Py_SOAC_TYPE_CONTRACT_ABI ||
        spec->struct_size != sizeof(*spec) || spec->reserved != 0 ||
        spec->construction_mode > Py_SOAC_TYPE_CONSTRUCT_PENDING ||
        spec->owner == NULL ||
        (dataclass_slots ? spec->namespace_function != NULL
                         : (spec->namespace_function == NULL ||
                            !PyFunction_Check(spec->namespace_function))) ||
        spec->name == NULL || !PyUnicode_CheckExact(spec->name) ||
        spec->bases == NULL || !PyTuple_CheckExact(spec->bases) ||
        spec->namespace_dict == NULL || !PyDict_CheckExact(spec->namespace_dict) ||
        spec->keywords == NULL || !PyDict_CheckExact(spec->keywords) ||
        (spec->construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING
            ? (spec->commit_final == NULL || !soac_empty_contract_spec(&spec->contract))
            : (spec->commit_final != NULL || !soac_valid_contract_spec(&spec->contract)))) {
        PyErr_SetString(soac_type_mutation_error(), "invalid native strict class construction plan");
        return NULL;
    }
    /* Keep the validated scalar callback/mode/operand view fixed through any
     * preparation callbacks. The caller continues to own its actual operands. */
    PySoacTypeConstructionSpec captured = *spec;
    spec = &captured;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(spec->bases); ++i) {
        PyObject *base = PyTuple_GET_ITEM(spec->bases, i);
        if (!PyType_Check(base) || Py_TYPE(base) != &PyType_Type) {
            PyErr_SetString(soac_type_mutation_error(), "strict type constructor requires verified builtin-metaclass bases");
            return NULL;
        }
        if (_PySOAC_CheckTypeAllocation((PyTypeObject *)base) < 0) return NULL;
        if (spec->contract.new_instance_dict != NULL &&
            ((PyTypeObject *)base)->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
            PyErr_SetString(soac_type_mutation_error(),
                "strict dictionary construction cannot replace an inherited inline-values layout");
            return NULL;
        }
    }
    PyObject *unsupported_hooks[] = {
        &_Py_ID(__getattribute__), &_Py_ID(__getattr__),
        &_Py_ID(__setattr__), &_Py_ID(__delattr__), NULL,
    };
    for (PyObject **hook = unsupported_hooks;
         spec->construction_mode == Py_SOAC_TYPE_CONSTRUCT_ENFORCED && *hook != NULL;
         ++hook) {
        PyObject *value = PyDict_GetItemWithError(spec->namespace_dict, *hook);
        if (value != NULL) {
            if (slots_context == NULL) {
                PyErr_SetString(soac_type_mutation_error(), "custom attribute hooks require a verified adapter or dynamic construction");
                return NULL;
            }
            if (_PySOAC_DataclassValidateCopiedHook(slots_context, *hook, value) < 0) {
                return NULL;
            }
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
    handle->dataclass_slots = dataclass_slots;
    handle->borrowed_inputs = 0;
    handle->reserved_state = NULL;
    handle->root = NULL;
    handle->lineage_phase = SOAC_LINEAGE_OPEN;
    handle->active_constructions = 0;
    handle->active_adapters = 0;
    handle->selected = NULL;
    Py_INCREF(spec->owner);
    Py_XINCREF(spec->namespace_function);
    Py_INCREF(spec->name);
    Py_INCREF(spec->bases);
    Py_INCREF(spec->namespace_dict);
    Py_INCREF(spec->keywords);
    Py_XINCREF(spec->contract.fields);
    Py_XINCREF(spec->contract.object_slot_fields);
    Py_XINCREF(spec->contract.protected_names);
    Py_XINCREF(spec->contract.final_methods);
    if (spec->construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING) {
        handle->reserved_state = soac_new_type_state(spec->owner, Py_SOAC_TYPE_STATE_PENDING);
        if (handle->reserved_state == NULL) {
            Py_DECREF(handle);
            return NULL;
        }
        handle->reserved_state->commit_final = captured.commit_final;
        PyObject_GC_Track(handle->reserved_state);
    }
    PyObject_GC_Track(handle);
    return (PyObject *)handle;
#endif
}

PyObject *
PyType_NewSoacConstructionHandle(const PySoacTypeConstructionSpec *spec)
{
    return soac_new_construction_handle(spec, NULL);
}

PyObject *
PyType_NewSoacDataclassSlotsHandle(const PySoacDataclassFrameView *producer,
                                   const PySoacTypeConstructionSpec *spec)
{
    _PySoacDataclassSlotsContext *context =
        _PySOAC_DataclassBeginSlotsHandle(producer, spec);
    if (context == NULL) {
        return NULL;
    }
    PyObject *handle = soac_new_construction_handle(spec, context);
    if (handle == NULL ||
        _PySOAC_DataclassRecordSlotsHandle(context, handle,
                                          &((SoacConstructionHandle *)handle)->spec) < 0) {
        _PySOAC_DataclassFailSlots(context, "replacement handle preparation failed");
        if (handle != NULL) {
            _PySOAC_ClearDataclassSlotsHandle(handle);
        }
        Py_XDECREF(handle);
        return NULL;
    }
    return handle;
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
soac_check_class_write_state(PyTypeObject *type, SoacTypeContract *state,
                             PyObject *name, PyObject *value,
                             PyObject *member_operation)
{
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
        if (soac_object_slot_name_index(state, name) >= 0) {
            PyErr_SetString(soac_type_mutation_error(),
                            "cannot change an installed strict object-slot descriptor");
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

static int
soac_check_class_write(PyTypeObject *type, PyObject *name, PyObject *value,
                       PyObject *member_operation)
{
    if (_PySOAC_CheckPendingTypeWrite(type) < 0) return -1;
    SoacTypeContract *state = soac_type_contract(type);
    if (member_operation != NULL && state == NULL) state = soac_type_state(type);
    /* Pending generated-member authorization is the same exact operation;
     * it does not publish a speculative permanent namespace/instance policy. */
    if (state != NULL && !state->permanent_contract_published) {
        if (member_operation != NULL &&
            _PySOAC_DataclassCheckMember(member_operation, (PyObject *)type,
                                          state->owner, name, value) < 0) return -1;
        return soac_final_override(type, name);
    }
    return soac_check_class_write_state(type, state, name, value, member_operation);
}

static int soac_inherited_final_dictionary_policy(
    PyObject *, PyObject *, PyObject *, PyObject *, int, PyObject *);

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
    if (!state->permanent_contract_published &&
        state->phase != Py_SOAC_TYPE_STATE_ADMITTING &&
        state->inherited_final_names != NULL) {
        /* This is the actual inherited-only policy installed by Ready, not a
         * speculative own contract. Dynamic disposition/type death preserves
         * its original final-name restrictions and unrelated mutable bindings. */
        return soac_inherited_final_dictionary_policy(state->inherited_final_names,
            dict, key, value, operation, provenance);
    }
    if (operation == PyDict_SOAC_TERMINAL_TEARDOWN) {
        state->terminal = 1;
        state->type = NULL;
        state->initial_frozen_setattr = NULL;
        state->initial_frozen_delattr = NULL;
        return 0;
    }
    if (operation == PyDict_SOAC_VALIDATE_INITIAL) {
        /* Recheck the actual copied namespace at installation, not just the
         * earlier input dict: allocations may have run unrelated GC callbacks. */
        if (soac_object_slot_name_index(state, key) >= 0) {
            return soac_validate_existing_slot_descriptor(state, key, value);
        }
        if (!state->sealed && value != NULL && key != NULL &&
            PyUnicode_CheckExact(key) &&
            ((value == state->initial_frozen_setattr &&
              _PyUnicode_Equal(key, &_Py_ID(__setattr__))) ||
             (value == state->initial_frozen_delattr &&
              _PyUnicode_Equal(key, &_Py_ID(__delattr__))))) {
            if (soac_contract_live(state) < 0) {
                return -1;
            }
            return soac_final_override(state->type, key);
        }
        return soac_check_class_write_state(state->type, state, key, value, NULL);
    }
    if (soac_contract_live(state) < 0 ||
        (state->type != NULL && _PySOAC_CheckPendingTypeWrite(state->type) < 0)) {
        return -1;
    }
    if (operation == PyDict_SOAC_SLOT_DESCRIPTOR_INSERT ||
        operation == PyDict_SOAC_SLOT_DESCRIPTOR_REPLACE) {
        return soac_validate_slot_publication(state, dict, key, value, provenance);
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

/* The same immutable policy preparation serves legacy construction and
 * pending final admission. Its temporary, untracked result is not attached to
 * a second type slot; pending admission MOVE-transfers its policy payload to
 * the one previously published state. No execution primary is added. */
static SoacTypeContract *
soac_prepare_type_contract(PyTypeObject *type, PyObject *owner,
                           const PySoacTypeContractSpecV4 *spec)
{
    SoacTypeContract *state = soac_new_type_state(owner, Py_SOAC_TYPE_STATE_ENFORCED);
    if (state == NULL) return NULL;
    state->fields = Py_NewRef(spec->fields);
    state->object_slot_fields = Py_XNewRef(spec->object_slot_fields);
    state->object_slots = NULL;
    state->object_slot_count = 0;
    state->protected_names = PySet_New(spec->protected_names);
    state->final_methods = PySet_New(spec->final_methods);
    state->flags = spec->flags;
    state->dictionary_mode = spec->dictionary_mode;
    state->prepare_instance_dictionary_policy = spec->prepare_instance_dictionary_policy;
    state->sealed = 0;
    state->terminal = 0;
    state->check_instance_write = spec->check_instance_write;
    state->new_instance_dict = spec->new_instance_dict;
    if (state->protected_names == NULL || state->final_methods == NULL) {
        Py_DECREF(state);
        return NULL;
    }
    if (spec->dictionary_mode == Py_SOAC_INSTANCE_DICT_ORDINARY &&
        ((type->tp_dictoffset == 0 && !(type->tp_flags & Py_TPFLAGS_MANAGED_DICT)) ||
         _PySOAC_UsesInstanceDictionaryPolicy(type))) {
        soac_type_state_error("ordinary field policy conflicts with the actual dictionary layout");
        Py_DECREF(state);
        return NULL;
    }
    if (soac_build_object_slot_catalog(state, type) < 0) {
        Py_DECREF(state);
        return NULL;
    }
    PyObject *bases = type->tp_bases;
    for (Py_ssize_t i = 0; bases != NULL && i < PyTuple_GET_SIZE(bases); ++i) {
        PyTypeObject *base_type = (PyTypeObject *)PyTuple_GET_ITEM(bases, i);
        SoacTypeContract *base = soac_type_contract(base_type);
        if (base == NULL) {
            if (base_type != &PyBaseObject_Type) {
                Py_DECREF(state);
                PyErr_SetString(soac_type_mutation_error(), "strict construction requires protected bases; classify unsupported bases before construction");
                return NULL;
            }
            continue;
        }
        if (soac_contract_live(base) < 0 || _PySet_Update(state->protected_names, base->protected_names) < 0 ||
            _PySet_Update(state->final_methods, base->final_methods) < 0) {
            Py_DECREF(state);
            return NULL;
        }
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(spec->fields); ++i) {
        PyObject *name = PyTuple_GET_ITEM(spec->fields, i);
        if (PySet_Contains(state->final_methods, name) > 0) {
            PyErr_Format(soac_type_mutation_error(), "field cannot replace strict final method %R", name);
            Py_DECREF(state);
            return NULL;
        }
        if (PySet_Discard(state->protected_names, name) < 0) {
            Py_DECREF(state);
            return NULL;
        }
    }
    for (Py_ssize_t i = 0; i < state->object_slot_count; ++i) {
        PyObject *name = PyTuple_GET_ITEM(state->object_slot_fields, i);
        if (PySet_Contains(state->final_methods, name) > 0) {
            PyErr_Format(soac_type_mutation_error(),
                         "object slot cannot replace strict final method %R", name);
            Py_DECREF(state);
            return NULL;
        }
        if (PySet_Discard(state->protected_names, name) < 0) {
            Py_DECREF(state);
            return NULL;
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
        return NULL;
    }
    Py_SETREF(state->protected_names, protected_names);
    Py_SETREF(state->final_methods, final_methods);
    return state;
}

static int
soac_install_type_contract(PyTypeObject *type, SoacConstructionHandle *handle,
                           _PySoacDataclassSlotsContext *slots_context)
{
    if (handle->spec.construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING) {
        return soac_pending_constructor_live(type);
    }
    PySoacTypeConstructionSpec *spec = &handle->spec;
    SoacTypeContract *state = soac_prepare_type_contract(type, spec->owner, &spec->contract);
    if (state == NULL) return -1;
    state->permanent_contract_published = 1;
    state->type = type;
    ((PyHeapTypeObject *)type)->ht_soac_contract = (PyObject *)state;
    type->tp_flags |= Py_TPFLAGS_SOAC_CONTRACT;
    PyObject_GC_Track(state);
    if (slots_context != NULL &&
        _PySOAC_DataclassBindSlotsType(slots_context, (PyObject *)type, spec->owner) < 0) {
        goto failed_binding;
    }
    if (spec->bind_type != NULL && spec->bind_type(spec->owner, (PyObject *)type) < 0) {
        if (!PyErr_Occurred()) {
            PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
            if (error != NULL) {
                PyErr_SetString(error, "strict type owner rejected actual type binding");
            }
        }
        goto failed_binding;
    }
    if (slots_context != NULL &&
        _PySOAC_DataclassBindSlotsType(slots_context, (PyObject *)type, spec->owner) < 0) {
        goto failed_binding;
    }
    if (slots_context != NULL) {
        PyObject *hooks[] = {&_Py_ID(__setattr__), &_Py_ID(__delattr__)};
        PyObject **slots[] = {&state->initial_frozen_setattr,
                             &state->initial_frozen_delattr};
        for (size_t i = 0; i < Py_ARRAY_LENGTH(hooks); ++i) {
            PyObject *value = PyDict_GetItemWithError(type->tp_dict, hooks[i]);
            if ((value == NULL && PyErr_Occurred()) ||
                (value != NULL &&
                 _PySOAC_DataclassValidateCopiedHook(slots_context, hooks[i], value) < 0)) {
                goto failed_binding;
            }
            *slots[i] = value;
        }
    }
    int policy_status = PyDict_SetSoacPolicy(type->tp_dict, (PyObject *)state,
                                            soac_class_dictionary_policy, 0);
    state->initial_frozen_setattr = NULL;
    state->initial_frozen_delattr = NULL;
    if (policy_status == 0) state->class_dictionary_policy_installed = 1;
    if (policy_status < 0 ||
        (slots_context != NULL &&
         _PySOAC_DataclassBindSlotsType(slots_context, (PyObject *)type, spec->owner) < 0)) {
        goto failed_binding;
    }
    return 0;

failed_binding:
    state->terminal = 1;
    state->initial_frozen_setattr = NULL;
    state->initial_frozen_delattr = NULL;
    if (slots_context != NULL) {
        _PySOAC_DataclassFailSlots(slots_context,
                                   "replacement contract failed before Ready");
    }
    return -1;
}

static void
soac_retire_pending_handle_inputs(SoacConstructionHandle *handle)
{
    if (handle->spec.construction_mode != Py_SOAC_TYPE_CONSTRUCT_PENDING) return;
    PyObject *error = PyErr_GetRaisedException();
    soac_handle_unpublish_borrowed_inputs(handle);
    Py_CLEAR(handle->spec.owner);
    Py_CLEAR(handle->spec.namespace_function);
    Py_CLEAR(handle->spec.name);
    Py_CLEAR(handle->spec.bases);
    Py_CLEAR(handle->spec.namespace_dict);
    Py_CLEAR(handle->spec.keywords);
    Py_CLEAR(handle->reserved_state);
    PyErr_SetRaisedException(error);
}

/* Reserve before allocation, then move through a C local. A collecting type
 * allocator may fail the lineage but cannot release this future barrier. */
static SoacTypeContract *
soac_take_pending_state(SoacConstructionHandle *handle)
{
    if (handle == NULL ||
        handle->spec.construction_mode != Py_SOAC_TYPE_CONSTRUCT_PENDING) return NULL;
    SoacTypeContract *state = handle->reserved_state;
    if (state == NULL || soac_construction_root(handle)->lineage_phase != SOAC_LINEAGE_OPEN) {
        soac_fail_construction(handle);
        soac_type_state_error("pending type sidecar is unavailable before allocation");
        return NULL;
    }
    handle->reserved_state = NULL;
    return state;
}

static void
soac_attach_pending_type(PyTypeObject *type, SoacConstructionHandle *handle,
                         SoacTypeContract *state)
{
    if (state == NULL) return;
    assert(handle != NULL && soac_type_state(type) == NULL);
    /* Even if allocation-time GC failed the lineage, attach FIRST: error
     * construction/release must never expose an unguarded tracked type. */
    state->type = type;
    state->construction = (SoacConstructionHandle *)Py_NewRef(handle);
    ((PyHeapTypeObject *)type)->ht_soac_contract = (PyObject *)state;
}

static int
soac_bind_pending_type(PyTypeObject *type, SoacConstructionHandle *handle,
                       _PySoacDataclassSlotsContext *slots_context)
{
    if (handle == NULL ||
        handle->spec.construction_mode != Py_SOAC_TYPE_CONSTRUCT_PENDING) return 0;
    SoacTypeContract *state = soac_type_state(type);
    PyObject *owner = handle->spec.owner;
    if (state == NULL || state->owner != owner || soac_pending_constructor_live(type) < 0) {
        return soac_type_state_error("pending native owner binding is unavailable");
    }
    if (slots_context != NULL &&
        _PySOAC_DataclassBindSlotsType(slots_context, (PyObject *)type, owner) < 0) {
        soac_fail_construction(handle);
        return -1;
    }
    int status = handle->spec.bind_type == NULL ? 0
        : handle->spec.bind_type(owner, (PyObject *)type);
    /* Pending preparation may reenter. Constructor C owners support actual
     * type/handle, but logical state must be re-read after the callback. */
    if (status != 0 || PyErr_Occurred() || soac_type_state(type) != state ||
        state->owner != owner || state->construction != handle ||
        soac_pending_constructor_live(type) < 0) {
        soac_fail_construction(handle);
        return soac_type_state_error("pending native owner binding failed");
    }
    if (slots_context != NULL &&
        _PySOAC_DataclassBindSlotsType(slots_context, (PyObject *)type, owner) < 0) {
        soac_fail_construction(handle);
        return -1;
    }
    return 0;
}

static PyObject *
soac_type_from_handle(SoacConstructionHandle *handle,
                      _PySoacDataclassSlotsContext *slots_context)
{
    handle->consumed = 1;
    SoacConstructionHandle *root = soac_construction_root(handle);
    int pending = handle->spec.construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING;
    if (pending) {
        if (root->lineage_phase != SOAC_LINEAGE_OPEN ||
            root->active_constructions == UINT_MAX) {
            soac_fail_construction(handle);
            soac_type_state_error("pending construction lineage is unavailable");
            soac_retire_pending_handle_inputs(handle);
            return NULL;
        }
        root->active_constructions++;
    }
    PySoacTypeConstructionSpec *spec = &handle->spec;
    PyObject *args = PyTuple_Pack(3, spec->name, spec->bases, spec->namespace_dict);
    PyObject *result = NULL;
    if (args != NULL) {
        type_new_ctx ctx = {
            .metatype = &PyType_Type, .args = args, .kwds = spec->keywords,
            .orig_dict = spec->namespace_dict, .name = spec->name, .bases = spec->bases,
            .soac_handle = (PyObject *)handle, .soac_dataclass_slots = slots_context,
        };
        int status = type_new_get_bases(&ctx, &result);
        if (status == 0) {
            result = type_new_impl(&ctx);
            if (result == NULL) soac_fail_construction(handle);
            Py_DECREF(ctx.bases);
        }
        else if (status == 1) {
            soac_fail_construction(handle);
            Py_CLEAR(result);
            soac_type_state_error("strict construction selected an unauthenticated metaclass");
        }
        if (result != NULL &&
            (PyType_Type.tp_init(result, args, spec->keywords) < 0 ||
             (pending && soac_pending_constructor_live((PyTypeObject *)result) < 0))) {
            soac_fail_construction(handle);
            Py_CLEAR(result);
        }
    }
    if (result == NULL) soac_fail_construction(handle);
    Py_XDECREF(args);
    if (pending) {
        /* Keep construction active across all input/metadata releases. An
         * input finalizer cannot admit a type before this C operation exits. */
        soac_retire_pending_handle_inputs(handle);
        assert(root->active_constructions > 0);
        root->active_constructions--;
    }
    return result;
}

PyObject *
PyType_FromSoacConstructionHandle(PyObject *object, PyObject *namespace_function)
{
    if (!Py_IS_TYPE(object, &SoacConstructionHandle_Type)) {
        PyErr_SetString(soac_type_mutation_error(), "expected an interpreter-owned strict construction handle");
        return NULL;
    }
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    if (handle->dataclass_slots || handle->consumed ||
        handle->interpreter != _PyInterpreterState_GET() ||
        handle->spec.namespace_function != namespace_function) {
        PyErr_SetString(soac_type_mutation_error(), "strict construction handle was consumed, transferred, or used by the wrong namespace function");
        return NULL;
    }
    return soac_type_from_handle(handle, NULL);
}

/* Same one-use handle and same type_new pipeline as the public constructor.
 * This join is core-private and has one caller: the actual __build_class__
 * operation, whose native argument stack and C locals own every borrowed
 * input through all type callbacks. It does not grant a body or source role.
 */
PyObject *
_PySOAC_TypeFromInterpreterConstructionHandle(
    PyObject *object, PyObject *namespace_function, PyObject *metaclass,
    PyObject *name, PyObject *bases, PyObject *namespace_dict, PyObject *keywords)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacConstructionHandle_Type)) {
        PyErr_SetString(soac_type_mutation_error(),
                        "expected an interpreter-owned strict construction handle");
        return NULL;
    }
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    PySoacTypeConstructionSpec *spec = &handle->spec;
    if (handle->dataclass_slots || handle->consumed || handle->borrowed_inputs ||
        handle->interpreter != _PyInterpreterState_GET() ||
        metaclass != (PyObject *)&PyType_Type ||
        namespace_function == NULL || spec->namespace_function != namespace_function ||
        name == NULL || spec->name != name || bases == NULL || spec->bases != bases ||
        namespace_dict == NULL || spec->namespace_dict != namespace_dict ||
        (keywords != NULL && spec->keywords != keywords) ||
        (keywords == NULL && (spec->keywords == NULL ||
          !PyDict_CheckExact(spec->keywords) || PyDict_GET_SIZE(spec->keywords) != 0))) {
        PyErr_SetString(soac_type_mutation_error(),
                        "strict construction handle does not match the actual native class operation");
        return NULL;
    }
    /* Publish one-use/borrowed mode before retiring duplicates. Each DECREF
     * below has a separate real C/argument owner, so none can finalize an
     * input. The handle remains an owned C root throughout construction. */
    handle->consumed = 1;
    handle->borrowed_inputs =
        SOAC_HANDLE_BORROWS_FUNCTION | SOAC_HANDLE_BORROWS_NAME |
        SOAC_HANDLE_BORROWS_BASES | SOAC_HANDLE_BORROWS_NAMESPACE;
    if (keywords != NULL) {
        handle->borrowed_inputs |= SOAC_HANDLE_BORROWS_KEYWORDS;
    }
    Py_DECREF(namespace_function);
    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(namespace_dict);
    if (keywords != NULL) {
        Py_DECREF(keywords);
    }
    /* No-keyword native calls have no mkw owner to borrow. The handle keeps
     * its existing exact empty keyword dictionary as an OWNED spec field. */
    PyObject *result = soac_type_from_handle(handle, NULL);
    soac_handle_unpublish_borrowed_inputs(handle);
    return result;
}

PyObject *
_PySOAC_TypeFromDataclassSlotsHandle(_PySoacDataclassSlotsContext *context,
                                     PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacConstructionHandle_Type)) {
        _PySOAC_DataclassFailSlots(context, "replacement needs its native construction handle");
        return NULL;
    }
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    if (!handle->dataclass_slots || handle->consumed ||
        handle->interpreter != _PyInterpreterState_GET() ||
        _PySOAC_DataclassValidateSlotsHandle(context, object, &handle->spec) < 0) {
        _PySOAC_DataclassFailSlots(context, "replacement construction handle is unavailable");
        return NULL;
    }
    return soac_type_from_handle(handle, context);
}

void
_PySOAC_ClearDataclassSlotsHandle(PyObject *object)
{
    assert(Py_IS_TYPE(object, &SoacConstructionHandle_Type));
    SoacConstructionHandle *handle = (SoacConstructionHandle *)object;
    assert(handle->dataclass_slots);
    PyObject *exception = PyErr_GetRaisedException();
    if (handle->spec.construction_mode == Py_SOAC_TYPE_CONSTRUCT_PENDING) {
        soac_retire_pending_handle_inputs(handle);
    }
    else {
        soac_handle_clear(handle);
    }
    PyErr_SetRaisedException(exception);
}

#include "soac_type_pending.inc"

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
    /* A selected native slot is validated by actual physical member offset at
     * the final PyMember_SetOne commit, not twice by its lookup spelling. */
    if (state->dictionary_mode != Py_SOAC_INSTANCE_DICT_ORDINARY &&
        state->check_instance_write != NULL &&
        soac_object_slot_name_index(state, name) < 0) {
        return state->check_instance_write(state->owner, object, name, value);
    }
    return 0;
}

int
_PySOAC_CheckClassAssignment(PyTypeObject *old_type, PyTypeObject *new_type)
{
    if (_PySOAC_CheckTypeAllocation(old_type) < 0 ||
        _PySOAC_CheckTypeAllocation(new_type) < 0) return -1;
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
    if (_PySOAC_UsesInstanceDictionaryPolicy(Py_TYPE(object))) {
        PyErr_SetString(soac_type_mutation_error(), "cannot replace a strict instance dictionary");
        return -1;
    }
    return 0;
}

int
_PySOAC_CheckTypeBases(PyTypeObject *type, PyObject *bases)
{
    if (_PySOAC_CheckTypeAllocation(type) < 0 ||
        _PySOAC_CheckPendingBases(bases) < 0) return -1;
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
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(new_mro); ++i) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(new_mro, i);
        if (initial && base == type) continue;
        if (_PySOAC_CheckTypeAllocation(base) < 0) return -1;
    }
    if (_PySOAC_CheckPendingBases(lookup_tp_bases(type)) < 0) return -1;
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
        SoacTypeContract *state = soac_type_state(type);
        if (state != NULL && !state->permanent_contract_published &&
            state->construction != NULL) {
            if (soac_pending_constructor_live(type) < 0 ||
                state->inherited_final_names != NULL || PyDict_HasSoacPolicy(type->tp_dict)) {
                Py_DECREF(names);
                return soac_type_state_error("pending inherited namespace policy changed");
            }
            state->inherited_final_names = names;  /* immutable MOVE */
            if (PyDict_SetSoacPolicy(type->tp_dict, (PyObject *)state,
                                     soac_class_dictionary_policy, 0) < 0) return -1;
            state->class_dictionary_policy_installed = 1;
        }
        else {
            if (!PyDict_HasSoacPolicy(type->tp_dict) &&
                PyDict_SetSoacPolicy(type->tp_dict, names,
                                     soac_inherited_final_dictionary_policy, 0) < 0) {
                Py_DECREF(names);
                return -1;
            }
            Py_DECREF(names);
        }
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
    SoacTypeContract *state = soac_type_state(type);
    if (state != NULL) {
        /* Invalidate before callbacks; preserve the existing positions of
         * all decrefs so this does not reorder user-visible finalizers. */
        state->terminal = 1;
        state->type = NULL;
        state->initial_frozen_setattr = NULL;
        state->initial_frozen_delattr = NULL;
    }
}

void
_PySOAC_TypeContractClear(PyTypeObject *type)
{
    SoacTypeContract *state = soac_type_state(type);
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


static SoacTypeContract *
soac_type_ordinary_dictionary_contract(PyTypeObject *type)
{
    SoacTypeContract *own = soac_type_contract(type);
    if (own != NULL && own->dictionary_mode == Py_SOAC_INSTANCE_DICT_ORDINARY) return own;
    PyObject *mro = type->tp_mro;
    for (Py_ssize_t i = 1; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *base = soac_type_contract((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (base != NULL && base->dictionary_mode == Py_SOAC_INSTANCE_DICT_ORDINARY) return base;
    }
    return NULL;
}

int
_PySOAC_HasOrdinaryInstanceWrites(PyTypeObject *type)
{
    /* Pure actual-state/MRO query. Terminal policies stay selected so the
     * eventual write/prepare helper refuses rather than treating them ordinary. */
    return soac_type_ordinary_dictionary_contract(type) != NULL;
}

int
_PySOAC_CheckInlineInstanceWrite(PyObject *object, PyObject *name, PyObject *value)
{
    if (PyErr_Occurred()) return -1;
    PyTypeObject *type = Py_TYPE(object);
    SoacTypeContract *state = soac_type_ordinary_dictionary_contract(type);
    if (state == NULL) return 0;
    if (name == NULL || !PyUnicode_CheckExact(name) ||
        soac_contract_live(state) < 0 || state->owner == NULL ||
        state->check_instance_write == NULL) {
        return soac_type_state_error("ordinary inline field policy is unavailable");
    }
    PyObject *owner = state->owner;
    int result = state->check_instance_write(owner, object, name, value);
    if (result != 0 || PyErr_Occurred() || Py_TYPE(object) != type ||
        soac_type_ordinary_dictionary_contract(type) != state ||
        state->owner != owner || soac_contract_live(state) < 0) {
        return soac_type_state_error("ordinary inline field check failed or changed");
    }
    return 0;
}

int
_PySOAC_PrepareInstanceDictPolicy(PyObject *object, PyObject *dict,
                                 const PySoacInstanceDictPolicy *existing,
                                 PySoacInstanceDictPolicy *out)
{
    out->owner = NULL;
    out->validate = NULL;
    if (PyErr_Occurred()) return -1;
    PyTypeObject *type = Py_TYPE(object);
    SoacTypeContract *state = soac_type_ordinary_dictionary_contract(type);
    if (state == NULL) return 0;
    if (dict == NULL || !PyDict_Check(dict) || state->owner == NULL ||
        state->prepare_instance_dictionary_policy == NULL ||
        soac_contract_live(state) < 0) {
        return soac_type_state_error("ordinary dictionary policy preparation is unavailable");
    }
    PyObject *owner = state->owner;
    int result = state->prepare_instance_dictionary_policy(owner, object, dict, existing, out);
    if (result != 0 || PyErr_Occurred() || Py_TYPE(object) != type ||
        soac_type_ordinary_dictionary_contract(type) != state || state->owner != owner ||
        soac_contract_live(state) < 0 || out->owner == NULL || out->validate == NULL ||
        (existing != NULL &&
         (out->owner != existing->owner || out->validate != existing->validate))) {
        soac_type_state_error("ordinary dictionary policy factory failed or changed");
        PyObject *error = PyErr_GetRaisedException();
        PyObject *discard = out->owner;
        out->owner = NULL;
        out->validate = NULL;
        Py_XDECREF(discard);
        PyErr_SetRaisedException(error);
        return -1;
    }
    return 1;
}
