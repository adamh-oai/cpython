#ifndef Py_CPYTHON_DICTOBJECT_H
#  error "this header file must not be included directly"
#endif

typedef struct _dictkeysobject PyDictKeysObject;
typedef struct _dictvalues PyDictValues;

/* The ma_values pointer is NULL for a combined table
 * or points to an array of PyObject* for a split table
 */
typedef struct {
    PyObject_HEAD

    /* Number of items in the dictionary */
    Py_ssize_t ma_used;

    /* This is a private field for CPython's internal use.
     * Bits 0-7 are for dict watchers.
     * Bits 8-11 are for the watched mutation counter (used by tier2 optimization)
     * Bit 12 marks a permanently owned SOAC dictionary policy.
     * Bit 13 records a noncanonical lookup alias in stable-prefix storage.
     * Bit 14 blocks first policy installation during an in-place split clear.
     * Bits 15-17 hold attachment-local mutation/terminal/installing state for optional
     * direct type-state dictionaries; the immutable state can be shared.
     * Bits 18-31 are currently unused
     * Bits 32-63 are a unique id in the free threading build (used for per-thread refcounting)
     */
    uint64_t _ma_watcher_tag;

    PyDictKeysObject *ma_keys;

    /* If ma_values is NULL, the table is "combined": keys and values
       are stored in ma_keys.

       If ma_values is not NULL, the table is split:
       keys are stored in ma_keys and values are stored in ma_values */
    PyDictValues *ma_values;
} PyDictObject;

/* SOAC policies are native-owned write barriers, not dictionary watchers.
 * Successful installation is permanent; there is no replacement/unseal API.
 * The owner is a strong, GC-visible edge of this exact dictionary.  Callbacks
 * validate before commit, return 0 or -1 with an exception, and may not mutate
 * this dictionary.  SET means a currently absent binding; SET_EXISTING means
 * an existing binding or an earlier write in the same staged bulk input.
 * VALIDATE_INITIAL checks existing contents before installation succeeds.
 * provenance is NULL for mapping writes.  Private CACHE_INSERT/CACHE_REPLACE
 * carry their provider; ATTRIBUTE_SET/ATTRIBUTE_SET_EXISTING carry the original
 * Unicode attribute name, separately from the once-resolved canonical key.
 * CLASS_MEMBER_INSERT/CLASS_MEMBER_REPLACE carry one opaque native dataclass
 * member operation. They are never emitted by mapping or attribute writes;
 * the exact operation is checked again after the last dictionary watcher.
 *
 * TERMINAL_TEARDOWN is an irreversible notification from unreachable GC or
 * module destruction: the owner must make dependent execution unavailable
 * before returning.  It must not fail or execute Python.  The dictionary
 * remains protected, and all subsequent public writes fail.
 */
enum {
    PyDict_SOAC_VALIDATE_INITIAL = 0,
    PyDict_SOAC_SET = 1,
    PyDict_SOAC_DELETE = 2,
    PyDict_SOAC_CLEAR = 3,
    PyDict_SOAC_TERMINAL_TEARDOWN = 4,
    PyDict_SOAC_SET_EXISTING = 5,
    PyDict_SOAC_CACHE_INSERT = 6,
    PyDict_SOAC_CACHE_REPLACE = 7,
    PyDict_SOAC_ATTRIBUTE_SET = 8,
    PyDict_SOAC_ATTRIBUTE_SET_EXISTING = 9,
    PyDict_SOAC_CLASS_MEMBER_INSERT = 10,
    PyDict_SOAC_CLASS_MEMBER_REPLACE = 11,
    /* Only type_ready's explicit canonical physical-member publication.
     * Provenance is that exact native descriptor, checked after watchers. */
    PyDict_SOAC_SLOT_DESCRIPTOR_INSERT = 12,
    PyDict_SOAC_SLOT_DESCRIPTOR_REPLACE = 13,
    /* Admission-only liveness check before an ordinary empty-dict bulk clone.
     * key/value/provenance are NULL; no per-key hash/equality is repeated. */
    PyDict_SOAC_CLONE = 14
};
#define PyDict_SOAC_ALLOW_NONSTRING_KEYS 1u
#define PyDict_SOAC_READ_ONLY 2u
/* Authentication/lifetime ownership without namespace or field restrictions.
 * Mutually exclusive with the other modes; requires ordinary dict storage.
 * The trusted owner validates liveness at resolved write commits, not keys or
 * values. Installation is permanent and this mode cannot be sealed. */
#define PyDict_SOAC_ADMISSION_ONLY 4u
typedef int (*PyDict_SoacPolicyCallback)(
    PyObject *owner, PyObject *dict, PyObject *key, PyObject *value,
    int operation, PyObject *provenance);
/* ABI4 type-contract metadata factory view. The forward typedef is in
 * cpython/object.h. Existing inputs borrow the actual dictionary's metadata;
 * output owner is a new reference transferred to the native transaction.
 * This adds no dictionary/value/instance owner and no source authority. */
struct _PySoacInstanceDictPolicy {
    PyObject *owner;
    PyDict_SoacPolicyCallback validate;
};

/* Immutable storage rules prepared from actual native type bindings. The
 * callback sees resolved rules, not a receiver whose MRO must be rediscovered.
 * NULL value denotes deletion; canonical_name is the existing Unicode payload. */
typedef int (*PyTypeStateFieldCheckV1)(
    PyObject *rule_owner, PyObject *canonical_name, PyObject *value);

typedef struct {
    /* Borrowed only while NewV1 validates the actual declaring catalogue. */
    PyObject *expected_class_owner;
    Py_ssize_t field_index;
    Py_ssize_t offset;
    PyObject *canonical_name;
    PyObject *rule_owner;
    PyTypeStateFieldCheckV1 validate;
} PyTypeStateSlotSpecV1;

#define Py_TYPE_STATE_ABI_V1 1u
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    /* Dictionary-only metadata: do not retain unrelated native-slot rules.
     * All three are NULL when there are no dictionary obligations. */
    PyObject *dictionary_owner;
    PyDict_SoacPolicyCallback validate_dictionary;
    PyTypeStateFieldCheckV1 validate_inline;
    Py_ssize_t slot_count;
    const PyTypeStateSlotSpecV1 *slots;
} PyTypeStateSpecV1;

/* Copies the complete actual native slot projection and retains only rule
 * owners/names. The returned instance state owns a separately shareable
 * dictionary projection; an escaped dict need not retain slot-only owners.
 * No receiver/type backedge is added solely for finding the policy. Returns
 * one owned native PyObject-compatible state reference, or NULL with error. */
PyAPI_FUNC(PyTypeState *) PyTypeState_NewV1(
    PyObject *actual_type, const PyTypeStateSpecV1 *spec, size_t spec_size);
PyAPI_FUNC(int) PyDict_SetSoacPolicy(
    PyObject *dict, PyObject *owner, PyDict_SoacPolicyCallback validate,
    unsigned int flags);
PyAPI_FUNC(int) PyDict_SealSoacNamespace(PyObject *dict);
PyAPI_FUNC(int) PyDict_HasSoacPolicy(PyObject *dict);
PyAPI_FUNC(int) _PyDict_HasSoacBindingPolicy(PyObject *dict);
/* Liveness only, not successful-registration or owner authentication. A
   caller must already prove the read-only policy completed installation. */
PyAPI_FUNC(int) _PyDict_HasLiveSoacReadOnlyPolicy(PyObject *dict);
PyAPI_FUNC(int) PyDict_MatchesSoacPolicy(
    PyObject *dict, PyObject *owner, PyDict_SoacPolicyCallback validate,
    unsigned int flags);
/* Authenticate the actual class namespace against its private native policy
 * and expected interpreter-owned contract owner. The expected pointer is
 * compared, never dereferenced. Returns 1 for a match, 0 for an unrelated
 * dictionary/owner, and -1 with an exception for a terminal or unavailable
 * native class contract. */
PyAPI_FUNC(int) PyDict_MatchesSoacClassNamespace(
    PyObject *dict, PyObject *expected_owner);

PyAPI_FUNC(PyObject *) _PyDict_GetItem_KnownHash(PyObject *mp, PyObject *key,
                                                 Py_hash_t hash);
// PyDict_GetItemStringRef() can be used instead
Py_DEPRECATED(3.14) PyAPI_FUNC(PyObject *) _PyDict_GetItemStringWithError(PyObject *, const char *);
PyAPI_FUNC(PyObject *) PyDict_SetDefault(
    PyObject *mp, PyObject *key, PyObject *defaultobj);

/* Get the number of items of a dictionary. */
static inline Py_ssize_t PyDict_GET_SIZE(PyObject *op) {
    PyDictObject *mp;
    assert(PyDict_Check(op));
    mp = _Py_CAST(PyDictObject*, op);
#ifdef Py_GIL_DISABLED
    return _Py_atomic_load_ssize_relaxed(&mp->ma_used);
#else
    return mp->ma_used;
#endif
}
#define PyDict_GET_SIZE(op) PyDict_GET_SIZE(_PyObject_CAST(op))

PyAPI_FUNC(int) PyDict_ContainsString(PyObject *mp, const char *key);

PyAPI_FUNC(PyObject *) _PyDict_NewPresized(Py_ssize_t minused);

PyAPI_FUNC(int) PyDict_Pop(PyObject *dict, PyObject *key, PyObject **result);
PyAPI_FUNC(int) PyDict_PopString(PyObject *dict, const char *key, PyObject **result);

// Use PyDict_Pop() instead
Py_DEPRECATED(3.14) PyAPI_FUNC(PyObject *) _PyDict_Pop(
    PyObject *dict,
    PyObject *key,
    PyObject *default_value);

/* Dictionary watchers */

#define PY_FOREACH_DICT_EVENT(V) \
    V(ADDED)                     \
    V(MODIFIED)                  \
    V(DELETED)                   \
    V(CLONED)                    \
    V(CLEARED)                   \
    V(DEALLOCATED)

typedef enum {
    #define PY_DEF_EVENT(EVENT) PyDict_EVENT_##EVENT,
    PY_FOREACH_DICT_EVENT(PY_DEF_EVENT)
    #undef PY_DEF_EVENT
} PyDict_WatchEvent;

// Callback to be invoked when a watched dict is cleared, dealloced, or modified.
// In clear/dealloc case, key and new_value will be NULL. Otherwise, new_value will be the
// new value for key, NULL if key is being deleted.
typedef int(*PyDict_WatchCallback)(PyDict_WatchEvent event, PyObject* dict, PyObject* key, PyObject* new_value);

// Register/unregister a dict-watcher callback
PyAPI_FUNC(int) PyDict_AddWatcher(PyDict_WatchCallback callback);
PyAPI_FUNC(int) PyDict_ClearWatcher(int watcher_id);

// Mark given dictionary as "watched" (callback will be called if it is modified)
PyAPI_FUNC(int) PyDict_Watch(int watcher_id, PyObject* dict);
PyAPI_FUNC(int) PyDict_Unwatch(int watcher_id, PyObject* dict);
