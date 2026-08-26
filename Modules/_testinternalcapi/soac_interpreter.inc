/* Trusted native KERNEL fixture, not production ty authentication.
 *
 * Installs the proposed interpreter callbacks. It borrows exact source
 * nodes/store coordinates from one native schema-5 Details result during cold
 * setup, then stores scalar identities only. It does NOT retain that result,
 * any source code/function/module/globals, or any argument/result object.
 * Deliberate Python probes are test observers, not production checks.
 *
 * Requires the separately proposed headers + real native hooks. There are no
 * fallback exports, fake bodies, or default-PyEval permission stubs here.
 */
#include "cpython/soac_interpreter.h"
#include "pycore_soac_interpreter.h"
#include "pycore_interpframe.h"
#include "pycore_opcode_utils.h"
#include "pycore_symtable.h"

#define SOAC_IP_MAX_NODES 128
#define SOAC_IP_MAX_SITES 2048
#define SOAC_IP_MAX_VALUES 16
#define SOAC_IP_MAX_EVENTS 4096
#define SOAC_IP_MAX_PENDING 1024

/* Closed, cold test policy. Neither choice supplies production ty authority. */
enum {
    SOAC_IP_CLASS_ENFORCED = 0,
    SOAC_IP_CLASS_DYNAMIC = 1,
};

enum {
    SOAC_IP_ROOT_BEGIN = 1, SOAC_IP_ROOT_END, SOAC_IP_BIRTH,
    SOAC_IP_ENTER, SOAC_IP_RETURN = 6, SOAC_IP_LEAVE,
    SOAC_IP_PREPARE_TYPE, SOAC_IP_BIND_TYPE, SOAC_IP_STORE,
    SOAC_IP_ATTRIBUTE, SOAC_IP_STARTED, SOAC_IP_FAILED,
    SOAC_IP_CALL, SOAC_IP_SELECTED_FINISHED
};

typedef struct {
    uintptr_t code;
    Py_ssize_t parent;
    unsigned int kind, native_scope;
    long symtable_kind;
    Py_ssize_t provider_for_node;
} SoacInterpreterTestNode;

typedef struct {
    Py_ssize_t node, ordinal;
    unsigned int lane, origin, form;
    Py_ssize_t definition_node;  /* Exact native parent+declaration span join. */
} SoacInterpreterTestSite;

typedef struct {
    Py_ssize_t node, ordinal, definition_node, decorators;
    unsigned int role, detail;
} SoacInterpreterTestCallSite;

typedef struct {
    int event;
    Py_ssize_t node, serial, units, ordinal;
    unsigned int lane;
    uintptr_t value;
    Py_ssize_t refcount;
    unsigned int aux;
    Py_ssize_t slots;
    uintptr_t values[SOAC_IP_MAX_VALUES];
    Py_ssize_t counts[SOAC_IP_MAX_VALUES];
    unsigned int call_form, channel, call_flags, decorator_source;
    Py_ssize_t positional, keywords, incoming_node, incoming_ordinal;
} SoacInterpreterTestEvent;

typedef struct {
    PyObject_HEAD
    PyObject *probe;              /* Explicit test observer; no hidden value log. */
    PyObject *empty;
    PyObject *protected_names;
    /* Callback-free weak witnesses only, never function or owner primaries. */
    Py_ssize_t pending_count;
    PyObject *pending[SOAC_IP_MAX_PENDING];
    PyInterpreterState *interpreter;
    uintptr_t module, globals, root_code;
    uint64_t source_id;
    int root_phase;              /* 0:fresh, 1:consumed, 2:done, 3:failed/cleared */
    int overflow;
    unsigned int call_fault; /* Trusted test decision corruption, never source policy. */
    int class_decision;       /* Closed test-only Enforced / Dynamic decision. */
    Py_ssize_t serial, node_count, site_count, event_count;
    SoacInterpreterTestNode nodes[SOAC_IP_MAX_NODES];
    SoacInterpreterTestSite sites[SOAC_IP_MAX_SITES];
    Py_ssize_t call_count;
    SoacInterpreterTestCallSite calls[SOAC_IP_MAX_SITES];
    SoacInterpreterTestEvent events[SOAC_IP_MAX_EVENTS];
} SoacInterpreterFixture;

typedef struct {
    PyObject_HEAD
    SoacInterpreterFixture *fixture; /* One actual metadata edge. */
    Py_ssize_t node, serial;
    uint32_t phase;
} SoacInterpreterTestCall;

typedef struct {
    PyObject_HEAD
    SoacInterpreterFixture *fixture;
    Py_ssize_t node;
    uintptr_t function;          /* Comparison only, NEVER dereference later. */
    PyObject *self_weak, *provider_weak;
    Py_ssize_t birth_serial;
    int pending, terminal;
} SoacInterpreterTestFunction;

static PyTypeObject SoacInterpreterFixture_Type;
static PyTypeObject SoacInterpreterTestCall_Type;
static PyTypeObject SoacInterpreterTestFunction_Type;

static int
soac_ip_error(const char *message)
{
    if (!PyErr_Occurred()) {
        PyErr_SetString(PySoac_GetStrictRuntimeUnavailableError(), message);
    }
    return -1;
}

static int
soac_ip_tuple(PyObject *value, Py_ssize_t size)
{
    return value != NULL && PyTuple_CheckExact(value) &&
        (size < 0 || PyTuple_GET_SIZE(value) == size);
}

static SoacInterpreterFixture *
soac_ip_fixture(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacInterpreterFixture_Type)) {
        soac_ip_error("interpreter fixture owner is absent or foreign");
        return NULL;
    }
    SoacInterpreterFixture *fixture = (SoacInterpreterFixture *)object;
    if (fixture->interpreter != _PyInterpreterState_GET() ||
        fixture->probe == NULL || fixture->source_id == 0) {
        soac_ip_error("interpreter fixture owner is terminal or foreign");
        return NULL;
    }
    return fixture;
}

static SoacInterpreterTestFunction *
soac_ip_function(PyObject *function)
{
    if (function == NULL || !PyFunction_Check(function)) {
        soac_ip_error("interpreter fixture function is absent");
        return NULL;
    }
    PyObject *owner = PyFunction_GetSoacStrictOwner(function);
    if (owner == NULL || !Py_IS_TYPE(owner, &SoacInterpreterTestFunction_Type)) {
        soac_ip_error("interpreter fixture function has no actual birth owner");
        return NULL;
    }
    SoacInterpreterTestFunction *record = (SoacInterpreterTestFunction *)owner;
    if (soac_ip_fixture((PyObject *)record->fixture) == NULL ||
        record->node < 0 || record->node >= record->fixture->node_count ||
        record->function != (uintptr_t)function || record->terminal) {
        soac_ip_error("interpreter fixture function birth/code identity changed");
        return NULL;
    }
    return record;
}

static SoacInterpreterTestCall *
soac_ip_call(PyObject *object)
{
    if (object == NULL || !Py_IS_TYPE(object, &SoacInterpreterTestCall_Type)) {
        soac_ip_error("interpreter fixture call state is absent");
        return NULL;
    }
    SoacInterpreterTestCall *state = (SoacInterpreterTestCall *)object;
    if (soac_ip_fixture((PyObject *)state->fixture) == NULL ||
        state->node < 0 || state->node >= state->fixture->node_count) {
        soac_ip_error("interpreter fixture call state is terminal");
        return NULL;
    }
    return state;
}

static int
soac_ip_info(const PySoacInterpreterFrameViewV1 *view,
             PySoacInterpreterFrameInfoV1 *info)
{
    memset(info, 0, sizeof(*info));
    if (PySoac_GetInterpreterFrameInfoV1(view, info, sizeof(*info)) < 0) {
        return -1;
    }
    if (info->abi_version != Py_SOAC_INTERPRETER_ABI_V1 ||
        info->function == NULL || !PyFunction_Check(info->function) ||
        info->code == NULL || !PyCode_Check(info->code)) {
        return soac_ip_error("interpreter fixture has an invalid actual frame view");
    }
    return 0;
}

static Py_ssize_t
soac_ip_node(SoacInterpreterFixture *fixture, PyObject *code)
{
    if (!PyCode_Check(code) ||
        PyCode_GetSoacStrictSourceId(code) != fixture->source_id) {
        soac_ip_error("interpreter fixture code has foreign source identity");
        return -1;
    }
    for (Py_ssize_t i = 0; i < fixture->node_count; i++) {
        if (fixture->nodes[i].code == (uintptr_t)code) {
            return i;
        }
    }
    soac_ip_error("interpreter fixture has no exact native code node");
    return -1;
}

/* No allocations, value owners, or Python calls. Overflow is always surfaced
 * by events(); void retirement callbacks set the flag without replacing error. */
static SoacInterpreterTestEvent *
soac_ip_event(SoacInterpreterFixture *fixture, int kind, Py_ssize_t node,
              Py_ssize_t serial)
{
    if (fixture->event_count >= SOAC_IP_MAX_EVENTS) {
        fixture->overflow = 1;
        return NULL;
    }
    SoacInterpreterTestEvent *event = &fixture->events[fixture->event_count++];
    memset(event, 0, sizeof(*event));
    event->event = kind;
    event->node = node;
    event->serial = serial;
    event->units = event->ordinal = -1;
    return event;
}

static int
soac_ip_probe(SoacInterpreterFixture *fixture, int event,
              Py_ssize_t node, Py_ssize_t serial)
{
    if (fixture->overflow) {
        PyErr_SetString(PyExc_OverflowError, "interpreter fixture event buffer overflow");
        return -1;
    }
    if (fixture->probe == Py_None) {
        return 0;
    }
    /* Only scalar observations cross this explicit test callback. Passing an
     * argument/result/function object here would add a misleading value pin. */
    PyObject *result = PyObject_CallFunction(fixture->probe, "inn", event, node, serial);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static int
soac_ip_fixture_traverse(SoacInterpreterFixture *fixture, visitproc visit, void *arg)
{
    Py_VISIT(fixture->probe);
    Py_VISIT(fixture->empty);
    Py_VISIT(fixture->protected_names);
    for (Py_ssize_t i = 0; i < fixture->pending_count; i++) {
        Py_VISIT(fixture->pending[i]);
    }
    return 0;
}

static int
soac_ip_fixture_clear(SoacInterpreterFixture *fixture)
{
    fixture->root_phase = 3;
    fixture->source_id = 0;
    fixture->module = fixture->globals = fixture->root_code = 0;
    Py_CLEAR(fixture->probe);
    Py_CLEAR(fixture->empty);
    Py_CLEAR(fixture->protected_names);
    Py_ssize_t count = fixture->pending_count;
    fixture->pending_count = 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        Py_CLEAR(fixture->pending[i]);
    }
    return 0;
}

static void
soac_ip_fixture_dealloc(SoacInterpreterFixture *fixture)
{
    PyObject_GC_UnTrack(fixture);
    soac_ip_fixture_clear(fixture);
    PyObject_GC_Del(fixture);
}

static int
soac_ip_call_traverse(SoacInterpreterTestCall *state, visitproc visit, void *arg)
{
    Py_VISIT(state->fixture);
    return 0;
}

static int
soac_ip_call_clear(SoacInterpreterTestCall *state)
{
    state->phase = Py_SOAC_INTERPRETER_RETIRED;
    state->node = -1;
    Py_CLEAR(state->fixture);
    return 0;
}

static void
soac_ip_call_dealloc(SoacInterpreterTestCall *state)
{
    PyObject_GC_UnTrack(state);
    soac_ip_call_clear(state);
    PyObject_GC_Del(state);
}

static int
soac_ip_function_traverse(SoacInterpreterTestFunction *record, visitproc visit, void *arg)
{
    Py_VISIT(record->fixture);
    Py_VISIT(record->self_weak);
    Py_VISIT(record->provider_weak);
    return 0;
}

static int
soac_ip_function_clear(SoacInterpreterTestFunction *record)
{
    record->node = -1;
    record->function = 0;
    record->terminal = 1;
    record->pending = 0;
    Py_CLEAR(record->self_weak);
    Py_CLEAR(record->provider_weak);
    Py_CLEAR(record->fixture);
    return 0;
}

static void
soac_ip_function_dealloc(SoacInterpreterTestFunction *record)
{
    PyObject_GC_UnTrack(record);
    soac_ip_function_clear(record);
    PyObject_GC_Del(record);
}

static PyTypeObject SoacInterpreterTestFunction_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_testinternalcapi.InterpreterFunctionOwner",
    .tp_basicsize = sizeof(SoacInterpreterTestFunction),
    .tp_dealloc = (destructor)soac_ip_function_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_ip_function_traverse,
    .tp_clear = (inquiry)soac_ip_function_clear,
};

static PyTypeObject SoacInterpreterFixture_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_testinternalcapi.InterpreterFixture",
    .tp_basicsize = sizeof(SoacInterpreterFixture),
    .tp_dealloc = (destructor)soac_ip_fixture_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_ip_fixture_traverse,
    .tp_clear = (inquiry)soac_ip_fixture_clear,
};
static PyTypeObject SoacInterpreterTestCall_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_testinternalcapi.InterpreterCall",
    .tp_basicsize = sizeof(SoacInterpreterTestCall),
    .tp_dealloc = (destructor)soac_ip_call_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_traverse = (traverseproc)soac_ip_call_traverse,
    .tp_clear = (inquiry)soac_ip_call_clear,
};

static int
soac_ip_root_begin(PyObject *owner, PyObject *module, PyObject *code)
{
    SoacInterpreterFixture *fixture = soac_ip_fixture(owner);
    if (fixture == NULL) return -1;
    if (fixture->root_phase != 0 || fixture->module != (uintptr_t)module ||
        fixture->root_code != (uintptr_t)code || !PyModule_Check(module) ||
        fixture->globals != (uintptr_t)PyModule_GetDict(module) ||
        soac_ip_node(fixture, code) != 0) {
        return soac_ip_error("interpreter fixture root is consumed, copied or foreign");
    }
    fixture->root_phase = 1;  /* BEFORE allocation / wrapper CREATE reentry. */
    soac_ip_event(fixture, SOAC_IP_ROOT_BEGIN, 0, 0);
    return fixture->overflow ? soac_ip_error("interpreter fixture overflow") : 0;
}

static void
soac_ip_root_end(PyObject *owner, int succeeded)
{
    SoacInterpreterFixture *fixture = (SoacInterpreterFixture *)owner;
    fixture->root_phase = succeeded ? 2 : 3;
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_ROOT_END, 0, 0);
    if (event != NULL) event->aux = succeeded != 0;
}

static int
soac_ip_birth(const PySoacInterpreterFrameViewV1 *parent, PyObject *function,
              PyObject **new_owner)
{
    PySoacInterpreterFrameInfoV1 info;
    if (soac_ip_info(parent, &info) < 0 || !PyFunction_Check(function)) return -1;
    SoacInterpreterTestCall *state = soac_ip_call(info.call_state);
    if (state == NULL) return -1;
    SoacInterpreterFixture *fixture = state->fixture;
    PyFunctionObject *fn = (PyFunctionObject *)function;
    Py_ssize_t node = soac_ip_node(fixture, fn->func_code);
    if (node < 0 || fixture->nodes[node].parent != state->node ||
        fn->func_globals != info.globals ||
        (uintptr_t)fn->func_globals != fixture->globals ||
        PyFunction_GetSoacStrictOwner(function) != NULL || PyErr_Occurred()) {
        return soac_ip_error("interpreter fixture CREATE parent/code/globals mismatch");
    }
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_BIRTH, node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->value = (uintptr_t)function;
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
    /* One distinct birth metadata owner, not a shared code permission.
     * No Python probe/publication; native commit precedes actual CREATE. */
    SoacInterpreterTestFunction *record = PyObject_GC_New(
        SoacInterpreterTestFunction, &SoacInterpreterTestFunction_Type);
    if (record == NULL) return -1;
    record->fixture = (SoacInterpreterFixture *)Py_NewRef((PyObject *)fixture);
    record->node = node;
    record->function = (uintptr_t)function;
    record->birth_serial = state->serial;
    record->pending = 1;
    record->terminal = 0;
    record->self_weak = NULL;
    record->provider_weak = Py_NewRef(Py_None);
    /* Native already initialized func_weakreflist and owns the fresh function.
     * Allocation is at Birth, never at the actual attribute publication seam. */
    record->self_weak = PyWeakref_NewRef(function, NULL);
    if (record->self_weak == NULL || fixture->pending_count == SOAC_IP_MAX_PENDING) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_OverflowError, "interpreter fixture pending buffer overflow");
        }
        PyObject *error = PyErr_GetRaisedException();
        soac_ip_function_clear(record);
        PyObject_GC_Del(record);
        PyErr_SetRaisedException(error);
        return -1;
    }
    fixture->pending[fixture->pending_count++] = Py_NewRef(record->self_weak);
    PyObject_GC_Track(record);
    *new_owner = (PyObject *)record;
    return 0;
}

/* The fixture validates actual native operands/nodes, not a ty provider schema.
 * The native instruction installs the operand before this callback. All success
 * paths below are callback/allocation-free; no borrowed operand survives return. */
static int
soac_ip_function_attribute(const PySoacInterpreterFrameViewV1 *parent,
                           PyObject *function, uint32_t flag, PyObject *value)
{
    PySoacInterpreterFrameInfoV1 info;
    if (soac_ip_info(parent, &info) < 0) return -1;
    SoacInterpreterTestCall *state = soac_ip_call(info.call_state);
    SoacInterpreterTestFunction *record = soac_ip_function(function);
    if (state == NULL || record == NULL) return -1;
    if (record->fixture != state->fixture || record->birth_serial != state->serial ||
        record->fixture->nodes[record->node].parent != state->node ||
        (uintptr_t)((PyFunctionObject *)function)->func_code !=
            record->fixture->nodes[record->node].code) {
        return soac_ip_error("attribute callback has foreign actual birth/code");
    }
    PyFunctionObject *fn = (PyFunctionObject *)function;
    PyObject *installed;
    switch (flag) {
        case MAKE_FUNCTION_DEFAULTS: installed = fn->func_defaults; break;
        case MAKE_FUNCTION_KWDEFAULTS: installed = fn->func_kwdefaults; break;
        case MAKE_FUNCTION_ANNOTATIONS: installed = fn->func_annotations; break;
        case MAKE_FUNCTION_CLOSURE: installed = fn->func_closure; break;
        case MAKE_FUNCTION_ANNOTATE: installed = fn->func_annotate; break;
        default: return soac_ip_error("attribute callback has an unknown actual flag");
    }
    if (installed == NULL || installed != value) {
        return soac_ip_error("attribute callback did not observe the installed operand");
    }
    if (flag == MAKE_FUNCTION_ANNOTATE) {
        SoacInterpreterTestFunction *provider = soac_ip_function(value);
        if (provider == NULL) return -1;
        if (provider->fixture != record->fixture ||
            provider->birth_serial != record->birth_serial ||
            provider->fixture->nodes[provider->node].parent != state->node ||
            provider->fixture->nodes[provider->node].native_scope != Py_SOAC_SCOPE_ANNOTATIONS ||
            provider->fixture->nodes[provider->node].provider_for_node != record->node ||
            provider->fixture->nodes[provider->node].code !=
                (uintptr_t)((PyFunctionObject *)value)->func_code ||
            record->provider_weak != Py_None) {
            return soac_ip_error("attribute callback provider birth or reserved witness differs");
        }
        /* Share metadata weakref, not the provider function. No allocation or
         * callback: the displaced reserved value is immortal None. */
        Py_SETREF(record->provider_weak, Py_NewRef(provider->self_weak));
    }
    SoacInterpreterTestEvent *event = soac_ip_event(
        state->fixture, SOAC_IP_ATTRIBUTE, record->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->value = (uintptr_t)function;
    event->aux = flag;
    event->slots = 1;
    event->values[0] = (uintptr_t)value;
    event->counts[0] = Py_REFCNT(value);
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
    return 0;
}

/* Callback-free terminal metadata transition for this kernel. It deliberately
 * does not claim production nominal finalization/sealing. Source Store already
 * seals exact published functions; failure never terminalizes those owners.
 * The installed callback table is GIL-only. No strong weak-target acquisition
 * and no Python callback may occur between this supported weak read and use. */
static void
soac_ip_complete_pending(SoacInterpreterTestCall *state, int failed)
{
    SoacInterpreterFixture *fixture = state->fixture;
    for (Py_ssize_t i = 0; i < fixture->pending_count; i++) {
        PyObject *weak = fixture->pending[i];
        if (PyWeakref_IsDead(weak)) continue;
        PyObject *function = ((PyWeakReference *)weak)->wr_object;
        if (!PyFunction_Check(function)) continue;
        PyObject *owner = PyFunction_GetSoacStrictOwner(function);
        if (owner == NULL || !Py_IS_TYPE(owner, &SoacInterpreterTestFunction_Type)) continue;
        SoacInterpreterTestFunction *record = (SoacInterpreterTestFunction *)owner;
        if (record->fixture != fixture || record->birth_serial != state->serial ||
            !record->pending || record->terminal ||
            record->function != (uintptr_t)function ||
            PyFunction_GetSoacStrictId(function) != 0) continue;
        record->pending = 0;
        record->terminal = failed != 0;
    }
}

static int
soac_ip_started(PyObject *object, const PySoacInterpreterFrameViewV1 *view)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || soac_ip_info(view, &info) < 0) return -1;
    if (info.call_state != object || info.phase != Py_SOAC_INTERPRETER_RUNNING ||
        (state->phase != Py_SOAC_INTERPRETER_BINDING &&
         state->phase != Py_SOAC_INTERPRETER_BOUND &&
         state->phase != Py_SOAC_INTERPRETER_RUNNING) || PyErr_Occurred()) {
        return soac_ip_error("interpreter started is not a committed native VM entry");
    }
    int first = state->phase != Py_SOAC_INTERPRETER_RUNNING;
    state->phase = Py_SOAC_INTERPRETER_RUNNING;
    SoacInterpreterTestEvent *event = soac_ip_event(
        state->fixture, SOAC_IP_STARTED, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->aux = first != 0;
    /* Success is scalar only: never invoke the fixture's Python probe here. */
    return 0;
}

static int
soac_ip_enter(uint32_t kind, PyObject *owner,
              const PySoacInterpreterFrameViewV1 *view,
              const PySoacInterpreterFrameViewV1 *parent,
              PyObject **new_state)
{
    PySoacInterpreterFrameInfoV1 info;
    if (soac_ip_info(view, &info) < 0) return -1;
    SoacInterpreterTestFunction *record = NULL;
    SoacInterpreterFixture *fixture;
    if (kind == Py_SOAC_INTERPRETER_ROOT) {
        fixture = soac_ip_fixture(owner);
    }
    else {
        record = soac_ip_function(info.function);
        fixture = record == NULL ? NULL : record->fixture;
    }
    if (fixture == NULL) return -1;
    if (fixture->root_phase == 3) {
        return soac_ip_error("interpreter fixture module initialization failed");
    }
    /* STRICT_MODULES permits an unchecked/unsealed function's ordinary-code
     * replacement during initialization. Its permanent birth owner still
     * authenticates this call; it does NOT make ordinary code a source node.
     * Native keeps a non-source activation and suppresses Birth/Store/class
     * source propagation for it. Sealing later freezes that chosen ordinary
     * code; it does not retroactively make calls to it forbidden. */
    int ordinary_replacement =
        record != NULL && kind == Py_SOAC_INTERPRETER_FUNCTION &&
        fixture->nodes[record->node].kind == Py_SOAC_INTERPRETER_FUNCTION &&
        (uintptr_t)info.code != fixture->nodes[record->node].code &&
        !(((PyCodeObject *)info.code)->co_flags & CO_FUTURE_STRICT) &&
        PyCode_GetSoacStrictSourceId(info.code) == 0;
    Py_ssize_t node = ordinary_replacement ? record->node : soac_ip_node(fixture, info.code);
    if (node < 0 || (record != NULL && record->node != node) ||
        kind != fixture->nodes[node].kind ||
        (uintptr_t)info.globals != fixture->globals ||
        info.phase != Py_SOAC_INTERPRETER_BINDING || info.call_state != NULL) {
        return soac_ip_error("interpreter fixture entry has wrong frame/kind/phase");
    }
    if (kind != Py_SOAC_INTERPRETER_ROOT &&
        PyFunction_GetSoacStrictOwner(info.function) != owner) {
        return soac_ip_error("interpreter fixture entry lacks its actual function owner");
    }
    if (kind == Py_SOAC_INTERPRETER_ROOT && fixture->root_phase != 1) {
        return soac_ip_error("interpreter fixture root entry has no consumed attempt");
    }
    if (kind == Py_SOAC_INTERPRETER_CLASS_NAMESPACE) {
        PySoacInterpreterFrameInfoV1 caller;
        if (parent == NULL || soac_ip_info(parent, &caller) < 0) return -1;
        SoacInterpreterTestCall *caller_state = soac_ip_call(caller.call_state);
        if (caller_state == NULL || caller_state->fixture != fixture ||
            fixture->nodes[node].parent != caller_state->node) {
            return soac_ip_error("interpreter fixture class lacks its explicit parent");
        }
    }
    SoacInterpreterTestCall *state = PyObject_GC_New(
        SoacInterpreterTestCall, &SoacInterpreterTestCall_Type);
    if (state == NULL) return -1;
    state->fixture = (SoacInterpreterFixture *)Py_NewRef((PyObject *)fixture);
    state->node = node;
    state->serial = ++fixture->serial;
    state->phase = Py_SOAC_INTERPRETER_BINDING;
    PyObject_GC_Track(state);
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_ENTER, node, state->serial);
    if (event != NULL) {
        event->aux = ordinary_replacement != 0;
        event->value = (uintptr_t)info.code; /* Captured frame code; scalar only. */
    }
    if (soac_ip_probe(fixture, SOAC_IP_ENTER, node, state->serial) < 0) {
        PyObject *error = PyErr_GetRaisedException();
        Py_DECREF(state);
        PyErr_SetRaisedException(error);
        return -1;
    }
    *new_state = (PyObject *)state;
    return 0;
}

static int
soac_ip_returned(PyObject *object, const PySoacInterpreterFrameViewV1 *view,
                 PyObject *result)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || soac_ip_info(view, &info) < 0) return -1;
    if (state->phase != Py_SOAC_INTERPRETER_RUNNING ||
        info.phase != Py_SOAC_INTERPRETER_RETURNING || info.call_state != object ||
        result == NULL || PyErr_Occurred()) {
        return soac_ip_error("interpreter fixture return was repeated or has a body error");
    }
    state->phase = Py_SOAC_INTERPRETER_RETURNING; /* Before probe/reentry. */
    SoacInterpreterFixture *fixture = state->fixture;
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_RETURN, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->value = (uintptr_t)result;
    event->refcount = Py_REFCNT(result);
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
    if (soac_ip_probe(fixture, SOAC_IP_RETURN, state->node, state->serial) < 0) {
        PyObject *error = PyErr_GetRaisedException();
        soac_ip_complete_pending(state, 1);
        PyErr_SetRaisedException(error);
        return -1;
    }
    soac_ip_complete_pending(state, 0);
    return 0;
}

static int
soac_ip_failed(PyObject *object, const PySoacInterpreterFrameViewV1 *view)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || soac_ip_info(view, &info) < 0) return -1;
    if (info.call_state != object || info.phase != Py_SOAC_INTERPRETER_FAILING ||
        state->phase == Py_SOAC_INTERPRETER_FAILING ||
        state->phase == Py_SOAC_INTERPRETER_RETIRED || PyErr_Occurred()) {
        return soac_ip_error("interpreter failure completion has a repeated or attached primary error");
    }
    state->phase = Py_SOAC_INTERPRETER_FAILING; /* Before any probe/reentry. */
    SoacInterpreterTestEvent *event = soac_ip_event(
        state->fixture, SOAC_IP_FAILED, state->node, state->serial);
    if (event != NULL) {
        event->units = info.instruction_units;
        event->ordinal = info.instruction_ordinal;
        }
    int status = soac_ip_probe(state->fixture, SOAC_IP_FAILED, state->node, state->serial);
    if (status < 0) {
        PyObject *secondary = PyErr_GetRaisedException();
        soac_ip_complete_pending(state, 1);  /* Before the native unraisable hook. */
        PyErr_SetRaisedException(secondary);
        return -1;
    }
    soac_ip_complete_pending(state, 0);
    return 0;
}

static void
soac_ip_leave(PyObject *object, uint32_t reason)
{
    SoacInterpreterTestCall *state = (SoacInterpreterTestCall *)object;
    state->phase = Py_SOAC_INTERPRETER_RETIRED;
    SoacInterpreterTestEvent *event = soac_ip_event(
        state->fixture, SOAC_IP_LEAVE, state->node, state->serial);
    if (event != NULL) event->aux = reason;
}

static int
soac_ip_bind_type(PyObject *object, PyObject *actual_type)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    if (state == NULL) return -1;
    SoacInterpreterFixture *fixture = state->fixture;
    if (!PyType_Check(actual_type) ||
        PyType_GetSoacContractOwner(actual_type) != object ||
        state->phase != Py_SOAC_INTERPRETER_RETIRED ||
        fixture->nodes[state->node].kind != Py_SOAC_INTERPRETER_CLASS_NAMESPACE) {
        return soac_ip_error("interpreter fixture pre-Ready owner was not installed");
    }
    /* Required callback-free pre-Ready observer. Reuse the actual namespace
     * call-state as owner; no hidden shared 'currently preparing class' slot
     * that a nested allocation/finalizer could overwrite. */
    SoacInterpreterTestEvent *event = soac_ip_event(
        fixture, SOAC_IP_BIND_TYPE, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->value = (uintptr_t)actual_type;
    event->aux = (((PyTypeObject *)actual_type)->tp_flags & Py_TPFLAGS_READY) != 0;
    return 0;
}

static int
soac_ip_prepare_type(PyObject *object,
                     const PySoacInterpreterFrameViewV1 *parent,
#ifdef Py_SOAC_INTERPRETER_CALL_ABI_V1
                     const PySoacInterpreterCallInfoV1 *call_info,
                     const PySoacInterpreterCallViewV1 *call_operands,
#endif
                     PyObject *function, PyObject *metaclass, PyObject *name,
                     PyObject *bases, PyObject *namespace_dict, PyObject *keywords,
                     PyObject **new_handle)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || soac_ip_info(parent, &info) < 0) return -1;
    SoacInterpreterFixture *fixture = state->fixture;
    if (state->phase != Py_SOAC_INTERPRETER_RETIRED ||
        fixture->nodes[state->node].kind != Py_SOAC_INTERPRETER_CLASS_NAMESPACE ||
        !PyFunction_Check(function)) {
        return soac_ip_error("interpreter fixture class state was not transferred");
    }
    SoacInterpreterTestFunction *record = soac_ip_function(function);
    if (record == NULL || record->fixture != fixture || record->node != state->node) {
        return soac_ip_error("interpreter fixture class function owner mismatch");
    }
    /* Intentional fixture dynamic decline before any handle is installed. */
    if (metaclass != (PyObject *)&PyType_Type || !PyDict_CheckExact(namespace_dict)) {
        return 0;
    }
    SoacInterpreterTestEvent *event = soac_ip_event(
        fixture, SOAC_IP_PREPARE_TYPE, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->value = (uintptr_t)function;
    event->refcount = Py_REFCNT(function);
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
#ifdef Py_SOAC_INTERPRETER_CALL_ABI_V1
    if (call_info == NULL || call_info->phase != Py_SOAC_INTERPRETER_CALL_PREPARE_TYPE ||
        call_info->decorator_count < 0 || call_info->decorator_count > SOAC_IP_MAX_VALUES)
        return soac_ip_error("prepare-type lost finite decorator window");
    event->decorator_source = call_info->decorator_source;
    event->slots = call_info->decorator_count;
    event->incoming_node = event->incoming_ordinal = -1;
    if (call_info->direct_caller != NULL) {
        PySoacInterpreterFrameInfoV1 direct;
        if (soac_ip_info(call_info->direct_caller->frame, &direct) < 0) return -1;
        SoacInterpreterTestCall *incoming = soac_ip_call(direct.call_state);
        if (incoming == NULL || incoming->fixture != fixture) return -1;
        event->incoming_node = incoming->node;
        event->incoming_ordinal = direct.instruction_ordinal;
    }
    for (Py_ssize_t i = 0; i < event->slots; i++) {
        PySoacInterpreterCallOperandV1 operand;
        if (PySoac_InterpreterCallOperandV1(call_operands,
            Py_SOAC_INTERPRETER_OPERAND_DECORATOR, i, &operand, sizeof(operand)) < 0)
            return -1;
        event->values[i] = (uintptr_t)operand.value;
        event->counts[i] = Py_REFCNT(operand.value);
    }
#endif
    /* Observe the complete actual CALL/decorator edge before declining. This
     * fixture choice never installs a handle, base policy, or final type seal.
     * The default Enforced decision still exercises native contract refusal. */
    if (fixture->class_decision == SOAC_IP_CLASS_DYNAMIC) {
        return 0;
    }
    PyObject *empty_keywords = keywords == NULL ? PyDict_New() : NULL;
    if (keywords == NULL && empty_keywords == NULL) return -1;
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(PySoacTypeConstructionSpec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_ENFORCED,
        .owner = object,
        .namespace_function = function,
        .name = name,
        .bases = bases,
        .namespace_dict = namespace_dict,
        .keywords = keywords == NULL ? empty_keywords : keywords,
        .bind_type = soac_ip_bind_type,
        .contract = {
            .fields = fixture->empty,
            .protected_names = fixture->protected_names,
            .final_methods = fixture->empty,
        },
    };
    PyObject *handle = PyType_NewSoacConstructionHandle(&spec);
    Py_XDECREF(empty_keywords);
    if (handle == NULL) return -1;
    *new_handle = handle;
    return 0;
}

/* Test-only stack-publication discriminator. It does not create a value owner
 * or infer source authority; the production callback sees only opaque views.
 * Analyze/regenerate every callback-bearing store with a genuinely published
 * native SP before entering this fixture or a Python/GC callback. */
static int
soac_ip_stack_contains(const PySoacInterpreterFrameViewV1 *view, PyObject *value)
{
    _PyInterpreterFrame *frame = view->frame;
    _PyStackRef *top = frame->stackpointer;
    _PyStackRef *bottom = frame->localsplus + _PyFrame_GetCode(frame)->co_nlocalsplus;
    if (top == NULL || top < bottom ||
        top > frame->localsplus + _PyFrame_GetCode(frame)->co_nlocalsplus +
                                _PyFrame_GetCode(frame)->co_stacksize) {
        return 0;
    }
    for (_PyStackRef *slot = bottom; slot < top; slot++) {
        /* Tuple/list iteration keeps a native tagged index on this stack.
         * It is not a Python object or an alternative owner of value. */
        if (!PyStackRef_IsNull(*slot) &&
            !PyStackRef_IsTaggedInt(*slot) &&
            PyStackRef_AsPyObjectBorrow(*slot) == value) return 1;
    }
    return 0;
}

static int
soac_ip_store(const PySoacInterpreterFrameViewV1 *view,
              uint32_t lane, PyObject *value)
{
    PySoacInterpreterFrameInfoV1 info;
    if (soac_ip_info(view, &info) < 0) return -1;
    SoacInterpreterTestCall *state = soac_ip_call(info.call_state);
    if (state == NULL) return -1;
    SoacInterpreterFixture *fixture = state->fixture;
    const SoacInterpreterTestSite *site = NULL;
    for (Py_ssize_t i = 0; i < fixture->site_count; i++) {
        const SoacInterpreterTestSite *candidate = &fixture->sites[i];
        if (candidate->node == state->node &&
            candidate->ordinal == info.instruction_ordinal &&
            candidate->lane == lane) {
            if (site != NULL) return soac_ip_error("ambiguous fixture Store receipt");
            site = candidate;
        }
    }
    /* Compiler-created stores lack original binding rows. Production likewise
     * performs its no-effect scalar lookup and returns for non-definitions. */
    if (site == NULL) return 0;
    SoacInterpreterTestEvent *event = soac_ip_event(
        fixture, SOAC_IP_STORE, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
    event->lane = lane;
    event->value = (uintptr_t)value;
    event->refcount = Py_REFCNT(value);
    event->aux = soac_ip_stack_contains(view, value) ? 1u : 0u;
    if (!(event->aux & 1u)) {
        return soac_ip_error("Store callback entered with unpublished operand stack");
    }
    /* Stress all original Name stores in TESTS ONLY. Production nondefinition
     * lookup returns immediately without this probe, allocation or Python. */
    if (soac_ip_probe(fixture, SOAC_IP_STORE, state->node, state->serial) < 0) return -1;
    PySoacInterpreterFrameInfoV1 after;
    if (soac_ip_info(view, &after) < 0 ||
        after.instruction_ordinal != info.instruction_ordinal ||
        after.instruction_units != info.instruction_units ||
        !soac_ip_stack_contains(view, value)) {
        return soac_ip_error("Store callback lost its captured site or live operand");
    }
    event->aux |= 2u;
    if (site->origin == Py_SOAC_BINDING_FUNCTION ||
        site->origin == Py_SOAC_BINDING_ASYNC_FUNCTION) {
        if (PyFunction_Check(value)) {
            PyObject *owner = PyFunction_GetSoacStrictOwner(value);
            if (owner != NULL && Py_IS_TYPE(owner, &SoacInterpreterTestFunction_Type)) {
                SoacInterpreterTestFunction *record = soac_ip_function(value);
                if (record == NULL) return -1;
                if (record->fixture == fixture && record->node == site->definition_node) {
                    PyObject *actual_code = ((PyFunctionObject *)value)->func_code;
                    if ((uintptr_t)actual_code != fixture->nodes[record->node].code &&
                        ((((PyCodeObject *)actual_code)->co_flags & CO_FUTURE_STRICT) ||
                         PyCode_GetSoacStrictSourceId(actual_code) != 0)) {
                        return soac_ip_error("definition completion has transplanted strict code");
                    }
                    /* Exact original code OR the documented permitted unchecked
                     * ordinary replacement; never transplant donor authority. */
                    int status = PyFunction_SealSoacStrict(value, (uint64_t)record->node + 1);
                    if (status == 0) record->pending = 0;
                    return status;
                }
            }
        }
        /* Decorator replacement does not revoke original installed policy.
         * The real Rust pending registry must finalize abandoned originals. */
        return PyErr_Occurred() ? -1 : 0;
    }
    if (site->origin == Py_SOAC_BINDING_CLASS && PyType_Check(value)) {
        PyObject *owner = PyType_GetSoacContractOwner(value);
        if (owner != NULL && Py_IS_TYPE(owner, &SoacInterpreterTestCall_Type)) {
            SoacInterpreterTestCall *class_state = soac_ip_call(owner);
            if (class_state == NULL) return -1;
            if (class_state->fixture == fixture &&
                class_state->node == site->definition_node) {
                return PyType_SealSoacContract(value, owner);
            }
        }
        return PyErr_Occurred() ? -1 : 0;
    }
    return 0;
}


#ifdef Py_SOAC_INTERPRETER_CALL_ABI_V1
static int
soac_ip_call_getter_controls(const PySoacInterpreterCallViewV1 *view,
                            PySoacInterpreterCallOperandV1 *callable)
{
    if (PySoac_InterpreterCallOperandV1(view,
            Py_SOAC_INTERPRETER_OPERAND_CALLABLE, 0,
            callable, sizeof(*callable)) < 0 || callable->value == NULL) return -1;
    PySoacInterpreterCallOperandV1 out = {0};
    if (PySoac_InterpreterCallOperandV1(view, 99, 0, &out, sizeof(out)) != -1 ||
        !PyErr_Occurred()) return soac_ip_error("invalid call operand kind was accepted");
    PyErr_Clear();
    if (PySoac_InterpreterCallOperandV1(view,
            Py_SOAC_INTERPRETER_OPERAND_CALLABLE, 0, &out, sizeof(out)-1) != -1 ||
        !PyErr_Occurred()) return soac_ip_error("short call operand output was accepted");
    PyErr_Clear();

    /* A callback-local copy is not the original opaque view. No stale pointer
     * is dereferenced after its C lifetime or adopted as a durable capability. */
    PySoacInterpreterCallViewV1 copy = *view;
    if (PySoac_InterpreterCallOperandV1(&copy,
            Py_SOAC_INTERPRETER_OPERAND_CALLABLE, 0, &out, sizeof(out)) != -1 ||
        !PyErr_Occurred()) return soac_ip_error("copied call view was accepted");
    PyErr_Clear();

    PyErr_SetString(PyExc_ValueError, "existing operand-query exception");
    PyObject *primary = PyErr_GetRaisedException();
    PyErr_SetRaisedException(Py_NewRef(primary));
    int valid = PySoac_InterpreterCallOperandV1(view,
        Py_SOAC_INTERPRETER_OPERAND_CALLABLE, 0, &out, sizeof(out));
    PyObject *after = PyErr_GetRaisedException();
    int same = valid == 0 && after == primary && out.value == callable->value;
    Py_XDECREF(after);
    PyErr_SetRaisedException(Py_NewRef(primary));
    int invalid = PySoac_InterpreterCallOperandV1(view, 99, 0, &out, sizeof(out));
    after = PyErr_GetRaisedException();
    same = same && invalid == -1 && after == primary;
    Py_XDECREF(after);
    Py_DECREF(primary);
    return same ? 0 : soac_ip_error("operand getter changed the incoming error");
}

static const SoacInterpreterTestCallSite *
soac_ip_call_site(SoacInterpreterFixture *fixture, Py_ssize_t node, Py_ssize_t ordinal)
{
    for (Py_ssize_t i = 0; i < fixture->call_count; i++) {
        const SoacInterpreterTestCallSite *site = &fixture->calls[i];
        if (site->node == node && site->ordinal == ordinal) return site;
    }
    return NULL;
}

static int
soac_ip_select_call(PyObject *object, const PySoacInterpreterCallInfoV1 *info,
                    const PySoacInterpreterCallViewV1 *view,
                    PySoacInterpreterCallDecisionV1 *out, size_t out_size)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 frame;
    if (state == NULL || info == NULL || out == NULL || out_size != sizeof(*out) ||
        info->abi_version != Py_SOAC_INTERPRETER_CALL_ABI_V1 ||
        info->phase != Py_SOAC_INTERPRETER_CALL_SELECT ||
        soac_ip_info(info->current.frame, &frame) < 0 || frame.call_state != object)
        return soac_ip_error("kernel call selection lost its actual frame");
    SoacInterpreterFixture *fixture = state->fixture;
    PySoacInterpreterCallOperandV1 callable;
    if (soac_ip_call_getter_controls(view, &callable) < 0) return -1;
    SoacInterpreterTestEvent *event = soac_ip_event(
        fixture, SOAC_IP_CALL, state->node, state->serial);
    if (event == NULL) return soac_ip_error("call event capacity exceeded");
    event->value = (uintptr_t)callable.value;
    event->refcount = Py_REFCNT(callable.value);
    event->units = frame.instruction_units;
    event->ordinal = frame.instruction_ordinal;
    event->call_form = info->current.form;
    event->channel = info->current.channel;
    event->positional = info->current.positional_count;
    event->keywords = info->current.keyword_count;
    event->incoming_node = event->incoming_ordinal = -1;
    event->call_flags = 15;
    Py_ssize_t count = info->current.positional_count;
    if (info->current.form != Py_SOAC_INTERPRETER_CALL_EXPANDED)
        count += info->current.keyword_count;
    if (count > SOAC_IP_MAX_VALUES) return soac_ip_error("call fixture operands exceed capacity");
    for (Py_ssize_t i = 0; i < count; i++) {
        uint32_t kind = i < info->current.positional_count
            ? Py_SOAC_INTERPRETER_OPERAND_POSITIONAL
            : Py_SOAC_INTERPRETER_OPERAND_KEYWORD_VALUE;
        Py_ssize_t index = i < info->current.positional_count
            ? i : i - info->current.positional_count;
        PySoacInterpreterCallOperandV1 value;
        if (PySoac_InterpreterCallOperandV1(view, kind, index, &value, sizeof(value)) < 0 ||
            value.value == NULL) return -1;
        event->values[i] = (uintptr_t)value.value;
        event->counts[i] = Py_REFCNT(value.value);
    }
    event->slots = count;
    if (soac_ip_probe(fixture, SOAC_IP_CALL, state->node, state->serial) < 0) return -1;
    const SoacInterpreterTestCallSite *site = soac_ip_call_site(
        fixture, state->node, frame.instruction_ordinal);
    if (site == NULL) return 0;
    if (site->role == Py_SOAC_CALL_CLASS) {
        out->kind = Py_SOAC_INTERPRETER_CALL_CLASS;
        out->decorator_count = site->decorators;
        out->decorator_source = info->direct_caller == NULL
            ? Py_SOAC_INTERPRETER_DECORATORS_CURRENT
            : Py_SOAC_INTERPRETER_DECORATORS_DIRECT_CALLER;
    }
    else if (site->role == Py_SOAC_CALL_GENERIC_SCOPE) {
        out->kind = Py_SOAC_INTERPRETER_CALL_GENERIC_SCOPE;
    }
    else if (site->role == Py_SOAC_CALL_DECORATOR && site->decorators == 1 &&
             site->detail == 0 &&
             (callable.value == (PyObject *)&PyStaticMethod_Type ||
              callable.value == (PyObject *)&PyClassMethod_Type ||
              callable.value == (PyObject *)&PyProperty_Type)) {
        PySoacInterpreterCallOperandV1 function;
        if (info->current.positional_count != 1 || info->current.keyword_count != 0 ||
            PySoac_InterpreterCallOperandV1(view,
                Py_SOAC_INTERPRETER_OPERAND_POSITIONAL, 0, &function, sizeof(function)) < 0)
            return soac_ip_error("kernel descriptor call has wrong operands");
        SoacInterpreterTestFunction *birth = soac_ip_function(function.value);
        if (birth == NULL || birth->fixture != fixture ||
            birth->node != site->definition_node) return -1;
        out->kind = Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR;
        out->metadata = Py_NewRef(object);
        out->expected_function_owner = (PyObject *)birth;
        out->verified_code = ((PyFunctionObject *)function.value)->func_code;
        switch (fixture->call_fault) {
            case 0: break;
            case 1: out->kind = 99; break;
            case 2:
                out->kind = Py_SOAC_INTERPRETER_CALL_ORDINARY;
                out->expected_function_owner = out->verified_code = NULL;
                break;
            case 3:
                out->kind = Py_SOAC_INTERPRETER_CALL_DATACLASS_ROOT;
                out->dataclass_stage = 99;
                out->expected_function_owner = out->verified_code = NULL;
                break;
            case 4: out->expected_function_owner = object; break;
            case 5: out->verified_code = Py_None; break;
            case 6: out->dataclass_stage = 1; break;
        }
    }
    return 0;
}

static int
soac_ip_selected_finished(PyObject *object, const PySoacInterpreterFrameViewV1 *caller,
                          uint32_t kind, PyObject *metadata, PyObject *dataclass_owner,
                          uint32_t stage, PyObject *result)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || kind != Py_SOAC_INTERPRETER_CALL_BUILTIN_DESCRIPTOR ||
        metadata != object || dataclass_owner != NULL || stage != 0 ||
        soac_ip_info(caller, &info) < 0 || info.call_state != object)
        return soac_ip_error("kernel selected completion is mismatched");
    if (result == NULL) return 0;
    if (PySoac_GetDescriptorBirthOwner(result) != metadata ||
        PySoac_GetDescriptorBirthId(result) == 0)
        return soac_ip_error("descriptor completion has no actual native birth");
    SoacInterpreterTestEvent *event = soac_ip_event(
        state->fixture, SOAC_IP_SELECTED_FINISHED, state->node, state->serial);
    if (event == NULL) return soac_ip_error("selected completion event capacity exceeded");
    event->value = (uintptr_t)result;
    event->refcount = Py_REFCNT(result);
    event->units = info.instruction_units;
    event->ordinal = info.instruction_ordinal;
    event->aux = kind;
    return soac_ip_probe(state->fixture, SOAC_IP_SELECTED_FINISHED, state->node, state->serial);
}
#endif

static const PySoacInterpreterCallbacksV2 soac_ip_callbacks = {
    .abi_version = Py_SOAC_INTERPRETER_CALLBACKS_ABI_V2,
    .root_begin = soac_ip_root_begin, .root_end = soac_ip_root_end,
    .birth = soac_ip_birth, .function_attribute = soac_ip_function_attribute,
    .enter = soac_ip_enter, .started = soac_ip_started,
#ifdef Py_SOAC_INTERPRETER_CALL_ABI_V1
    .call = soac_ip_select_call, .selected_call_finished = soac_ip_selected_finished,
#endif
    .returned = soac_ip_returned, .failed = soac_ip_failed, .leave = soac_ip_leave,
    .prepare_type = soac_ip_prepare_type, .definition_store = soac_ip_store,
};

/* Cold fixture setup only. Actual native Details supplies one code-node tree
 * and final Store receipts. We deliberately do not consume scope/lifetime
 * recipes, and this small fixture parser is not the production Rust parser. */
static int
soac_ip_read_nodes(SoacInterpreterFixture *fixture, PyObject *nodes)
{
    if (!soac_ip_tuple(nodes, -1) || PyTuple_GET_SIZE(nodes) < 1 ||
        PyTuple_GET_SIZE(nodes) > SOAC_IP_MAX_NODES) {
        return soac_ip_error("fixture needs a bounded exact native node tuple");
    }
    fixture->node_count = PyTuple_GET_SIZE(nodes);
    for (Py_ssize_t i = 0; i < fixture->node_count; i++) {
        PyObject *row = PyTuple_GET_ITEM(nodes, i);
        if (!soac_ip_tuple(row, 6) || PyLong_AsSsize_t(PyTuple_GET_ITEM(row, 0)) != i) {
            return soac_ip_error("fixture native node identity is not dense/exact");
        }
        PyObject *parent = PyTuple_GET_ITEM(row, 1);
        PyObject *code = PyTuple_GET_ITEM(row, 2);
        Py_ssize_t parent_id = parent == Py_None ? -1 : PyLong_AsSsize_t(parent);
        long scope = PyLong_AsLong(PyTuple_GET_ITEM(row, 3));
        long symtable_kind = PyLong_AsLong(PyTuple_GET_ITEM(row, 4));
        PySoacCodeView view;
        if (PyErr_Occurred() || !PyCode_Check(code) ||
            PySoac_GetCodeView(code, &view, sizeof(view)) < 0 ||
            view.strict_source_id != fixture->source_id ||
            !(view.flags & CO_FUTURE_STRICT) ||
            (i == 0 ? parent_id != -1 : (parent_id < 0 || parent_id >= i)) ||
            scope < Py_SOAC_SCOPE_MODULE || scope > Py_SOAC_SCOPE_ANNOTATIONS) {
            return soac_ip_error("fixture native node source/parent is invalid");
        }
        if (i == 0 && (uintptr_t)code != fixture->root_code) {
            return soac_ip_error("fixture Details root differs from code");
        }
        for (Py_ssize_t j = 0; j < i; j++) {
            if (fixture->nodes[j].code == (uintptr_t)code) {
                return soac_ip_error("fixture native nodes duplicate one code object");
            }
        }
        if (parent_id >= 0) {
            PyObject *parent_code = PyTuple_GET_ITEM(
                PyTuple_GET_ITEM(nodes, parent_id), 2);
            PyObject *constants = ((PyCodeObject *)parent_code)->co_consts;
            int found = 0;
            for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(constants); j++) {
                found |= PyTuple_GET_ITEM(constants, j) == code;
            }
            if (!found) return soac_ip_error("fixture child is not in its actual parent code");
        }
        SoacInterpreterTestNode *node = &fixture->nodes[i];
        node->code = (uintptr_t)code;
        node->parent = parent_id;
        node->native_scope = (unsigned int)scope;
        node->symtable_kind = symtable_kind;
        node->provider_for_node = -1;
        node->kind = i == 0 ? Py_SOAC_INTERPRETER_ROOT :
            scope == Py_SOAC_SCOPE_CLASS ? Py_SOAC_INTERPRETER_CLASS_NAMESPACE :
                                         Py_SOAC_INTERPRETER_FUNCTION;
    }
    /* Function providers and bodies have the same original statement location
     * and creation parent, but different actual native/symtable roles. No helper
     * name or visit-order association; missing/ambiguous association stays -1. */
    for (Py_ssize_t i = 0; i < fixture->node_count; i++) {
        SoacInterpreterTestNode *provider = &fixture->nodes[i];
        if (provider->native_scope != Py_SOAC_SCOPE_ANNOTATIONS ||
            provider->symtable_kind != AnnotationBlock) continue;
        PyObject *span = PyTuple_GET_ITEM(PyTuple_GET_ITEM(nodes, i), 5);
        if (span == Py_None) continue;
        for (Py_ssize_t j = 0; j < fixture->node_count; j++) {
            SoacInterpreterTestNode *body = &fixture->nodes[j];
            if (body->parent != provider->parent || body->symtable_kind != FunctionBlock ||
                (body->native_scope != Py_SOAC_SCOPE_FUNCTION &&
                 body->native_scope != Py_SOAC_SCOPE_ASYNC_FUNCTION)) continue;
            int same = PyObject_RichCompareBool(
                span, PyTuple_GET_ITEM(PyTuple_GET_ITEM(nodes, j), 5), Py_EQ);
            if (same < 0) return -1;
            if (same) {
                if (provider->provider_for_node >= 0) {
                    return soac_ip_error("fixture function provider association is ambiguous");
                }
                provider->provider_for_node = j;
            }
        }
    }
    return 0;
}

/* Generic definitions are published outside their type-parameter helper.
 * Accept that one exact native edge, not an ancestor search or a helper name. */
static int
soac_ip_definition_parent_matches(SoacInterpreterFixture *fixture,
                                  PyObject *nodes, PyObject *table,
                                  Py_ssize_t publisher, Py_ssize_t child,
                                  PyObject *declaration)
{
    Py_ssize_t parent = fixture->nodes[child].parent;
    if (parent == publisher) return 1;
    if (parent < 0 || fixture->nodes[parent].parent != publisher ||
        fixture->nodes[parent].native_scope != Py_SOAC_SCOPE_ANNOTATIONS ||
        fixture->nodes[parent].symtable_kind != Py_SOAC_SYMTABLE_TYPE_PARAMETERS ||
        declaration == Py_None)
        return 0;
    int same = PyObject_RichCompareBool(
        PyTuple_GET_ITEM(PyTuple_GET_ITEM(nodes, parent), 5), declaration, Py_EQ);
    if (same <= 0) return same;
    PyObject *calls = PyTuple_GET_ITEM(table, 5);
    if (!soac_ip_tuple(calls, -1))
        return soac_ip_error("fixture generic parent CALL table malformed");
    int found = 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(calls); i++) {
        PyObject *row = PyTuple_GET_ITEM(calls, i);
        if (!soac_ip_tuple(row, 2) ||
            !soac_ip_tuple(PyTuple_GET_ITEM(row, 0), 3) ||
            !soac_ip_tuple(PyTuple_GET_ITEM(row, 1), -1))
            return soac_ip_error("fixture generic parent CALL row malformed");
        PyObject *origin = PyTuple_GET_ITEM(row, 0);
        long role = PyLong_AsLong(PyTuple_GET_ITEM(origin, 0));
        if (PyErr_Occurred()) return -1;
        if (role != Py_SOAC_CALL_GENERIC_SCOPE) continue;
        Py_ssize_t target = PyLong_AsSsize_t(PyTuple_GET_ITEM(origin, 2));
        if (PyErr_Occurred()) return -1;
        if (target != parent) continue;
        same = PyObject_RichCompareBool(
            PyTuple_GET_ITEM(origin, 1), declaration, Py_EQ);
        if (same < 0) return -1;
        PyObject *emissions = PyTuple_GET_ITEM(row, 1);
        if (!same || PyTuple_GET_SIZE(emissions) == 0) continue;
        for (Py_ssize_t e = 0; e < PyTuple_GET_SIZE(emissions); e++) {
            PyObject *emission = PyTuple_GET_ITEM(emissions, e);
            if (!soac_ip_tuple(emission, 6))
                return soac_ip_error("fixture generic parent CALL emission malformed");
            Py_ssize_t ordinal = PyLong_AsSsize_t(PyTuple_GET_ITEM(emission, 0));
            if (PyErr_Occurred() || ordinal < 0)
                return soac_ip_error("fixture generic parent CALL has no physical site");
        }
        if (found++) return soac_ip_error("fixture generic definition edge is ambiguous");
    }
    return found;
}

static int
soac_ip_read_sites(SoacInterpreterFixture *fixture, PyObject *nodes, PyObject *tables)
{
    if (!soac_ip_tuple(tables, fixture->node_count)) {
        return soac_ip_error("fixture needs one native operation table per node");
    }
    for (Py_ssize_t i = 0; i < fixture->node_count; i++) {
        PyObject *table = PyTuple_GET_ITEM(tables, i);
        if (!soac_ip_tuple(table, 7) ||
            PyLong_AsSsize_t(PyTuple_GET_ITEM(table, 0)) != i) {
            return soac_ip_error("fixture operation table has wrong code identity");
        }
        PyObject *stores = PyTuple_GET_ITEM(table, 4);
        if (!soac_ip_tuple(stores, -1)) return soac_ip_error("fixture Store table is not a tuple");
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(stores); j++) {
            PyObject *row = PyTuple_GET_ITEM(stores, j);
            if (!soac_ip_tuple(row, 2) ||
                !soac_ip_tuple(PyTuple_GET_ITEM(row, 0), 4) ||
                !soac_ip_tuple(PyTuple_GET_ITEM(row, 1), -1)) {
                return soac_ip_error("fixture Store origin/emissions malformed");
            }
            PyObject *binding = PyTuple_GET_ITEM(row, 0);
            long origin = PyLong_AsLong(PyTuple_GET_ITEM(binding, 0));
            Py_ssize_t definition_node = -1;
            if (origin == Py_SOAC_BINDING_FUNCTION ||
                origin == Py_SOAC_BINDING_ASYNC_FUNCTION || origin == Py_SOAC_BINDING_CLASS) {
                for (Py_ssize_t n = 0; n < fixture->node_count; n++) {
                    unsigned int expected_scope = origin == Py_SOAC_BINDING_FUNCTION
                        ? Py_SOAC_SCOPE_FUNCTION : origin == Py_SOAC_BINDING_ASYNC_FUNCTION
                        ? Py_SOAC_SCOPE_ASYNC_FUNCTION : Py_SOAC_SCOPE_CLASS;
                    if (fixture->nodes[n].native_scope != expected_scope) continue;
                    int parent_matches = soac_ip_definition_parent_matches(
                        fixture, nodes, table, i, n, PyTuple_GET_ITEM(binding, 1));
                    if (parent_matches < 0) return -1;
                    if (!parent_matches) continue;
                    PyObject *native_node = PyTuple_GET_ITEM(nodes, n);
                    int same = PyObject_RichCompareBool(
                        PyTuple_GET_ITEM(native_node, 5), PyTuple_GET_ITEM(binding, 1), Py_EQ);
                    if (same < 0) return -1;
                    if (same) {
                        if (definition_node >= 0) return soac_ip_error("fixture definition node is ambiguous");
                        definition_node = n;
                    }
                }
                if (definition_node < 0) return soac_ip_error("fixture definition has no actual native child");
            }
            PyObject *emissions = PyTuple_GET_ITEM(row, 1);
            for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(emissions); k++) {
                PyObject *emission = PyTuple_GET_ITEM(emissions, k);
                if (!soac_ip_tuple(emission, 6)) return soac_ip_error("fixture Store emission malformed");
                Py_ssize_t ordinal = PyLong_AsSsize_t(PyTuple_GET_ITEM(emission, 0));
                long form = PyLong_AsLong(PyTuple_GET_ITEM(emission, 1));
                long lane = PyLong_AsLong(PyTuple_GET_ITEM(emission, 4));
                if (PyErr_Occurred() || ordinal < 0 || lane < 0 || lane > 1) {
                    return soac_ip_error("fixture Store coordinate malformed");
                }
                /* This hook covers Name-binding publication, not object writes,
                 * Delete or an invented compiler-created binding origin. */
                if (form < Py_SOAC_STORE_FAST || form > Py_SOAC_STORE_GLOBAL) continue;
                if (fixture->site_count == SOAC_IP_MAX_SITES) {
                    return soac_ip_error("fixture Store capacity exceeded");
                }
                for (Py_ssize_t n = 0; n < fixture->site_count; n++) {
                    SoacInterpreterTestSite *old = &fixture->sites[n];
                    if (old->node == i && old->ordinal == ordinal && old->lane == (unsigned)lane) {
                        return soac_ip_error("fixture native Store lane is ambiguous");
                    }
                }
                fixture->sites[fixture->site_count++] = (SoacInterpreterTestSite){
                    .node = i, .ordinal = ordinal, .lane = (unsigned int)lane,
                    .origin = (unsigned int)origin, .form = (unsigned int)form,
                    .definition_node = definition_node};
            }
        }
    }
    return 0;
}


/* Only cold native fixture setup joins original declaration spans. Runtime
 * dispatch uses the resulting exact (actual node, final ordinal) coordinates. */
static int
soac_ip_read_calls(SoacInterpreterFixture *fixture, PyObject *nodes, PyObject *tables)
{
    for (Py_ssize_t n = 0; n < fixture->node_count; n++) {
        PyObject *calls = PyTuple_GET_ITEM(PyTuple_GET_ITEM(tables, n), 5);
        if (!soac_ip_tuple(calls, -1)) return soac_ip_error("fixture CALL table malformed");
        for (Py_ssize_t r = 0; r < PyTuple_GET_SIZE(calls); r++) {
            PyObject *row = PyTuple_GET_ITEM(calls, r);
            if (!soac_ip_tuple(row, 2) || !soac_ip_tuple(PyTuple_GET_ITEM(row, 0), 3) ||
                !soac_ip_tuple(PyTuple_GET_ITEM(row, 1), -1))
                return soac_ip_error("fixture CALL origin malformed");
            PyObject *origin = PyTuple_GET_ITEM(row, 0);
            long role = PyLong_AsLong(PyTuple_GET_ITEM(origin, 0));
            if (role != Py_SOAC_CALL_CLASS && role != Py_SOAC_CALL_GENERIC_SCOPE &&
                role != Py_SOAC_CALL_DECORATOR) continue;
            Py_ssize_t detail = PyLong_AsSsize_t(PyTuple_GET_ITEM(origin, 2));
            Py_ssize_t definition = role == Py_SOAC_CALL_DECORATOR ? -1 : detail;
            if (role == Py_SOAC_CALL_DECORATOR) {
                for (Py_ssize_t b = 0; b < fixture->node_count; b++) {
                    unsigned int scope = fixture->nodes[b].native_scope;
                    if (scope != Py_SOAC_SCOPE_FUNCTION && scope != Py_SOAC_SCOPE_ASYNC_FUNCTION &&
                        scope != Py_SOAC_SCOPE_CLASS) continue;
                    int equal = PyObject_RichCompareBool(PyTuple_GET_ITEM(origin, 1),
                        PyTuple_GET_ITEM(PyTuple_GET_ITEM(nodes, b), 5), Py_EQ);
                    if (equal < 0) return -1;
                    if (equal) {
                        if (definition >= 0) return soac_ip_error("fixture decorator owner ambiguous");
                        definition = b;
                    }
                }
            }
            if (PyErr_Occurred() || detail < 0 || definition < 0 ||
                definition >= fixture->node_count)
                return soac_ip_error("fixture CALL child/detail invalid");
            Py_ssize_t decorators = 0;
            for (Py_ssize_t parent = 0; parent < fixture->node_count; parent++) {
                PyObject *other_calls = PyTuple_GET_ITEM(PyTuple_GET_ITEM(tables, parent), 5);
                for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(other_calls); k++) {
                    PyObject *other = PyTuple_GET_ITEM(PyTuple_GET_ITEM(other_calls, k), 0);
                    if (PyLong_AsLong(PyTuple_GET_ITEM(other, 0)) != Py_SOAC_CALL_DECORATOR) continue;
                    int equal = PyObject_RichCompareBool(PyTuple_GET_ITEM(other, 1),
                        PyTuple_GET_ITEM(PyTuple_GET_ITEM(nodes, definition), 5), Py_EQ);
                    if (equal < 0) return -1;
                    if (equal) {
                        Py_ssize_t index = PyLong_AsSsize_t(PyTuple_GET_ITEM(other, 2));
                        if (index < 0 || index == PY_SSIZE_T_MAX) return -1;
                        if (decorators < index + 1) decorators = index + 1;
                    }
                }
            }
            PyObject *emissions = PyTuple_GET_ITEM(row, 1);
            for (Py_ssize_t e = 0; e < PyTuple_GET_SIZE(emissions); e++) {
                PyObject *emission = PyTuple_GET_ITEM(emissions, e);
                if (!soac_ip_tuple(emission, 6) || fixture->call_count == SOAC_IP_MAX_SITES)
                    return soac_ip_error("fixture CALL emission malformed/too many");
                Py_ssize_t ordinal = PyLong_AsSsize_t(PyTuple_GET_ITEM(emission, 0));
                if (PyErr_Occurred() || ordinal < 0) return -1;
                for (Py_ssize_t old = 0; old < fixture->call_count; old++)
                    if (fixture->calls[old].node == n && fixture->calls[old].ordinal == ordinal)
                        return soac_ip_error("fixture CALL coordinate ambiguous");
                fixture->calls[fixture->call_count++] = (SoacInterpreterTestCallSite) {
                    .node = n, .ordinal = ordinal, .definition_node = definition,
                    .decorators = decorators, .role = (unsigned int)role,
                    .detail = (unsigned int)detail,
                };
            }
        }
    }
    return 0;
}

static PyObject *
soac_interpreter_fixture(PyObject *self, PyObject *args)
{
    PyObject *module, *details, *probe;
    unsigned int call_fault = 0;
    int class_decision = SOAC_IP_CLASS_ENFORCED;
    if (!PyArg_ParseTuple(args, "OOO|Ii:soac_interpreter_fixture",
                          &module, &details, &probe, &call_fault,
                          &class_decision)) return NULL;
    if (class_decision != SOAC_IP_CLASS_ENFORCED &&
        class_decision != SOAC_IP_CLASS_DYNAMIC) {
        PyErr_SetString(PyExc_ValueError, "invalid interpreter fixture class decision");
        return NULL;
    }
    if (call_fault > 6) {
        PyErr_SetString(PyExc_ValueError, "invalid interpreter call test fault");
        return NULL;
    }
    if (!PyModule_Check(module) || !soac_ip_tuple(details, 3) ||
        !PyCode_Check(PyTuple_GET_ITEM(details, 0)) ||
        (probe != Py_None && !PyCallable_Check(probe))) {
        soac_ip_error("fixture needs actual module, native Details and scalar probe");
        return NULL;
    }
    PyObject *catalog = PyTuple_GET_ITEM(details, 2);
    if (!soac_ip_tuple(catalog, 4) ||
        PyLong_AsLong(PyTuple_GET_ITEM(catalog, 0)) != Py_SOAC_CLASS_BINDINGS_SCHEMA) {
        soac_ip_error("interpreter fixture requires exact native Details schema 5");
        return NULL;
    }
    PyObject *code = PyTuple_GET_ITEM(details, 0);
    uint64_t source_id = PyCode_GetSoacStrictSourceId(code);
    if (source_id == 0 || PyErr_Occurred()) {
        soac_ip_error("fixture root is not an actual verified native compilation");
        return NULL;
    }
    if (PyType_Ready(&SoacInterpreterFixture_Type) < 0 ||
        PyType_Ready(&SoacInterpreterTestCall_Type) < 0 ||
        PyType_Ready(&SoacInterpreterTestFunction_Type) < 0 ||
        PySoac_SetInterpreterCallbacksV2(&soac_ip_callbacks, sizeof(soac_ip_callbacks)) < 0) return NULL;
    SoacInterpreterFixture *fixture = PyObject_GC_New(
        SoacInterpreterFixture, &SoacInterpreterFixture_Type);
    if (fixture == NULL) return NULL;
    memset((char *)fixture + sizeof(PyObject), 0, sizeof(*fixture) - sizeof(PyObject));
    fixture->interpreter = _PyInterpreterState_GET();
    fixture->source_id = source_id;
    fixture->module = (uintptr_t)module;
    fixture->globals = (uintptr_t)PyModule_GetDict(module);
    fixture->root_code = (uintptr_t)code;
    fixture->probe = Py_NewRef(probe);
    fixture->call_fault = call_fault;
    fixture->class_decision = class_decision;
    fixture->empty = PyTuple_New(0);
    fixture->protected_names = Py_BuildValue("(s)", "locked");
    if (fixture->empty == NULL || fixture->protected_names == NULL ||
        soac_ip_read_nodes(fixture, PyTuple_GET_ITEM(catalog, 1)) < 0 ||
        soac_ip_read_sites(fixture, PyTuple_GET_ITEM(catalog, 1), PyTuple_GET_ITEM(catalog, 3)) < 0 ||
        soac_ip_read_calls(fixture, PyTuple_GET_ITEM(catalog, 1), PyTuple_GET_ITEM(catalog, 3)) < 0) {
        PyObject *error = PyErr_GetRaisedException();
        Py_DECREF(fixture);
        PyErr_SetRaisedException(error);
        return NULL;
    }
    PyObject_GC_Track(fixture);
    return (PyObject *)fixture;
}

static PyObject *
soac_interpreter_eval(PyObject *self, PyObject *args)
{
    PyObject *fixture, *module, *root;
    if (!PyArg_ParseTuple(args, "OOO:soac_interpreter_eval",
                          &fixture, &module, &root)) return NULL;
    if (soac_ip_fixture(fixture) == NULL) return NULL;
    /* Real proposed native entry. Never substitute PyEval_EvalCode or an
     * interpreter of our own if the required implementation is missing. */
    return PySoac_EvalInterpreterModuleV1(module, root, fixture);
}

static PyObject *
soac_interpreter_events(PyObject *self, PyObject *object)
{
    SoacInterpreterFixture *fixture = soac_ip_fixture(object);
    if (fixture == NULL) return NULL;
    if (fixture->overflow) {
        PyErr_SetString(PyExc_OverflowError, "interpreter fixture event buffer overflow");
        return NULL;
    }
    PyObject *result = PyList_New(fixture->event_count);
    if (result == NULL) return NULL;
    for (Py_ssize_t i = 0; i < fixture->event_count; i++) {
        SoacInterpreterTestEvent *event = &fixture->events[i];
        PyObject *slots = PyTuple_New(event->slots);
        if (slots == NULL) goto error;
        for (Py_ssize_t j = 0; j < event->slots; j++) {
            PyObject *row = Py_BuildValue("(nKn)", j,
                (unsigned long long)event->values[j], event->counts[j]);
            if (row == NULL) {
                Py_DECREF(slots);
                goto error;
            }
            PyTuple_SET_ITEM(slots, j, row);
        }
        PyObject *row = Py_BuildValue(
            "{s:i,s:n,s:n,s:n,s:n,s:I,s:K,s:n,s:I,s:O,"
            "s:I,s:I,s:I,s:I,s:n,s:n,s:n,s:n}",
            "event", event->event, "node", event->node, "serial", event->serial,
            "units", event->units, "ordinal", event->ordinal,
            "lane", event->lane,
            "value_id", (unsigned long long)event->value,
            "refcount", event->refcount, "aux", event->aux, "slots", slots,
            "call_form", event->call_form, "channel", event->channel,
            "call_flags", event->call_flags, "decorator_source", event->decorator_source,
            "positional", event->positional, "keywords", event->keywords,
            "incoming_node", event->incoming_node, "incoming_ordinal", event->incoming_ordinal);
        Py_DECREF(slots);
        if (row == NULL) goto error;
        PyList_SET_ITEM(result, i, row);
    }
    return result;
error:
    Py_DECREF(result);
    return NULL;
}

static PyObject *
soac_ip_forward_stock(PyObject *function, PyObject *const *args,
                       size_t nargsf, PyObject *kwnames)
{
    /* A semantics-preserving C forwarding vectorcall. The common native frame
     * initializer, not this pointer, must enforce actual source ownership. */
    return _PyFunction_Vectorcall(function, args, nargsf, kwnames);
}

static PyObject *
soac_interpreter_forward_entry(PyObject *self, PyObject *function)
{
    SoacInterpreterTestFunction *record = soac_ip_function(function);
    if (record == NULL || record->fixture->nodes[record->node].code !=
        (uintptr_t)((PyFunctionObject *)function)->func_code) {
        soac_ip_error("fixture forwarding setup has changed code");
        return NULL;
    }
    PyFunction_SetVectorcall((PyFunctionObject *)function, soac_ip_forward_stock);
    Py_RETURN_NONE;
}
