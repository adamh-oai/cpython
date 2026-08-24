#ifndef Py_SOAC_INTERPRETER_H
#define Py_SOAC_INTERPRETER_H
#ifndef Py_LIMITED_API
#ifdef __cplusplus
extern "C" {
#endif

/* Versioned ordinary-interpreter enforcement ABI.
 * The trusted loader, not these hooks, authenticates the ty/source artifact.
 * Ordinary native frames/binding/closures/recursion/observers remain in use. */
#define Py_SOAC_INTERPRETER_ABI_V1 1u

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

typedef struct {
    uint32_t abi_version;             /* Set by GetInfo, exactly V1. */
    uint32_t phase;
    PyObject *function;               /* Borrowed actual frame f_funcobj. */
    PyObject *code;                   /* Borrowed actual frame f_executable. */
    PyObject *globals;                /* Borrowed actual native frame view. */
    PyObject *builtins;               /* Borrowed actual native frame view. */
    PyObject *locals;                 /* Borrowed f_locals; may be NULL. */
    PyObject *call_state;             /* Borrowed; NULL while enter runs. */
    Py_ssize_t instruction_units;     /* Captured actual opcode, code units. */
    Py_ssize_t instruction_ordinal;   /* Final ordinal, no EXTENDED_ARG/CACHE. */
    Py_ssize_t localsplus_count;
} PySoacInterpreterFrameInfoV1;

typedef struct {
    uint32_t abi_version;
    uint32_t flags;                   /* V1 requires zero. */

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
     * Success supplies ONE owned metadata reference and required bit 0 or 1.
     * Native installs owner + required bit BEFORE CREATE, retaining stock
     * vectorcall. Common native frame init enforces the actual owner/code;
     * public vectorcall pointer equality is never semantic authority.
     * Neither callback nor owner may publish the uncommitted child.
     * Birth is not final source-definition/decorator completion or sealing. */
    int (*birth)(const PySoacInterpreterFrameViewV1 *parent,
                 PyObject *function, PyObject **new_owner,
                 uint32_t *required_boundary);

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

    /* An ordinary native frame already owns its function and captured code,
     * and all unbound localsplus slots are initialized to native Empty.
     * Snapshot is captured before binder/allocation callbacks and is immutable.
     * For ROOT the explicit API caller supports subject_owner; otherwise the
     * actual function's permanent owner edge supports it. No extra value pin.
     * Success supplies ONE owned metadata state, transferred into the existing
     * checked-activation frame slot. New-state may be the owner's NewRef; its
     * contents must not duplicate function/code/maps/argument ownership. */
    int (*enter)(uint32_t kind, PyObject *subject_owner,
                 const PySoacInterpreterFrameViewV1 *frame,
                 const PySoacInterpreterFrameViewV1 *parent,
                 uint32_t boundary_snapshot, PyObject **new_call_state);

    /* Exactly once for a required synchronous boundary, only AFTER normal
     * native binding/default insertion succeeds, BEFORE COPY_FREE_VARS and
     * MAKE_CELL. All actual parameters, including unused, defaulted, varargs and varkwargs,
     * occupy their native localsplus indices. No annotation evaluation. */
    int (*bound)(PyObject *state, const PySoacInterpreterFrameViewV1 *frame);

    /* Committed default-VM entry, after ordinary binding, required checks,
     * evaluator selection and recursion success, BEFORE the first original
     * opcode. RUNNING view; source authority only. Success performs only
     * callback/allocation-free scalar validation and entry-witness recording.
     * Native generator resumes may repeat this idempotent notification; no new
     * activation, extra ownership edge or suspension ABI is introduced.
     * Binder/required-check/PEP523 refusals cannot produce this witness. */
    int (*started)(PyObject *state, const PySoacInterpreterFrameViewV1 *frame);

    /* A borrowed result only: native retains the original result token.
     * Called for EVERY successful synchronous original FUNCTION, including
     * snapshot=0, so pending child definitions can complete. Required result
     * predicates still run ONLY when the captured snapshot selected them.
     * Called once after semantic finally/handler retirement with the caller's
     * handled-exception state restored, before source locals/frame teardown.
     * Native publishes attempted before this callback. Rejection bypasses
     * the callee's handlers, closes the exact result once, preserves this
     * error, and follows the ordinary traceback/monitor-unwind exit.
     * Successful return instrumentation follows acceptance. Body errors never
     * invoke this callback; generator/coroutine/asyncgen completions are not a
     * new signature-check policy. Ordinary replacement activations have no
     * source completion authority. */
    int (*returned)(PyObject *state,
                    const PySoacInterpreterFrameViewV1 *frame,
                    PyObject *borrowed_result);

    /* Exceptional completion of an original FUNCTION only, after the native
     * no-handler decision (handled==0), semantic handlers/finally retired,
     * before native operand/local cleanup. The view phase is FAILING.
     * Native publishes failure-attempted first and saves/detaches the exact
     * primary PyErr: this callback enters with no pending error.
     * Finalize only still-pending/unsealed child definitions. If completion
     * fails, first terminalize those still-pending children (never revoke an
     * existing published module, class or function contract), then return -1
     * with the secondary error. Native reports that secondary through standard
     * unraisable handling with the actual source function as borrowed context,
     * and restores the unchanged original primary exception/context.
     * Success is 0 with no pending error. Leave remains metadata-only. */
    int (*failed)(PyObject *state, const PySoacInterpreterFrameViewV1 *frame);

    /* Scalar/metadata retirement, no Python, allocation or error replacement.
     * Once per successful enter. Namespace success reports TRANSFERRED before
     * its ONE state edge moves from frame to __build_class__'s C stack; no
     * views may be retained. Every other reason retires the state outright. */
    void (*leave)(PyObject *state, uint32_t reason);

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
                        PyObject *namespace_function, PyObject *metaclass,
                        PyObject *name, PyObject *bases,
                        PyObject *namespace_dict, PyObject *keywords,
                        PyObject **new_handle);

    /* BEFORE each actual Name-binding STORE lane. A production callback does
     * callback-free (code, ordinal, lane) lookup in the authenticated operation
     * table and immediately returns 0 for a non-definition origin: no Python,
     * allocation or name inference. Selected FUNCTION/ASYNC_FUNCTION/CLASS
     * stores complete definitions AFTER decorators at their real final lane.
     * The native producer/callsite association is required: neither spelling,
     * final SET_FUNCTION_ATTRIBUTE nor an arbitrary code pointer is authority.
     * Fused stores retain their real lane/order or use safe generic fallback. */
    int (*definition_store)(const PySoacInterpreterFrameViewV1 *frame,
                            uint32_t lane, PyObject *borrowed_value);
} PySoacInterpreterCallbacksV1;

/* Exactly four public exports. GIL-build only in V1; free-threaded
 * registration/evaluation fail explicitly. Per-interpreter immutable callback
 * table, exact sizeof required, every function non-NULL, unknown flags reject.
 * Semantics-preserving C forwarding/restoration of _PyFunction_Vectorcall
 * remains checked through common native frame initialization. Unowned/copy
 * frames and mismatched actual owner/code never acquire authority that way.
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
PyAPI_FUNC(int) PySoac_SetInterpreterCallbacksV1(
    const PySoacInterpreterCallbacksV1 *callbacks, size_t callbacks_size);

PyAPI_FUNC(PyObject *) PySoac_EvalInterpreterModuleV1(
    PyObject *module, PyObject *root_code, PyObject *module_owner);

PyAPI_FUNC(int) PySoac_GetInterpreterFrameInfoV1(
    const PySoacInterpreterFrameViewV1 *view,
    PySoacInterpreterFrameInfoV1 *out, size_t out_size);

/* Borrowed raw slot, not implicit CellGet. NULL/no error means native Unbound;
 * NULL/error means invalid view/index. In particular Py_None is not Unbound. */
PyAPI_FUNC(PyObject *) PySoac_InterpreterFrameLocalV1(
    const PySoacInterpreterFrameViewV1 *view, Py_ssize_t index);

#ifdef __cplusplus
}
#endif
#endif /* !Py_LIMITED_API */
#endif /* Py_SOAC_INTERPRETER_H */
