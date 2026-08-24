#include "Python.h"

#include "pycore_object.h"
#include "pycore_stackref.h"

#if !defined(Py_GIL_DISABLED) && defined(Py_STACKREF_DEBUG)

#if SIZEOF_VOID_P < 8
#error "Py_STACKREF_DEBUG requires 64 bit machine"
#endif

#include "pycore_interp.h"
#include "pycore_hashtable.h"
#include "pycore_frame.h"
#include "pycore_interpframe.h"

struct _PyStackRefFrameTupleEntry {
    struct _PyStackRefFrameTupleEntry *next;
    PyFrameObject *frame;  /* Borrowed: the actual frame owns this ledger. */
    Py_ssize_t index;
    _PyStackRef reference;
};

typedef struct _table_entry {
    PyObject *obj;
    const char *classname;
    const char *filename;
    int linenumber;
    const char *filename_borrow;
    int linenumber_borrow;
    int borrows;
    _PyStackRef borrowed_from;
    struct _PyStackRefFrameTupleEntry overwritten;
} TableEntry;

TableEntry *
make_table_entry(PyObject *obj, const char *filename, int linenumber)
{
    TableEntry *result = malloc(sizeof(TableEntry));
    if (result == NULL) {
        return NULL;
    }
    result->obj = obj;
    result->classname = Py_TYPE(obj)->tp_name;
    result->filename = filename;
    result->linenumber = linenumber;
    result->filename_borrow = NULL;
    result->linenumber_borrow = 0;
    result->borrows = 0;
    result->borrowed_from = PyStackRef_NULL;
    result->overwritten = (struct _PyStackRefFrameTupleEntry){
        .next = NULL, .frame = NULL, .index = -1, .reference = PyStackRef_NULL,
    };
    return result;
}

/* A transferred entry remains an exact borrow-parent identity, but is no
 * longer an executable StackRef primary. Its sole Python backing is the real
 * frame tuple item; this ledger never INCREFs or visits another object edge. */
static void
check_stackref_not_transferred(TableEntry *entry, _PyStackRef ref,
                              const char *operation)
{
    if (entry->overwritten.frame != NULL) {
        _Py_FatalErrorFormat(operation,
            "StackRef with ID %" PRIu64
            " was transferred to overwritten frame tuple item %zd",
            ref.index, entry->overwritten.index);
    }
}


PyObject *
_Py_stackref_get_object(_PyStackRef ref)
{
    assert(!PyStackRef_IsError(ref));
    if (ref.index == 0) {
        return NULL;
    }
    PyInterpreterState *interp = PyInterpreterState_Get();
    assert(interp != NULL);
    if (ref.index >= interp->next_stackref) {
        _Py_FatalErrorFormat(__func__,
            "Garbled stack ref with ID %" PRIu64 "\n", ref.index);
    }
    TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
    if (entry == NULL) {
        _Py_FatalErrorFormat(__func__,
            "Accessing closed stack ref with ID %" PRIu64 "\n", ref.index);
    }
    check_stackref_not_transferred(entry, ref, __func__);
    return entry->obj;
}

int
PyStackRef_Is(_PyStackRef a, _PyStackRef b)
{
    return _Py_stackref_get_object(a) == _Py_stackref_get_object(b);
}

PyObject *
_Py_stackref_close(_PyStackRef ref, const char *filename, int linenumber)
{
    assert(!PyStackRef_IsError(ref));
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (ref.index >= interp->next_stackref) {
        _Py_FatalErrorFormat(__func__,
            "Invalid StackRef with ID %" PRIu64 " at %s:%d\n",
            ref.index, filename, linenumber);
    }
    PyObject *obj;
    if (ref.index < INITIAL_STACKREF_INDEX) {
        if (ref.index == 0) {
            _Py_FatalErrorFormat(__func__,
                "Passing NULL to _Py_stackref_close at %s:%d\n",
                filename, linenumber);
        }
        // Pre-allocated reference to None, False or True -- Do not clear
        TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
        obj = entry->obj;
    }
    else {
        TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
        if (entry != NULL) {
            /* A moved handle cannot be stolen out of its frame ledger. */
            check_stackref_not_transferred(entry, ref, __func__);
        }
        entry = _Py_hashtable_steal(interp->open_stackrefs_table, (void *)ref.index);
        if (entry == NULL) {
#ifdef Py_STACKREF_CLOSE_DEBUG
            entry = _Py_hashtable_get(interp->closed_stackrefs_table, (void *)ref.index);
            if (entry != NULL) {
                _Py_FatalErrorFormat(__func__,
                    "Double close of ref ID %" PRIu64 " at %s:%d. Referred to instance of %s at %p. Closed at %s:%d\n",
                    ref.index, filename, linenumber, entry->classname, entry->obj, entry->filename, entry->linenumber);
            }
#endif
            _Py_FatalErrorFormat(__func__,
                "Invalid StackRef with ID %" PRIu64 " at %s:%d\n",
                ref.index, filename, linenumber);
        }
        if (!PyStackRef_IsNull(entry->borrowed_from)) {
            _PyStackRef borrowed_from = entry->borrowed_from;
            TableEntry *entry_borrowed = _Py_hashtable_get(interp->open_stackrefs_table, (void *)borrowed_from.index);
            if (entry_borrowed == NULL) {
                _Py_FatalErrorFormat(__func__,
                    "Invalid borrowed StackRef with ID %" PRIu64 " at %s:%d\n",
                    borrowed_from.index, filename, linenumber);
            }
            entry_borrowed->borrows--;
        }
        if (entry->borrows > 0) {
            _Py_FatalErrorFormat(__func__,
                "StackRef with ID %" PRIu64 " closed with %d borrowed refs at %s:%d. Opened at %s:%d\n",
                ref.index, entry->borrows, filename, linenumber, entry->filename, entry->linenumber);
        }
        obj = entry->obj;
        free(entry);
#ifdef Py_STACKREF_CLOSE_DEBUG
        TableEntry *close_entry = make_table_entry(obj, filename, linenumber);
        if (close_entry == NULL) {
            Py_FatalError("No memory left for stackref debug table");
        }
        if (_Py_hashtable_set(interp->closed_stackrefs_table, (void *)ref.index, close_entry) < 0) {
            Py_FatalError("No memory left for stackref debug table");
        }
#endif
    }
    return obj;
}

_PyStackRef
_Py_stackref_create(PyObject *obj, uint16_t flags, const char *filename, int linenumber)
{
    if (obj == NULL) {
        Py_FatalError("Cannot create a stackref for NULL");
    }
    PyInterpreterState *interp = PyInterpreterState_Get();
    uint64_t new_id = interp->next_stackref;
    interp->next_stackref = new_id + (1 << Py_TAGGED_SHIFT);
    TableEntry *entry = make_table_entry(obj, filename, linenumber);
    if (entry == NULL) {
        Py_FatalError("No memory left for stackref debug table");
    }
    new_id |= flags;
    if (_Py_hashtable_set(interp->open_stackrefs_table, (void *)new_id, entry) < 0) {
        Py_FatalError("No memory left for stackref debug table");
    }
    return (_PyStackRef){ .index = new_id };
}

void
_Py_stackref_record_borrow(_PyStackRef ref, const char *filename, int linenumber)
{
    assert(!PyStackRef_IsError(ref));
    if (ref.index < INITIAL_STACKREF_INDEX) {
        return;
    }
    PyInterpreterState *interp = PyInterpreterState_Get();
    TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
    if (entry == NULL) {
#ifdef Py_STACKREF_CLOSE_DEBUG
        entry = _Py_hashtable_get(interp->closed_stackrefs_table, (void *)ref.index);
        if (entry != NULL) {
            _Py_FatalErrorFormat(__func__,
                "Borrow of closed ref ID %" PRIu64 " at %s:%d. Referred to instance of %s at %p. Closed at %s:%d\n",
                ref.index, filename, linenumber, entry->classname, entry->obj, entry->filename, entry->linenumber);
        }
#endif
        _Py_FatalErrorFormat(__func__,
            "Invalid StackRef with ID %" PRIu64 " at %s:%d\n",
            ref.index, filename, linenumber);
    }
    check_stackref_not_transferred(entry, ref, __func__);
    entry->filename_borrow = filename;
    entry->linenumber_borrow = linenumber;
}

_PyStackRef
_Py_stackref_get_borrowed_from(_PyStackRef ref, const char *filename, int linenumber)
{
    assert(!PyStackRef_IsError(ref));
    PyInterpreterState *interp = PyInterpreterState_Get();

    TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
    if (entry == NULL) {
        _Py_FatalErrorFormat(__func__,
            "Invalid StackRef with ID %" PRIu64 " at %s:%d\n",
            ref.index, filename, linenumber);
    }

    check_stackref_not_transferred(entry, ref, __func__);
    return entry->borrowed_from;
}

// This function should be used no more than once per ref.
void
_Py_stackref_set_borrowed_from(_PyStackRef ref, _PyStackRef borrowed_from, const char *filename, int linenumber)
{
    assert(!PyStackRef_IsError(ref));
    PyInterpreterState *interp = PyInterpreterState_Get();

    TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table, (void *)ref.index);
    if (entry == NULL) {
        _Py_FatalErrorFormat(__func__,
            "Invalid StackRef (ref) with ID %" PRIu64 " at %s:%d\n",
            ref.index, filename, linenumber);
    }

    check_stackref_not_transferred(entry, ref, __func__);
    assert(PyStackRef_IsNull(entry->borrowed_from));
    if (PyStackRef_IsNull(borrowed_from)) {
        return;
    }

    TableEntry *entry_borrowed = _Py_hashtable_get(interp->open_stackrefs_table, (void *)borrowed_from.index);
    if (entry_borrowed == NULL) {
        _Py_FatalErrorFormat(__func__,
            "Invalid StackRef (borrowed_from) with ID %" PRIu64 " at %s:%d\n",
            borrowed_from.index, filename, linenumber);
    }

    entry->borrowed_from = borrowed_from;
    entry_borrowed->borrows++;
}

void
_Py_stackref_transfer_overwritten_local(PyFrameObject *frame,
                                        Py_ssize_t native_fast_slot,
                                        const char *filename, int linenumber)
{
    if (frame == NULL || frame->f_frame == NULL) {
        _Py_FatalErrorFormat(__func__, "missing overwritten-local frame at %s:%d",
                            filename, linenumber);
    }
    PyCodeObject *code = _PyFrame_GetCode(frame->f_frame);
    if (native_fast_slot < 0 || native_fast_slot >= code->co_nlocalsplus) {
        _Py_FatalErrorFormat(__func__, "invalid overwritten-local slot at %s:%d",
                            filename, linenumber);
    }
    _PyStackRef *slot = _PyFrame_GetLocalsArray(frame->f_frame) + native_fast_slot;
    _PyStackRef ref = *slot;
    PyInterpreterState *interp = PyInterpreterState_Get();
    TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table,
                                         (void *)ref.index);
    if (ref.index < INITIAL_STACKREF_INDEX || entry == NULL ||
        entry->borrows < 0) {
        _Py_FatalErrorFormat(__func__,
            "invalid overwritten-local StackRef with ID %" PRIu64 " at %s:%d",
            ref.index, filename, linenumber);
    }
    check_stackref_not_transferred(entry, ref, __func__);

    PyObject *tuple = frame->f_overwritten_fast_locals;
    if (tuple == NULL || !PyTuple_CheckExact(tuple) || PyTuple_GET_SIZE(tuple) < 1) {
        _Py_FatalErrorFormat(__func__, "missing actual overwritten-local tuple at %s:%d",
                            filename, linenumber);
    }
    Py_ssize_t index = PyTuple_GET_SIZE(tuple) - 1;
    struct _PyStackRefFrameTupleEntry *previous = frame->f_overwritten_fast_locals_debug;
    if (PyTuple_GET_ITEM(tuple, index) != entry->obj ||
        (previous == NULL && index != 0) ||
        (previous != NULL &&
         (previous->frame != frame || previous->index != index - 1))) {
        _Py_FatalErrorFormat(__func__, "overwritten-local tuple backing mismatch at %s:%d",
                            filename, linenumber);
    }

    TableEntry *parent = NULL;
    if (!PyStackRef_IsNull(entry->borrowed_from)) {
        parent = _Py_hashtable_get(interp->open_stackrefs_table,
                                   (void *)entry->borrowed_from.index);
        if (parent == NULL || parent == entry || parent->obj != entry->obj ||
            parent->borrows <= 0) {
            _Py_FatalErrorFormat(__func__,
                "invalid overwritten-local borrow parent for ID %" PRIu64 " at %s:%d",
                ref.index, filename, linenumber);
        }
    }
    bool decref_object = PyStackRef_RefcountOnObject(ref);
    PyObject *obj = entry->obj;
    if (decref_object && Py_REFCNT(obj) < 2) {
        _Py_FatalErrorFormat(__func__, "overwritten-local tuple did not acquire its object at %s:%d",
                            filename, linenumber);
    }

    /* Reuse the already-allocated diagnostic entry. Tuple allocation, element
     * INCREF, publication and old-tuple retirement have already succeeded. */
    entry->overwritten = (struct _PyStackRefFrameTupleEntry){
        .next = previous,
        .frame = frame,
        .index = index,
        .reference = ref,
    };
    frame->f_overwritten_fast_locals_debug = &entry->overwritten;
    *slot = PyStackRef_NULL;
    if (parent != NULL) {
        /* The tuple now backs this identity. Keep all of its children, but
         * do not keep an artificial borrow of an ancestor's original owner. */
        parent->borrows--;
        entry->borrowed_from = PyStackRef_NULL;
    }
    if (decref_object) {
        /* Exactly the old CLOSE's reference-count effect at the same phase.
         * The new tuple edge prevents a Python finalizer here. */
        Py_DECREF(obj);
    }
}

void
_Py_stackref_release_overwritten_locals(PyFrameObject *frame,
                                       const char *filename, int linenumber)
{
    PyObject *tuple = frame->f_overwritten_fast_locals;
    struct _PyStackRefFrameTupleEntry *head = frame->f_overwritten_fast_locals_debug;
    if (tuple == NULL) {
        if (head != NULL) {
            _Py_FatalErrorFormat(__func__, "overwritten-local ledger lost its tuple at %s:%d",
                                filename, linenumber);
        }
        return;
    }
    if (!PyTuple_CheckExact(tuple)) {
        _Py_FatalErrorFormat(__func__, "invalid overwritten-local tuple at %s:%d",
                            filename, linenumber);
    }
    PyInterpreterState *interp = PyInterpreterState_Get();
    Py_ssize_t expected = PyTuple_GET_SIZE(tuple) - 1;
    for (struct _PyStackRefFrameTupleEntry *node = head; node != NULL; node = node->next) {
        TableEntry *entry = _Py_hashtable_get(interp->open_stackrefs_table,
                                             (void *)node->reference.index);
        if (expected < 0 || entry == NULL || &entry->overwritten != node ||
            node->frame != frame || node->index != expected ||
            PyTuple_GET_ITEM(tuple, expected) != entry->obj ||
            !PyStackRef_IsNull(entry->borrowed_from)) {
            _Py_FatalErrorFormat(__func__, "invalid overwritten-local ledger backing at %s:%d",
                                filename, linenumber);
        }
        if (entry->borrows != 0) {
            _Py_FatalErrorFormat(__func__,
                "overwritten-local StackRef with ID %" PRIu64
                " released with %d borrowed refs at %s:%d. Opened at %s:%d",
                node->reference.index, entry->borrows, filename, linenumber,
                entry->filename, entry->linenumber);
        }
        expected--;
    }
    if (expected != -1) {
        _Py_FatalErrorFormat(__func__, "overwritten-local tuple has unrecorded owners at %s:%d",
                            filename, linenumber);
    }

    /* Validate the whole old ledger first. Detach before the existing tuple
     * DECREF can run finalizers that publish a new tuple and a new ledger. */
    frame->f_overwritten_fast_locals_debug = NULL;
    while (head != NULL) {
        struct _PyStackRefFrameTupleEntry *next = head->next;
        _PyStackRef ref = head->reference;
        head->frame = NULL;  /* Permit only this already-validated release. */
        head->next = NULL;
        head->index = -1;
        head->reference = PyStackRef_NULL;
        (void)_Py_stackref_close(ref, filename, linenumber);
        /* No DECREF: the unchanged following Py_CLEAR(tuple) owns that duty. */
        head = next;
    }
}

void
_Py_stackref_associate(PyInterpreterState *interp, PyObject *obj, _PyStackRef ref)
{
    assert(!PyStackRef_IsError(ref));
    assert(ref.index < INITIAL_STACKREF_INDEX);
    TableEntry *entry = make_table_entry(obj, "builtin-object", 0);
    if (entry == NULL) {
        Py_FatalError("No memory left for stackref debug table");
    }
    if (_Py_hashtable_set(interp->open_stackrefs_table, (void *)ref.index, (void *)entry) < 0) {
        Py_FatalError("No memory left for stackref debug table");
    }
}


static int
report_leak(_Py_hashtable_t *ht, const void *key, const void *value, void *leak)
{
    TableEntry *entry = (TableEntry *)value;
    if (!_Py_IsStaticImmortal(entry->obj)) {
        *(int *)leak = 1;
        printf("Stackref leak. Refers to instance of %s at %p. Created at %s:%d",
               entry->classname, entry->obj, entry->filename, entry->linenumber);
        if (entry->filename_borrow != NULL) {
            printf(". Last borrow at %s:%d",entry->filename_borrow, entry->linenumber_borrow);
        }
        printf("\n");
    }
    return 0;
}

void
_Py_stackref_report_leaks(PyInterpreterState *interp)
{
    int leak = 0;
    _Py_hashtable_foreach(interp->open_stackrefs_table, report_leak, &leak);
    if (leak) {
        fflush(stdout);
        Py_FatalError("Stackrefs leaked.");
    }
}

_PyStackRef PyStackRef_TagInt(intptr_t i)
{
    assert(Py_ARITHMETIC_RIGHT_SHIFT(intptr_t, (i << Py_TAGGED_SHIFT), Py_TAGGED_SHIFT) == i);
    return (_PyStackRef){ .index = (i << Py_TAGGED_SHIFT) | Py_INT_TAG };
}

intptr_t
PyStackRef_UntagInt(_PyStackRef i)
{
    assert(PyStackRef_IsTaggedInt(i));
    intptr_t val = (intptr_t)i.index;
    return Py_ARITHMETIC_RIGHT_SHIFT(intptr_t, val, Py_TAGGED_SHIFT);
}

bool
PyStackRef_IsNullOrInt(_PyStackRef ref)
{
    return PyStackRef_IsNull(ref) || PyStackRef_IsTaggedInt(ref);
}

_PyStackRef
PyStackRef_IncrementTaggedIntNoOverflow(_PyStackRef ref)
{
    assert(PyStackRef_IsTaggedInt(ref));
    assert((ref.index & (~Py_TAG_BITS)) != (INTPTR_MAX & (~Py_TAG_BITS))); // Isn't about to overflow
    return (_PyStackRef){ .index = ref.index + (1 << Py_TAGGED_SHIFT) };
}


#endif
