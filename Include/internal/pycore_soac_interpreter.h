#ifndef Py_INTERNAL_SOAC_INTERPRETER_H
#define Py_INTERNAL_SOAC_INTERPRETER_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE"
#endif

#include "pycore_typedefs.h"
#include "pycore_stackref.h"
#include "cpython/soac_interpreter.h"

/* No operand/value owner and no public stack capability. The exact incoming
 * observation is copied only into an existing synchronous checked activation. */
typedef struct {
    _PyInterpreterFrame *caller;
    Py_ssize_t instruction_units, instruction_ordinal, prefix_depth;
    uint32_t form, channel, instruction_argument, committed;
    Py_ssize_t positional_count, keyword_count;
} _PySoacInterpreterConsumedCallV1;

typedef struct _PySoacInterpreterCallV1 _PySoacInterpreterCallV1;

/* Private source-execution metadata, never public authority. The existing
 * frame init/GC traverse/copy-MOVE/clear sites carry this one metadata edge.
 * Unknown activation types fail closed. It owns no argument-type obligations.
 */
typedef struct {
    PyObject_HEAD
    PyObject *state;                  /* Sole owned interpreter metadata edge. */
    PyThreadState *thread;            /* Comparison only during active frame. */
    PyFunctionObject *function;       /* Borrowed, actual native frame pins. */
    PyCodeObject *code;               /* Borrowed, actual f_executable pins. */
    uint32_t kind;
    uint32_t phase;
    uint32_t source_authority;        /* Only after authenticated original entry. */
    uint32_t return_attempted;
    uint32_t failure_attempted;
    PyObject **namespace_state_out;   /* Borrowed actual __build_class__ C slot. */
    _PySoacInterpreterConsumedCallV1 incoming_call;
} _PySoacInterpreterActivationV1;

struct _PySoacInterpreterFrameViewV1 {
    _PyInterpreterFrame *frame;       /* Borrowed exact ordinary native frame. */
    PyThreadState *thread;
    const struct _PySoacInterpreterFrameViewV1 *self;
    PyObject *call_state;             /* Borrowed, enter may expose NULL. */
    Py_ssize_t instruction_units;     /* Freeze BEFORE callback/reentry. */
    Py_ssize_t instruction_ordinal;   /* Trusted base-op/cache walk, also frozen. */
    uint32_t phase;
};

/* Stack-local explicit entry context. Never stored in tstate or a Python
 * attribute; parent is supplied only by the actual native opcode call edge.
 * At initialization the new native frame already owns function/code and
 * all native Empty slots. No metadata allocation can precede that code pin.
 */
typedef struct {
    uint32_t kind;
    PyObject *subject_owner;          /* Borrowed, caller/frame supports it. */
    const PySoacInterpreterFrameViewV1 *parent;
    PyObject **namespace_state_out;   /* NULL except namespace; *out starts NULL. */
    const _PySoacInterpreterConsumedCallV1 *incoming_call; /* Before transfer only. */
} _PySoacInterpreterEntryV1;

/* Callback-local, borrowed view. SELECT uses the published ordinary stack.
 * PREPARE_TYPE gets a new view backed by that exact C dispatch's live argv or
 * tuple/dict, plus the bounded selected decorator prefix. Never persist self. */
struct _PySoacInterpreterCallViewV1 {
    const struct _PySoacInterpreterCallViewV1 *self;
    PyThreadState *thread;
    const PySoacInterpreterCallInfoV1 *info;
    _PyInterpreterFrame *input_frame;
    _PyStackRef *stack_inputs;
    PyObject *callable;
    PyObject *const *argv;
    PyObject *names;
    PyObject *expanded_args, *expanded_kwargs;
    _PyInterpreterFrame *decorator_frame;
    Py_ssize_t decorator_prefix;
    uint32_t backing; /* 0:published stack, 1:ordinary C argv */
};

struct _PySoacInterpreterCallV1 {
    _PyInterpreterFrame *caller;
    const _Py_CODEUNIT *instruction;
    PySoacInterpreterFrameViewV1 current_frame, direct_frame;
    PySoacInterpreterCallSiteV1 direct_site;
    PySoacInterpreterCallInfoV1 info;
    PySoacInterpreterCallViewV1 view;
    PySoacInterpreterCallDecisionV1 decision;
    _PySoacInterpreterConsumedCallV1 incoming;
    Py_ssize_t prefix_depth, input_depth;
    uint32_t selected, began, root_detached;
};

/* Only stack-local metadata continuations. The ordinary native result token or
 * primary exception remains in the evaluator, not in this struct. */
typedef struct {
    PyObject *invocation;  /* MOVE of the exact root frame's existing edge. */
    _PyInterpreterFrame *caller;
    const _Py_CODEUNIT *instruction;
    _PySoacInterpreterCallV1 *deferred; /* Exact C dispatch, never a frame walk. */
    uint32_t stage;
} _PySoacInterpreterRootFinishV1;

PyAPI_FUNC(int) _PySOAC_InterpreterSelectCall(
    _PyInterpreterFrame *, const _Py_CODEUNIT *, uint32_t, uint32_t,
    _PySoacInterpreterCallV1 *);
PyAPI_FUNC(int) _PySOAC_InterpreterCallCommit(
    _PySoacInterpreterCallV1 *, _PyInterpreterFrame *);
PyAPI_FUNC(void) _PySOAC_InterpreterCallClear(_PySoacInterpreterCallV1 *);
extern void _PySOAC_InterpreterCallBindingFailed(_PySoacInterpreterCallV1 *);
PyAPI_FUNC(int) _PySOAC_InterpreterCallFailed(_PySoacInterpreterCallV1 *);
PyAPI_FUNC(int) _PySOAC_InterpreterCallFinished(
    _PySoacInterpreterCallV1 *, PyObject **);
PyAPI_FUNC(void) _PySOAC_InterpreterTakeDataclassRoot(
    _PyInterpreterFrame *, _PySoacInterpreterRootFinishV1 *);
PyAPI_FUNC(int) _PySOAC_InterpreterFinishDataclassRoot(
    _PySoacInterpreterRootFinishV1 *, PyObject *);
PyAPI_FUNC(_PyInterpreterFrame *) _PySOAC_InterpreterPushCall(
    _PySoacInterpreterCallV1 *, PyThreadState *, _PyStackRef, PyObject *,
    _PyStackRef const *, size_t, PyObject *, _PyInterpreterFrame *);
PyAPI_FUNC(_PyInterpreterFrame *) _PySOAC_InterpreterPushCallEx(
    _PySoacInterpreterCallV1 *, PyThreadState *, _PyStackRef, PyObject *,
    Py_ssize_t, PyObject *, PyObject *, _PyInterpreterFrame *);
extern PyObject *_PySOAC_InterpreterCallVector(
    _PySoacInterpreterCallV1 *, _PyInterpreterFrame *, const _Py_CODEUNIT *,
    PyObject *, PyObject *const *, size_t, PyObject *);
extern PyObject *_PySOAC_InterpreterCallEvalVector(
    _PySoacInterpreterCallV1 *, PyThreadState *, PyFunctionObject *,
    PyObject *const *, size_t, PyObject *);
extern int _PySOAC_InterpreterCallPrepareType(
    _PySoacInterpreterCallV1 *, PyObject *, PyObject *, PyObject *,
    PyObject *, PyObject *, PyObject *, PyObject *, PyObject **);
extern int _PySOAC_InterpreterCallHasClass(_PySoacInterpreterCallV1 *);
extern int _PySOAC_TypeCallIsOriginal(void);
extern int _PySOAC_DescriptorFactoryIsOriginal(PyObject *);
extern int _PySOAC_PropertyFactoryIsOriginal(void);

/* Core-only joins except the explicitly PyAPI_FUNC helpers consumed by the
 * generated _testinternalcapi interpreter. These add no public capability.
 * Existing dataclass frame-aware call helpers should dispatch source context
 * first, then their unchanged dataclass context, then ordinary vectorcall. */
extern int _PySOAC_InterpreterIsActivation(PyObject *object);
extern int _PySOAC_InterpreterView(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    uint32_t phase, PyObject *state, PySoacInterpreterFrameViewV1 *view);
extern int _PySOAC_InterpreterParentView(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    PySoacInterpreterFrameViewV1 *view);
PyAPI_FUNC(int) _PySOAC_CheckedFrameReturnCommit(_PyInterpreterFrame *frame);
PyAPI_FUNC(int) _PySOAC_InterpreterCheckEvalHook(_PyInterpreterFrame *frame);
extern PyObject *_PySOAC_InterpreterEvalVector(
    PyThreadState *tstate, PyFunctionObject *function, PyObject *locals,
    PyObject *const *args, size_t argcount, PyObject *kwnames,
    const _PySoacInterpreterEntryV1 *entry);
PyAPI_FUNC(PyObject *) _PySOAC_InterpreterObjectCallFromFrame(
    _PyInterpreterFrame *parent, const _Py_CODEUNIT *this_instr,
    PyObject *callable, PyObject *args, PyObject *kwargs,
    _PySoacInterpreterCallV1 *call);

extern int _PySOAC_InterpreterBirth(
    _PyInterpreterFrame *actual_parent, const _Py_CODEUNIT *this_instr,
    PyFunctionObject *fresh_function);
PyAPI_FUNC(int) _PySOAC_InterpreterFunctionAttribute(
    _PyInterpreterFrame *actual_parent, const _Py_CODEUNIT *this_instr,
    PyFunctionObject *function, uint32_t attribute_flag,
    PyObject *borrowed_installed_value);
extern int _PySOAC_InterpreterInitFrame(
    _PyInterpreterFrame *frame, const _PySoacInterpreterEntryV1 *entry);
extern int _PySOAC_CheckedFrameBound(_PyInterpreterFrame *frame);
PyAPI_FUNC(int) _PySOAC_CheckedFrameExecution(_PyInterpreterFrame *frame);
PyAPI_FUNC(int) _PySOAC_CheckedFrameReturn(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    PyObject *borrowed_result);
PyAPI_FUNC(int) _PySOAC_CheckedFrameFailed(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr);
extern void _PySOAC_CheckedFrameClear(
    _PyInterpreterFrame *frame, uint32_t reason);

/* Native __build_class__ takes this explicit source edge, not a matching
 * __build_class__ name. On namespace success its one metadata state is detached
 * before native frame clear and moved into this C operation; clear it on every
 * failure or after handle consumption. Existing PyType handle APIs unchanged.
 * namespace_state_out points only into the waiting __build_class__ operation.
 * Success publishes *out and empties activation.state, then retires its output
 * pointer before any release; failures leave *out NULL. Clear the borrowed
 * destination before every escaped-frame copy or native stack retirement. */
extern PyObject *_PySOAC_InterpreterBuildClassFromFrame(
    _PyInterpreterFrame *actual_parent, const _Py_CODEUNIT *this_instr,
    PyObject *builtin,
    PyObject *const *args, size_t nargsf, PyObject *kwnames,
    _PySoacInterpreterCallV1 *call);
PyAPI_FUNC(int) _PySOAC_InterpreterDefinitionStore(
    _PyInterpreterFrame *frame, const _Py_CODEUNIT *this_instr,
    uint32_t actual_lane, PyObject *borrowed_value);

/* Required implementation invariants:
 * - Exact owner/source/native code+parent checks precede callback grants.
 * - Ordinary _PyFrame_Initialize, initialize_locals, COPY_FREE_VARS/MAKE_CELL,
 *   RETURN_GENERATOR and frame cleanup own every Python execution value.
 * - Common native init enforces actual source owners, including a restored
 *   stock vectorcall or a semantics-preserving C forwarder. Do not use public
 *   vectorcall pointer equality as authority; arbitrary/unowned frames refuse.
 * - Capture the actual source owner before binding. Incompatible fast calls
 *   deopt before operand transfer.
 * - Mark phase/attempted and unpublish borrowed fields BEFORE releases/reentry.
 * - Never route failed definition completion through callee handlers.
 * - _PyFrame_Copy MOVE-transfers the one activation and zeros the source.
 *   Completed activations are cleared before escaped-frame take_ownership.
 * - No SOAC lifetime/code-token/managed-generator/JIT sidecar is involved.
 */

#endif
