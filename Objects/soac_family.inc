/* Actual closed membership, owned by the root's existing native type state.
 * Included after receiver policy: birth coverage and closure are independent. */
enum { SOAC_FAMILY_PREPARING = 1, SOAC_FAMILY_CLOSED, SOAC_FAMILY_TERMINAL };
struct SoacFamilyClosure {
    PyObject_HEAD
    PyObject *root;                 /* callback-free weak identity */
    PyObject *members;              /* exact tuple of callback-free weakrefs */
    unsigned int phase;
};

static void
soac_family_retire(SoacFamilyClosure *family)
{
    if (family != NULL) family->phase = SOAC_FAMILY_TERMINAL;
}

static int
soac_family_traverse(SoacFamilyClosure *family, visitproc visit, void *arg)
{
    Py_VISIT(family->root);
    Py_VISIT(family->members);
    return 0;
}

static int
soac_family_clear(SoacFamilyClosure *family)
{
    soac_family_retire(family);
    Py_CLEAR(family->root);
    Py_CLEAR(family->members);
    return 0;
}

static void
soac_family_dealloc(SoacFamilyClosure *family)
{
    PyObject_GC_UnTrack(family);
    soac_family_clear(family);
    PyObject_GC_Del(family);
}

static PyTypeObject SoacFamilyClosure_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_soac.ClosedClassFamily",
    .tp_basicsize = sizeof(SoacFamilyClosure),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_family_traverse,
    .tp_clear = (inquiry)soac_family_clear,
    .tp_dealloc = (destructor)soac_family_dealloc,
};

static SoacFamilyClosure *
soac_family_new(void)
{
    size_t presize = _PyType_PreHeaderSize(&SoacFamilyClosure_Type);
    char *memory = _PyObject_MallocWithType(&SoacFamilyClosure_Type,
                                           presize + sizeof(SoacFamilyClosure));
    if (memory == NULL) return (SoacFamilyClosure *)PyErr_NoMemory();
    memset(memory, 0, presize + sizeof(SoacFamilyClosure));
    SoacFamilyClosure *family = (SoacFamilyClosure *)(memory + presize);
    family->phase = SOAC_FAMILY_PREPARING;
    _PyObject_GC_Link((PyObject *)family);
    _PyObject_Init((PyObject *)family, &SoacFamilyClosure_Type);
    /* CREATE sees initialized unavailable metadata. Never undo retirement. */
    if (family->phase != SOAC_FAMILY_PREPARING) {
        Py_DECREF(family);
        soac_type_state_error("family receipt retired during allocation");
        return NULL;
    }
    PyObject_GC_Track(family);
    return family;
}

static int
soac_family_contains(SoacFamilyClosure *family, PyTypeObject *type)
{
    if (family->members == NULL) return 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(family->members); ++i) {
        PyTypeObject *member = type_from_ref(PyTuple_GET_ITEM(family->members, i));
        int same = member == type;
        Py_XDECREF(member);
        if (same) return 1;
    }
    return 0;
}

static int
soac_family_live(SoacFamilyClosure *family)
{
    if (family->phase != SOAC_FAMILY_CLOSED || family->root == NULL ||
        family->members == NULL)
        return soac_type_state_error("closed family receipt is unavailable");
    PyTypeObject *root = type_from_ref(family->root);
    SoacTypeContract *state = root == NULL ? NULL : soac_type_state(root);
    int live = state != NULL && !state->terminal && state->type == root &&
        state->family_closure == family && state->owner != NULL && state->sealed;
    Py_XDECREF(root);
    return live ? 0 : soac_type_state_error("closed family root is retired");
}

static int
soac_family_check_base(PyTypeObject *base)
{
    PyObject *mro = lookup_tp_mro(base);
    SoacTypeContract *own = soac_type_state(base);
    if (own != NULL && (own->preparing_family || own->family_closure != NULL)) goto closed;
    for (Py_ssize_t i = 0; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        SoacTypeContract *state = soac_type_state((PyTypeObject *)PyTuple_GET_ITEM(mro, i));
        if (state != NULL && (state->preparing_family || state->family_closure != NULL)) goto closed;
    }
    return 0;
closed:
    return soac_type_state_error("cannot introduce a subclass of a preparing or closed strict family");
}

static int
soac_family_check_publication(PyTypeObject *type, PyObject *mro)
{
    for (Py_ssize_t i = 0; mro != NULL && i < PyTuple_GET_SIZE(mro); ++i) {
        PyTypeObject *root = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        SoacTypeContract *state = soac_type_state(root);
        SoacFamilyClosure *family = state == NULL ? NULL : state->family_closure;
        if (state != NULL && state->preparing_family && root != type)
            return soac_type_state_error("new class cannot join a preparing strict family");
        if (family == NULL || root == type) continue;
        if (!(type->tp_flags & Py_TPFLAGS_READY) ||
            family->phase != SOAC_FAMILY_CLOSED ||
            soac_family_live(family) < 0 || !soac_family_contains(family, type))
            return soac_type_state_error("new class cannot join a preparing or closed strict family");
    }
    return 0;
}

typedef struct {
    PyTypeObject *type;
    PyObject *mro, *bases;
    SoacTypeContract *state;
    SoacReceiverReceipt *receiver;
    SoacFamilyClosure *family;
    Py_ssize_t root_index, member_count, last_member, fill;
    int new_family;
} SoacFamilyRow;

typedef struct {
    PySoacFamilyCaptureSpecV1 spec;
    SoacFamilyRow *rows;
    Py_ssize_t *table;  /* transient pointer index into strongly pinned rows */
    Py_ssize_t *pending;
    size_t table_size;
    Py_ssize_t count, pending_count, roots, edges, memberships;
} SoacFamilyCapture;

static int
soac_family_limit(const char *message)
{
    PyErr_SetString(PyExc_OverflowError, message);
    return -1;
}

/* No metaclass hash/equality, Python containers, allocation, or callbacks. */
static size_t
soac_family_slot(SoacFamilyCapture *capture, PyTypeObject *type)
{
    size_t slot = ((uintptr_t)type >> 4) & (capture->table_size - 1);
    while (capture->table[slot] != 0 &&
           capture->rows[capture->table[slot] - 1].type != type)
        slot = (slot + 1) & (capture->table_size - 1);
    return slot;
}

static SoacFamilyRow *
soac_family_row(SoacFamilyCapture *capture, PyTypeObject *type)
{
    Py_ssize_t index = capture->table[soac_family_slot(capture, type)];
    return index == 0 ? NULL : &capture->rows[index - 1];
}

static int
soac_family_add_type(SoacFamilyCapture *capture, PyTypeObject *type)
{
    size_t slot = soac_family_slot(capture, type);
    if (capture->table[slot] != 0) return 0;
    if (capture->count == capture->spec.max_classes)
        return soac_family_limit("family capture exceeds max_classes");
    if (capture->pending_count == capture->spec.max_frontier)
        return soac_family_limit("family capture exceeds max_frontier");
    capture->pending[capture->pending_count++] = capture->count;
    SoacFamilyRow *row = &capture->rows[capture->count++];
    row->type = (PyTypeObject *)Py_NewRef(type);
    row->root_index = row->last_member = -1;
    capture->table[slot] = capture->count;
    return 0;
}

static int
soac_family_scan(SoacFamilyCapture *capture)
{
    if (soac_family_add_type(capture, &PyBaseObject_Type) < 0) return -1;
    while (capture->pending_count != 0) {
        Py_ssize_t cursor = capture->pending[--capture->pending_count];
        PyObject *subclasses = lookup_tp_subclasses(capture->rows[cursor].type);
        Py_ssize_t position = 0;
        PyObject *ref;
        while (subclasses != NULL && PyDict_Next(subclasses, &position, NULL, &ref)) {
            if (capture->edges++ == capture->spec.max_edges)
                return soac_family_limit("family graph exceeds max_edges");
            PyTypeObject *child = type_from_ref(ref);
            if (child == NULL) continue;
            int result = soac_family_add_type(capture, child);
            Py_DECREF(child);
            if (result < 0) return -1;
        }
    }
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        row->mro = Py_XNewRef(lookup_tp_mro(row->type));
        row->bases = Py_XNewRef(lookup_tp_bases(row->type));
        row->state = (SoacTypeContract *)Py_XNewRef(soac_type_state(row->type));
        row->receiver = row->state == NULL ? NULL :
            (SoacReceiverReceipt *)Py_XNewRef(row->state->receiver_policy);
        if (row->mro == NULL || row->bases == NULL)
            return soac_type_state_error("family capture encountered an unfinished type");
        Py_ssize_t refs = PyTuple_GET_SIZE(row->mro) + PyTuple_GET_SIZE(row->bases);
        if (refs > capture->spec.max_edges - capture->edges)
            return soac_family_limit("family type references exceed max_edges");
        capture->edges += refs;
        SoacTypeContract *state = row->state;
        if (state == NULL || soac_type_phase(state) == Py_SOAC_TYPE_STATE_DYNAMIC ||
            soac_type_phase(state) == SOAC_TYPE_STATE_RECEIVER_ONLY) continue;
        if (state->terminal || soac_type_phase(state) != Py_SOAC_TYPE_STATE_ENFORCED ||
            !state->permanent_contract_published || !state->sealed || state->owner == NULL ||
            state->type != row->type || PyTuple_GET_SIZE(row->mro) == 0 ||
            PyTuple_GET_ITEM(row->mro, 0) != (PyObject *)row->type)
            return soac_type_state_error("family capture encountered unresolved strict construction");
        row->root_index = capture->roots++;
    }
    /* MRO-to-index assignment handles injected strict ancestors and diamonds
     * without a roots-times-classes pass. Duplicate MRO entries add no member. */
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *member = &capture->rows[i];
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(member->mro); ++j) {
            SoacFamilyRow *root = soac_family_row(capture,
                (PyTypeObject *)PyTuple_GET_ITEM(member->mro, j));
            if (root == NULL || root->root_index < 0 || root->last_member == i) continue;
            root->last_member = i;
            if (capture->memberships++ == capture->spec.max_memberships)
                return soac_family_limit("family capture exceeds max_memberships");
            ++root->member_count;
            if (!(member->type->tp_flags & Py_TPFLAGS_READY) || is_readying(member->type) ||
                member->state == NULL || member->state->terminal ||
                soac_type_allocation_barrier(member->type) < 0 ||
                soac_receiver_live(member->type, member->receiver) < 0)
                return soac_type_state_error("family member lacks completed native birth coverage");
        }
    }
    return 0;
}

static int
soac_family_revalidate(SoacFamilyCapture *capture)
{
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        if (lookup_tp_mro(row->type) != row->mro || lookup_tp_bases(row->type) != row->bases ||
            soac_type_state(row->type) != row->state ||
            (row->state != NULL && row->state->receiver_policy != row->receiver))
            return soac_type_state_error("class graph changed during family capture");
        if (row->receiver != NULL && soac_receiver_live(row->type, row->receiver) < 0) return -1;
        if (row->root_index >= 0 &&
            (row->state->terminal || row->state->type != row->type || !row->state->sealed ||
             soac_type_phase(row->state) != Py_SOAC_TYPE_STATE_ENFORCED ||
             row->state->preparing_family != row->new_family ||
             row->state->family_closure != row->family || row->family == NULL ||
             row->family->phase != (row->new_family ? SOAC_FAMILY_PREPARING : SOAC_FAMILY_CLOSED)))
            return soac_type_state_error("strict root changed during family capture");
        PyObject *subclasses = lookup_tp_subclasses(row->type);
        PyObject *ref;
        Py_ssize_t position = 0;
        while (subclasses != NULL && PyDict_Next(subclasses, &position, NULL, &ref)) {
            PyTypeObject *child = type_from_ref(ref);
            if (child == NULL) continue;
            int known = soac_family_row(capture, child) != NULL;
            Py_DECREF(child);
            if (!known) return soac_type_state_error("new class appeared during family capture");
        }
    }
    return 0;
}

static int
soac_family_prepare(SoacFamilyCapture *capture)
{
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        if (row->root_index < 0) continue;
        SoacTypeContract *state = row->state;
        if (state->preparing_family)
            return soac_type_state_error("strict family capture is already in progress");
        if (state->family_closure != NULL) {
            row->family = (SoacFamilyClosure *)Py_NewRef(state->family_closure);
            if (soac_family_live(row->family) < 0) return -1;
            continue;
        }
        row->new_family = 1;
        state->preparing_family = 1;
    }
    /* Every open root is barred before any shell CREATE or allocation can
     * reenter. Marking and authenticating the full root set above has no
     * callback or allocation; failed preparation clears only our markers. */
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        if (!row->new_family) continue;
        SoacTypeContract *state = row->state;
        row->family = soac_family_new();
        if (row->family == NULL) return -1;
        if (soac_type_state(row->type) != state || state->terminal ||
            !state->preparing_family || state->family_closure != NULL ||
            row->family->phase != SOAC_FAMILY_PREPARING)
            return soac_type_state_error("family root changed during preparation");
        state->family_closure = (SoacFamilyClosure *)Py_NewRef(row->family);
    }
    return soac_family_revalidate(capture);
}

static int
soac_family_snapshot(SoacFamilyCapture *capture, PyObject **types_out,
                     PyObject **roots_out, PyObject **members_out, PyObject **receipts_out)
{
    *types_out = PyTuple_New(capture->count);
    *roots_out = PyTuple_New(capture->roots);
    *members_out = PyTuple_New(capture->roots);
    *receipts_out = PyTuple_New(capture->roots);
    if (*types_out == NULL || *roots_out == NULL || *members_out == NULL || *receipts_out == NULL)
        return -1;
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        PyTuple_SET_ITEM(*types_out, i, Py_NewRef(row->type));
        row->last_member = -1;
        if (row->root_index < 0) continue;
        PyObject *members = PyTuple_New(row->member_count);
        if (members == NULL) return -1;
        PyTuple_SET_ITEM(*roots_out, row->root_index, Py_NewRef(row->type));
        PyTuple_SET_ITEM(*members_out, row->root_index, members);
        PyTuple_SET_ITEM(*receipts_out, row->root_index, Py_NewRef(row->family));
    }
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(row->mro); ++j) {
            SoacFamilyRow *root = soac_family_row(capture, (PyTypeObject *)PyTuple_GET_ITEM(row->mro, j));
            if (root == NULL || root->root_index < 0 || root->last_member == i) continue;
            root->last_member = i;
            PyObject *members = PyTuple_GET_ITEM(*members_out, root->root_index);
            PyTuple_SET_ITEM(members, root->fill++, Py_NewRef(row->type));
        }
    }
    for (Py_ssize_t i = 0; i < capture->count; ++i) {
        SoacFamilyRow *row = &capture->rows[i];
        if (row->root_index < 0) continue;
        if (!row->new_family) {
            if (soac_family_live(row->family) < 0) return -1;
            Py_ssize_t live = 0;
            for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(row->family->members); ++j) {
                PyTypeObject *type = type_from_ref(PyTuple_GET_ITEM(row->family->members, j));
                if (type == NULL) continue;
                SoacFamilyRow *member = soac_family_row(capture, type);
                int known = member != NULL && soac_mro_contains_type(member->mro, row->type);
                Py_DECREF(type);
                if (!known) return soac_type_state_error("closed family membership changed");
                ++live;
            }
            if (live != row->member_count)
                return soac_type_state_error("closed family gained an unrecorded member");
            continue;
        }
        PyObject *members = PyTuple_GET_ITEM(*members_out, row->root_index);
        PyObject *weak = PyTuple_New(row->member_count);
        if (weak == NULL) return -1;
        for (Py_ssize_t j = 0; j < row->member_count; ++j) {
            PyObject *ref = PyWeakref_NewRef(PyTuple_GET_ITEM(members, j), NULL);
            if (ref == NULL) { Py_DECREF(weak); return -1; }
            PyTuple_SET_ITEM(weak, j, ref);
        }
        PyObject *root = PyWeakref_NewRef((PyObject *)row->type, NULL);
        if (root == NULL) { Py_DECREF(weak); return -1; }
        if (row->family->phase != SOAC_FAMILY_PREPARING) {
            Py_DECREF(root); Py_DECREF(weak);
            return soac_type_state_error("family receipt retired during snapshot");
        }
        row->family->root = root;
        row->family->members = weak;
    }
    return soac_family_revalidate(capture);
}

int
PyType_CaptureSoacFamiliesV1(const PySoacFamilyCaptureSpecV1 *spec,
                           PyObject **receipts_out, PyObject **types_out)
{
    if (receipts_out != NULL) *receipts_out = NULL;
    if (types_out != NULL) *types_out = NULL;
#ifdef Py_GIL_DISABLED
    return soac_type_state_error("closed strict families require the GIL configuration");
#else
    if (PyErr_Occurred()) return -1;
    if (spec == NULL || spec->abi_version != 1 || spec->struct_size != sizeof(*spec) ||
        spec->validate == NULL || receipts_out == NULL || types_out == NULL ||
        receipts_out == types_out || spec->max_classes <= 0 || spec->max_frontier <= 0 || spec->max_edges <= 0 ||
        spec->max_memberships <= 0 || (size_t)spec->max_classes > SIZE_MAX / sizeof(SoacFamilyRow) ||
        (size_t)spec->max_classes > SIZE_MAX / (4 * sizeof(Py_ssize_t))) {
        PyErr_SetString(PyExc_ValueError, "invalid family capture specification");
        return -1;
    }
    if (PyType_Ready(&SoacFamilyClosure_Type) < 0) return -1;
    SoacFamilyCapture capture = {.spec = *spec, .table_size = 2};
    while (capture.table_size < (size_t)spec->max_classes * 2) capture.table_size *= 2;
    capture.rows = PyMem_Calloc(spec->max_classes, sizeof(SoacFamilyRow));
    capture.table = PyMem_Calloc(capture.table_size, sizeof(Py_ssize_t));
    capture.pending = PyMem_Calloc(spec->max_classes, sizeof(Py_ssize_t));
    PyObject *types = NULL, *roots = NULL, *members = NULL, *receipts = NULL;
    int result = -1;
    if (capture.rows == NULL || capture.table == NULL || capture.pending == NULL) {
        PyErr_NoMemory(); goto done;
    }
    /* Raw pointer-index scan and INCREFs only; the held GIL makes this initial
     * scan callback-free. Later allocations/callbacks run with roots preparing. */
    if (soac_family_scan(&capture) < 0 || soac_family_prepare(&capture) < 0 ||
        soac_family_snapshot(&capture, &types, &roots, &members, &receipts) < 0) goto done;
    if (capture.spec.validate(capture.spec.context, types, roots, members) != 0 || PyErr_Occurred()) {
        soac_type_state_error("family validation callback failed");
        goto done;
    }
    if (soac_family_revalidate(&capture) < 0) goto done;
    /* No allocations, releases or callbacks between the final checks and the
     * all-root commit. Existing receipts are never replaced or reopened. */
    for (Py_ssize_t i = 0; i < capture.count; ++i) {
        if (capture.rows[i].new_family) {
            capture.rows[i].family->phase = SOAC_FAMILY_CLOSED;
            capture.rows[i].state->preparing_family = 0;
        }
    }
    *receipts_out = receipts; receipts = NULL;
    *types_out = types; types = NULL;
    result = 0;
done:
    {
        PyObject *error = PyErr_GetRaisedException();
        if (result < 0) {
            /* Roll back every own preparing marker before any callback-capable
             * cleanup. Row references keep detached shells alive through this. */
            for (Py_ssize_t i = 0; i < capture.count; ++i) {
                SoacFamilyRow *row = &capture.rows[i];
                if (!row->new_family) continue;
                row->state->preparing_family = 0;
                if (row->family != NULL && row->state->family_closure == row->family) {
                    row->state->family_closure = NULL;
                    soac_family_retire(row->family);
                    Py_DECREF(row->family);
                }
            }
        }
        Py_XDECREF(types); Py_XDECREF(roots); Py_XDECREF(members); Py_XDECREF(receipts);
        for (Py_ssize_t i = 0; i < capture.count; ++i) {
            SoacFamilyRow *row = &capture.rows[i];
            Py_XDECREF(row->family); Py_XDECREF(row->receiver); Py_XDECREF(row->state);
            Py_XDECREF(row->mro); Py_XDECREF(row->bases); Py_DECREF(row->type);
        }
        PyMem_Free(capture.pending); PyMem_Free(capture.table); PyMem_Free(capture.rows);
        PyErr_SetRaisedException(error);
    }
    return result;
#endif
}

int
PyType_IsSoacFamilyMemberV1(PyObject *object, PyObject *actual_type)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacFamilyClosure_Type) ||
        actual_type == NULL || !PyType_Check(actual_type))
        return soac_type_state_error("expected a native family receipt and actual type");
    SoacFamilyClosure *family = (SoacFamilyClosure *)object;
    if (soac_family_live(family) < 0) return -1;
    return soac_family_contains(family, (PyTypeObject *)actual_type);
}

PyObject *
PyType_GetSoacFamilyMembersV1(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacFamilyClosure_Type)) {
        soac_type_state_error("expected a native family receipt");
        return NULL;
    }
    SoacFamilyClosure *family = (SoacFamilyClosure *)object;
    if (soac_family_live(family) < 0) return NULL;
    PyObject *weak = Py_NewRef(family->members);
    PyObject *members = PyList_New(0);
    if (members == NULL) { Py_DECREF(weak); return NULL; }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(weak); ++i) {
        PyTypeObject *type = type_from_ref(PyTuple_GET_ITEM(weak, i));
        if (type == NULL) continue;
        int result = PyList_Append(members, (PyObject *)type);
        Py_DECREF(type);
        if (result < 0) { Py_DECREF(weak); Py_DECREF(members); return NULL; }
    }
    Py_DECREF(weak);
    PyObject *tuple = PyList_AsTuple(members);
    Py_DECREF(members);
    if (tuple != NULL && soac_family_live(family) < 0) Py_CLEAR(tuple);
    return tuple;
}
