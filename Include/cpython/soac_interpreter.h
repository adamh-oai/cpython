#ifndef Py_SOAC_INTERPRETER_H
#define Py_SOAC_INTERPRETER_H
#ifndef Py_LIMITED_API
#ifdef __cplusplus
extern "C" {
#endif

/* Versioned ordinary-interpreter enforcement ABI.
 * The trusted loader, not these hooks, authenticates the ty/source artifact.
 * Ordinary native frames/binding/closures/recursion/observers remain in use. */
#define Py_SOAC_INTERPRETER_ABI_V2 2u
#define Py_SOAC_INTERPRETER_CALLBACKS_ABI_V4 4u

#define Py_SOAC_INTERPRETER_ROOT 1u
#define Py_SOAC_INTERPRETER_FUNCTION 2u
#define Py_SOAC_INTERPRETER_CLASS_NAMESPACE 3u

#define Py_SOAC_INTERPRETER_BINDING 1u
#define Py_SOAC_INTERPRETER_BOUND 2u
#define Py_SOAC_INTERPRETER_RUNNING 3u
#define Py_SOAC_INTERPRETER_RETURNING 4u
#define Py_SOAC_INTERPRETER_RETIRED 5u
#define Py_SOAC_INTERPRETER_FAILING 6u

#define Py_SOAC_INTERPRETER_BIND_FAILED 1u
#define Py_SOAC_INTERPRETER_CHECK_FAILED 2u
#define Py_SOAC_INTERPRETER_BODY_FAILED 3u
#define Py_SOAC_INTERPRETER_RETURNED 4u
#define Py_SOAC_INTERPRETER_FRAME_CLEARED 5u
#define Py_SOAC_INTERPRETER_NAMESPACE_TRANSFERRED 6u

typedef struct _PySoacInterpreterFrameViewV1
    PySoacInterpreterFrameViewV1;

/* Immutable prepared requirements, not permission to execute a copied code
 * object or a function constructed outside its actual native birth. */
#define Py_SOAC_DESCRIPTOR_ANNOTATION 1u
#define Py_SOAC_DESCRIPTOR_CLASS_NAMESPACE 2u
#define Py_SOAC_DESCRIPTOR_COMPLETION 4u
#define Py_SOAC_DESCRIPTOR_GENERIC 8u
#define Py_SOAC_DESCRIPTOR_NOMINAL 16u
#define Py_SOAC_DESCRIPTOR_BODY_CONTEXT 32u
#define Py_SOAC_DESCRIPTOR_REQUIREMENTS_MASK 63u

typedef struct _PySoacInterpreterDescriptorV1 {
    uint32_t ordinal;
    uint32_t parent_ordinal;          /* UINT32_MAX for the root. */
    uint32_t requirements;
    uint32_t scope_kind;
    PyObject *original_code;          /* Comparison only, never an owned edge. */
} PySoacInterpreterDescriptorV1;

typedef struct {
    uint32_t abi_version;             /* Exactly 1. */
    uint32_t owner_kind;              /* 0:none, 1:dynamic, 2:shared descriptor. */
    uint32_t ordinal;
    uint32_t requirements;
    int64_t interpreter_id;
    uint64_t birth_activation_id;
    PyObject *binding;                /* Borrowed, supported by the function. */
    uint32_t ready, terminal, entered, reserved;
} PySoacInterpreterFunctionViewV1;

/* One GC-visible binding per actual module execution. These APIs do not
 * authenticate source artifacts. The trusted caller has already verified the
 * exact original code tree and supplies its module execution/guard. Native
 * copies POD descriptors and stores globals/code only as identity scalars.
 * The binding never pins code, functions, defaults, cells or globals. */
PyAPI_FUNC(PyObject *) PySoac_NewInterpreterSourceBindingV1(
    PyObject *root_owner, PyObject *execution_guard, PyObject *globals,
    uint64_t source_id, const PySoacInterpreterDescriptorV1 *descriptors,
    size_t count, size_t descriptor_size);
PyAPI_FUNC(int) PySoac_IsInterpreterSourceBindingV1(PyObject *binding);
PyAPI_FUNC(PyObject *) PySoac_GetInterpreterSourcePublicationV1(PyObject *binding);
/* Same-interpreter borrowed publication, including terminal bindings whose
 * publication edge still exists. No allocation, errors or reference changes. */
PyAPI_FUNC(PyObject *) PySoac_GetInterpreterSourcePublicationForTeardownV1(PyObject *binding);
PyAPI_FUNC(int) PySoac_InterpreterSourceBindingInvalidateV1(PyObject *binding);
PyAPI_FUNC(int) PySoac_SetInterpreterCodeDescriptorV1(
    PyObject *code, uint32_t ordinal, uint32_t requirements);
PyAPI_FUNC(int) PySoac_GetInterpreterFunctionViewV1(
    PyObject *function, PySoacInterpreterFunctionViewV1 *out, size_t out_size);
PyAPI_FUNC(PyObject *) PySoac_InterpreterFrameSourceBindingV1(
    const PySoacInterpreterFrameViewV1 *view);

/* Callback-local native execution facts. A construction activation pair is
 * reserved before enter, survives suspension/native frame moves while its
 * event is active, and is never reused. Ordinary function calls have no such
 * activation; check_entry reports activation_id=0 and call_state=NULL. The
 * pair is correspondence, not execution authority or a Python value owner. */
typedef struct {
    uint32_t abi_version;             /* Set by GetInfo, exactly V2. */
    uint32_t phase;
    uint32_t kind;
    uint32_t source_authority;        /* Actual original-source execution grant. */
    int64_t interpreter_id;
    uint64_t activation_id;           /* Nonzero only for actual construction state. */
    PyObject *function;               /* Borrowed actual frame f_funcobj. */
    PyObject *code;                   /* Borrowed actual frame f_executable. */
    PyObject *globals;                /* Borrowed actual native frame view. */
    PyObject *builtins;               /* Borrowed actual native frame view. */
    PyObject *locals;                 /* Borrowed f_locals; may be NULL. */
    PyObject *call_state;             /* Borrowed; NULL while enter runs. */
    Py_ssize_t instruction_units;     /* Captured actual opcode, code units. */
    Py_ssize_t instruction_ordinal;   /* Final ordinal, no EXTENDED_ARG/CACHE. */
    Py_ssize_t localsplus_count;
} PySoacInterpreterFrameInfoV2;

/* Native call-operand projection. The enclosing unpublished callback table
 * remains a single exact-size table: no old-table or no-call fallback. */
#define Py_SOAC_INTERPRETER_CALL_ABI_V1 1u
#define Py_SOAC_INTERPRETER_CALL_SELECT 1u
#define Py_SOAC_INTERPRETER_CALL_PREPARE_TYPE 2u

#define Py_SOAC_INTERPRETER_CALL_VECTOR 1u
#define Py_SOAC_INTERPRETER_CALL_VECTOR_KW 2u
#define Py_SOAC_INTERPRETER_CALL_EXPANDED 3u
/* Runtime observation only. VALUE does not guess MethodSelf versus the
 * compiler's LeadingArgument; the authenticated receipt distinguishes them. */
#define Py_SOAC_INTERPRETER_CALL_NULL_CHANNEL 0u
#define Py_SOAC_INTERPRETER_CALL_VALUE_CHANNEL 1u

#define Py_SOAC_INTERPRETER_DECORATORS_NONE 0u
#define Py_SOAC_INTERPRETER_DECORATORS_CURRENT 1u
#define Py_SOAC_INTERPRETER_DECORATORS_DIRECT_CALLER 2u

#define Py_SOAC_INTERPRETER_OPERAND_CALLABLE 1u
#define Py_SOAC_INTERPRETER_OPERAND_POSITIONAL 2u
#define Py_SOAC_INTERPRETER_OPERAND_KEYWORD_VALUE 3u
#define Py_SOAC_INTERPRETER_OPERAND_KEYWORD_NAMES 4u
#define Py_SOAC_INTERPRETER_OPERAND_EXPANDED_ARGS 5u
#define Py_SOAC_INTERPRETER_OPERAND_EXPANDED_KWARGS 6u
#define Py_SOAC_INTERPRETER_OPERAND_DECORATOR 7u

#define Py_SOAC_INTERPRETER_CREATION_NONE 0u
#define Py_SOAC_INTERPRETER_CREATION_LIVE 1u
#define Py_SOAC_INTERPRETER_CREATION_INVALID 2u

#define Py_SOAC_INTERPRETER_CALL_ORDINARY 0u
#define Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT 1u
#define Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR 2u
#define Py_SOAC_INTERPRETER_CALL_CLASS 3u
#define Py_SOAC_INTERPRETER_CALL_GENERIC_SCOPE 4u

typedef struct _PySoacInterpreterCallViewV1 PySoacInterpreterCallViewV1;

typedef struct {
    uint32_t form;
    uint32_t channel;
    uint32_t instruction_argument;    /* Actual full native oparg. */
    uint32_t reserved;                /* Zero. */
    Py_ssize_t positional_count;      /* Includes actual non-NULL channel. */
    Py_ssize_t keyword_count;
    const PySoacInterpreterFrameViewV1 *frame;
} PySoacInterpreterCallSiteV1;

typedef struct {
    uint32_t abi_version;
    uint32_t phase;
    PySoacInterpreterCallSiteV1 current;
    /* At most ONE exact active incoming consumed native generic-scope edge.
     * Its arguments have moved and are not accessible through this view.
     * NULL for C-forwarded/public/resumed or otherwise unproved edges. */
    const PySoacInterpreterCallSiteV1 *direct_caller;
    uint32_t decorator_source;
    uint32_t reserved;
    Py_ssize_t decorator_count;       /* Zero during SELECT. */
} PySoacInterpreterCallInfoV1;

typedef struct {
    uint32_t abi_version;
    uint32_t creation_status;
    uint32_t creation_role;
    uint32_t reserved;
    uint64_t creation_identity;
    PyObject *value;                  /* Borrowed actual selected operand. */
    /* Only LIVE: borrowed from that operand's existing exact native creation
     * record, current original code and live same-interpreter invocation.
     * NONE/INVALID never expose these pointers. No record is created/renewed. */
    PyObject *dataclass_invocation;
    PyObject *dataclass_owner;
} PySoacInterpreterCallOperandV1;

typedef struct {
    uint32_t abi_version;
    uint32_t kind;
    uint32_t dataclass_stage;
    uint32_t decorator_source;
    Py_ssize_t decorator_count;
    /* Exactly ONE owned metadata edge for DATACLASS_ROOT (existing invocation)
     * or BUILTIN_DESCRIPTOR (namespace birth witness); NULL for other kinds.
     * Never a callable, argument, defaults, code, namespace map or result. */
    PyObject *metadata;
    /* BUILTIN_DESCRIPTOR only. Borrowed comparison inputs, protected by the
     * actual function operand and authenticated active original code graph.
     * All other kinds require both NULL. They must survive no unsupported
     * callback window; native revalidates before the one constructor call. */
    PyObject *expected_function_owner;
    PyObject *verified_code;
} PySoacInterpreterCallDecisionV1;

typedef struct {
    uint32_t abi_version;
    uint32_t flags;                   /* Callback ABI V4 requires zero. */

    /* Authenticate the actual module/dict/root/owner and consume this root
     * initialization attempt before its wrapper's CREATE notification.
     * This is NOT a reusable permission attached to the temporary wrapper.
     * Failure is terminal for that attempted initialization. */
    int (*root_begin)(PyObject *owner, PyObject *module, PyObject *code);
    /* No Python, allocation or error replacement. Called once iff root_begin
     * succeeded, including wrapper creation, binding or evaluation failure. */
    void (*root_end)(PyObject *owner, int succeeded);

    /* Actual MAKE_FUNCTION child; every field is initialized, but ordinary
     * SET_FUNCTION_ATTRIBUTE has NOT yet supplied defaults/closure/annotations.
     * Parent is explicit and its instruction was captured before callbacks.
     * Success supplies ONE owned metadata reference.
     * Native installs that owner BEFORE CREATE, retaining stock
     * vectorcall. Common native frame init enforces the actual owner/code;
     * public vectorcall pointer equality is never semantic authority.
     * Neither callback nor owner may publish the uncommitted child.
     * Birth is not final source-definition/decorator completion or sealing. */
    int (*birth)(const PySoacInterpreterFrameViewV1 *parent,
                 PyObject *function, PyObject **new_owner);

    /* Actual SET_FUNCTION_ATTRIBUTE, AFTER native publication and BEFORE any
     * decorator. The installed operand is borrowed from the actual function.
     * The target and provider already have their exact birth owners; source
     * and parent-invocation equality alone cannot pair repeated definitions.
     * attribute_flag is the actual MAKE_FUNCTION_* bit used by this opcode.
     * Successful association is callback/allocation-free: share the provider's
     * callback-free weak witness prepared at its birth into a reserved target
     * metadata slot. No extra provider/code/default/closure owner is acquired.
     * On error, stop reading borrowed operands before allocating/raising.
     * This records producer identity; it is not final sealing. */
    int (*function_attribute)(const PySoacInterpreterFrameViewV1 *parent,
                              PyObject *function, uint32_t attribute_flag,
                              PyObject *borrowed_installed_value);

    /* ROOT/CLASS_NAMESPACE enter in BINDING with source_authority=0, before
     * native binding/VM entry. FUNCTION enter is lazy: the actual frame already
     * runs authenticated original code, and a definition/birth/selected CALL
     * event needs state. Its phase is RUNNING and source_authority=1. Untaken
     * definitions and ordinary calls never invoke enter.
     * The frame owns its captured function/code. For ROOT the explicit API
     * caller supports subject_owner; otherwise the actual function's permanent
     * owner edge supports it. No extra execution-value pin is added.
     * Success supplies ONE owned metadata state, transferred into the existing
     * checked-activation frame slot. New-state may be the owner's NewRef; its
     * contents must not duplicate function/code/maps/argument ownership. A
     * reused owner must not hold mutable per-invocation phase or identity:
     * GetInterpreterFrameInfoV2 supplies those frozen native activation facts. */
    int (*enter)(uint32_t kind, PyObject *subject_owner,
                 const PySoacInterpreterFrameViewV1 *frame,
                 const PySoacInterpreterFrameViewV1 *parent,
                 PyObject **new_call_state);

    /* ROOT/CLASS_NAMESPACE committed default-VM entry after native binding,
     * evaluator selection and recursion success, before their first opcode.
     * RUNNING view; successful validation is callback/allocation-free. Ordinary
     * function entry/resume does not invoke this callback; its actual VM-entry
     * witness is the scalar InterpreterFunctionEnteredV1 guard bit. Binder,
     * ownership and external-evaluator refusals do not fabricate that bit. */
    int (*started)(PyObject *state, const PySoacInterpreterFrameViewV1 *frame);

    /* An indexed, potentially relevant source-authorized CALL, after native
     * CALL monitoring/normalization and before input consumption. Native
     * publishes the operand stack first. Unselected ordinary sites never
     * construct a call view or invoke this callback. Selected sites may still
     * return ORDINARY after checking their actual operands. Selection
     * joins the immutable original receipt to actual native operands; names,
     * result identity, public vectorcall equality or table presence grant none.
     * Out starts zeroed except abi_version; on failure metadata stays NULL.
     * No selected branch may retry ordinary dispatch after failure. */
    int (*call)(PyObject *state,
                 const PySoacInterpreterCallInfoV1 *info,
                 const PySoacInterpreterCallViewV1 *operands,
                 PySoacInterpreterCallDecisionV1 *out, size_t out_size);

    /* Closed selected kinds only: BUILTIN_DESCRIPTOR or DATACLASS_ROOT.
     * Descriptor: immediately after its actual native constructor, before
     * publication; metadata is its same birth witness, dataclass_owner is NULL,
     * stage is zero. Dataclass: AFTER native callee/input unlink/clear,
     * before caller resumes or the result token is released. No CallView
     * survives consumption. Caller owns state; the native finish continuation
     * owns invocation metadata; dataclass_owner is borrowed from that SAME edge
     * (including failure); borrowed_result has its actual native token.
     * Success completes the existing Rust owner/native invocation transaction.
     * NULL result means native body/binder failure: native detaches the exact
     * primary PyErr, the callback fails only this attempted invocation, secondary
     * errors are unraisable, and native restores the exact primary afterward.
     * Public legacy DataclassVectorcall retains its own wrapper completion;
     * it must not receive this callback a second time. */
    int (*selected_call_finished)(
        PyObject *state, const PySoacInterpreterFrameViewV1 *caller,
        uint32_t kind, PyObject *metadata, PyObject *dataclass_owner,
        uint32_t stage, PyObject *borrowed_result);

    /* Scalar/metadata retirement, no Python, allocation or error replacement.
     * Once per successful enter. Namespace success reports TRANSFERRED before
     * its ONE state edge moves from frame to __build_class__'s C stack; no
     * views may be retained. Every other reason retires construction state; a
     * reused permanent function owner remains live and unchanged. The frozen
     * native token also identifies unfinished children during direct GC/frame
     * clearing when no executable frame view remains for definition_abort.
     * Successful RETURNED/NAMESPACE_TRANSFERRED preserve completed or explicitly
     * transferred metadata. Other reasons invalidate only unfinished metadata
     * for this token using callback-free weak witnesses. */
    void (*leave)(PyObject *state, uint32_t reason,
                  int64_t interpreter_id, uint64_t activation_id);

    /* Only the actual opcode-dispatched builtin __build_class__ with exact
     * parent/site and successfully evaluated namespace function can reach
     * this callback. All operands borrowed; namespace_state is the moved
     * metadata state, never another function/frame/map owner.
     * Return 0 + ONE owned existing PyType_NewSoacConstructionHandle result,
     * or 0 + NULL to decline BEFORE installation, or -1 with error.
     * No late decline/revocation after the handle starts construction.
     * Actual type/descriptor callbacks continue in the native constructor.
     * keywords may be NULL for no native keyword dictionary. */
    int (*prepare_type)(PyObject *namespace_state,
                        const PySoacInterpreterFrameViewV1 *parent,
                        const PySoacInterpreterCallInfoV1 *call_info,
                        const PySoacInterpreterCallViewV1 *call_operands,
                        PyObject *namespace_function, PyObject *metaclass,
                        PyObject *name, PyObject *bases,
                        PyObject *namespace_dict, PyObject *keywords,
                        PyObject **new_handle);

    /* The explicit SOAC_COMPLETE_DEFINITION operation runs AFTER metadata and
     * decorators, BEFORE the final binding. Its authenticated completion
     * receipt identifies the source definition and actual native site; the
     * value still needs its actual creation owner. No local/cell store invokes
     * this callback. Neither a following STORE shape, spelling, final
     * SET_FUNCTION_ATTRIBUTE nor an arbitrary code pointer is authority. */
    int (*definition_complete)(const PySoacInterpreterFrameViewV1 *frame,
                               PyObject *borrowed_value);
    /* Cold unsealed/uncached ownership validation, before native argument
     * binding. BINDING view with no activation/state. It can permit an actual
     * ordinary code replacement, never a transplanted strict code object.
     * Ready guards avoid this callback; native checks retain actual identity,
     * supported mutation rules and shared execution-guard liveness. */
    int (*check_entry)(PyObject *owner, const PySoacInterpreterFrameViewV1 *frame);
    /* Actual post-attribute function birth, or one exact generic-scope result
     * handoff. complete_context=0: keep enclosing definition context active;
     * =1: finish a standalone birth context; =2: the result belongs to an exact
     * committed generic CALL and completion_parent is its active caller view.
     * Only =2 supplies completion_parent; validate that edge before transferring
     * the named result's pending-completion token, retaining its lexical birth
     * token. Parent COMPLETE performs final metadata/decorator sealing. This is
     * not a general return notification or an argument/result-type check. */
    int (*definition_end)(const PySoacInterpreterFrameViewV1 *frame,
                          PyObject *function, uint32_t complete_context,
                          const PySoacInterpreterFrameViewV1 *completion_parent);
    /* An exception is leaving the currently active definition region, including
     * a caught exception whose handler lies outside that region. Native detaches
     * the primary error first; fail unfinished objects before any secondary
     * error escapes. Completed children stay valid. Native reports secondary
     * errors as unraisable, retires this event, and restores the primary error. */
    int (*definition_abort)(const PySoacInterpreterFrameViewV1 *frame);
} PySoacInterpreterCallbacksV4;

typedef struct {
    uint32_t instruction_ordinal;
    uint32_t flags;
} PySoacInterpreterCodeEventV1;
#define Py_SOAC_INTERPRETER_EVENT_CALL 1u
/* Cold exact source-code registration. Rows are sorted, unique final ordinals
 * of potentially relevant CALLs; unknown flags/coordinates reject. The native
 * index also validates BEGIN/COMPLETE pairing. Identical reinstall is harmless,
 * changed tables reject. Routing metadata does not grant runtime authority. */
PyAPI_FUNC(int) PySoac_SetInterpreterCodeEventsV1(
    PyObject *, const PySoacInterpreterCodeEventV1 *, size_t, size_t);
/* Shared native metadata guards, with no owned function/code/argument values.
 * GetFunctionGuard and NewExecutionGuard return new references. Ready caches
 * validated native entry ownership; it is not metadata sealing or a value-type
 * guarantee. Invalidate is terminal. Entered records actual committed VM entry.
 * BindExecution ties every function to its module's admission lifetime; failed
 * initialization/terminal teardown must invalidate that shared guard. */
PyAPI_FUNC(PyObject *) PySoac_GetInterpreterFunctionGuardV1(PyObject *);
PyAPI_FUNC(int) PySoac_InterpreterFunctionReadyV1(PyObject *);
PyAPI_FUNC(int) PySoac_InterpreterFunctionInvalidateV1(PyObject *);
PyAPI_FUNC(int) PySoac_InterpreterFunctionEnteredV1(PyObject *);
PyAPI_FUNC(PyObject *) PySoac_NewInterpreterExecutionGuardV1(void);
PyAPI_FUNC(int) PySoac_InvalidateInterpreterExecutionGuardV1(PyObject *);
PyAPI_FUNC(int) PySoac_BindInterpreterFunctionExecutionV1(PyObject *, PyObject *);

/* GIL-build-only callback ABI V4; unchanged frame and call views remain V1.
 * Free-threaded registration/evaluation fail explicitly. Per-interpreter immutable callback
 * table, exact sizeof required, every function non-NULL, unknown flags reject.
 * Semantics-preserving C forwarding/restoration of _PyFunction_Vectorcall
 * retains source validation through common native frame initialization.
 * Unowned/copy frames and mismatched actual owner/code acquire no authority.
 * Reinstalling the identical table is idempotent; replacing/teardown reuse is
 * forbidden. No inheritance into another interpreter. No Python value refs
 * are owned by the table. Callbacks return 0/no-error or -1/error; malformed
 * callback outcomes fail closed. Out-reference slots start NULL and must stay
 * NULL on callback failure. Native clears any malformed output preserving the
 * original pending error. Public view getters are callback-free on success.
 *
 * A view is a borrowed callback-scoped C object, not a capability for Python.
 * Do not retain, copy, use from another thread, or dereference after return.
 * NULL/out-of-range use fails; no promise validates arbitrary stale C memory.
 * Returned Python references may not outlive their actual native support.
 */
PyAPI_FUNC(int) PySoac_SetInterpreterCallbacksV4(
    const PySoacInterpreterCallbacksV4 *callbacks, size_t callbacks_size);

PyAPI_FUNC(PyObject *) PySoac_EvalInterpreterModuleV2(
    PyObject *module, PyObject *root_code, PyObject *source_binding);

PyAPI_FUNC(int) PySoac_GetInterpreterFrameInfoV2(
    const PySoacInterpreterFrameViewV1 *view,
    PySoacInterpreterFrameInfoV2 *out, size_t out_size);

/* Borrowed raw slot, not implicit CellGet. NULL/no error means native Unbound;
 * NULL/error means invalid view/index. In particular Py_None is not Unbound. */
PyAPI_FUNC(PyObject *) PySoac_InterpreterFrameLocalV1(
    const PySoacInterpreterFrameViewV1 *view, Py_ssize_t index);

/* Fills borrowed POD without allocation, Python callbacks,
 * attribute lookup, hash/equality, stackref casts or frame/locals materializing.
 * Kind+index selects current actual call inputs, or the already selected finite
 * decorator window during PREPARE_TYPE. There is no arbitrary stack index,
 * prefix-length query, ancestor traversal, or access to consumed parent args.
 * DECORATOR indices are original evaluation order, 0..decorator_count-1.
 * KEYWORD_VALUE/NAMES are vector-KW only; expanded calls expose their actual
 * normalized tuple/dict, not a second unpack or guessed keyword ordering.
 * Scalar operands require index=0. An absent native keywords object is NULL
 * with status 0; Python None is Py_None. Invalid kind/index/view returns -1.
 * Exact out_size is mandatory. Successful queries preserve incoming PyErr;
 * invalid queries do not replace an already pending error. No returned
 * pointer may outlive this callback and its actual native supporting owner. */
PyAPI_FUNC(int) PySoac_InterpreterCallOperandV1(
    const PySoacInterpreterCallViewV1 *view, uint32_t kind, Py_ssize_t index,
    PySoacInterpreterCallOperandV1 *out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif /* !Py_LIMITED_API */
#endif /* Py_SOAC_INTERPRETER_H */
