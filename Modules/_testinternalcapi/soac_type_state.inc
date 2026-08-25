/* Native-only storage fixtures. These do not construct source/checker records
 * or grant strict module/function authority. Included after the ordinary field
 * fixture so both representations use exactly its predicates and admission. */

typedef struct {
    PyObject *recipient;       /* wrapper-owned arguments, no candidate escape */
    PyObject *sibling;
    PyObject *primary;
    int attempts;
    int rejected;
} SoacTestTypeStateEscape;

static int
soac_test_type_state_private_write(PyObject *owner, PyObject *dictionary,
                                   PyObject *value, int operation)
{
    if (!_PyObject_HasTypeStateSlot(dictionary) ||
        !(((PyDictObject *)dictionary)->_ma_watcher_tag & _Py_DICT_TYPE_STATE_INSTALLING) ||
        (operation != PyDict_SOAC_ATTRIBUTE_SET && operation != PyDict_SOAC_ATTRIBUTE_SET_EXISTING)) {
        return 0;
    }
    PyObject *observers = soac_test_ordinary_observers(owner);
    if (observers == NULL) return -1;
    PyObject *capsule = PyList_GET_ITEM(observers, 1);
    if (!PyCapsule_IsValid(capsule, "test.soac.type_state.private_escape")) return 0;
    SoacTestTypeStateEscape *probe = PyCapsule_GetPointer(capsule, "test.soac.type_state.private_escape");
    if (probe == NULL) return -1;
    if (probe->attempts != 0) return 0;  /* one explicit probe, not a validator-count contract */
    ++probe->attempts;
    /* Exercise the actual callback's C dictionary argument, without exposing
     * or retaining the private header in Python. Even an ordinary recipient
     * must reject this while the original private write is suspended. */
    int status = PyObject_GenericSetDict(probe->recipient, dictionary, NULL);
    if (status != -1 || !PyErr_ExceptionMatches(PyExc_TypeError)) {
        PyErr_SetString(PyExc_AssertionError, "private direct dictionary escaped its installation");
        return -1;
    }
    PyErr_Clear();
    ++probe->rejected;
    PyObject *name = PyTuple_GET_ITEM(PyTuple_GET_ITEM(owner, 0), 0);
    if (PyDict_SetItem(probe->sibling, name, value) < 0) return -1;
    if (probe->primary != Py_None) {
        PyErr_SetRaisedException(Py_NewRef(probe->primary));
        return -1;
    }
    return 0;
}

static PyObject *
probe_soac_type_state_private_escape(PyObject *self, PyObject *args)
{
    PyObject *instance, *recipient, *sibling, *name, *value, *primary;
    if (!PyArg_ParseTuple(args, "OOOOOO:probe_soac_type_state_private_escape",
                         &instance, &recipient, &sibling, &name, &value, &primary)) return NULL;
    if (!_PyObject_HasTypeStateSlot(instance) || !PyDict_CheckExact(sibling) ||
        !PyUnicode_Check(name) || PyUnicode_CheckExact(name) || !PyLong_CheckExact(value) ||
        (primary != Py_None && !PyExceptionInstance_Check(primary))) {
        PyErr_SetString(PyExc_TypeError, "private escape probe requires stateful storage, a subclass name, and native operands");
        return NULL;
    }
    PyObject *owner = soac_test_ordinary_owner((PyObject *)Py_TYPE(instance));
    if (owner == NULL) return NULL;
    PyObject *observers = PyTuple_GET_ITEM(owner, 2);
    if (PyList_GET_ITEM(observers, 1) != Py_None) {
        PyErr_SetString(PyExc_TypeError, "private escape probe is already armed");
        return NULL;
    }
    PyObject *original = PyObject_GenericGetDict(recipient, NULL);
    if (original == NULL) return NULL;
    SoacTestTypeStateEscape probe = {.recipient = recipient, .sibling = sibling, .primary = primary};
    PyObject *capsule = PyCapsule_New(&probe, "test.soac.type_state.private_escape", NULL);
    if (capsule == NULL) {
        Py_DECREF(original);
        return NULL;
    }
    PyObject *old = PyList_GET_ITEM(observers, 1);
    PyList_SET_ITEM(observers, 1, capsule);
    Py_DECREF(old);
    int status = PyObject_GenericSetAttr(instance, name, value);
    PyObject *error = PyErr_GetRaisedException();
    PyList_SET_ITEM(observers, 1, Py_NewRef(Py_None));
    Py_DECREF(capsule);
    PyObject *after = PyObject_GenericGetDict(recipient, NULL);
    int unchanged = after == original;
    Py_XDECREF(after);
    Py_DECREF(original);
    if (probe.attempts != 1 || probe.rejected != 1 || !unchanged ||
        (status == 0) != (error == NULL) || PyErr_Occurred()) {
        Py_XDECREF(error);
        PyErr_SetString(PyExc_AssertionError, "private dictionary callback did not exercise one rejected escape");
        return NULL;
    }
    return error == NULL ? Py_NewRef(Py_None) : error;
}

static int
soac_test_type_state_factory(PyObject *owner, PyObject *actual_type,
                             PyTypeState **out)
{
    *out = NULL;
    PyObject *mro = ((PyTypeObject *)actual_type)->tp_mro;
    if (mro == NULL || !PyTuple_CheckExact(mro)) {
        PyErr_SetString(PyExc_TypeError, "native fixture requires an actual MRO");
        return -1;
    }
    /* This small fixture supports one declaring owner and ordinary
     * descendants. It must never omit another native declaration's checks. */
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
        PyObject *declaring = PyType_GetSoacContractOwner(PyTuple_GET_ITEM(mro, i));
        if (declaring == NULL && PyErr_Occurred()) return -1;
        if (declaring != NULL && declaring != owner) {
            PyErr_SetString(PyExc_TypeError, "native fixture cannot merge distinct declaring owners");
            return -1;
        }
    }
    PyTypeStateSpecV1 spec = {
        .abi_version = Py_TYPE_STATE_ABI_V1,
        .struct_size = sizeof(spec),
        .dictionary_owner = owner,
        .validate_dictionary = soac_test_ordinary_instance_policy,
        .validate_inline = soac_test_ordinary_field,
    };
    *out = PyTypeState_NewV1(actual_type, &spec, sizeof(spec));
    return *out == NULL ? -1 : 0;
}

static int
soac_test_type_state_bind(PyObject *owner, PyObject *actual_type)
{
    return PyType_SetSoacStorageStateFactoryV1(
        actual_type, owner, soac_test_type_state_factory);
}

static PyObject *
get_soac_type_state_info(PyObject *self, PyObject *object)
{
    int has_slot = _PyObject_HasTypeStateSlot(object);
    int terminal = 0;
    PyTypeState *state = NULL;
    PyTypeState **slot = NULL;
    const char *mode = "ordinary";
    if (has_slot) {
        slot = _PyObject_TypeStateSlot(object);
        if (slot == NULL) return NULL;
        terminal = *slot == NULL || (PyDict_CheckExact(object) &&
            (((PyDictObject *)object)->_ma_watcher_tag & _Py_DICT_TYPE_STATE_TERMINAL));
        if (!terminal) {
            state = PyObject_GetTypeState(object);
            if (state == NULL) return NULL;
        }
        mode = "direct";
    }
    else if ((PyDict_Check(object) && PyDict_HasSoacPolicy(object)) ||
             _PySOAC_HasOrdinaryInstanceWrites(Py_TYPE(object)) ||
             _PySOAC_UsesObjectSlotPolicy(Py_TYPE(object))) {
        mode = "legacy";
    }
    if (PyErr_Occurred()) return NULL;
    PyTypeState *dictionary = state == NULL ? NULL : PyDict_CheckExact(object)
        ? state : state->dictionary;
    PyObject *state_id = state == NULL ? Py_NewRef(Py_None) : PyLong_FromVoidPtr(state);
    PyObject *dictionary_id = dictionary == NULL ? Py_NewRef(Py_None)
        : PyLong_FromVoidPtr(dictionary);
    PyObject *offset = slot == NULL ? Py_NewRef(Py_None)
        : PyLong_FromSsize_t((char *)slot - (char *)object);
    if (state_id == NULL || dictionary_id == NULL || offset == NULL) {
        Py_XDECREF(state_id);
        Py_XDECREF(dictionary_id);
        Py_XDECREF(offset);
        return NULL;
    }
    return Py_BuildValue("{s:O,s:n,s:N,s:N,s:N,s:s,s:O}",
        "has_slot", has_slot ? Py_True : Py_False,
        "extra_slot_bytes", has_slot ? (Py_ssize_t)sizeof(PyTypeState *) : 0,
        "tail_offset", offset, "state_id", state_id,
        "dictionary_state_id", dictionary_id, "storage_mode", mode,
        "terminal", terminal ? Py_True : Py_False);
}

/* This bounded allocator observer records requests made to the actual OBJ
 * domain. It is independent of getsizeof and of the computed trailer offset.
 * Missing records mean no allocation was observed (e.g. an ordinary freelist),
 * never zero extra bytes. No Python allocation occurs in these callbacks. */
#define SOAC_TEST_ALLOCATION_RECORDS 4096
typedef struct {
    void *pointer;
    size_t bytes;
} SoacTestAllocationRecord;

typedef struct {
    PyMemAllocatorEx previous;
    SoacTestAllocationRecord records[SOAC_TEST_ALLOCATION_RECORDS];
    Py_ssize_t used;
    Py_ssize_t requests;
    Py_ssize_t fail_at;
    int armed;
    int overflow;
} SoacTestAllocationCapture;

static void
soac_test_allocation_restore(SoacTestAllocationCapture *capture)
{
    if (capture->armed) {
        capture->armed = 0;
        PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &capture->previous);
    }
}

static int
soac_test_allocation_fail(SoacTestAllocationCapture *capture)
{
    ++capture->requests;
    if (capture->fail_at > 0 && capture->requests == capture->fail_at) {
        /* Error creation and cleanup use the original allocator. */
        soac_test_allocation_restore(capture);
        return 1;
    }
    return 0;
}

static void
soac_test_allocation_record(SoacTestAllocationCapture *capture, void *pointer, size_t bytes)
{
    if (pointer == NULL) return;
    if (capture->used == SOAC_TEST_ALLOCATION_RECORDS) {
        capture->overflow = 1;
        return;
    }
    capture->records[capture->used++] = (SoacTestAllocationRecord){pointer, bytes};
}

static void
soac_test_allocation_forget(SoacTestAllocationCapture *capture, void *pointer)
{
    for (Py_ssize_t i = capture->used; i > 0; --i) {
        if (capture->records[i - 1].pointer == pointer) {
            capture->records[i - 1].pointer = NULL;
            return;
        }
    }
}

static void *
soac_test_allocation_malloc(void *ctx, size_t bytes)
{
    SoacTestAllocationCapture *capture = ctx;
    if (soac_test_allocation_fail(capture)) return NULL;
    void *pointer = capture->previous.malloc(capture->previous.ctx, bytes);
    soac_test_allocation_record(capture, pointer, bytes);
    return pointer;
}

static void *
soac_test_allocation_calloc(void *ctx, size_t count, size_t size)
{
    SoacTestAllocationCapture *capture = ctx;
    if (soac_test_allocation_fail(capture)) return NULL;
    void *pointer = capture->previous.calloc(capture->previous.ctx, count, size);
    if (size != 0 && count > SIZE_MAX / size) capture->overflow = 1;
    else soac_test_allocation_record(capture, pointer, count * size);
    return pointer;
}

static void *
soac_test_allocation_realloc(void *ctx, void *pointer, size_t bytes)
{
    SoacTestAllocationCapture *capture = ctx;
    if (soac_test_allocation_fail(capture)) return NULL;
    void *replacement = capture->previous.realloc(capture->previous.ctx, pointer, bytes);
    if (replacement != NULL) {
        soac_test_allocation_forget(capture, pointer);
        soac_test_allocation_record(capture, replacement, bytes);
    }
    return replacement;
}

static void
soac_test_allocation_free(void *ctx, void *pointer)
{
    SoacTestAllocationCapture *capture = ctx;
    soac_test_allocation_forget(capture, pointer);
    capture->previous.free(capture->previous.ctx, pointer);
}

static PyObject *
capture_soac_type_state_allocation(PyObject *self, PyObject *args)
{
#if !_Py_TYPE_STATE_SUPPORTED
    PyErr_SetString(PyExc_NotImplementedError, "allocation capture requires the supported GIL layout");
    return NULL;
#else
    PyObject *callable;
    Py_ssize_t fail_at = 0;
    if (!PyArg_ParseTuple(args, "O|n:capture_soac_type_state_allocation", &callable, &fail_at)) return NULL;
    if (!PyCallable_Check(callable) || fail_at < 0) {
        PyErr_SetString(PyExc_TypeError, "allocation capture requires a callable and nonnegative fault index");
        return NULL;
    }
    SoacTestAllocationCapture capture = {.fail_at = fail_at, .armed = 1};
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &capture.previous);
    PyMemAllocatorEx allocator = {
        .ctx = &capture, .malloc = soac_test_allocation_malloc,
        .calloc = soac_test_allocation_calloc, .realloc = soac_test_allocation_realloc,
        .free = soac_test_allocation_free,
    };
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
    PyObject *result = PyObject_CallNoArgs(callable);
    soac_test_allocation_restore(&capture);
    if (result == NULL) return NULL;
    if (capture.overflow) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_AssertionError, "allocation evidence exceeded its bounded record buffer");
        return NULL;
    }
    const void *base = (char *)result - _PyType_PreHeaderSize(Py_TYPE(result));
    PyObject *requested = Py_NewRef(Py_None);
    for (Py_ssize_t i = capture.used; i > 0; --i) {
        if (capture.records[i - 1].pointer == base) {
            Py_SETREF(requested, PyLong_FromSize_t(capture.records[i - 1].bytes));
            break;
        }
    }
    if (requested == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    return Py_BuildValue("(NN)", result, requested);
#endif
}

typedef struct {
    Py_ssize_t created;
    Py_ssize_t destroyed;
    int incomplete;
} SoacTestTypeStateTrace;

static int
soac_test_type_state_trace(PyObject *object, PyRefTracerEvent event, void *data)
{
    SoacTestTypeStateTrace *trace = data;
    if (object == NULL || !_PyObject_HasTypeStateSlot(object)) return 0;
    if (event == PyRefTracer_CREATE) {
        ++trace->created;
        /* Reject an incomplete allocation before calling the checked helper:
         * reftracers must neither allocate nor change the exception state. */
        PyTypeObject *type = Py_TYPE(object);
        if (!PyDict_CheckExact(object)) {
            if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
                !(type->tp_flags & Py_TPFLAGS_HAVE_GC) || type->tp_itemsize != 0 ||
                type->tp_basicsize < (Py_ssize_t)sizeof(PyObject) ||
                type->tp_basicsize > PY_SSIZE_T_MAX - 4096) {
                trace->incomplete = 1;
                return 0;
            }
#if _Py_TYPE_STATE_SUPPORTED
            if (type->tp_flags & Py_TPFLAGS_INLINE_VALUES) {
                PyDictValues *values = _PyObject_InlineValues(object);
                if (values->soac_allocated_capacity == 0 ||
                    values->soac_allocated_capacity > SHARED_KEYS_MAX_SIZE ||
                    values->soac_allocated_capacity < values->capacity) {
                    trace->incomplete = 1;
                    return 0;
                }
            }
#endif
        }
        PyTypeState **slot = _PyObject_TypeStateSlot(object);
        if (slot == NULL || *slot == NULL || (*slot)->terminal ||
            (PyDict_CheckExact(object)
                ? (*slot)->kind != _Py_TYPE_STATE_DICTIONARY ||
                  ((PyDictObject *)object)->ma_keys == NULL ||
                  !(((PyDictObject *)object)->_ma_watcher_tag & _Py_DICT_TYPE_STATE_INSTALLING)
                : (*slot)->kind != _Py_TYPE_STATE_INSTANCE ||
                  (*slot)->allocation_type != Py_TYPE(object))) {
            trace->incomplete = 1;
        }
    }
    else if (event == PyRefTracer_DESTROY) ++trace->destroyed;
    return 0;
}

static PyObject *
check_soac_type_state_reftracer(PyObject *self, PyObject *callable)
{
    void *old_data;
    if (PyRefTracer_GetTracer(&old_data) != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "allocation fixture will not replace an installed reftracer");
        return NULL;
    }
    SoacTestTypeStateTrace trace = {0};
    if (PyRefTracer_SetTracer(soac_test_type_state_trace, &trace) < 0) return NULL;
    PyObject *result = PyObject_CallNoArgs(callable);
    PyObject *error = PyErr_GetRaisedException();
    int removed = PyRefTracer_SetTracer(NULL, NULL);
    if (error != NULL) {
        Py_XDECREF(result);
        PyErr_SetRaisedException(error);
        return NULL;
    }
    if (removed < 0) {
        Py_XDECREF(result);
        return NULL;
    }
    if (result == NULL) {
        PyErr_SetString(PyExc_SystemError, "allocation fixture lost its callable error");
        return NULL;
    }
    if (trace.incomplete) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_AssertionError, "CREATE observed an incomplete stateful allocation");
        return NULL;
    }
    return Py_BuildValue("(Nnn)", result, trace.created, trace.destroyed);
}

static PyObject *
soac_type_state_clear(PyObject *self, PyObject *object)
{
    if (!_PyObject_HasTypeStateSlot(object) || Py_TYPE(object)->tp_clear == NULL ||
        PyObject_GetTypeState(object) == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "clear fixture requires a live stateful GC participant");
        }
        return NULL;
    }
    if (Py_TYPE(object)->tp_clear(object) < 0 || PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
soac_type_state_negative_refcount(PyObject *self, PyObject *object)
{
#if _Py_TYPE_STATE_SUPPORTED && defined(Py_REF_DEBUG)
    if (!_PyObject_HasTypeStateSlot(object) || PyObject_GetTypeState(object) == NULL) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "negative-refcount probe requires actual stateful allocation");
        return NULL;
    }
    /* Deliberately invalid ownership in a terminating subprocess only. The
     * actual allocation/marker remains untouched; no ordinary header retag. */
    object->ob_refcnt = 0;
    Py_DECREF(object);
    Py_FatalError("stateful zero-refcount decrement was not diagnosed");
#else
    PyErr_SetString(PyExc_RuntimeError, "negative-refcount probe requires the supported Py_REF_DEBUG build");
    return NULL;
#endif
}

static PyObject *
soac_type_state_write_member(PyObject *self, PyObject *args)
{
#if !_Py_TYPE_STATE_SUPPORTED
    PyErr_SetString(PyExc_NotImplementedError, "member probe requires the supported GIL layout");
    return NULL;
#else
    PyObject *object, *value;
    const char *target;
    if (!PyArg_ParseTuple(args, "OsO:soac_type_state_write_member", &object, &target, &value)) return NULL;
    if (!_PyObject_HasTypeStateSlot(object) || PyObject_GetTypeState(object) == NULL) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "member probe requires an actual stateful allocation");
        return NULL;
    }
    PyMemberDef view = {.name = "test-layout-view", .flags = 0};
    if (strcmp(target, "flags") == 0) {
        view.type = Py_T_USHORT;
        view.offset = offsetof(PyObject, ob_flags);
    }
    else if (strcmp(target, "inline_extent") == 0 &&
             (Py_TYPE(object)->tp_flags & Py_TPFLAGS_INLINE_VALUES)) {
        view.type = Py_T_UBYTE;
        view.offset = (char *)_PyObject_InlineValues(object) - (char *)object +
                      offsetof(PyDictValues, soac_allocated_capacity);
    }
    else if (strcmp(target, "ordinary_slot") == 0) {
        const PyMemberDef *member = Py_TYPE(object)->tp_members;
        while (member != NULL && member->name != NULL && strcmp(member->name, target) != 0) ++member;
        if (member == NULL || member->name == NULL) {
            PyErr_SetString(PyExc_TypeError, "member probe has no actual ordinary slot");
            return NULL;
        }
        view = *member;
    }
    else {
        PyErr_SetString(PyExc_ValueError, "unknown or unavailable member probe target");
        return NULL;
    }
    if (PyMember_SetOne((char *)object, &view, value) < 0) return NULL;
    Py_RETURN_NONE;
#endif
}

static PyObject *
probe_soac_type_state_lookups(PyObject *self, PyObject *args)
{
#if !_Py_TYPE_STATE_SUPPORTED
    PyErr_SetString(PyExc_NotImplementedError, "lookup probe requires the supported GIL layout");
    return NULL;
#else
    PyObject *actual_type, *dictionary, *callable;
    if (!PyArg_ParseTuple(args, "OOO:probe_soac_type_state_lookups",
                         &actual_type, &dictionary, &callable)) return NULL;
    if (!PyType_Check(actual_type) ||
        (dictionary != Py_None && !PyDict_Check(dictionary)) || !PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "lookup probe requires an actual type, dict or None, and callable");
        return NULL;
    }
    struct _Py_dict_state *dictionary_state = &_PyInterpreterState_GET()->dict_state;
    if (dictionary_state->soac_storage_lookup_probe != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "another native storage lookup probe is active");
        return NULL;
    }
    /* Explicit C-supported targets exclude unrelated exception/result objects.
     * Clear the stack pointer before any cleanup or result allocation. */
    Py_INCREF(actual_type);
    Py_INCREF(dictionary);
    Py_INCREF(callable);
    struct _PySoacStorageLookupProbe probe = {
        .actual_type = (PyTypeObject *)actual_type,
        .dictionary = dictionary == Py_None ? NULL : dictionary,
    };
    dictionary_state->soac_storage_lookup_probe = &probe;
    PyObject *result = PyObject_CallNoArgs(callable);
    dictionary_state->soac_storage_lookup_probe = NULL;
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(callable);
    Py_DECREF(dictionary);
    Py_DECREF(actual_type);
    PyErr_SetRaisedException(error);
    if (result == NULL) return NULL;
    if (PyErr_Occurred()) {
        Py_DECREF(result);
        return NULL;
    }
    return Py_BuildValue("(N{s:K,s:K,s:K})", result,
        "ordinary_type_lookups", (unsigned long long)probe.ordinary_type_lookups,
        "slot_type_lookups", (unsigned long long)probe.slot_type_lookups,
        "dictionary_identity_lookups", (unsigned long long)probe.dictionary_identity_lookups);
#endif
}
