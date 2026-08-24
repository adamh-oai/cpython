#ifndef Py_INTERNAL_DICT_H
#define Py_INTERNAL_DICT_H
#ifdef __cplusplus
extern "C" {
#endif

#define _PyDict_SOAC_POLICY_TAG (UINT64_C(1) << 12)
#define _PyDict_SOAC_LOOKUP_ALIASES_TAG (UINT64_C(1) << 13)
#define _PyDict_SOAC_SPLIT_CLEAR_TAG (UINT64_C(1) << 14)
#ifdef Py_GIL_DISABLED
#define _PyDict_HasSoacPolicy(mp) (0)
#else
#define _PyDict_HasSoacPolicy(mp) \
    (((mp)->_ma_watcher_tag & _PyDict_SOAC_POLICY_TAG) != 0)
#endif

/* Only unreachable GC and terminal module cleanup may use these.  Neither
   helper makes normal/public writes legal again. */
extern void _PyDict_SoacBeginTeardown(PyObject *dict);
/* GenericAlloc only: initialize an unpublished, untracked, zeroed instance.
   Failure leaves its dictionary pointer NULL and preserves the exception. */
extern int _PyDict_InitSoacInstanceStorage(PyObject *obj);
extern void _PyDict_ClearForTeardown(PyObject *dict);
extern int _PyDict_SetItemForTeardown(
    PyObject *dict, PyObject *key, PyObject *value);
/* Owner-validated native memoization.  The provider is explicit provenance,
   never a temporary permission flag available to reentrant public writes. */
extern int _PyDict_SetItemForRuntimeCache(
    PyObject *dict, PyObject *key, PyObject *value, PyObject *provider);
/* One exact native member operation, restricted to the actual class namespace.
   The caller holds the type/dict lock. No ordinary-dictionary fallback. */
extern int _PyDict_SetItemForSoacDataclassMember(
    PyObject *dict, PyObject *key, PyObject *value, PyObject *operation,
    PyObject *expected_class_owner);
/* type_ready only: insert the descriptor for an already installed native
   object-slot catalog. No ambient construction or ordinary-dict fallback. */
extern int _PyDict_SetItemForSoacSlotDescriptor(
    PyObject *dict, PyObject *key, PyObject *descriptor,
    PyObject *expected_class_owner);
/* Attribute provenance belongs to this one lookup/validation/commit, not a
   precheck or ambient permit.  Mapping writes never emit ATTRIBUTE_SET. */
PyAPI_FUNC(int) _PyDict_SetItemForAttribute(
    PyObject *dict, PyObject *name, PyObject *value);

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_object.h"               // PyManagedDictPointer
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_LOAD_SSIZE_ACQUIRE
#include "pycore_stackref.h"             // _PyStackRef
#include "pycore_stats.h"

// Unsafe flavor of PyDict_GetItemWithError(): no error checking
extern PyObject* _PyDict_GetItemWithError(PyObject *dp, PyObject *key);

// Delete an item from a dict if a predicate is true
// Returns -1 on error, 1 if the item was deleted, 0 otherwise
// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyDict_DelItemIf(PyObject *mp, PyObject *key,
                                  int (*predicate)(PyObject *value, void *arg),
                                  void *arg);

// "KnownHash" variants
// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                          PyObject *item, Py_hash_t hash);
// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyDict_DelItem_KnownHash(PyObject *mp, PyObject *key,
                                          Py_hash_t hash);

extern int _PyDict_DelItem_KnownHash_LockHeld(PyObject *mp, PyObject *key,
                                              Py_hash_t hash);

extern int _PyDict_Contains_KnownHash(PyObject *, PyObject *, Py_hash_t);

extern int _PyDict_Next(
    PyObject *mp, Py_ssize_t *pos, PyObject **key, PyObject **value, Py_hash_t *hash);

extern int _PyDict_HasOnlyStringKeys(PyObject *mp);

// Export for '_ctypes' shared extension
PyAPI_FUNC(Py_ssize_t) _PyDict_SizeOf(PyDictObject *);

extern Py_ssize_t _PyDict_SizeOf_LockHeld(PyDictObject *);

#define _PyDict_HasSplitTable(d) ((d)->ma_values != NULL)
#define DK_IS_INDEXED(dk) \
    ((dk)->dk_kind == DICT_KEYS_INDEXED_UNICODE || \
     (dk)->dk_kind == DICT_KEYS_INDEXED_GENERAL)
#define _PyDict_HasIndexedTable(d) \
    ((d)->ma_values != NULL && DK_IS_INDEXED((d)->ma_keys))

/* Like PyDict_Merge, but override can be 0, 1 or 2.  If override is 0,
   the first occurrence of a key wins, if override is 1, the last occurrence
   of a key wins, if override is 2, a KeyError with conflicting key as
   argument is raised.
*/
PyAPI_FUNC(int) _PyDict_MergeEx(PyObject *mp, PyObject *other, int override);

extern void _PyDict_DebugMallocStats(FILE *out);


/* _PyDictView */

typedef struct {
    PyObject_HEAD
    PyDictObject *dv_dict;
} _PyDictViewObject;

extern PyObject* _PyDictView_New(PyObject *, PyTypeObject *);
extern PyObject* _PyDictView_Intersect(PyObject* self, PyObject *other);

/* other API */

typedef struct {
    /* Cached hash code of me_key. */
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value; /* This field is only meaningful for combined tables */
} PyDictKeyEntry;

typedef struct {
    PyObject *me_key;   /* The key must be Unicode and have hash. */
    PyObject *me_value; /* This field is only meaningful for combined tables */
} PyDictUnicodeEntry;

extern PyDictKeysObject *_PyDict_NewKeysForClass(PyHeapTypeObject *);
extern PyObject *_PyDict_FromKeys(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(PyDictKeysObject *) _PyDict_NewIndexedKeySet(PyObject *keys);
PyAPI_FUNC(PyObject *) _PyDict_NewWithIndexedKeySet(PyDictKeysObject *keys);
/* Fresh empty storage with the same immutable prefix, no copied policy,
   values, or overflow.  The template is an ordinary GC-owned Python edge. */
PyAPI_FUNC(PyObject *) _PyDict_NewFromIndexedSchema(PyObject *template);
PyAPI_FUNC(Py_ssize_t) _PyDict_IndexedKeyIndex(PyObject *dict, PyObject *key);
/* Recheck this positive guard after effects before interpreting a physical
   prefix slot as a Python lookup.  Stored keys may change equality without a
   dictionary mutation.  Once aliases are possible, the flag stays set. */
PyAPI_FUNC(int) _PyDict_HasNoLookupAliases(PyObject *dict);
/* Native owner initialization only: reserve invisible stable names before
   sealing, never replace/revoke a policy or expose a Python-visible setter. */
PyAPI_FUNC(int) _PyDict_ReserveSoacNamespaceKeys(
    PyObject *dict, PyObject *owner, PyObject *names);
/* Module annotation setters only: 1 success, 0 missing primary delete,
   -1 error.  Once sealed, only a primary first insert with absent companion
   is supported: replacement/deletion could run finalizers that introduce a
   second binding. Unsealed initialization keeps ordinary sequential behavior. */
PyAPI_FUNC(int) _PyDict_SetItemAndDeleteForModule(
    PyObject *dict, PyObject *primary_key, PyObject *value, PyObject *companion_key);
PyAPI_FUNC(int) _PyDict_GetIndexedItem(
        PyObject *dict, Py_ssize_t index, PyObject **result);
PyAPI_FUNC(int) _PyDict_SetIndexedItem(
        PyObject *dict, Py_ssize_t index, PyObject *value);
PyAPI_FUNC(int) _PyDict_WatchSplitKeysForType(PyObject *type);
PyAPI_FUNC(PyObject *) _PyDict_GetKeyLayoutEvents(void);
PyAPI_DATA(char) _PyDict_IndexedValueTombstone;

/* Gets a version number unique to the current state of the keys of dict, if possible.
 * Returns the version number, or zero if it was not possible to get a version number. */
extern uint32_t _PyDictKeys_GetVersionForCurrentState(
        PyInterpreterState *interp, PyDictKeysObject *dictkeys);

/* Gets a version number unique to the current state of the keys of dict, if possible.
 *
 * In free-threaded builds ensures that the dict can be used for lock-free
 * reads if a version was assigned.
 *
 * The caller must hold the per-object lock on dict.
 *
 * Returns the version number, or zero if it was not possible to get a version number. */
extern uint32_t _PyDict_GetKeysVersionForCurrentState(
        PyInterpreterState *interp, PyDictObject *dict);

extern size_t _PyDict_KeysSize(PyDictKeysObject *keys);

PyAPI_FUNC(void) _PyDictKeys_DecRef(PyDictKeysObject *keys);

/* _Py_dict_lookup() returns index of entry which can be used like DK_ENTRIES(dk)[index].
 * -1 when no entry found, -3 when compare raises error.
 */
extern Py_ssize_t _Py_dict_lookup(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr);
extern Py_ssize_t _Py_dict_lookup_threadsafe(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr);
extern Py_ssize_t _Py_dict_lookup_threadsafe_stackref(PyDictObject *mp, PyObject *key, Py_hash_t hash, _PyStackRef *value_addr);

extern int _PyDict_GetMethodStackRef(PyDictObject *dict, PyObject *name, _PyStackRef *method);

extern Py_ssize_t _PyDict_LookupIndexAndValue(PyDictObject *, PyObject *, PyObject **);
extern Py_ssize_t _PyDict_LookupIndex(PyDictObject *, PyObject *);
extern Py_ssize_t _PyDictKeys_StringLookup(PyDictKeysObject* dictkeys, PyObject *key);

/* Look up a string key in an all unicode dict keys, assign the keys object a version, and
 * store it in version.
 *
 * Returns DKIX_ERROR if key is not a string or if the keys object is not all
 * strings.
 *
 * Returns DKIX_EMPTY if the key is not present.
 */
extern Py_ssize_t _PyDictKeys_StringLookupAndVersion(PyDictKeysObject* dictkeys, PyObject *key, uint32_t *version);
extern Py_ssize_t _PyDictKeys_StringLookupSplit(PyDictKeysObject* dictkeys, PyObject *key);
PyAPI_FUNC(PyObject *)_PyDict_LoadGlobal(PyDictObject *, PyDictObject *, PyObject *);
PyAPI_FUNC(void) _PyDict_LoadGlobalStackRef(PyDictObject *, PyDictObject *, PyObject *, _PyStackRef *);

// Loads the __builtins__ object from the globals dict. Returns a new reference.
extern PyObject *_PyDict_LoadBuiltinsFromGlobals(PyObject *globals);

/* Consumes references to key and value */
PyAPI_FUNC(int) _PyDict_SetItem_Take2(PyDictObject *op, PyObject *key, PyObject *value);
extern int _PyDict_SetItem_LockHeld(PyDictObject *dict, PyObject *name, PyObject *value);
// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash_LockHeld(PyDictObject *mp, PyObject *key,
                                                   PyObject *value, Py_hash_t hash);
// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyDict_GetItemRef_KnownHash_LockHeld(PyDictObject *op, PyObject *key, Py_hash_t hash, PyObject **result);
extern int _PyDict_GetItemRef_KnownHash(PyDictObject *op, PyObject *key, Py_hash_t hash, PyObject **result);
extern int _PyDict_GetItemRef_Unicode_LockHeld(PyDictObject *op, PyObject *key, PyObject **result);
PyAPI_FUNC(int) _PyObjectDict_SetItem(PyTypeObject *tp, PyObject *obj, PyObject **dictptr, PyObject *name, PyObject *value);

extern int _PyDict_Pop_KnownHash(
    PyDictObject *dict,
    PyObject *key,
    Py_hash_t hash,
    PyObject **result);

extern void _PyDict_Clear_LockHeld(PyObject *op);

#ifdef Py_GIL_DISABLED
PyAPI_FUNC(void) _PyDict_EnsureSharedOnRead(PyDictObject *mp);
#endif

#define DKIX_EMPTY (-1)
#define DKIX_DUMMY (-2)  /* Used internally */
#define DKIX_ERROR (-3)
#define DKIX_KEY_CHANGED (-4) /* Used internally */

typedef enum {
    DICT_KEYS_GENERAL = 0,
    DICT_KEYS_UNICODE = 1,
    DICT_KEYS_SPLIT = 2,
    DICT_KEYS_INDEXED_UNICODE = 3,
    DICT_KEYS_INDEXED_GENERAL = 4
} DictKeysKind;

/* See dictobject.c for actual layout of DictKeysObject */
struct _dictkeysobject {
    Py_ssize_t dk_refcnt;

    /* Size of the hash table (dk_indices). It must be a power of 2. */
    uint8_t dk_log2_size;

    /* Size of the hash table (dk_indices) by bytes. */
    uint8_t dk_log2_index_bytes;

    /* Kind of keys */
    uint8_t dk_kind;

#ifdef Py_GIL_DISABLED
    /* Lock used to protect shared keys */
    PyMutex dk_mutex;
#endif

    /* Version number -- Reset to 0 by any modification to keys */
    uint32_t dk_version;

    /* Number of usable entries in dk_entries. */
    Py_ssize_t dk_usable;

    /* Number of used entries in dk_entries. */
    Py_ssize_t dk_nentries;


    /* Actual hash table of dk_size entries. It holds indices in dk_entries,
       or DKIX_EMPTY(-1) or DKIX_DUMMY(-2).

       Indices must be: 0 <= indice < USABLE_FRACTION(dk_size).

       The size in bytes of an indice depends on dk_size:

       - 1 byte if dk_size <= 0xff (char*)
       - 2 bytes if dk_size <= 0xffff (int16_t*)
       - 4 bytes if dk_size <= 0xffffffff (int32_t*)
       - 8 bytes otherwise (int64_t*)

       Dynamically sized, SIZEOF_VOID_P is minimum. */
    char dk_indices[];  /* char is required to avoid strict aliasing. */

    /* "PyDictKeyEntry or PyDictUnicodeEntry dk_entries[USABLE_FRACTION(DK_SIZE(dk))];" array follows:
       see the DK_ENTRIES() / DK_UNICODE_ENTRIES() functions below */
};

/* This must be no more than 250, for the prefix size to fit in one byte. */
#define SHARED_KEYS_MAX_SIZE 30
#define NEXT_LOG2_SHARED_KEYS_MAX_SIZE 6

/* Layout of dict values:
 *
 * The PyObject *values are preceded by an array of bytes holding
 * the insertion order and size.
 * [-1] = prefix size. [-2] = used size. size[-2-n...] = insertion order.
 */
struct _dictvalues {
    uint8_t capacity;
    uint8_t size;
    uint8_t embedded;
    uint8_t valid;
    PyObject *values[1];
};

/* One authoritative values array for a stable-prefix dictionary.
 *
 * prefix_keys owns an immutable exact-string name/index descriptor.  Actual
 * ma_keys contains only visible bindings, with reserved prefix positions and
 * ordinary overflow after them.  UNSET slots have no key in the lookup table
 * and no value.  The values are followed by ``capacity`` Py_ssize_t insertion
 * order indices.  Growth may move this allocation, never a published prefix
 * index: consumers must reload the base after effects.
 */
typedef struct {
    Py_ssize_t capacity;
    Py_ssize_t order_size;
    PyDictKeysObject *prefix_keys;
    PyObject *values[1];
} PyDictIndexedValues;

#define DK_LOG_SIZE(dk)  _Py_RVALUE((dk)->dk_log2_size)
#if SIZEOF_VOID_P > 4
#define DK_SIZE(dk)      (((int64_t)1)<<DK_LOG_SIZE(dk))
#else
#define DK_SIZE(dk)      (1<<DK_LOG_SIZE(dk))
#endif

static inline void* _DK_ENTRIES(PyDictKeysObject *dk) {
    int8_t *indices = (int8_t*)(dk->dk_indices);
    size_t index = (size_t)1 << dk->dk_log2_index_bytes;
    return (&indices[index]);
}

static inline PyDictKeyEntry* DK_ENTRIES(PyDictKeysObject *dk) {
    assert(dk->dk_kind == DICT_KEYS_GENERAL || dk->dk_kind == DICT_KEYS_INDEXED_GENERAL);
    return (PyDictKeyEntry*)_DK_ENTRIES(dk);
}
static inline PyDictUnicodeEntry* DK_UNICODE_ENTRIES(PyDictKeysObject *dk) {
    assert(dk->dk_kind != DICT_KEYS_GENERAL && dk->dk_kind != DICT_KEYS_INDEXED_GENERAL);
    return (PyDictUnicodeEntry*)_DK_ENTRIES(dk);
}

#define DK_IS_UNICODE(dk) \
    ((dk)->dk_kind != DICT_KEYS_GENERAL && (dk)->dk_kind != DICT_KEYS_INDEXED_GENERAL)

#define DICT_VERSION_INCREMENT (1 << (DICT_MAX_WATCHERS + DICT_WATCHED_MUTATION_BITS))
#define DICT_WATCHER_MASK ((1 << DICT_MAX_WATCHERS) - 1)
#define DICT_WATCHER_AND_MODIFICATION_MASK ((1 << (DICT_MAX_WATCHERS + DICT_WATCHED_MUTATION_BITS)) - 1)
#define DICT_UNIQUE_ID_SHIFT (32)
#define DICT_UNIQUE_ID_MAX ((UINT64_C(1) << (64 - DICT_UNIQUE_ID_SHIFT)) - 1)


PyAPI_FUNC(void)
_PyDict_SendEvent(int watcher_bits,
                  PyDict_WatchEvent event,
                  PyDictObject *mp,
                  PyObject *key,
                  PyObject *value);

static inline void
_PyDict_NotifyEvent(PyDict_WatchEvent event,
                    PyDictObject *mp,
                    PyObject *key,
                    PyObject *value)
{
    assert(Py_REFCNT((PyObject*)mp) > 0);
    int watcher_bits = mp->_ma_watcher_tag & DICT_WATCHER_MASK;
    if (watcher_bits) {
        RARE_EVENT_STAT_INC(watched_dict_modification);
        _PyDict_SendEvent(watcher_bits, event, mp, key, value);
    }
}

extern PyDictObject *_PyObject_MaterializeManagedDict(PyObject *obj);

PyAPI_FUNC(PyObject *)_PyDict_FromItems(
        PyObject *const *keys, Py_ssize_t keys_offset,
        PyObject *const *values, Py_ssize_t values_offset,
        Py_ssize_t length);

static inline uint8_t *
get_insertion_order_array(PyDictValues *values)
{
    return (uint8_t *)&values->values[values->capacity];
}

static inline void
_PyDictValues_AddToInsertionOrder(PyDictValues *values, Py_ssize_t ix)
{
    assert(ix < SHARED_KEYS_MAX_SIZE);
    int size = values->size;
    uint8_t *array = get_insertion_order_array(values);
    assert(size < values->capacity);
    assert(((uint8_t)ix) == ix);
    array[size] = (uint8_t)ix;
    values->size = size+1;
}

static inline size_t
shared_keys_usable_size(PyDictKeysObject *keys)
{
    // dk_usable will decrease for each instance that is created and each
    // value that is added.  dk_nentries will increase for each value that
    // is added.  We want to always return the right value or larger.
    // We therefore increase dk_nentries first and we decrease dk_usable
    // second, and conversely here we read dk_usable first and dk_entries
    // second (to avoid the case where we read entries before the increment
    // and read usable after the decrement)
    Py_ssize_t dk_usable = FT_ATOMIC_LOAD_SSIZE_ACQUIRE(keys->dk_usable);
    Py_ssize_t dk_nentries = FT_ATOMIC_LOAD_SSIZE_ACQUIRE(keys->dk_nentries);
    return dk_nentries + dk_usable;
}

static inline size_t
_PyInlineValuesSize(PyTypeObject *tp)
{
    PyDictKeysObject *keys = ((PyHeapTypeObject*)tp)->ht_cached_keys;
    assert(keys != NULL);
    size_t size = shared_keys_usable_size(keys);
    size_t prefix_size = _Py_SIZE_ROUND_UP(size, sizeof(PyObject *));
    assert(prefix_size < 256);
    return prefix_size + (size + 1) * sizeof(PyObject *);
}

int
_PyDict_DetachFromObject(PyDictObject *dict, PyObject *obj);

// Enables per-thread ref counting on this dict in the free threading build
extern void _PyDict_EnablePerThreadRefcounting(PyObject *op);

PyDictObject *_PyObject_MaterializeManagedDict_LockHeld(PyObject *);

// See `_Py_INCREF_TYPE()` in pycore_object.h
#ifndef Py_GIL_DISABLED
#  define _Py_INCREF_DICT Py_INCREF
#  define _Py_DECREF_DICT Py_DECREF
#  define _Py_INCREF_BUILTINS Py_INCREF
#  define _Py_DECREF_BUILTINS Py_DECREF
#else
static inline Py_ssize_t
_PyDict_UniqueId(PyDictObject *mp)
{
    return (Py_ssize_t)(mp->_ma_watcher_tag >> DICT_UNIQUE_ID_SHIFT);
}

static inline void
_Py_INCREF_DICT(PyObject *op)
{
    assert(PyDict_Check(op));
    Py_ssize_t id = _PyDict_UniqueId((PyDictObject *)op);
    _Py_THREAD_INCREF_OBJECT(op, id);
}

static inline void
_Py_DECREF_DICT(PyObject *op)
{
    assert(PyDict_Check(op));
    Py_ssize_t id = _PyDict_UniqueId((PyDictObject *)op);
    _Py_THREAD_DECREF_OBJECT(op, id);
}

// Like `_Py_INCREF_DICT`, but also handles non-dict objects because builtins
// may not be a dict.
static inline void
_Py_INCREF_BUILTINS(PyObject *op)
{
    if (PyDict_CheckExact(op)) {
        _Py_INCREF_DICT(op);
    }
    else {
        Py_INCREF(op);
    }
}

static inline void
_Py_DECREF_BUILTINS(PyObject *op)
{
    if (PyDict_CheckExact(op)) {
        _Py_DECREF_DICT(op);
    }
    else {
        Py_DECREF(op);
    }
}
#endif

#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_DICT_H */
