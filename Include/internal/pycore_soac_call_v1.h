/* REVIEW DRAFT ONLY. Private native producer storage, not a Rust layout or an
 * execution capability. Requires the unchanged native_reference_v1 public
 * declarations and primitive implementation. No producer is enabled by this
 * header. A C probe must link the matching library, never supply its own binder.
 */
#ifndef Py_INTERNAL_SOAC_CALL_V1_H
#define Py_INTERNAL_SOAC_CALL_V1_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "native_reference_v1.h"
#include "pycore_interpframe.h"

enum {
    _Py_SOAC_CALL_BINDING_V1 = 1,
    _Py_SOAC_CALL_BOUND_V1,
    _Py_SOAC_CALL_BODY_READY_V1,
    _Py_SOAC_CALL_TAKEN_V1,
    _Py_SOAC_CALL_ABORTED_V1,
};

struct PySoacBoundCallViewV1 {
    const PySoacBoundCallViewV1 *self;
    uint32_t abi_version;
    uint32_t state;
    PyThreadState *thread;
    _PyInterpreterFrame *frame;       /* temporary, NEVER current/exposed */
    _PyInterpreterFrame *caller;      /* borrowed while producer is active */
    _PyStackRef *ready_stackpointer;  /* producer's published post-DEAD SP */
    size_t frame_size;
    PySoacSourceCallInfoV1 info;
    unsigned char *supplied;         /* borrowed caller-owned scratch */
    PySoacRefV1 namespace_value;     /* moved from f_locals, before Ready */
};

/* Validated native producer only. Infallible/no callbacks/no new Python refs.
 * code is the exact code selected at the native frame-binding phase, not a
 * second lookup after keyword callbacks. Capture -> immutable Rust Pin -> Bind
 * has no intervening Python callback. EX normalization/unpacking which native
 * performs before that capture must remain before it in the eventual producer.
 */
PyAPI_FUNC(void) _PySoacCall_CaptureInfoV1(
    PySoacSourceCallInfoV1 *out, uint32_t kind,
    _PyStackRef function, PyCodeObject *code);

/* Caller-owned, address-stable fresh view + scratch for the whole transaction.
 * supplied has exactly captured->supplied_count bytes (NULL only at zero).
 * Native inputs obey _PyEvalFramePushAndInit's shape/liveness preconditions;
 * violations are programming errors, not an ambiguous consumption result.
 *
 * COMMIT: consumes function, raw owned locals, and EVERY argument token on
 * both success and failure, exactly once. kwnames and captured are borrowed.
 * The caller must retire the obsolete input-array transport bits without
 * closing them again. The view never owns/frees the mask or argument scratch.
 *
 * Return 0: Bound, not BodyReady. Return -1: Aborted; no view access is valid,
 * no temporary frame survives, and the native binding exception is pending.
 * No original bytecode, strict predicate, or Execute callback is run here.
 */
PyAPI_FUNC(int) _PySoacCall_BindV1(
    PySoacBoundCallViewV1 *view, PyThreadState *thread,
    const PySoacSourceCallInfoV1 *captured,
    _PyStackRef function, PyObject *locals,
    const _PyStackRef *args, Py_ssize_t argcount, PyObject *kwnames,
    unsigned char *supplied, Py_ssize_t supplied_count);

/* The producer has completed its exact CALL/KW/EX container/operand retirement
 * and DEAD/SYNC_SP phase. It must publish its post-retirement stack pointer
 * before this barrier. NULL is valid only with no native current parent.
 * Checks same thread, current parent, top temporary frame and published SP.
 * It cannot prove that EX containers were retired: that remains the explicit
 * producer obligation, with separate before-body behavioral controls required.
 * No Execute is invoked. Rejection leaves Bound unchanged; Abort is still valid.
 */
PyAPI_FUNC(int) _PySoacCall_MarkBodyReadyV1(
    PySoacBoundCallViewV1 *view, _PyStackRef *post_retirement_stackpointer);

#endif
