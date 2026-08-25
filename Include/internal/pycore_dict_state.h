#ifndef Py_INTERNAL_DICT_STATE_H
#define Py_INTERNAL_DICT_STATE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#define DICT_MAX_WATCHERS 8
#define DICT_WATCHED_MUTATION_BITS 4

struct _Py_hashtable_t;

/* Bounded _testinternalcapi observation only. Targets are owned by the active
 * GIL-only wrapper, never by this pointer; it grants no storage/type authority.
 * Counters sit on legacy discovery paths, not on direct stores. */
struct _PySoacStorageLookupProbe {
    PyTypeObject *actual_type;
    PyObject *dictionary;
    uint64_t ordinary_type_lookups;
    uint64_t slot_type_lookups;
    uint64_t dictionary_identity_lookups;
};

struct _Py_dict_state {
    uint32_t next_keys_version;
    PyDict_WatchCallback watchers[DICT_MAX_WATCHERS];
    PyObject *key_layout_watch_types;
    PyObject *key_layout_events;
    /* Non-owning dictionary keys; each value is owned and traversed by its
       dictionary, never rooted by the interpreter. */
    struct _Py_hashtable_t *soac_policies;
    struct _PySoacStorageLookupProbe *soac_storage_lookup_probe;
};

#define _dict_state_INIT \
    { \
        .next_keys_version = 2, \
    }


#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_DICT_STATE_H */
