/* Function object interface */

#ifndef Py_LIMITED_API
#ifndef Py_FUNCOBJECT_H
#define Py_FUNCOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


#define _Py_COMMON_FIELDS(PREFIX) \
    PyObject *PREFIX ## globals; \
    PyObject *PREFIX ## builtins; \
    PyObject *PREFIX ## name; \
    PyObject *PREFIX ## qualname; \
    PyObject *PREFIX ## code;        /* A code object, the __code__ attribute */ \
    PyObject *PREFIX ## defaults;    /* NULL or a tuple */ \
    PyObject *PREFIX ## kwdefaults;  /* NULL or a dict */ \
    PyObject *PREFIX ## closure;     /* NULL or a tuple of cell objects */

typedef struct {
    _Py_COMMON_FIELDS(fc_)
} PyFrameConstructor;

/* Function objects and code objects should not be confused with each other:
 *
 * Function objects are created by the execution of the 'def' statement.
 * They reference a code object in their __code__ attribute, which is a
 * purely syntactic object, i.e. nothing more than a compiled version of some
 * source code lines.  There is one code object per source code "fragment",
 * but each code object can be referenced by zero or many function objects
 * depending only on how many times the 'def' statement in the source was
 * executed so far.
 */

typedef struct {
    PyObject_HEAD
    _Py_COMMON_FIELDS(func_)
    PyObject *func_doc;         /* The __doc__ attribute, can be anything */
    PyObject *func_dict;        /* The __dict__ attribute, a dict or NULL */
    PyObject *func_weakreflist; /* List of weak references */
    PyObject *func_module;      /* The __module__ attribute, can be anything */
    PyObject *func_annotations; /* Annotations, a dict or NULL */
    PyObject *func_annotate;    /* Callable to fill the annotations dictionary */
    PyObject *func_typeparams;  /* Tuple of active type variables or NULL */
    vectorcallfunc vectorcall;
    void *func_soac_metadata;   /* Private SOAC metadata pointer or NULL */
    void (*func_soac_metadata_destructor)(void *);
    uint64_t func_soac_function_id; /* 0 means no SOAC function id is registered */
    /* Version number for use by specializer.
     * Can set to non-zero when we want to specialize.
     * Will be set to zero if any of these change:
     *     defaults
     *     kwdefaults (only if the object changes, not the contents of the dict)
     *     code
     *     annotations
     *     vectorcall function pointer */
    uint32_t func_version;
    /* Permanent semantic identity, independent of replaceable JIT metadata. */
    uint64_t func_soac_strict_id;
    /* GC-visible strict runtime state; never stored in the opaque JIT pointer. */
    PyObject *func_soac_strict_owner;
    uint8_t func_soac_strict_owner_state;
    /* Permanent pre-seal code guard for an installed mandatory call boundary. */
    uint8_t func_soac_required_boundary;

    /* Invariant:
     *     func_closure contains the bindings for func_code->co_freevars, so
     *     PyTuple_Size(func_closure) == PyCode_GetNumFree(func_code)
     *     (func_closure may be NULL if PyCode_GetNumFree(func_code) == 0).
     */
} PyFunctionObject;

#undef _Py_COMMON_FIELDS

PyAPI_DATA(PyTypeObject) PyFunction_Type;

#define PyFunction_Check(op) Py_IS_TYPE((op), &PyFunction_Type)

PyAPI_FUNC(PyObject *) PyFunction_New(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_NewWithQualName(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetCode(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetGlobals(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetModule(PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetDefaults(PyObject *);
PyAPI_FUNC(int) PyFunction_SetDefaults(PyObject *, PyObject *);
PyAPI_FUNC(void) PyFunction_SetVectorcall(PyFunctionObject *, vectorcallfunc);
PyAPI_FUNC(int) PyFunction_SetSoacMetadata(
    PyObject *,
    uint64_t soac_function_id,
    void *metadata,
    void (*destructor)(void *)
);
PyAPI_FUNC(void *) PyFunction_GetSoacMetadata(PyObject *);
/* Borrow the current opaque pointer only when its actual owning destructor
 * equals the caller's non-NULL expected destructor. NULL/no new error means
 * no metadata. A mismatch raises RuntimeError; invalid input also fails.
 * Every path preserves an already pending exception. Matching/absent valid
 * queries allocate nothing, call no Python callback and add no owner.
 * The result is valid only until a callback/release/replacement; clone any
 * Rust code/context owner in a callback-free scope, then end this borrow.
 * This is ownership consistency, never source execution authority. */
PyAPI_FUNC(void *) PyFunction_GetSoacMetadataForDestructorV1(
    PyObject *function, void (*expected_destructor)(void *));
PyAPI_FUNC(uint64_t) PyFunction_GetSoacFunctionId(PyObject *);
PyAPI_FUNC(int) PyFunction_SealSoacStrict(PyObject *, uint64_t identity);
PyAPI_FUNC(uint64_t) PyFunction_GetSoacStrictId(PyObject *);
/* New-entry precondition before binding/using frozen keyword defaults.
 * Returns 0, or -1 if a sealed mapping has become terminal/unavailable.
 * Do not recheck already-bound activations or suspended-frame snapshots. */
PyAPI_FUNC(int) PyFunction_CheckSoacStrictDefaults(PyObject *);
/* Single assignment before sealing; the identical owner is idempotent.
 * Get returns a borrowed reference, NULL/no error when never attached, or
 * NULL/StrictRuntimeUnavailableError after irreversible GC clearing. */
PyAPI_FUNC(int) PyFunction_SetSoacStrictOwner(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetSoacStrictOwner(PyObject *);
/* Requires the exact already-attached, nonterminal native function owner.
 * One-way: all __code__ setter attempts fail before audit/watchers, including
 * identical code objects. Defaults remain mutable until full strict sealing. */
PyAPI_FUNC(int) PyFunction_MarkSoacRequiredBoundary(PyObject *, PyObject *);
/* Permanent metadata only, not owner authentication or execution authority. */
PyAPI_FUNC(int) PyFunction_HasSoacRequiredBoundary(PyObject *);
/* Exact native generated-function provenance, not source/JIT/check authority.
 * Has is a role query: 0 for unrelated functions, 1 for this exact attached
 * record, -1 for a cleared or replayed record. Expired adoption provenance
 * does not prevent ordinary execution or mutation of an unsealed function.
 * Matches additionally requires the active invocation, original code, and
 * producer-assigned role. The caller pins the actual function. */
PyAPI_FUNC(int) PyFunction_HasSoacDataclassCreation(PyObject *function);
PyAPI_FUNC(int) PyFunction_MatchesSoacDataclassCreation(
    PyObject *function, PyObject *invocation, unsigned int role);

/* Private native adapter protocol. Callback views are valid only for the
 * duration of that callback, never Python objects or reusable capabilities.
 * Offsets count _Py_CODEUNIT entries, including inline caches, from the
 * executed code's first instruction (not bytes or instruction ordinals).
 * Callbacks must not execute Python; successful validation must not allocate.
 * Enter runs after ordinary argument binding and returns 1 for an explicit
 * privileged edge, 0 for an ordinary child, or -1 on an invalid transition.
 * Create has the same return convention and precedes CREATE watchers. It is
 * pure/idempotent and may run repeatedly across native allocations; any
 * one-way per-function birth consumption belongs in Created instead.
 * ValidateMember returns 0 for success and -1 for failure.
 * Bridge also returns 0/-1. SOURCE publishes one reached fragment; EXEC and
 * MEMBER must support repeated validation across native allocation/audit
 * boundaries. Compiled receives the exact native compiler result and a
 * C-allocated weak-only tree: (weakref(code), ((const_index, child_tree), ...)).
 * It publishes this one compilation without retaining a code/co_consts tree.
 * Native code materializes every code's ordinary bytecode cache before this
 * callback, then revalidates the exact compile edge after those allocations.
 * The owner must authenticate source fragments/call edges, never accept a
 * public tuple or a helper name as production authority. */
#define Py_SOAC_DATACLASS_ABI 3
#define Py_SOAC_DATACLASS_ROOT_FACTORY 1
#define Py_SOAC_DATACLASS_ROOT_APPLY 2
/* Callback-only stage; never accepted by the public root vectorcall. */
#define Py_SOAC_DATACLASS_GENERATED_EXEC 3
#define Py_SOAC_DATACLASS_BRIDGE_SOURCE 1
#define Py_SOAC_DATACLASS_BRIDGE_EXEC 2
#define Py_SOAC_DATACLASS_BRIDGE_MEMBER 3
#define Py_SOAC_DATACLASS_BUILTIN_EXEC 4
#define Py_SOAC_DATACLASS_BUILTIN_SETATTR 5
#define Py_SOAC_DATACLASS_BRIDGE_VALUE 6
#define Py_SOAC_DATACLASS_BRIDGE_INIT_VALUE 7
#define Py_SOAC_DATACLASS_BRIDGE_NEW_SLOTS 8
#define Py_SOAC_DATACLASS_MEMBER 1
#define Py_SOAC_DATACLASS_FROZEN_SETATTR 2
#define Py_SOAC_DATACLASS_FROZEN_DELATTR 3
#define Py_SOAC_DATACLASS_DECORATOR 256
#define Py_SOAC_DATACLASS_GENERATED_FACTORY 257
#define Py_SOAC_DATACLASS_ANNOTATION_PROVIDER 258
#define Py_SOAC_DATACLASS_REPR_IMPLEMENTATION 259
#define Py_SOAC_DATACLASS_COMPONENT_ANNOTATE 1
#define Py_SOAC_DATACLASS_COMPONENT_REPR 2
typedef struct _PySoacDataclassFrameView PySoacDataclassFrameView;
typedef struct {
    unsigned int abi_version;
    int (*enter)(PyObject *, unsigned int,
                 const PySoacDataclassFrameView *,
                 const PySoacDataclassFrameView *, unsigned int *);
    int (*create)(PyObject *, const PySoacDataclassFrameView *,
                  PyObject *, unsigned int *);
    int (*validate_member)(PyObject *, PyObject *, PyObject *,
                           PyObject *, PyObject *, unsigned int);
    int (*bridge)(PyObject *, const PySoacDataclassFrameView *, PyObject *,
                  unsigned int, PyObject *const *, Py_ssize_t);
    int (*compiled)(PyObject *, const PySoacDataclassFrameView *, PyObject *,
                    PyObject *, PyObject *);
    /* The actual record is attached but the function is not yet tracked or
     * published to CREATE watchers. Unlike validation callbacks, Created may
     * allocate a prebuilt check delegate/spec through Configure. Configure
     * must precede any GC-visible weak function witness. It must not execute
     * Python or deliberately publish a strong function reference. Native
     * revalidation and safe terminal disposal on failure follow. */
    int (*created)(PyObject *, PyObject *, const PySoacDataclassFrameView *,
                   PyObject *, unsigned int);
    /* One byte per native positional/keyword-only parameter, including self.
     * Supplied was captured after caller binding, BEFORE default insertion.
     * Bound may mark only omitted factory slots deferred. These bits then
     * remain immutable for the entire activation, including repeated lines. */
    int (*bound)(PyObject *, const PySoacDataclassFrameView *,
                 const unsigned char *, unsigned char *, Py_ssize_t);
    int (*value)(PyObject *, Py_ssize_t, PyObject *);
    int (*validate_component)(PyObject *, PyObject *, PyObject *,
                              unsigned int, Py_ssize_t);
    /* Return 1 for a selected required factory expression, 0 unchanged, -1
     * on error. A selected name is borrowed exact Unicode owned by the
     * prepared compiler plan, collision-free against its parameter names.
     * Repeated validation across native allocations must be idempotent. */
    int (*init_value)(PyObject *, const PySoacDataclassFrameView *,
                      PyObject *, PyObject *, PyObject *, PyObject **);
    /* One-way preparation after pure bridge validation. The five arguments
     * are already evaluated metaclass/name/bases/namespace/original, in that
     * order. Return zero with ONE owned replacement handle minted by
     * PyType_NewSoacDataclassSlotsHandle using this exact callback view, or
     * -1. May allocate native/Rust state, but must not execute Python. The
     * ordinary bridge callback revalidates after allocations and before Ready.
     * No decline is legal after the original class has been bound. */
    int (*prepare_slots)(PyObject *, const PySoacDataclassFrameView *,
                         PyObject *, PyObject *, PyObject *, PyObject *,
                         PyObject *, PyObject **);
} PySoacDataclassCallbacks;

#define Py_SOAC_DATACLASS_BOUNDARY_ABI 1
typedef struct {
    Py_ssize_t instruction;  /* Native code units, including inline caches. */
    Py_ssize_t parameter_index;
} PySoacDataclassValueSite;
typedef struct {
    unsigned int abi_version;
    Py_ssize_t parameter_count;
    const unsigned char *factory_mask;
    Py_ssize_t value_site_count;
    const PySoacDataclassValueSite *value_sites;
} PySoacDataclassBoundarySpec;

/* Configure only during this exact record's unpublished Created callback.
 * Copies all arrays; owns the GC-visible delegate, never a function/class
 * backedge. The actual code must be an ordinary synchronous fixed-signature
 * function. Its required entry is armed before CREATE watchers, while normal
 * SET_FUNCTION_ATTRIBUTE still supplies defaults/closure afterward. */
PyAPI_FUNC(int) PyFunction_ConfigureSoacDataclassBoundary(
    PyObject *invocation, PyObject *function, PyObject *verified_code,
    PyObject *check_owner, const PySoacDataclassBoundarySpec *, size_t spec_size);
/* 1 for the exact installed delegate, 0 unrelated/different delegate, -1 for
 * a recognized invalid/terminal record. No vectorcall equality requirement:
 * forwarding preserves the boundary; stock entry still fails its frame guard. */
PyAPI_FUNC(int) PyFunction_MatchesSoacDataclassBoundary(
    PyObject *function, PyObject *expected_check_owner);
/* Fresh owned components only. ANNOTATE uses closure_index=-1 and the actual
 * func_annotate edge; REPR uses the explicit actual closure-cell projection.
 * Native roles and the fixed component-policy callback are both required. */
PyAPI_FUNC(int) PyFunction_AdoptSoacDataclassComponent(
    PyObject *invocation, PyObject *method, PyObject *component,
    unsigned int kind, Py_ssize_t closure_index);

/* One immutable callback table per interpreter, closed before teardown. */
PyAPI_FUNC(int) PySoac_SetDataclassCallbacks(const PySoacDataclassCallbacks *);
PyAPI_FUNC(PyObject *) PySoac_NewDataclassInvocation(PyObject *owner);
PyAPI_FUNC(PyObject *) PySoac_DataclassVectorcall(
    PyObject *invocation, unsigned int root_stage, PyObject *callable,
    PyObject *const *args, size_t nargsf, PyObject *kwnames);
/* Called exactly once after the native class contract is installed, before
 * PyType_Ready callbacks. No allocation or Python call on success. */
PyAPI_FUNC(int) PySoac_DataclassBindClass(
    PyObject *invocation, PyObject *actual_type, PyObject *expected_class_owner);
/* Callback-free proof of this active invocation's distinct replacement. The
 * native association and callback-free weak witness precede spec.bind_type;
 * no original binding is transferred. 1 exact, 0 unrelated/mismatched, -1
 * expired/terminal/invalid. The caller independently pins actual_type. */
PyAPI_FUNC(int) PySoac_DataclassMatchesSlotsClass(
    PyObject *invocation, PyObject *actual_type, PyObject *expected_class_owner);
PyAPI_FUNC(int) PySoac_CompleteDataclassInvocation(PyObject *invocation);
PyAPI_FUNC(int) PySoac_FailDataclassInvocation(PyObject *invocation);
/* Only before BindClass: permanently disable adoption, but preserve ordinary
 * decorator execution and unsealed generated-function metadata semantics. */
PyAPI_FUNC(int) PySoac_DeclineDataclassInvocation(PyObject *invocation);
/* Borrowed canonical native object, NULL/no error if absent/dead. The weak
 * witnesses are captured at native creation, not from mutable module attrs;
 * an expired witness is never replaced. Invalid kind raises ValueError. */
PyAPI_FUNC(PyObject *) PySoac_GetDataclassBuiltin(unsigned int kind);
/* Fresh ordinary code decoded from a native-build frozen recipe. No module
 * execution, mutable __file__ lookup, or persistent Python code roots.
 * Recipes use optimize=0 and <frozen NAME> filenames; consumers project the
 * filename explicitly and attest the rest of the complete graph/environment. */
#define Py_SOAC_DATACLASS_RECIPE_DATACLASSES 1
#define Py_SOAC_DATACLASS_RECIPE_REPRLIB 2
PyAPI_FUNC(PyObject *) PySoac_GetDataclassRecipe(unsigned int kind);
/* Semantic builtin implementation match for a trusted counted native name.
 * Requires the private builtin method-table entry, its default native entry,
 * and the creation-witnessed current builtin-module self. Equivalent copies
 * of that native function may match; this never grants a privileged bridge's
 * canonical-object authority. Returns 1/0, or -1 for invalid/closed state.
 * Successful matching is allocation/callback-free; no Python attrs are read. */
PyAPI_FUNC(int) PySoac_MatchesBuiltinFunction(
    PyObject *actual, const char *name, Py_ssize_t name_length);

/* Borrowed results. Enter and Bound precede MAKE_CELL/COPY_FREE_VARS: bound
 * parameter slots are raw values, and free variables still belong to the
 * authenticated actual function closure. Later views use the executed
 * code's explicit local/cell layout. Never infer a slot kind from its value.
 * Local returns NULL/no error for an unbound slot; a bound
 * Python None is Py_None. CellValue distinguishes an empty actual cell
 * (NULL/no error) from a non-cell slot (NULL/TypeError). Invalid indices
 * raise IndexError. No accessor materializes f_locals or calls Python. */
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameFunction(const PySoacDataclassFrameView *);
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameCode(const PySoacDataclassFrameView *);
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameGlobals(const PySoacDataclassFrameView *);
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameBuiltins(const PySoacDataclassFrameView *);
/* The exact invocation carried by this explicit callback view, including a
 * root/child Enter view before its frame is attached. NULL for no view or for
 * a generated required-boundary call (which has no construction authority).
 * Borrowed only for the callback's duration; never consults ambient frames. */
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameInvocation(const PySoacDataclassFrameView *);
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameLocal(
    const PySoacDataclassFrameView *, Py_ssize_t);
PyAPI_FUNC(PyObject *) PySoac_DataclassFrameCellValue(
    const PySoacDataclassFrameView *, Py_ssize_t);
PyAPI_FUNC(unsigned int) PySoac_DataclassFrameRole(const PySoacDataclassFrameView *);
PyAPI_FUNC(Py_ssize_t) PySoac_DataclassFrameInstruction(const PySoacDataclassFrameView *);
/* The trusted caller must authenticate the provider's source role, logical
 * owner, and complete capture layout. This checks the actual native function
 * owner/code identity and recursively clones ordinary code with no SOAC IDs.
 * Neither the original code nor any native execution guard is modified. */
PyAPI_FUNC(PyObject *) PySoac_CloneAnnotationReplayCode(
    PyObject *provider, PyObject *expected_owner, PyObject *verified_code);
PyAPI_FUNC(PyObject *) PyFunction_GetKwDefaults(PyObject *);
PyAPI_FUNC(int) PyFunction_SetKwDefaults(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetClosure(PyObject *);
PyAPI_FUNC(int) PyFunction_SetClosure(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyFunction_GetAnnotations(PyObject *);
PyAPI_FUNC(int) PyFunction_SetAnnotations(PyObject *, PyObject *);

#define _PyFunction_CAST(func) \
    (assert(PyFunction_Check(func)), _Py_CAST(PyFunctionObject*, func))

/* Static inline functions for direct access to these values.
   Type checks are *not* done, so use with care. */
static inline PyObject* PyFunction_GET_CODE(PyObject *func) {
    return _PyFunction_CAST(func)->func_code;
}
#define PyFunction_GET_CODE(func) PyFunction_GET_CODE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_GLOBALS(PyObject *func) {
    return _PyFunction_CAST(func)->func_globals;
}
#define PyFunction_GET_GLOBALS(func) PyFunction_GET_GLOBALS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_MODULE(PyObject *func) {
    return _PyFunction_CAST(func)->func_module;
}
#define PyFunction_GET_MODULE(func) PyFunction_GET_MODULE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_DEFAULTS(PyObject *func) {
    return _PyFunction_CAST(func)->func_defaults;
}
#define PyFunction_GET_DEFAULTS(func) PyFunction_GET_DEFAULTS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_KW_DEFAULTS(PyObject *func) {
    return _PyFunction_CAST(func)->func_kwdefaults;
}
#define PyFunction_GET_KW_DEFAULTS(func) PyFunction_GET_KW_DEFAULTS(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_CLOSURE(PyObject *func) {
    return _PyFunction_CAST(func)->func_closure;
}
#define PyFunction_GET_CLOSURE(func) PyFunction_GET_CLOSURE(_PyObject_CAST(func))

static inline PyObject* PyFunction_GET_ANNOTATIONS(PyObject *func) {
    return _PyFunction_CAST(func)->func_annotations;
}
#define PyFunction_GET_ANNOTATIONS(func) PyFunction_GET_ANNOTATIONS(_PyObject_CAST(func))

/* The classmethod and staticmethod types lives here, too */
PyAPI_DATA(PyTypeObject) PyClassMethod_Type;
PyAPI_DATA(PyTypeObject) PyStaticMethod_Type;

PyAPI_FUNC(PyObject *) PyClassMethod_New(PyObject *);
PyAPI_FUNC(PyObject *) PyStaticMethod_New(PyObject *);

#define PY_FOREACH_FUNC_EVENT(V) \
    V(CREATE)                    \
    V(DESTROY)                   \
    V(MODIFY_CODE)               \
    V(MODIFY_DEFAULTS)           \
    V(MODIFY_KWDEFAULTS)         \
    V(MODIFY_QUALNAME)

typedef enum {
    #define PY_DEF_EVENT(EVENT) PyFunction_EVENT_##EVENT,
    PY_FOREACH_FUNC_EVENT(PY_DEF_EVENT)
    #undef PY_DEF_EVENT
} PyFunction_WatchEvent;

/*
 * A callback that is invoked for different events in a function's lifecycle.
 *
 * The callback is invoked with a borrowed reference to func, after it is
 * created and before it is modified or destroyed. The callback should not
 * modify func.
 *
 * When a function's code object, defaults, or kwdefaults are modified the
 * callback will be invoked with the respective event and new_value will
 * contain a borrowed reference to the new value that is about to be stored in
 * the function. Otherwise the third argument is NULL.
 *
 * If the callback returns with an exception set, it must return -1. Otherwise
 * it should return 0.
 */
typedef int (*PyFunction_WatchCallback)(
  PyFunction_WatchEvent event,
  PyFunctionObject *func,
  PyObject *new_value);

/*
 * Register a per-interpreter callback that will be invoked for function lifecycle
 * events.
 *
 * Returns a handle that may be passed to PyFunction_ClearWatcher on success,
 * or -1 and sets an error if no more handles are available.
 */
PyAPI_FUNC(int) PyFunction_AddWatcher(PyFunction_WatchCallback callback);

/*
 * Clear the watcher associated with the watcher_id handle.
 *
 * Returns 0 on success or -1 if no watcher exists for the supplied id.
 */
PyAPI_FUNC(int) PyFunction_ClearWatcher(int watcher_id);

#ifdef __cplusplus
}
#endif
#endif /* !Py_FUNCOBJECT_H */
#endif /* Py_LIMITED_API */
