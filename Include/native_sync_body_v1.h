/* Native synchronous source-body interval. No execution authority is created. */
#ifndef PY_SOAC_SYNC_BODY_V1_H
#define PY_SOAC_SYNC_BODY_V1_H

#include <Python.h>
#include <stddef.h>
#include <stdint.h>
#include "native_reference_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Py_SOAC_SYNC_BODY_ABI_V1 1u
#define Py_SOAC_SYNC_BODY_V1_WORDS 12u
#define Py_SOAC_SYNC_BODY_GIL_64 1u
#define Py_SOAC_SYNC_BODY_DEBUG_HANDLES 2u
#define Py_SOAC_SYNC_BODY_NATIVE_RECURSION 4u
#define Py_SOAC_SYNC_BODY_INITIAL_RESUME 8u
#define Py_SOAC_SYNC_BODY_NO_SOURCE_OBSERVERS 16u
#define Py_SOAC_SYNC_BODY_EMPTY_PREFIX_ONLY 32u

/* Caller-owned, explicitly all-zero-initialized, one-use, address-stable
 * storage. It contains an embedded native lifetime scope, never a second
 * linked frame. Do not copy/move/decode while live. This zero-initialization
 * rule is NOT the native-reference Empty rule. No Python owners reside here; the frame's native borrowed function handle is closed at End.
 */
typedef struct {
    uintptr_t _opaque[Py_SOAC_SYNC_BODY_V1_WORDS];
} PySoacSyncBodyIntervalV1;

typedef struct {
    uint32_t abi_version;
    uint32_t word_count;
    size_t interval_size;
    size_t interval_alignment;
    uint32_t capabilities;
    uint32_t reserved;
} PySoacSyncBodyLayoutV1;

/* Native instruction offset in BYTES, not a source-text byte offset. The
 * native producer derives this from captured code._co_firsttraceable; the
 * caller supplies no position. It is a site for actual initial-RESUME work,
 * not permission to execute code or a position for inserted return checks.
 * code is borrowed from the same lifetime-frame executable primary.
 */
typedef struct {
    uint32_t abi_version;
    uint32_t reserved;
    PyCodeObject *code;
    int instruction_offset_bytes;
    int source_lineno;
} PySoacSyncBodyEntrySiteV1;

/* Exact version/size. GIL/64-bit build and native debug-handle capability are
 * reported explicitly. Success neither allocates nor changes pending PyErr.
 * An existing PyErr is returned unchanged before invalid-input error creation.
 */
PyAPI_FUNC(int) PyFrame_GetSoacSyncBodyLayoutV1(
    uint32_t version, size_t out_size, PySoacSyncBodyLayoutV1 *out);

/* The trusted Execute consumer has already completed BodyReady and actual
 * TakeBinding. captured is its copied native GetInfo record, supported by
 * the current real parameter/function/code primaries, not a fresh lookup of
 * mutable function code/defaults. frame owns the exact moved code token.
 * This interface does not authenticate a Rust artifact or authorize a body.
 *
 * Validate every range/size/alignment/alias, source/captured-code identity,
 * one-use state, GIL/thread and initial synchronous empty-prefix restriction
 * before publication. Current unsupported observers/eval-frame hooks refuse
 * explicitly before link; no retry or rebinding follows a consumed call.
 *
 * C-stack check precedes link. The same frame is linked/protected before
 * Python-recursion entry; native zero-prefix completeness is already true.
 * Failed Python-recursion entry refunds its decrement and terminally unlinks
 * without a callee traceback. Preserve the precise native error.
 *
 * 0: Begin succeeded, interval linked, one Python level charged, entry_site
 * initialized, Resume/End required. -1: no interval remains linked; no End.
 * A preexisting PyErr leaves all storage/frames unchanged. A fresh preflight
 * failure leaves them unchanged. A native recursion failure may consume this
 * one-use interval; all source primaries still require ordinary rollback.
 * No Python ownership edge is acquired or token primary consumed. The frame
 * does acquire its native borrowed f_funcobj handle, closed before support dies.
 */
PyAPI_FUNC(int) PyFrame_BeginSoacSyncBodyV1(
    PySoacSyncBodyIntervalV1 *interval, size_t interval_size,
    PyFrameObject *frame,
    const PySoacSourceCallInfoV1 *captured, size_t captured_size,
    PySoacSyncBodyEntrySiteV1 *entry_site, size_t entry_site_size);

/* One actual initial-RESUME phase. No original bytecode is evaluated.
 * The caller runs no callback/check/body operation between Begin and Resume.
 * Publish progress, refresh native instrumentation when required and perform
 * native check_periodics (including signal/pending/GC/GIL/async work).
 * Source observer entitlement remains excluded by admission+mutation guards.
 * 0: required argument checks/body may proceed under their independent proof.
 * -1: exact pending exception; interval STAYS linked, so caller can attach the
 * actual entry_site event and unwind/End. No late policy error replaces it.
 * Preexisting-error/invalid-call rejection changes no progress; callers must
 * distinguish API misuse from an actual failed Resume, never invent its TB.
 */
PyAPI_FUNC(int) PyFrame_ResumeSoacSyncBodyV1(
    PySoacSyncBodyIntervalV1 *interval, size_t interval_size);

/* Exactly once after successful Begin, on success or error. Validate exact
 * storage/thread/LIFO/state before any mutation; no partial counter refund.
 * Keep protection through required-return error construction and result Close
 * BEFORE this call. Increment the CURRENT Python counter once, then restore
 * parent and clear raw previous/maps. Never restore a saved counter value.
 *
 * No new periodic poll/observer check and no source-primary retirement.
 * Preserve the exact pending error across terminal parent retention; native
 * parent-frame allocation failure has the existing Leave suppression policy.
 * Existing Finish and ordered actual token Close follow, with source unlinked.
 * 0 valid; -1 invalid untouched (existing error preserved). A production
 * consumer treats unexpected invalidity as non-unwinding invariant failure,
 * not permission to drop linked storage. No suspension support in V1.
 */
PyAPI_FUNC(int) PyFrame_EndSoacSyncBodyV1(
    PySoacSyncBodyIntervalV1 *interval, size_t interval_size);

#ifdef __cplusplus
}
#endif
#endif
