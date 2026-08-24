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
#define SOAC_IP_MAX_PARAMS 16
#define SOAC_IP_MAX_EVENTS 4096
#define SOAC_IP_MAX_PENDING 1024

enum {
    SOAC_IP_ROOT_BEGIN = 1, SOAC_IP_ROOT_END, SOAC_IP_BIRTH,
    SOAC_IP_ENTER, SOAC_IP_BOUND, SOAC_IP_RETURN, SOAC_IP_LEAVE,
    SOAC_IP_PREPARE_TYPE, SOAC_IP_BIND_TYPE, SOAC_IP_STORE,
    SOAC_IP_ATTRIBUTE, SOAC_IP_STARTED, SOAC_IP_FAILED
};

typedef struct {
    uintptr_t code;
    Py_ssize_t parent;
    unsigned int kind, native_scope;
    long symtable_kind;
    Py_ssize_t provider_for_node;
    unsigned int required;
    unsigned int result_rule;     /* 0:any, 1:exact int */
    Py_ssize_t parameters;
    unsigned char rules[SOAC_IP_MAX_PARAMS]; /* 0:any,1:int,2:tuple[int],3:dict[..,int] */
} SoacInterpreterTestNode;

typedef struct {
    Py_ssize_t node, ordinal;
    unsigned int lane, origin, form;
    Py_ssize_t definition_node;  /* Exact native parent+declaration span join. */
} SoacInterpreterTestSite;

typedef struct {
    int event;
    Py_ssize_t node, serial, units, ordinal;
    unsigned int lane, boundary;
    uintptr_t value;
    Py_ssize_t refcount;
    unsigned int aux;
    Py_ssize_t slots;
    uintptr_t values[SOAC_IP_MAX_PARAMS];
    Py_ssize_t counts[SOAC_IP_MAX_PARAMS];
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
    Py_ssize_t serial, node_count, site_count, event_count;
    SoacInterpreterTestNode nodes[SOAC_IP_MAX_NODES];
    SoacInterpreterTestSite sites[SOAC_IP_MAX_SITES];
    SoacInterpreterTestEvent events[SOAC_IP_MAX_EVENTS];
} SoacInterpreterFixture;

typedef struct {
    PyObject_HEAD
    SoacInterpreterFixture *fixture; /* One actual metadata edge. */
    Py_ssize_t node, serial;
    uint32_t phase, boundary;
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
              PyObject **new_owner, uint32_t *required)
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
    event->boundary = fixture->nodes[node].required;
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
    *required = fixture->nodes[node].required;
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
    event->boundary = state->boundary;
    /* Success is scalar only: never invoke the fixture's Python probe here. */
    return 0;
}

static int
soac_ip_enter(uint32_t kind, PyObject *owner,
              const PySoacInterpreterFrameViewV1 *view,
              const PySoacInterpreterFrameViewV1 *parent,
              uint32_t snapshot, PyObject **new_state)
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
        PyCode_GetSoacStrictSourceId(info.code) == 0 && snapshot == 0;
    Py_ssize_t node = ordinary_replacement ? record->node : soac_ip_node(fixture, info.code);
    if (node < 0 || (record != NULL && record->node != node) ||
        kind != fixture->nodes[node].kind || snapshot > 1 ||
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
    state->boundary = snapshot;
    PyObject_GC_Track(state);
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_ENTER, node, state->serial);
    if (event != NULL) {
        event->boundary = snapshot;
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
soac_ip_rule(PyObject *value, unsigned int rule)
{
    if (rule == 0) return 1;
    if (value == NULL) return 0;
    if (rule == 1) return PyLong_CheckExact(value);
    if (rule == 2 && PyTuple_CheckExact(value)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(value); i++) {
            if (!PyLong_CheckExact(PyTuple_GET_ITEM(value, i))) return 0;
        }
        return 1;
    }
    if (rule == 3 && PyDict_CheckExact(value)) {
        Py_ssize_t cursor = 0;
        PyObject *key, *item;
        while (PyDict_Next(value, &cursor, &key, &item)) {
            if (!PyLong_CheckExact(item)) return 0;
        }
        return 1;
    }
    return 0;
}

static int
soac_ip_bound(PyObject *object, const PySoacInterpreterFrameViewV1 *view)
{
    SoacInterpreterTestCall *state = soac_ip_call(object);
    PySoacInterpreterFrameInfoV1 info;
    if (state == NULL || soac_ip_info(view, &info) < 0) return -1;
    if (state->phase != Py_SOAC_INTERPRETER_BINDING || !state->boundary ||
        info.call_state != object || info.phase != Py_SOAC_INTERPRETER_BOUND) {
        return soac_ip_error("interpreter fixture bound check was repeated or too early");
    }
    SoacInterpreterFixture *fixture = state->fixture;
    SoacInterpreterTestNode *node = &fixture->nodes[state->node];
    SoacInterpreterTestEvent *event = soac_ip_event(fixture, SOAC_IP_BOUND, state->node, state->serial);
    if (event == NULL) return soac_ip_error("interpreter fixture overflow");
    event->boundary = state->boundary;
    event->slots = node->parameters;
    int rejected = 0;
    for (Py_ssize_t i = 0; i < node->parameters; i++) {
        PyObject *value = PySoac_InterpreterFrameLocalV1(view, i);
        if (value == NULL) return soac_ip_error("interpreter fixture bound parameter is missing");
        event->values[i] = (uintptr_t)value;
        event->counts[i] = Py_REFCNT(value);
        rejected |= !soac_ip_rule(value, node->rules[i]);
    }
    state->phase = Py_SOAC_INTERPRETER_BOUND;
    if (soac_ip_probe(fixture, SOAC_IP_BOUND, state->node, state->serial) < 0) return -1;
    if (rejected) {
        PyErr_SetString(PyExc_TypeError, "fixture parameter contract rejected");
        return -1;
    }
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
    event->boundary = state->boundary;
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
    if (state->boundary && !soac_ip_rule(result, fixture->nodes[state->node].result_rule)) {
        PyErr_SetString(PyExc_TypeError, "fixture return contract rejected");
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
        event->boundary = state->boundary;
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
                         PyCode_GetSoacStrictSourceId(actual_code) != 0 ||
                         PyFunction_HasSoacRequiredBoundary(value))) {
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

static const PySoacInterpreterCallbacksV1 soac_ip_callbacks = {
    .abi_version = Py_SOAC_INTERPRETER_ABI_V1,
    .root_begin = soac_ip_root_begin, .root_end = soac_ip_root_end,
    .birth = soac_ip_birth, .function_attribute = soac_ip_function_attribute,
    .enter = soac_ip_enter, .bound = soac_ip_bound, .started = soac_ip_started,
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
        node->parameters = (Py_ssize_t)view.argcount + view.kwonlyargcount +
            !!(view.flags & CO_VARARGS) + !!(view.flags & CO_VARKEYWORDS);
        if (node->parameters > SOAC_IP_MAX_PARAMS) {
            return soac_ip_error("fixture parameter count exceeds observer capacity");
        }
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

static int
soac_ip_read_sites(SoacInterpreterFixture *fixture, PyObject *nodes, PyObject *tables)
{
    if (!soac_ip_tuple(tables, fixture->node_count)) {
        return soac_ip_error("fixture needs one native operation table per node");
    }
    for (Py_ssize_t i = 0; i < fixture->node_count; i++) {
        PyObject *table = PyTuple_GET_ITEM(tables, i);
        if (!soac_ip_tuple(table, 8) ||
            PyLong_AsSsize_t(PyTuple_GET_ITEM(table, 0)) != i) {
            return soac_ip_error("fixture operation table has wrong code identity");
        }
        PyObject *stores = PyTuple_GET_ITEM(table, 5);
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
                    if (fixture->nodes[n].parent != i ||
                        fixture->nodes[n].native_scope != expected_scope) continue;
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

static int
soac_ip_read_policies(SoacInterpreterFixture *fixture, PyObject *policies)
{
    if (!soac_ip_tuple(policies, -1)) return soac_ip_error("fixture policies must be a tuple");
    unsigned char seen[SOAC_IP_MAX_NODES] = {0};
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(policies); i++) {
        PyObject *row = PyTuple_GET_ITEM(policies, i);
        if (!soac_ip_tuple(row, 4) || !soac_ip_tuple(PyTuple_GET_ITEM(row, 2), -1)) {
            return soac_ip_error("fixture policy needs node, required, rules, return rule");
        }
        Py_ssize_t id = PyLong_AsSsize_t(PyTuple_GET_ITEM(row, 0));
        long required = PyLong_AsLong(PyTuple_GET_ITEM(row, 1));
        long result_rule = PyLong_AsLong(PyTuple_GET_ITEM(row, 3));
        if (PyErr_Occurred() || id <= 0 || id >= fixture->node_count || seen[id] ||
            required < 0 || required > 1 || result_rule < 0 || result_rule > 1) {
            return soac_ip_error("fixture policy identity/flags invalid");
        }
        seen[id] = 1;
        SoacInterpreterTestNode *node = &fixture->nodes[id];
        PyObject *rules = PyTuple_GET_ITEM(row, 2);
        if (PyTuple_GET_SIZE(rules) != node->parameters ||
            node->kind != Py_SOAC_INTERPRETER_FUNCTION) {
            return soac_ip_error("fixture policy does not match actual native parameter slots");
        }
        node->required = (unsigned int)required;
        node->result_rule = (unsigned int)result_rule;
        for (Py_ssize_t n = 0; n < node->parameters; n++) {
            long rule = PyLong_AsLong(PyTuple_GET_ITEM(rules, n));
            if (PyErr_Occurred() || rule < 0 || rule > 3) {
                return soac_ip_error("fixture parameter rule invalid");
            }
            node->rules[n] = (unsigned char)rule;
        }
    }
    return 0;
}

static PyObject *
soac_interpreter_fixture(PyObject *self, PyObject *args)
{
    PyObject *module, *details, *policies, *probe;
    if (!PyArg_ParseTuple(args, "OOOO:soac_interpreter_fixture",
                          &module, &details, &policies, &probe)) return NULL;
    if (!PyModule_Check(module) || !soac_ip_tuple(details, 3) ||
        !PyCode_Check(PyTuple_GET_ITEM(details, 0)) ||
        (probe != Py_None && !PyCallable_Check(probe))) {
        soac_ip_error("fixture needs actual module, native Details and scalar probe");
        return NULL;
    }
    PyObject *catalog = PyTuple_GET_ITEM(details, 2);
    if (!soac_ip_tuple(catalog, 4) ||
        PyLong_AsLong(PyTuple_GET_ITEM(catalog, 0)) != 5) {
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
        PySoac_SetInterpreterCallbacksV1(&soac_ip_callbacks, sizeof(soac_ip_callbacks)) < 0) return NULL;
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
    fixture->empty = PyTuple_New(0);
    fixture->protected_names = Py_BuildValue("(s)", "locked");
    if (fixture->empty == NULL || fixture->protected_names == NULL ||
        soac_ip_read_nodes(fixture, PyTuple_GET_ITEM(catalog, 1)) < 0 ||
        soac_ip_read_sites(fixture, PyTuple_GET_ITEM(catalog, 1), PyTuple_GET_ITEM(catalog, 3)) < 0 ||
        soac_ip_read_policies(fixture, policies) < 0) {
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
            "{s:i,s:n,s:n,s:n,s:n,s:I,s:I,s:K,s:n,s:I,s:O}",
            "event", event->event, "node", event->node, "serial", event->serial,
            "units", event->units, "ordinal", event->ordinal,
            "lane", event->lane, "boundary", event->boundary,
            "value_id", (unsigned long long)event->value,
            "refcount", event->refcount, "aux", event->aux, "slots", slots);
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
