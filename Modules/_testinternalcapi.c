/*
 * C Extension module to test Python internal C APIs (Include/internal).
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

/* Always enable assertions */
#undef NDEBUG

#include "Python.h"
#include "pycore_soac_observers.h"
#include <string.h>
#include "pycore_backoff.h"       // JUMP_BACKWARD_INITIAL_VALUE
#include "pycore_bitutils.h"      // _Py_bswap32()
#include "pycore_bytesobject.h"   // _PyBytes_Find()
#include "pycore_call.h"          // Native dataclass fixture argument unpacking
#include "pycore_ceval.h"         // _PyEval_AddPendingCall()
#include "pycore_code.h"          // _PyCode_GetTLBCFast()
#include "pycore_compile.h"       // _PyCompile_CodeGen()
#include "pycore_context.h"       // _PyContext_NewHamtForTests()
#include "pycore_dict.h"          // PyDictValues
#include "pycore_soac_type.h"     // actual ordinary dictionary selection
#include "pycore_type_state.h"    // checked optional storage allocation
#include "pycore_fileutils.h"     // _Py_normpath()
#include "pycore_flowgraph.h"     // _PyCompile_OptimizeCfg()
#include "pycore_frame.h"         // _PyInterpreterFrame
#include "pycore_function.h"      // _PyFunction_GET_BUILTINS
#include "pycore_gc.h"            // PyGC_Head
#include "pycore_hashtable.h"     // _Py_hashtable_new()
#include "pycore_import.h"        // _PyImport_ClearExtension()
#include "pycore_initconfig.h"    // _Py_GetConfigsAsDict()
#include "pycore_instruction_sequence.h"  // _PyInstructionSequence_New()
#include "pycore_interpframe.h"   // _PyFrame_GetFunction()
#include "pycore_jit.h"           // _PyJIT_AddressInJitCode()
#include "pycore_object.h"        // _PyObject_IsFreed()
#include "pycore_optimizer.h"     // _Py_Executor_DependsOn
#include "pycore_pathconfig.h"    // _PyPathConfig_ClearGlobal()
#include "pycore_pyerrors.h"      // _PyErr_ChainExceptions1()
#include "pycore_pylifecycle.h"   // _PyInterpreterConfig_InitFromDict()
#include "pycore_pystate.h"       // _PyThreadState_GET()
#include "pycore_runtime_structs.h" // _PY_NSMALLPOSINTS
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_unicodeobject.h" // _PyUnicode_TransformDecimalAndSpaceToASCII()

#include "clinic/_testinternalcapi.c.h"

// Include test definitions from _testinternalcapi/
#include "_testinternalcapi/parts.h"

#if defined(HAVE_DLADDR) && !defined(__wasi__)
#  include <dlfcn.h>
#endif
#ifdef MS_WINDOWS
#  include <windows.h>
#  include <intrin.h>
#  include <winnt.h>
#  include <wchar.h>
#endif

#define MODULE_NAME "_testinternalcapi"


static const uintptr_t min_frame_pointer_addr = 0x1000;


static PyObject *
_get_current_module(void)
{
    // We ensured it was imported in _run_script().
    PyObject *name = PyUnicode_FromString(MODULE_NAME);
    if (name == NULL) {
        return NULL;
    }
    PyObject *mod = PyImport_GetModule(name);
    Py_DECREF(name);
    if (mod == NULL) {
        return NULL;
    }
    assert(mod != Py_None);
    return mod;
}


/* module state *************************************************************/

typedef struct {
    PyObject *record_list;
    PyObject *soac_create_globals;
    PyObject *soac_create_name;
    PyObject *soac_create_captures;
    PyObject *soac_create_callback;  /* Optional CREATE-only test observer. */
    int soac_create_watcher_plus_one;
} module_state;

static struct PyModuleDef _testcapimodule;
static void clear_soac_function_create_watch(module_state *);

static inline module_state *
get_module_state(PyObject *mod)
{
    assert(mod != NULL);
    module_state *state = PyModule_GetState(mod);
    assert(state != NULL);
    return state;
}

static int
traverse_module_state(module_state *state, visitproc visit, void *arg)
{
    Py_VISIT(state->record_list);
    Py_VISIT(state->soac_create_globals);
    Py_VISIT(state->soac_create_name);
    Py_VISIT(state->soac_create_captures);
    Py_VISIT(state->soac_create_callback);
    return 0;
}

static int
clear_module_state(module_state *state)
{
    clear_soac_function_create_watch(state);
    Py_CLEAR(state->record_list);
    return 0;
}


/* module functions *********************************************************/

/*[clinic input]
module _testinternalcapi
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=7bb583d8c9eb9a78]*/
static PyObject *
get_configs(PyObject *self, PyObject *Py_UNUSED(args))
{
    return _Py_GetConfigsAsDict();
}


static PyObject*
get_recursion_depth(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyThreadState *tstate = _PyThreadState_GET();

    return PyLong_FromLong(tstate->py_recursion_limit - tstate->py_recursion_remaining);
}


static PyObject*
get_c_recursion_remaining(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyThreadState *tstate = _PyThreadState_GET();
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    _PyThreadStateImpl *_tstate = (_PyThreadStateImpl *)tstate;
    int remaining = (int)((here_addr - _tstate->c_stack_soft_limit) / _PyOS_STACK_MARGIN_BYTES * 50);
    return PyLong_FromLong(remaining);
}

static PyObject*
get_stack_pointer(PyObject *self, PyObject *Py_UNUSED(args))
{
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    return PyLong_FromSize_t(here_addr);
}

static PyObject*
get_stack_margin(PyObject *self, PyObject *Py_UNUSED(args))
{
    return PyLong_FromSize_t(_PyOS_STACK_MARGIN_BYTES);
}

#ifdef MS_WINDOWS
static const char *
classify_address(uintptr_t addr, int jit_enabled, PyInterpreterState *interp)
{
    HMODULE module = NULL;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)addr,
            &module)) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(module, path, Py_ARRAY_LENGTH(path));
        if (len > 0 && len < Py_ARRAY_LENGTH(path)) {
            const wchar_t *base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            if (_wcsnicmp(base, L"python", 6) == 0) {
                return "python";
            }
            return "other";
        }
        /* Module resolved but path unavailable: treat as non-JIT. */
        return "other";
    }
#ifdef _Py_JIT
    if (jit_enabled && _PyJIT_AddressInJitCode(interp, addr)) {
        return "jit";
    }
#endif
    return "other";
}
#elif defined(HAVE_DLADDR) && !defined(__wasi__)
static const char *
classify_address(uintptr_t addr, int jit_enabled, PyInterpreterState *interp)
{
    Dl_info info;
    if (dladdr((void *)addr, &info) != 0
        && info.dli_fname != NULL
        && info.dli_fname[0] != '\0') {
        const char *base = strrchr(info.dli_fname, '/');
        base = base ? base + 1 : info.dli_fname;
        if (strncmp(base, "python", 6) == 0) {
            return "python";
        }
        return "other";
    }
#ifdef _Py_JIT
    if (jit_enabled && _PyJIT_AddressInJitCode(interp, addr)) {
        return "jit";
    }
#endif
    return "other";
}
#else
static const char *
classify_address(uintptr_t addr, int jit_enabled, PyInterpreterState *interp)
{
#ifdef _Py_JIT
    if (jit_enabled && _PyJIT_AddressInJitCode(interp, addr)) {
        return "jit";
    }
#endif
    return "other";
}
#endif

static PyObject *
classify_stack_addresses(PyObject *self, PyObject *args)
{
    PyObject *seq = NULL;
    int jit_enabled = 0;

    if (!PyArg_ParseTuple(args, "O|p:classify_stack_addresses",
                          &seq, &jit_enabled)) {
        return NULL;
    }
    PyObject *fast = PySequence_Fast(seq, "addresses must be iterable");
    if (fast == NULL) {
        return NULL;
    }
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    PyObject *labels = PyList_New(n);
    if (labels == NULL) {
        Py_DECREF(fast);
        return NULL;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate ? tstate->interp : NULL;
    PyObject **items = PySequence_Fast_ITEMS(fast);
    for (Py_ssize_t i = 0; i < n; i++) {
        unsigned long long value = PyLong_AsUnsignedLongLong(items[i]);
        if (PyErr_Occurred()) {
            Py_DECREF(labels);
            Py_DECREF(fast);
            return NULL;
        }
        const char *label = classify_address((uintptr_t)value, jit_enabled, interp);
        PyObject *label_obj = PyUnicode_FromString(label);
        if (label_obj == NULL) {
            Py_DECREF(labels);
            Py_DECREF(fast);
            return NULL;
        }
        PyList_SET_ITEM(labels, i, label_obj);
    }
    Py_DECREF(fast);
    return labels;
}

static PyObject *
get_jit_code_ranges(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *ranges = PyList_New(0);
    if (ranges == NULL) {
        return NULL;
    }
#ifdef _Py_JIT
    PyThreadState *tstate = _PyThreadState_GET();
    PyInterpreterState *interp = tstate ? tstate->interp : NULL;
    if (interp == NULL) {
        return ranges;
    }
    for (_PyExecutorObject *exec = interp->executor_list_head;
         exec != NULL;
         exec = exec->vm_data.links.next)
    {
        if (exec->jit_code == NULL || exec->jit_size == 0) {
            continue;
        }
        uintptr_t start = (uintptr_t)exec->jit_code;
        uintptr_t end = start + exec->jit_size;
        PyObject *start_obj = PyLong_FromUnsignedLongLong(start);
        PyObject *end_obj = PyLong_FromUnsignedLongLong(end);
        if (start_obj == NULL || end_obj == NULL) {
            Py_XDECREF(start_obj);
            Py_XDECREF(end_obj);
            Py_DECREF(ranges);
            return NULL;
        }
        PyObject *pair = PyTuple_New(2);
        if (pair == NULL) {
            Py_DECREF(start_obj);
            Py_DECREF(end_obj);
            Py_DECREF(ranges);
            return NULL;
        }
        PyTuple_SET_ITEM(pair, 0, start_obj);
        PyTuple_SET_ITEM(pair, 1, end_obj);
        if (PyList_Append(ranges, pair) < 0) {
            Py_DECREF(pair);
            Py_DECREF(ranges);
            return NULL;
        }
        Py_DECREF(pair);
    }
#endif
    return ranges;
}

static PyObject *
get_jit_backend(PyObject *self, PyObject *Py_UNUSED(args))
{
#ifdef _Py_JIT
    return PyUnicode_FromString("jit");
#elif defined(_Py_TIER2)
    return PyUnicode_FromString("interpreter");
#else
    Py_RETURN_NONE;
#endif
}

static PyObject *
manual_unwind_from_fp(uintptr_t *frame_pointer)
{
    Py_ssize_t max_depth = 200;
    int stack_grows_down = _Py_STACK_GROWS_DOWN;

    if (frame_pointer == NULL) {
        return PyList_New(0);
    }

    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    for (Py_ssize_t depth = 0;
         depth < max_depth && frame_pointer != NULL;
         depth++)
    {
        uintptr_t fp_addr = (uintptr_t)frame_pointer;
        if ((fp_addr % sizeof(uintptr_t)) != 0) {
            break;
        }
        uintptr_t return_addr = frame_pointer[1];

        PyObject *addr_obj = PyLong_FromUnsignedLongLong(return_addr);
        if (addr_obj == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        if (PyList_Append(result, addr_obj) < 0) {
            Py_DECREF(addr_obj);
            Py_DECREF(result);
            return NULL;
        }
        Py_DECREF(addr_obj);

        uintptr_t *next_fp = (uintptr_t *)frame_pointer[0];
        // Stop if the frame pointer is extremely low.
        if ((uintptr_t)next_fp < min_frame_pointer_addr) {
            break;
        }
        uintptr_t next_addr = (uintptr_t)next_fp;
        if (stack_grows_down) {
            if (next_addr <= fp_addr) {
                break;
            }
        }
        else {
            if (next_addr >= fp_addr) {
                break;
            }
        }
        frame_pointer = next_fp;
    }

    return result;
}
#if defined(__GNUC__) || defined(__clang__)
static PyObject *
manual_frame_pointer_unwind(PyObject *self, PyObject *args)
{
    uintptr_t *frame_pointer = (uintptr_t *)__builtin_frame_address(0);
    return manual_unwind_from_fp(frame_pointer);
}
#elif defined(MS_WINDOWS) && defined(_M_ARM64)
static PyObject *
manual_frame_pointer_unwind(PyObject *self, PyObject *args)
{
    CONTEXT ctx;
    uintptr_t *frame_pointer = NULL;

    RtlCaptureContext(&ctx);
    frame_pointer = (uintptr_t *)ctx.Fp;
    return manual_unwind_from_fp(frame_pointer);
}
#else
static PyObject *
manual_frame_pointer_unwind(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyErr_SetString(PyExc_RuntimeError,
                    "manual_frame_pointer_unwind is not supported on this platform");
    return NULL;
}
#endif

static PyObject*
test_bswap(PyObject *self, PyObject *Py_UNUSED(args))
{
    uint16_t u16 = _Py_bswap16(UINT16_C(0x3412));
    if (u16 != UINT16_C(0x1234)) {
        PyErr_Format(PyExc_AssertionError,
                     "_Py_bswap16(0x3412) returns %u", u16);
        return NULL;
    }

    uint32_t u32 = _Py_bswap32(UINT32_C(0x78563412));
    if (u32 != UINT32_C(0x12345678)) {
        PyErr_Format(PyExc_AssertionError,
                     "_Py_bswap32(0x78563412) returns %lu", u32);
        return NULL;
    }

    uint64_t u64 = _Py_bswap64(UINT64_C(0xEFCDAB9078563412));
    if (u64 != UINT64_C(0x1234567890ABCDEF)) {
        PyErr_Format(PyExc_AssertionError,
                     "_Py_bswap64(0xEFCDAB9078563412) returns %llu", u64);
        return NULL;
    }

    Py_RETURN_NONE;
}


static int
check_popcount(uint32_t x, int expected)
{
    // Use volatile to prevent the compiler to optimize out the whole test
    volatile uint32_t u = x;
    int bits = _Py_popcount32(u);
    if (bits != expected) {
        PyErr_Format(PyExc_AssertionError,
                     "_Py_popcount32(%lu) returns %i, expected %i",
                     (unsigned long)x, bits, expected);
        return -1;
    }
    return 0;
}


static PyObject*
test_popcount(PyObject *self, PyObject *Py_UNUSED(args))
{
#define CHECK(X, RESULT) \
    do { \
        if (check_popcount(X, RESULT) < 0) { \
            return NULL; \
        } \
    } while (0)

    CHECK(0, 0);
    CHECK(1, 1);
    CHECK(0x08080808, 4);
    CHECK(0x10000001, 2);
    CHECK(0x10101010, 4);
    CHECK(0x10204080, 4);
    CHECK(0xDEADCAFE, 22);
    CHECK(0xFFFFFFFF, 32);
    Py_RETURN_NONE;

#undef CHECK
}


static int
check_bit_length(unsigned long x, int expected)
{
    // Use volatile to prevent the compiler to optimize out the whole test
    volatile unsigned long u = x;
    int len = _Py_bit_length(u);
    if (len != expected) {
        PyErr_Format(PyExc_AssertionError,
                     "_Py_bit_length(%lu) returns %i, expected %i",
                     x, len, expected);
        return -1;
    }
    return 0;
}


static PyObject*
test_bit_length(PyObject *self, PyObject *Py_UNUSED(args))
{
#define CHECK(X, RESULT) \
    do { \
        if (check_bit_length(X, RESULT) < 0) { \
            return NULL; \
        } \
    } while (0)

    CHECK(0, 0);
    CHECK(1, 1);
    CHECK(0x1000, 13);
    CHECK(0x1234, 13);
    CHECK(0x54321, 19);
    CHECK(0x7FFFFFFF, 31);
    CHECK(0xFFFFFFFF, 32);
    Py_RETURN_NONE;

#undef CHECK
}


#define TO_PTR(ch) ((void*)(uintptr_t)ch)
#define FROM_PTR(ptr) ((uintptr_t)ptr)
#define VALUE(key) (1 + ((int)(key) - 'a'))

static Py_uhash_t
hash_char(const void *key)
{
    char ch = (char)FROM_PTR(key);
    return ch;
}


static int
hashtable_cb(_Py_hashtable_t *table,
             const void *key_ptr, const void *value_ptr,
             void *user_data)
{
    int *count = (int *)user_data;
    char key = (char)FROM_PTR(key_ptr);
    int value = (int)FROM_PTR(value_ptr);
    assert(value == VALUE(key));
    *count += 1;
    return 0;
}


static PyObject*
test_hashtable(PyObject *self, PyObject *Py_UNUSED(args))
{
    _Py_hashtable_t *table = _Py_hashtable_new(hash_char,
                                               _Py_hashtable_compare_direct);
    if (table == NULL) {
        return PyErr_NoMemory();
    }

    // Using an newly allocated table must not crash
    assert(table->nentries == 0);
    assert(table->nbuckets > 0);
    assert(_Py_hashtable_get(table, TO_PTR('x')) == NULL);

    // Test _Py_hashtable_set()
    char key;
    for (key='a'; key <= 'z'; key++) {
        int value = VALUE(key);
        if (_Py_hashtable_set(table, TO_PTR(key), TO_PTR(value)) < 0) {
            _Py_hashtable_destroy(table);
            return PyErr_NoMemory();
        }
    }
    assert(table->nentries == 26);
    assert(table->nbuckets > table->nentries);

    // Test _Py_hashtable_get_entry()
    for (key='a'; key <= 'z'; key++) {
        _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(table, TO_PTR(key));
        assert(entry != NULL);
        assert(entry->key == TO_PTR(key));
        assert(entry->value == TO_PTR(VALUE(key)));
    }

    // Test _Py_hashtable_get()
    for (key='a'; key <= 'z'; key++) {
        void *value_ptr = _Py_hashtable_get(table, TO_PTR(key));
        assert((int)FROM_PTR(value_ptr) == VALUE(key));
    }

    // Test _Py_hashtable_steal()
    key = 'p';
    void *value_ptr = _Py_hashtable_steal(table, TO_PTR(key));
    assert((int)FROM_PTR(value_ptr) == VALUE(key));
    assert(table->nentries == 25);
    assert(_Py_hashtable_get_entry(table, TO_PTR(key)) == NULL);

    // Test _Py_hashtable_foreach()
    int count = 0;
    int res = _Py_hashtable_foreach(table, hashtable_cb, &count);
    assert(res == 0);
    assert(count == 25);

    // Test _Py_hashtable_clear()
    _Py_hashtable_clear(table);
    assert(table->nentries == 0);
    assert(table->nbuckets > 0);
    assert(_Py_hashtable_get(table, TO_PTR('x')) == NULL);

    _Py_hashtable_destroy(table);
    Py_RETURN_NONE;
}


static PyObject *
test_reset_path_config(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(arg))
{
    _PyPathConfig_ClearGlobal();
    Py_RETURN_NONE;
}


static int
check_edit_cost(const char *a, const char *b, Py_ssize_t expected)
{
    int ret = -1;
    PyObject *a_obj = NULL;
    PyObject *b_obj = NULL;

    a_obj = PyUnicode_FromString(a);
    if (a_obj == NULL) {
        goto exit;
    }
    b_obj = PyUnicode_FromString(b);
    if (b_obj == NULL) {
        goto exit;
    }
    Py_ssize_t result = _Py_UTF8_Edit_Cost(a_obj, b_obj, -1);
    if (result != expected) {
        PyErr_Format(PyExc_AssertionError,
                     "Edit cost from '%s' to '%s' returns %zd, expected %zd",
                     a, b, result, expected);
        goto exit;
    }
    // Check that smaller max_edits thresholds are exceeded.
    Py_ssize_t max_edits = result;
    while (max_edits > 0) {
        max_edits /= 2;
        Py_ssize_t result2 = _Py_UTF8_Edit_Cost(a_obj, b_obj, max_edits);
        if (result2 <= max_edits) {
            PyErr_Format(PyExc_AssertionError,
                         "Edit cost from '%s' to '%s' (threshold %zd) "
                         "returns %zd, expected greater than %zd",
                         a, b, max_edits, result2, max_edits);
            goto exit;
        }
    }
    // Check that bigger max_edits thresholds don't change anything
    Py_ssize_t result3 = _Py_UTF8_Edit_Cost(a_obj, b_obj, result * 2 + 1);
    if (result3 != result) {
        PyErr_Format(PyExc_AssertionError,
                     "Edit cost from '%s' to '%s' (threshold %zd) "
                     "returns %zd, expected %zd",
                     a, b, result * 2, result3, result);
        goto exit;
    }
    ret = 0;
exit:
    Py_XDECREF(a_obj);
    Py_XDECREF(b_obj);
    return ret;
}

static PyObject *
test_edit_cost(PyObject *self, PyObject *Py_UNUSED(args))
{
    #define CHECK(a, b, n) do {              \
        if (check_edit_cost(a, b, n) < 0) {  \
            return NULL;                     \
        }                                    \
    } while (0)                              \

    CHECK("", "", 0);
    CHECK("", "a", 2);
    CHECK("a", "A", 1);
    CHECK("Apple", "Aple", 2);
    CHECK("Banana", "B@n@n@", 6);
    CHECK("Cherry", "Cherry!", 2);
    CHECK("---0---", "------", 2);
    CHECK("abc", "y", 6);
    CHECK("aa", "bb", 4);
    CHECK("aaaaa", "AAAAA", 5);
    CHECK("wxyz", "wXyZ", 2);
    CHECK("wxyz", "wXyZ123", 8);
    CHECK("Python", "Java", 12);
    CHECK("Java", "C#", 8);
    CHECK("AbstractFoobarManager", "abstract_foobar_manager", 3+2*2);
    CHECK("CPython", "PyPy", 10);
    CHECK("CPython", "pypy", 11);
    CHECK("AttributeError", "AttributeErrop", 2);
    CHECK("AttributeError", "AttributeErrorTests", 10);

    #undef CHECK
    Py_RETURN_NONE;
}


static int
check_bytes_find(const char *haystack0, const char *needle0,
                 int offset, Py_ssize_t expected)
{
    Py_ssize_t len_haystack = strlen(haystack0);
    Py_ssize_t len_needle = strlen(needle0);
    Py_ssize_t result_1 = _PyBytes_Find(haystack0, len_haystack,
                                        needle0, len_needle, offset);
    if (result_1 != expected) {
        PyErr_Format(PyExc_AssertionError,
                    "Incorrect result_1: '%s' in '%s' (offset=%zd)",
                    needle0, haystack0, offset);
        return -1;
    }
    // Allocate new buffer with no NULL terminator.
    char *haystack = PyMem_Malloc(len_haystack);
    if (haystack == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    char *needle = PyMem_Malloc(len_needle);
    if (needle == NULL) {
        PyMem_Free(haystack);
        PyErr_NoMemory();
        return -1;
    }
    memcpy(haystack, haystack0, len_haystack);
    memcpy(needle, needle0, len_needle);
    Py_ssize_t result_2 = _PyBytes_Find(haystack, len_haystack,
                                        needle, len_needle, offset);
    PyMem_Free(haystack);
    PyMem_Free(needle);
    if (result_2 != expected) {
        PyErr_Format(PyExc_AssertionError,
                    "Incorrect result_2: '%s' in '%s' (offset=%zd)",
                    needle0, haystack0, offset);
        return -1;
    }
    return 0;
}

static int
check_bytes_find_large(Py_ssize_t len_haystack, Py_ssize_t len_needle,
                       const char *needle)
{
    char *zeros = PyMem_RawCalloc(len_haystack, 1);
    if (zeros == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t res = _PyBytes_Find(zeros, len_haystack, needle, len_needle, 0);
    PyMem_RawFree(zeros);
    if (res != -1) {
        PyErr_Format(PyExc_AssertionError,
                    "check_bytes_find_large(%zd, %zd) found %zd",
                    len_haystack, len_needle, res);
        return -1;
    }
    return 0;
}

static PyObject *
test_bytes_find(PyObject *self, PyObject *Py_UNUSED(args))
{
    #define CHECK(H, N, O, E) do {               \
        if (check_bytes_find(H, N, O, E) < 0) {  \
            return NULL;                         \
        }                                        \
    } while (0)

    CHECK("", "", 0, 0);
    CHECK("Python", "", 0, 0);
    CHECK("Python", "", 3, 3);
    CHECK("Python", "", 6, 6);
    CHECK("Python", "yth", 0, 1);
    CHECK("ython", "yth", 1, 1);
    CHECK("thon", "yth", 2, -1);
    CHECK("Python", "thon", 0, 2);
    CHECK("ython", "thon", 1, 2);
    CHECK("thon", "thon", 2, 2);
    CHECK("hon", "thon", 3, -1);
    CHECK("Pytho", "zz", 0, -1);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ab", 0, -1);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ba", 0, -1);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bb", 0, -1);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", "ab", 0, 30);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaba", "ba", 0, 30);
    CHECK("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaabb", "bb", 0, 30);
    #undef CHECK

    // Hunt for segfaults
    // n, m chosen here so that (n - m) % (m + 1) == 0
    // This would make default_find in fastsearch.h access haystack[n].
    if (check_bytes_find_large(2048, 2, "ab") < 0) {
        return NULL;
    }
    if (check_bytes_find_large(4096, 16, "0123456789abcdef") < 0) {
        return NULL;
    }
    if (check_bytes_find_large(8192, 2, "ab") < 0) {
        return NULL;
    }
    if (check_bytes_find_large(16384, 4, "abcd") < 0) {
        return NULL;
    }
    if (check_bytes_find_large(32768, 2, "ab") < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject *
normalize_path(PyObject *self, PyObject *filename)
{
    Py_ssize_t size = -1;
    wchar_t *encoded = PyUnicode_AsWideCharString(filename, &size);
    if (encoded == NULL) {
        return NULL;
    }

    PyObject *result = PyUnicode_FromWideChar(_Py_normpath(encoded, size), -1);
    PyMem_Free(encoded);

    return result;
}

static PyObject *
get_getpath_codeobject(PyObject *self, PyObject *Py_UNUSED(args)) {
    return _Py_Get_Getpath_CodeObject();
}


static PyObject *
encode_locale_ex(PyObject *self, PyObject *args)
{
    PyObject *unicode;
    int current_locale = 0;
    wchar_t *wstr;
    PyObject *res = NULL;
    const char *errors = NULL;

    if (!PyArg_ParseTuple(args, "U|is", &unicode, &current_locale, &errors)) {
        return NULL;
    }
    wstr = PyUnicode_AsWideCharString(unicode, NULL);
    if (wstr == NULL) {
        return NULL;
    }
    _Py_error_handler error_handler = _Py_GetErrorHandler(errors);

    char *str = NULL;
    size_t error_pos;
    const char *reason = NULL;
    int ret = _Py_EncodeLocaleEx(wstr,
                                 &str, &error_pos, &reason,
                                 current_locale, error_handler);
    PyMem_Free(wstr);

    switch(ret) {
    case 0:
        res = PyBytes_FromString(str);
        PyMem_RawFree(str);
        break;
    case -1:
        PyErr_NoMemory();
        break;
    case -2:
        PyErr_Format(PyExc_RuntimeError, "encode error: pos=%zu, reason=%s",
                     error_pos, reason);
        break;
    case -3:
        PyErr_SetString(PyExc_ValueError, "unsupported error handler");
        break;
    default:
        PyErr_SetString(PyExc_ValueError, "unknown error code");
        break;
    }
    return res;
}


static PyObject *
decode_locale_ex(PyObject *self, PyObject *args)
{
    char *str;
    int current_locale = 0;
    PyObject *res = NULL;
    const char *errors = NULL;

    if (!PyArg_ParseTuple(args, "y|is", &str, &current_locale, &errors)) {
        return NULL;
    }
    _Py_error_handler error_handler = _Py_GetErrorHandler(errors);

    wchar_t *wstr = NULL;
    size_t wlen = 0;
    const char *reason = NULL;
    int ret = _Py_DecodeLocaleEx(str,
                                 &wstr, &wlen, &reason,
                                 current_locale, error_handler);

    switch(ret) {
    case 0:
        res = PyUnicode_FromWideChar(wstr, wlen);
        PyMem_RawFree(wstr);
        break;
    case -1:
        PyErr_NoMemory();
        break;
    case -2:
        PyErr_Format(PyExc_RuntimeError, "decode error: pos=%zu, reason=%s",
                     wlen, reason);
        break;
    case -3:
        PyErr_SetString(PyExc_ValueError, "unsupported error handler");
        break;
    default:
        PyErr_SetString(PyExc_ValueError, "unknown error code");
        break;
    }
    return res;
}

static PyObject *
set_eval_frame_default(PyObject *self, PyObject *Py_UNUSED(args))
{
    module_state *state = get_module_state(self);
    if (_PyInterpreterState_SetEvalFrameFuncChecked(
            _PyInterpreterState_GET(), _PyEval_EvalFrameDefault) < 0) return NULL;
    Py_CLEAR(state->record_list);
    Py_RETURN_NONE;
}

static PyObject *
record_eval(PyThreadState *tstate, struct _PyInterpreterFrame *f, int exc)
{
    if (PyStackRef_FunctionCheck(f->f_funcobj)) {
        PyFunctionObject *func = _PyFrame_GetFunction(f);
        PyObject *module = _get_current_module();
        assert(module != NULL);
        module_state *state = get_module_state(module);
        Py_DECREF(module);
        int res = PyList_Append(state->record_list, func->func_name);
        if (res < 0) {
            return NULL;
        }
    }
    return _PyEval_EvalFrameDefault(tstate, f, exc);
}


static PyObject *
set_eval_frame_record(PyObject *self, PyObject *list)
{
    module_state *state = get_module_state(self);
    if (!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a list");
        return NULL;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PySoacSource_BeginObserverReservation(tstate) < 0) return NULL;
    /* Preserve the original release-before-hook order. Finalizers may run
     * arbitrary Python or release the GIL, but cannot admit a source interval
     * on any native tstate until this exact transaction finishes. */
    Py_XSETREF(state->record_list, Py_NewRef(list));
    int status = _PyInterpreterState_SetEvalFrameFuncChecked(tstate->interp, record_eval);
    _PySoacSource_EndObserverReservation(tstate);
    if (status < 0) return NULL;
    Py_RETURN_NONE;
}

// Defined in interpreter.c
extern PyObject*
Test_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag);
extern int Test_EvalFrame_Resumes, Test_EvalFrame_Loads;

static PyObject *
get_eval_frame_stats(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *res = PyDict_New();
    if (res == NULL) {
        return NULL;
    }
    PyObject *resumes = PyLong_FromLong(Test_EvalFrame_Resumes);
    if (resumes == NULL || PyDict_SetItemString(res, "resumes", resumes) < 0) {
        Py_XDECREF(resumes);
        Py_DECREF(res);
        return NULL;
    }
    Py_DECREF(resumes);
    PyObject *loads = PyLong_FromLong(Test_EvalFrame_Loads);
    if (loads == NULL || PyDict_SetItemString(res, "loads", loads) < 0) {
        Py_XDECREF(loads);
        Py_DECREF(res);
        return NULL;
    }
    Py_DECREF(loads);
    Test_EvalFrame_Resumes = Test_EvalFrame_Loads = 0;
    return res;
}

static PyObject *
set_eval_frame_interp(PyObject *self, PyObject *Py_UNUSED(args))
{
    if (_PyInterpreterState_SetEvalFrameFuncChecked(
            _PyInterpreterState_GET(), Test_EvalFrame) < 0) return NULL;
    Py_RETURN_NONE;
}

/*[clinic input]

_testinternalcapi.compiler_cleandoc -> object

    doc: unicode

C implementation of inspect.cleandoc().
[clinic start generated code]*/

static PyObject *
_testinternalcapi_compiler_cleandoc_impl(PyObject *module, PyObject *doc)
/*[clinic end generated code: output=2dd203a80feff5bc input=2de03fab931d9cdc]*/
{
    return _PyCompile_CleanDoc(doc);
}

/*[clinic input]

_testinternalcapi.new_instruction_sequence -> object

Return a new, empty InstructionSequence.
[clinic start generated code]*/

static PyObject *
_testinternalcapi_new_instruction_sequence_impl(PyObject *module)
/*[clinic end generated code: output=ea4243fddb9057fd input=1dec2591b173be83]*/
{
    return _PyInstructionSequence_New();
}

/*[clinic input]

_testinternalcapi.compiler_codegen -> object

  ast: object
  filename: object
  optimize: int
  compile_mode: int = 0

Apply compiler code generation to an AST.
[clinic start generated code]*/

static PyObject *
_testinternalcapi_compiler_codegen_impl(PyObject *module, PyObject *ast,
                                        PyObject *filename, int optimize,
                                        int compile_mode)
/*[clinic end generated code: output=40a68f6e13951cc8 input=a0e00784f1517cd7]*/
{
    PyCompilerFlags *flags = NULL;
    return _PyCompile_CodeGen(ast, filename, flags, optimize, compile_mode);
}


/*[clinic input]

_testinternalcapi.optimize_cfg -> object

  instructions: object
  consts: object
  nlocals: int

Apply compiler optimizations to an instruction list.
[clinic start generated code]*/

static PyObject *
_testinternalcapi_optimize_cfg_impl(PyObject *module, PyObject *instructions,
                                    PyObject *consts, int nlocals)
/*[clinic end generated code: output=57c53c3a3dfd1df0 input=6a96d1926d58d7e5]*/
{
    return _PyCompile_OptimizeCfg(instructions, consts, nlocals);
}

static int
get_nonnegative_int_from_dict(PyObject *dict, const char *key) {
    PyObject *obj = PyDict_GetItemString(dict, key);
    if (obj == NULL) {
        return -1;
    }
    return PyLong_AsLong(obj);
}

/*[clinic input]

_testinternalcapi.assemble_code_object -> object

  filename: object
  instructions: object
  metadata: object

Create a code object for the given instructions.
[clinic start generated code]*/

static PyObject *
_testinternalcapi_assemble_code_object_impl(PyObject *module,
                                            PyObject *filename,
                                            PyObject *instructions,
                                            PyObject *metadata)
/*[clinic end generated code: output=38003dc16a930f48 input=e713ad77f08fb3a8]*/

{
    assert(PyDict_Check(metadata));
    _PyCompile_CodeUnitMetadata umd;

    umd.u_name = PyDict_GetItemString(metadata, "name");
    umd.u_qualname = PyDict_GetItemString(metadata, "qualname");

    assert(PyUnicode_Check(umd.u_name));
    assert(PyUnicode_Check(umd.u_qualname));

    umd.u_consts = PyDict_GetItemString(metadata, "consts");
    umd.u_names = PyDict_GetItemString(metadata, "names");
    umd.u_varnames = PyDict_GetItemString(metadata, "varnames");
    umd.u_cellvars = PyDict_GetItemString(metadata, "cellvars");
    umd.u_freevars = PyDict_GetItemString(metadata, "freevars");
    umd.u_fasthidden = PyDict_GetItemString(metadata, "fasthidden");

    assert(PyDict_Check(umd.u_consts));
    assert(PyDict_Check(umd.u_names));
    assert(PyDict_Check(umd.u_varnames));
    assert(PyDict_Check(umd.u_cellvars));
    assert(PyDict_Check(umd.u_freevars));
    assert(PyDict_Check(umd.u_fasthidden));

    umd.u_argcount = get_nonnegative_int_from_dict(metadata, "argcount");
    umd.u_posonlyargcount = get_nonnegative_int_from_dict(metadata, "posonlyargcount");
    umd.u_kwonlyargcount = get_nonnegative_int_from_dict(metadata, "kwonlyargcount");
    umd.u_firstlineno = get_nonnegative_int_from_dict(metadata, "firstlineno");

    assert(umd.u_argcount >= 0);
    assert(umd.u_posonlyargcount >= 0);
    assert(umd.u_kwonlyargcount >= 0);
    assert(umd.u_firstlineno >= 0);

    return (PyObject*)_PyCompile_Assemble(&umd, filename, instructions);
}


// Maybe this could be replaced by get_interpreter_config()?
static PyObject *
get_interp_settings(PyObject *self, PyObject *args)
{
    int interpid = -1;
    if (!PyArg_ParseTuple(args, "|i:get_interp_settings", &interpid)) {
        return NULL;
    }

    PyInterpreterState *interp = NULL;
    if (interpid < 0) {
        PyThreadState *tstate = _PyThreadState_GET();
        interp = tstate ? tstate->interp : _PyInterpreterState_Main();
    }
    else if (interpid == 0) {
        interp = _PyInterpreterState_Main();
    }
    else {
        PyErr_Format(PyExc_NotImplementedError,
                     "%zd", interpid);
        return NULL;
    }
    assert(interp != NULL);

    PyObject *settings = PyDict_New();
    if (settings == NULL) {
        return NULL;
    }

    /* Add the feature flags. */
    PyObject *flags = PyLong_FromUnsignedLong(interp->feature_flags);
    if (flags == NULL) {
        Py_DECREF(settings);
        return NULL;
    }
    int res = PyDict_SetItemString(settings, "feature_flags", flags);
    Py_DECREF(flags);
    if (res != 0) {
        Py_DECREF(settings);
        return NULL;
    }

    /* "own GIL" */
    PyObject *own_gil = interp->ceval.own_gil ? Py_True : Py_False;
    if (PyDict_SetItemString(settings, "own_gil", own_gil) != 0) {
        Py_DECREF(settings);
        return NULL;
    }

    return settings;
}


static PyObject *
clear_extension(PyObject *self, PyObject *args)
{
    PyObject *name = NULL, *filename = NULL;
    if (!PyArg_ParseTuple(args, "OO:clear_extension", &name, &filename)) {
        return NULL;
    }
    if (_PyImport_ClearExtension(name, filename) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
write_perf_map_entry(PyObject *self, PyObject *args)
{
    PyObject *code_addr_v;
    const void *code_addr;
    unsigned int code_size;
    const char *entry_name;

    if (!PyArg_ParseTuple(args, "OIs", &code_addr_v, &code_size, &entry_name))
        return NULL;
    code_addr = PyLong_AsVoidPtr(code_addr_v);
    if (code_addr == NULL) {
        return NULL;
    }

    int ret = PyUnstable_WritePerfMapEntry(code_addr, code_size, entry_name);
    if (ret < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    return PyLong_FromLong(ret);
}

static PyObject *
perf_map_state_teardown(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(ignored))
{
    PyUnstable_PerfMapState_Fini();
    Py_RETURN_NONE;
}

static PyObject *
iframe_getcode(PyObject *self, PyObject *frame)
{
    if (!PyFrame_Check(frame)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a frame");
        return NULL;
    }
    struct _PyInterpreterFrame *f = ((PyFrameObject *)frame)->f_frame;
    return PyUnstable_InterpreterFrame_GetCode(f);
}

static PyObject *
iframe_getline(PyObject *self, PyObject *frame)
{
    if (!PyFrame_Check(frame)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a frame");
        return NULL;
    }
    struct _PyInterpreterFrame *f = ((PyFrameObject *)frame)->f_frame;
    return PyLong_FromLong(PyUnstable_InterpreterFrame_GetLine(f));
}

static PyObject *
iframe_getlasti(PyObject *self, PyObject *frame)
{
    if (!PyFrame_Check(frame)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a frame");
        return NULL;
    }
    struct _PyInterpreterFrame *f = ((PyFrameObject *)frame)->f_frame;
    return PyLong_FromLong(PyUnstable_InterpreterFrame_GetLasti(f));
}

static PyObject *
code_returns_only_none(PyObject *self, PyObject *arg)
{
    if (!PyCode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a code object");
        return NULL;
    }
    PyCodeObject *code = (PyCodeObject *)arg;
    int res = _PyCode_ReturnsOnlyNone(code);
    return PyBool_FromLong(res);
}

static PyObject *
get_co_framesize(PyObject *self, PyObject *arg)
{
    if (!PyCode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a code object");
        return NULL;
    }
    PyCodeObject *code = (PyCodeObject *)arg;
    return PyLong_FromLong(code->co_framesize);
}

static PyObject *
get_co_localskinds(PyObject *self, PyObject *arg)
{
    if (!PyCode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a code object");
        return NULL;
    }
    PyCodeObject *co = (PyCodeObject *)arg;

    PyObject *kinds = PyDict_New();
    if (kinds == NULL) {
        return NULL;
    }
    for (int offset = 0; offset < co->co_nlocalsplus; offset++) {
        PyObject *name = PyTuple_GET_ITEM(co->co_localsplusnames, offset);
        _PyLocals_Kind k = _PyLocals_GetKind(co->co_localspluskinds, offset);
        PyObject *kind = PyLong_FromLong(k);
        if (kind == NULL) {
            Py_DECREF(kinds);
            return NULL;
        }
        int res = PyDict_SetItem(kinds, name, kind);
        Py_DECREF(kind);
        if (res < 0) {
            Py_DECREF(kinds);
            return NULL;
        }
    }
    return kinds;
}

static PyObject *
get_code_var_counts(PyObject *self, PyObject *_args, PyObject *_kwargs)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *codearg;
    PyObject *globalnames = NULL;
    PyObject *attrnames = NULL;
    PyObject *globalsns = NULL;
    PyObject *builtinsns = NULL;
    static char *kwlist[] = {"code", "globalnames", "attrnames", "globalsns",
                             "builtinsns", NULL};
    if (!PyArg_ParseTupleAndKeywords(_args, _kwargs,
                    "O|OOO!O!:get_code_var_counts", kwlist,
                    &codearg, &globalnames, &attrnames,
                    &PyDict_Type, &globalsns, &PyDict_Type, &builtinsns))
    {
        return NULL;
    }
    if (PyFunction_Check(codearg)) {
        if (globalsns == NULL) {
            globalsns = PyFunction_GET_GLOBALS(codearg);
        }
        if (builtinsns == NULL) {
            builtinsns = _PyFunction_GET_BUILTINS(codearg);
        }
        codearg = PyFunction_GET_CODE(codearg);
    }
    else if (!PyCode_Check(codearg)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument must be a code object or a function");
        return NULL;
    }
    PyCodeObject *code = (PyCodeObject *)codearg;

    _PyCode_var_counts_t counts = {0};
    _PyCode_GetVarCounts(code, &counts);
    if (_PyCode_SetUnboundVarCounts(
            tstate, code, &counts, globalnames, attrnames,
            globalsns, builtinsns) < 0)
    {
        return NULL;
    }

#define SET_COUNT(DICT, STRUCT, NAME) \
    do { \
        PyObject *count = PyLong_FromLong(STRUCT.NAME); \
        if (count == NULL) { \
            goto error; \
        } \
        int res = PyDict_SetItemString(DICT, #NAME, count); \
        Py_DECREF(count); \
        if (res < 0) { \
            goto error; \
        } \
    } while (0)

    PyObject *locals = NULL;
    PyObject *args = NULL;
    PyObject *cells = NULL;
    PyObject *hidden = NULL;
    PyObject *unbound = NULL;
    PyObject *globals = NULL;
    PyObject *countsobj = PyDict_New();
    if (countsobj == NULL) {
        return NULL;
    }
    SET_COUNT(countsobj, counts, total);

    // locals
    locals = PyDict_New();
    if (locals == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(countsobj, "locals", locals) < 0) {
        goto error;
    }
    SET_COUNT(locals, counts.locals, total);

    // locals.args
    args = PyDict_New();
    if (args == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(locals, "args", args) < 0) {
        goto error;
    }
    SET_COUNT(args, counts.locals.args, total);
    SET_COUNT(args, counts.locals.args, numposonly);
    SET_COUNT(args, counts.locals.args, numposorkw);
    SET_COUNT(args, counts.locals.args, numkwonly);
    SET_COUNT(args, counts.locals.args, varargs);
    SET_COUNT(args, counts.locals.args, varkwargs);

    // locals.numpure
    SET_COUNT(locals, counts.locals, numpure);

    // locals.cells
    cells = PyDict_New();
    if (cells == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(locals, "cells", cells) < 0) {
        goto error;
    }
    SET_COUNT(cells, counts.locals.cells, total);
    SET_COUNT(cells, counts.locals.cells, numargs);
    SET_COUNT(cells, counts.locals.cells, numothers);

    // locals.hidden
    hidden = PyDict_New();
    if (hidden == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(locals, "hidden", hidden) < 0) {
        goto error;
    }
    SET_COUNT(hidden, counts.locals.hidden, total);
    SET_COUNT(hidden, counts.locals.hidden, numpure);
    SET_COUNT(hidden, counts.locals.hidden, numcells);

    // numfree
    SET_COUNT(countsobj, counts, numfree);

    // unbound
    unbound = PyDict_New();
    if (unbound == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(countsobj, "unbound", unbound) < 0) {
        goto error;
    }
    SET_COUNT(unbound, counts.unbound, total);
    SET_COUNT(unbound, counts.unbound, numattrs);
    SET_COUNT(unbound, counts.unbound, numunknown);

    // unbound.globals
    globals = PyDict_New();
    if (globals == NULL) {
        goto error;
    }
    if (PyDict_SetItemString(unbound, "globals", globals) < 0) {
        goto error;
    }
    SET_COUNT(globals, counts.unbound.globals, total);
    SET_COUNT(globals, counts.unbound.globals, numglobal);
    SET_COUNT(globals, counts.unbound.globals, numbuiltin);
    SET_COUNT(globals, counts.unbound.globals, numunknown);

#undef SET_COUNT

    Py_DECREF(locals);
    Py_DECREF(args);
    Py_DECREF(cells);
    Py_DECREF(hidden);
    Py_DECREF(unbound);
    Py_DECREF(globals);
    return countsobj;

error:
    Py_DECREF(countsobj);
    Py_XDECREF(locals);
    Py_XDECREF(args);
    Py_XDECREF(cells);
    Py_XDECREF(hidden);
    Py_XDECREF(unbound);
    Py_XDECREF(globals);
    return NULL;
}

static PyObject *
verify_stateless_code(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *codearg;
    PyObject *globalnames = NULL;
    PyObject *globalsns = NULL;
    PyObject *builtinsns = NULL;
    static char *kwlist[] = {"code", "globalnames",
                             "globalsns", "builtinsns", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                    "O|O!O!O!:get_code_var_counts", kwlist,
                    &codearg, &PySet_Type, &globalnames,
                    &PyDict_Type, &globalsns, &PyDict_Type, &builtinsns))
    {
        return NULL;
    }
    if (PyFunction_Check(codearg)) {
        if (globalsns == NULL) {
            globalsns = PyFunction_GET_GLOBALS(codearg);
        }
        if (builtinsns == NULL) {
            builtinsns = _PyFunction_GET_BUILTINS(codearg);
        }
        codearg = PyFunction_GET_CODE(codearg);
    }
    else if (!PyCode_Check(codearg)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument must be a code object or a function");
        return NULL;
    }
    PyCodeObject *code = (PyCodeObject *)codearg;

    if (_PyCode_VerifyStateless(
                tstate, code, globalnames, globalsns, builtinsns) < 0)
    {
        return NULL;
    }
    Py_RETURN_NONE;
}

#ifdef _Py_TIER2

static PyObject *
add_executor_dependency(PyObject *self, PyObject *args)
{
    PyObject *exec;
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "OO", &exec, &obj)) {
        return NULL;
    }
    _Py_Executor_DependsOn((_PyExecutorObject *)exec, obj);
    Py_RETURN_NONE;
}

static PyObject *
invalidate_executors(PyObject *self, PyObject *obj)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    _Py_Executors_InvalidateDependency(interp, obj, 1);
    Py_RETURN_NONE;
}

static PyObject *
clear_executor_deletion_list(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    _Py_ClearExecutorDeletionList(interp);
    Py_RETURN_NONE;
}

static PyObject *
get_exit_executor(PyObject *self, PyObject *arg)
{
    if (!PyLong_CheckExact(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument must be an ID to an _PyExitData");
        return NULL;
    }
    uint64_t ptr;
    if (PyLong_AsUInt64(arg, &ptr) < 0) {
        // Error set by PyLong API
        return NULL;
    }
    _PyExitData *exit = (_PyExitData *)ptr;
    return Py_NewRef(exit->executor);
}

#endif

static int _pending_callback(void *arg)
{
    /* we assume the argument is callable object to which we own a reference */
    PyObject *callable = (PyObject *)arg;
    PyObject *r = PyObject_CallNoArgs(callable);
    Py_DECREF(callable);
    Py_XDECREF(r);
    return r != NULL ? 0 : -1;
}

/* The following requests n callbacks to _pending_callback.  It can be
 * run from any python thread.
 */
static PyObject *
pending_threadfunc(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *callable;
    unsigned int num = 1;
    int blocking = 0;
    int ensure_added = 0;
    static char *kwlist[] = {"callback", "num",
                             "blocking", "ensure_added", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O|I$pp:pending_threadfunc", kwlist,
                                     &callable, &num, &blocking, &ensure_added))
    {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();

    /* create the reference for the callbackwhile we hold the lock */
    for (unsigned int i = 0; i < num; i++) {
        Py_INCREF(callable);
    }

    PyThreadState *save_tstate = NULL;
    if (!blocking) {
        save_tstate = PyEval_SaveThread();
    }

    unsigned int num_added = 0;
    for (; num_added < num; num_added++) {
        if (ensure_added) {
            _Py_add_pending_call_result r;
            do {
                r = _PyEval_AddPendingCall(interp, &_pending_callback, callable, 0);
                assert(r == _Py_ADD_PENDING_SUCCESS
                       || r == _Py_ADD_PENDING_FULL);
            } while (r == _Py_ADD_PENDING_FULL);
        }
        else {
            if (_PyEval_AddPendingCall(interp, &_pending_callback, callable, 0) < 0) {
                break;
            }
        }
    }

    if (!blocking) {
        PyEval_RestoreThread(save_tstate);
    }

    for (unsigned int i = num_added; i < num; i++) {
        Py_DECREF(callable); /* unsuccessful add, destroy the extra reference */
    }

    /* The callable is decref'ed in _pending_callback() above. */
    return PyLong_FromUnsignedLong((unsigned long)num_added);
}


static struct {
    int64_t interpid;
} pending_identify_result;

static int
_pending_identify_callback(void *arg)
{
    PyThread_type_lock mutex = (PyThread_type_lock)arg;
    assert(pending_identify_result.interpid == -1);
    PyThreadState *tstate = PyThreadState_Get();
    pending_identify_result.interpid = PyInterpreterState_GetID(tstate->interp);
    PyThread_release_lock(mutex);
    return 0;
}

static PyObject *
pending_identify(PyObject *self, PyObject *args)
{
    PyObject *interpid;
    if (!PyArg_ParseTuple(args, "O:pending_identify", &interpid)) {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(interpid);
    if (interp == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "interpreter not found");
        }
        return NULL;
    }

    pending_identify_result.interpid = -1;

    PyThread_type_lock mutex = PyThread_allocate_lock();
    if (mutex == NULL) {
        return NULL;
    }
    PyThread_acquire_lock(mutex, WAIT_LOCK);
    /* It gets released in _pending_identify_callback(). */

    _Py_add_pending_call_result r;
    do {
        Py_BEGIN_ALLOW_THREADS
        r = _PyEval_AddPendingCall(interp,
                                   &_pending_identify_callback, (void *)mutex,
                                   0);
        Py_END_ALLOW_THREADS
        assert(r == _Py_ADD_PENDING_SUCCESS
               || r == _Py_ADD_PENDING_FULL);
    } while (r == _Py_ADD_PENDING_FULL);

    /* Wait for the pending call to complete. */
    PyThread_acquire_lock(mutex, WAIT_LOCK);
    PyThread_release_lock(mutex);
    PyThread_free_lock(mutex);

    PyObject *res = PyLong_FromLongLong(pending_identify_result.interpid);
    pending_identify_result.interpid = -1;
    if (res == NULL) {
        return NULL;
    }
    return res;
}

static PyObject *
tracemalloc_get_traceback(PyObject *self, PyObject *args)
{
    unsigned int domain;
    PyObject *ptr_obj;

    if (!PyArg_ParseTuple(args, "IO", &domain, &ptr_obj)) {
        return NULL;
    }
    void *ptr = PyLong_AsVoidPtr(ptr_obj);
    if (PyErr_Occurred()) {
        return NULL;
    }

    return _PyTraceMalloc_GetTraceback(domain, (uintptr_t)ptr);
}


// Test PyThreadState C API
static PyObject *
test_tstate_capi(PyObject *self, PyObject *Py_UNUSED(args))
{
    // PyThreadState_Get()
    PyThreadState *tstate = PyThreadState_Get();
    assert(tstate != NULL);

    // test _PyThreadState_GetDict()
    PyObject *dict = PyThreadState_GetDict();
    assert(dict != NULL);
    // dict is a borrowed reference

    PyObject *dict2 = _PyThreadState_GetDict(tstate);
    assert(dict2 == dict);
    // dict2 is a borrowed reference

    Py_RETURN_NONE;
}


/* Test _PyUnicode_TransformDecimalAndSpaceToASCII() */
static PyObject *
unicode_transformdecimalandspacetoascii(PyObject *self, PyObject *arg)
{
    if (arg == Py_None) {
        arg = NULL;
    }
    return _PyUnicode_TransformDecimalAndSpaceToASCII(arg);
}

static PyObject *
test_pyobject_is_freed(const char *test_name, PyObject *op)
{
    if (!_PyObject_IsFreed(op)) {
        PyErr_SetString(PyExc_AssertionError,
                        "object is not seen as freed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
check_pyobject_null_is_freed(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *op = NULL;
    return test_pyobject_is_freed("check_pyobject_null_is_freed", op);
}


static PyObject *
check_pyobject_uninitialized_is_freed(PyObject *self,
                                      PyObject *Py_UNUSED(args))
{
    PyObject *op = (PyObject *)PyObject_Malloc(sizeof(PyObject));
    if (op == NULL) {
        return NULL;
    }
    /* Initialize reference count to avoid early crash in ceval or GC */
    Py_SET_REFCNT(op, 1);
    /* object fields like ob_type are uninitialized! */
    return test_pyobject_is_freed("check_pyobject_uninitialized_is_freed", op);
}


static PyObject *
check_pyobject_forbidden_bytes_is_freed(PyObject *self,
                                        PyObject *Py_UNUSED(args))
{
    /* Allocate an incomplete PyObject structure: truncate 'ob_type' field */
    PyObject *op = (PyObject *)PyObject_Malloc(offsetof(PyObject, ob_type));
    if (op == NULL) {
        return NULL;
    }
    /* Initialize reference count to avoid early crash in ceval or GC */
    Py_SET_REFCNT(op, 1);
    /* ob_type field is after the memory block: part of "forbidden bytes"
       when using debug hooks on memory allocators! */
    return test_pyobject_is_freed("check_pyobject_forbidden_bytes_is_freed", op);
}


static PyObject *
check_pyobject_freed_is_freed(PyObject *self, PyObject *Py_UNUSED(args))
{
    /* ASan or TSan would report an use-after-free error */
#if defined(_Py_ADDRESS_SANITIZER) || defined(_Py_THREAD_SANITIZER)
    Py_RETURN_NONE;
#else
    PyObject *op = PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
    if (op == NULL) {
        return NULL;
    }
    Py_TYPE(op)->tp_dealloc(op);
    /* Reset reference count to avoid early crash in ceval or GC */
    Py_SET_REFCNT(op, 1);
    /* object memory is freed! */
    return test_pyobject_is_freed("check_pyobject_freed_is_freed", op);
#endif
}


static PyObject *
test_pymem_getallocatorsname(PyObject *self, PyObject *args)
{
    const char *name = _PyMem_GetCurrentAllocatorName();
    if (name == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "cannot get allocators name");
        return NULL;
    }
    return PyUnicode_FromString(name);
}

static PyObject *
get_object_dict_values(PyObject *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);
    if (!_PyType_HasFeature(type, Py_TPFLAGS_INLINE_VALUES)) {
        Py_RETURN_NONE;
    }
    PyDictValues *values = _PyObject_InlineValues(obj);
    if (!values->valid) {
        Py_RETURN_NONE;
    }
    PyDictKeysObject *keys = ((PyHeapTypeObject *)type)->ht_cached_keys;
    assert(keys != NULL);
    int size = (int)keys->dk_nentries;
    assert(size >= 0);
    PyObject *res = PyTuple_New(size);
    if (res == NULL) {
        return NULL;
    }
    _Py_DECLARE_STR(anon_null, "<NULL>");
    for(int i = 0; i < size; i++) {
        PyObject *item = values->values[i];
        if (item == NULL) {
            item = &_Py_STR(anon_null);
        }
        else {
            Py_INCREF(item);
        }
        PyTuple_SET_ITEM(res, i, item);
    }
    return res;
}


static PyObject*
new_hamt(PyObject *self, PyObject *args)
{
    return _PyContext_NewHamtForTests();
}


static PyObject*
dict_getitem_knownhash(PyObject *self, PyObject *args)
{
    PyObject *mp, *key, *result;
    Py_ssize_t hash;

    if (!PyArg_ParseTuple(args, "OOn:dict_getitem_knownhash",
                          &mp, &key, &hash)) {
        return NULL;
    }

    result = _PyDict_GetItem_KnownHash(mp, key, (Py_hash_t)hash);
    if (result == NULL && !PyErr_Occurred()) {
        _PyErr_SetKeyError(key);
        return NULL;
    }

    return Py_XNewRef(result);
}

static PyObject *
dict_new_indexed(PyObject *self, PyObject *keys_obj)
{
    PyDictKeysObject *keys = _PyDict_NewIndexedKeySet(keys_obj);
    if (keys == NULL) {
        return NULL;
    }
    PyObject *dict = _PyDict_NewWithIndexedKeySet(keys);
    _PyDictKeys_DecRef(keys);
    return dict;
}

static PyObject *
dict_has_indexed_keys(PyObject *self, PyObject *dict)
{
    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "expected a dictionary");
        return NULL;
    }
    return PyBool_FromLong(
        _PyDict_HasIndexedTable((PyDictObject *)dict));
}

static PyObject *
dict_indexed_key_index(PyObject *self, PyObject *args)
{
    PyObject *dict;
    PyObject *key;
    if (!PyArg_ParseTuple(
            args, "OO:dict_indexed_key_index", &dict, &key))
    {
        return NULL;
    }
    Py_ssize_t index = _PyDict_IndexedKeyIndex(dict, key);
    if (index < 0 && PyErr_Occurred()) {
        return NULL;
    }
    return PyLong_FromSsize_t(index);
}

static PyObject *
dict_has_no_lookup_aliases(PyObject *self, PyObject *dict)
{
    return PyBool_FromLong(_PyDict_HasNoLookupAliases(dict));
}

static PyObject *
dict_reserve_soac_namespace_keys(PyObject *self, PyObject *args)
{
    PyObject *dict, *owner, *names;
    if (!PyArg_ParseTuple(args, "OOO", &dict, &owner, &names)) {
        return NULL;
    }
    return _PyDict_ReserveSoacNamespaceKeys(dict, owner, names) < 0
        ? NULL : Py_NewRef(Py_None);
}

static PyObject *
dict_new_from_indexed_schema(PyObject *self, PyObject *template)
{
    return _PyDict_NewFromIndexedSchema(template);
}

static PyObject *
dict_setitem_knownhash(PyObject *self, PyObject *args)
{
    PyObject *dict, *key, *value;
    Py_ssize_t hash;
    if (!PyArg_ParseTuple(args, "OOOn", &dict, &key, &value, &hash)) {
        return NULL;
    }
    return _PyDict_SetItem_KnownHash(dict, key, value, (Py_hash_t)hash) < 0
        ? NULL : Py_NewRef(Py_None);
}

static PyObject *
dict_delitem_knownhash(PyObject *self, PyObject *args)
{
    PyObject *dict, *key;
    Py_ssize_t hash;
    if (!PyArg_ParseTuple(args, "OOn", &dict, &key, &hash)) {
        return NULL;
    }
    return _PyDict_DelItem_KnownHash(dict, key, (Py_hash_t)hash) < 0
        ? NULL : Py_NewRef(Py_None);
}

static PyObject *
dict_setitem_and_delete_for_module(PyObject *self, PyObject *args)
{
    PyObject *dict, *key, *value, *companion;
    if (!PyArg_ParseTuple(args, "OOOO", &dict, &key, &value, &companion)) {
        return NULL;
    }
    int result = _PyDict_SetItemAndDeleteForModule(
        dict, key, value == Py_None ? NULL : value, companion);
    return result < 0 ? NULL : PyLong_FromLong(result);
}

typedef struct {
    PyDictKeysObject *keys;
    int failure_mode;
} SoacTestIndexedLayout;

static void
soac_test_layout_destroy(PyObject *owner)
{
    SoacTestIndexedLayout *layout = PyCapsule_GetPointer(owner, "test.soac.indexed_layout");
    assert(layout != NULL);
    _PyDictKeys_DecRef(layout->keys);
    PyMem_Free(layout);
}

static int
soac_test_instance_policy(PyObject *owner, PyObject *dict, PyObject *key,
                          PyObject *value, int operation, PyObject *provenance)
{
    if (operation == PyDict_SOAC_TERMINAL_TEARDOWN ||
        operation == PyDict_SOAC_DELETE || operation == PyDict_SOAC_CLEAR) {
        return 0;
    }
    int attribute = operation == PyDict_SOAC_ATTRIBUTE_SET ||
                    operation == PyDict_SOAC_ATTRIBUTE_SET_EXISTING;
    if (!attribute && provenance != NULL) {
        PyErr_SetString(PyExc_TypeError, "test instance has no runtime cache provider");
        return -1;
    }
    int checked = PyUnicode_CheckExact(key) && _PyDict_IndexedKeyIndex(dict, key) >= 0;
    if (attribute) {
        assert(provenance != NULL && PyUnicode_Check(provenance));
        /* Copy the Unicode payload, never call str(), hash(), or equality on
           the original subclass a second time during policy validation. */
        PyObject *name = PyUnicode_FromObject(provenance);
        if (name == NULL) {
            return -1;
        }
        checked |= _PyDict_IndexedKeyIndex(dict, name) >= 0;
        Py_DECREF(name);
    }
    if (checked && !PyLong_CheckExact(value)) {
        PyErr_SetString(PyExc_TypeError, "test declared field requires an exact int");
        return -1;
    }
    return 0;
}

static PyObject *
soac_test_instance_factory(PyObject *owner, PyObject *instance)
{
    assert(!(Py_TYPE(instance)->tp_flags & Py_TPFLAGS_INLINE_VALUES));
    SoacTestIndexedLayout *layout = PyCapsule_GetPointer(owner, "test.soac.indexed_layout");
    if (layout == NULL) {
        return NULL;
    }
    if (layout->failure_mode == 1) {
        PyErr_SetString(PyExc_MemoryError, "test instance factory allocation failed");
        return NULL;
    }
    PyObject *dict = _PyDict_NewWithIndexedKeySet(layout->keys);
    if (dict != NULL && PyDict_SetSoacPolicy(dict, owner, soac_test_instance_policy,
                                            PyDict_SOAC_ALLOW_NONSTRING_KEYS) < 0) {
        Py_CLEAR(dict);
    }
    if (dict != NULL && layout->failure_mode == 2) {
        /* Deliberately malformed factory result: allocation cleanup must drop
           this partial edge before freeing the unpublished instance, without
           invoking its __del__ or losing the original boundary error. */
        if (PyDict_SetItemString(dict, "partial_instance", instance) < 0) {
            Py_CLEAR(dict);
        }
    }
    return dict;
}

static PyObject *
dict_new_soac_type(PyObject *self, PyObject *args)
{
    PyObject *name, *bases, *namespace, *fields, *namespace_function;
    PyObject *protected_names = NULL, *final_methods = NULL;
    int failure_mode = 0;
    if (!PyArg_ParseTuple(args, "OOOOO|iOO", &name, &bases, &namespace,
                          &fields, &namespace_function, &failure_mode,
                          &protected_names, &final_methods)) {
        return NULL;
    }
    if (failure_mode < 0 || failure_mode > 2) {
        PyErr_SetString(PyExc_ValueError, "invalid test instance factory failure mode");
        return NULL;
    }
    PyDictKeysObject *keys = _PyDict_NewIndexedKeySet(fields);
    if (keys == NULL) {
        return NULL;
    }
    SoacTestIndexedLayout *layout = PyMem_Malloc(sizeof(*layout));
    if (layout == NULL) {
        _PyDictKeys_DecRef(keys);
        return PyErr_NoMemory();
    }
    layout->keys = keys;
    layout->failure_mode = failure_mode;
    PyObject *owner = PyCapsule_New(layout, "test.soac.indexed_layout", soac_test_layout_destroy);
    if (owner == NULL) {
        _PyDictKeys_DecRef(keys);
        PyMem_Free(layout);
        return NULL;
    }
    PyObject *empty = PyTuple_New(0), *keywords = PyDict_New();
    if (empty == NULL || keywords == NULL) {
        Py_XDECREF(empty);
        Py_XDECREF(keywords);
        Py_DECREF(owner);
        return NULL;
    }
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(PySoacTypeConstructionSpec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_ENFORCED,
        .owner = owner,
        .namespace_function = namespace_function,
        .name = name,
        .bases = bases,
        .namespace_dict = namespace,
        .keywords = keywords,
        .bind_type = NULL,
        .contract = {
            .flags = 0,
            .fields = fields,
            .protected_names = protected_names == NULL ? empty : protected_names,
            .final_methods = final_methods == NULL ? empty : final_methods,
            .new_instance_dict = soac_test_instance_factory,
            .dictionary_mode = Py_SOAC_INSTANCE_DICT_INDEXED,
        },
    };
    PyObject *handle = PyType_NewSoacConstructionHandle(&spec);
    PyObject *type = handle == NULL ? NULL
        : PyType_FromSoacConstructionHandle(handle, namespace_function);
    if (type != NULL && PyType_SealSoacContract(type, owner) < 0) {
        Py_CLEAR(type);
    }
    Py_XDECREF(handle);
    Py_DECREF(empty);
    Py_DECREF(keywords);
    Py_DECREF(owner);
    return type;
}


/* Ordinary-storage field fixture. This is native policy authority only; it
 * never constructs a source/function/checker execution record. The actual
 * pending constructor keeps its ordinary Ready layout until final admission. */
static int
soac_test_ordinary_field(PyObject *owner, PyObject *name, PyObject *value)
{
    PyObject *fields = PyTuple_GET_ITEM(owner, 0);
    if (value == NULL || !PyUnicode_Check(name)) {
        return 0;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(fields); ++i) {
        int equal = PyUnicode_Compare(name, PyTuple_GET_ITEM(fields, i));
        if (equal == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (equal == 0 && !PyLong_CheckExact(value)) {
            PyErr_SetString(PyExc_TypeError, "test declared field requires an exact int");
            return -1;
        }
    }
    return 0;
}

/* These observer edges belong only to this existing test fixture's GC-visible
 * owner. No normal contract field, process-global registry or TLS is added. */
static PyObject *
soac_test_ordinary_observers(PyObject *owner)
{
    if (!PyTuple_CheckExact(owner) || PyTuple_GET_SIZE(owner) != 3 ||
        !PyList_CheckExact(PyTuple_GET_ITEM(owner, 2)) ||
        PyList_GET_SIZE(PyTuple_GET_ITEM(owner, 2)) != 2) {
        PyErr_SetString(PyExc_TypeError, "expected the ordinary dictionary test owner");
        return NULL;
    }
    return PyTuple_GET_ITEM(owner, 2);
}

static PyObject *
soac_test_ordinary_owner(PyObject *type)
{
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "ordinary test hook requires a type");
        return NULL;
    }
    PyObject *owner = PyType_GetSoacContractOwner(type);
    if (owner == NULL) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "type has no ordinary test owner");
        return NULL;
    }
    return soac_test_ordinary_observers(owner) == NULL ? NULL : owner;
}

typedef struct {
    PyMemAllocatorEx previous_allocator;
    PyObject *instance;              /* actual C argument support only */
    PyObject *candidate;
    PyDictObject *previous_dictionary;
    PyDictValues *previous_values;
    size_t expected_size;
    int initial_complete;
    int armed;
    int failed;
    int unexpected;
} SoacTestOrdinaryDetachFault;

static void
soac_test_ordinary_restore_allocator(SoacTestOrdinaryDetachFault *fault)
{
    if (fault->armed) {
        fault->armed = 0;
        PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &fault->previous_allocator);
    }
}

static void *
soac_test_ordinary_fault_malloc(void *ctx, size_t size)
{
    SoacTestOrdinaryDetachFault *fault = ctx;
    int matches = size == fault->expected_size && fault->initial_complete &&
        _PyObject_GetManagedDict(fault->instance) == fault->previous_dictionary &&
        _PyObject_InlineValues(fault->instance) == fault->previous_values &&
        fault->previous_dictionary->ma_values == fault->previous_values &&
        fault->previous_values->valid == 1;
    /* Restore BEFORE returning NULL; native detach then creates its original
     * MemoryError without the failing allocator still being installed. */
    soac_test_ordinary_restore_allocator(fault);
    if (matches) {
        fault->failed = 1;
        return NULL;
    }
    fault->unexpected = 1;
    return fault->previous_allocator.malloc(fault->previous_allocator.ctx, size);
}

static void *
soac_test_ordinary_fault_calloc(void *ctx, size_t count, size_t size)
{
    SoacTestOrdinaryDetachFault *fault = ctx;
    fault->unexpected = 1;
    soac_test_ordinary_restore_allocator(fault);
    return fault->previous_allocator.calloc(fault->previous_allocator.ctx, count, size);
}

static void *
soac_test_ordinary_fault_realloc(void *ctx, void *ptr, size_t size)
{
    SoacTestOrdinaryDetachFault *fault = ctx;
    fault->unexpected = 1;
    soac_test_ordinary_restore_allocator(fault);
    return fault->previous_allocator.realloc(fault->previous_allocator.ctx, ptr, size);
}

static void
soac_test_ordinary_fault_free(void *ctx, void *ptr)
{
    SoacTestOrdinaryDetachFault *fault = ctx;
    fault->previous_allocator.free(fault->previous_allocator.ctx, ptr);
}

static void
soac_test_ordinary_arm_fault(SoacTestOrdinaryDetachFault *fault)
{
    PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &fault->previous_allocator);
    PyMemAllocatorEx allocator = {
        .ctx = fault,
        .malloc = soac_test_ordinary_fault_malloc,
        .calloc = soac_test_ordinary_fault_calloc,
        .realloc = soac_test_ordinary_fault_realloc,
        .free = soac_test_ordinary_fault_free,
    };
    fault->armed = 1;
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &allocator);
}

static int
soac_test_ordinary_after_initial(PyObject *owner, PyObject *dictionary)
{
    PyObject *observers = soac_test_ordinary_observers(owner);
    if (observers == NULL) return -1;
    PyObject *capsule = PyList_GET_ITEM(observers, 1);
    if (capsule == Py_None) return 0;
    SoacTestOrdinaryDetachFault *fault = PyCapsule_GetPointer(
        capsule, "test.soac.ordinary.detach_fault");
    if (fault == NULL) return -1;
    if (fault->candidate != dictionary || fault->initial_complete ||
        ((PyDictObject *)dictionary)->ma_used != 1 ||
        _PyObject_GetManagedDict(fault->instance) != fault->previous_dictionary ||
        fault->previous_dictionary->ma_values != fault->previous_values ||
        fault->previous_values->valid != 1) {
        PyErr_SetString(PyExc_AssertionError, "detach fault missed its actual INITIAL boundary");
        return -1;
    }
    fault->initial_complete = 1;
    soac_test_ordinary_arm_fault(fault);
    return 0;
}

static int
soac_test_ordinary_invoke_hook(PyObject *owner, PyObject *instance, PyObject *candidate)
{
    PyObject *observers = soac_test_ordinary_observers(owner);
    if (observers == NULL) return -1;
    PyObject *hook = PyList_GET_ITEM(observers, 0);
    if (hook == Py_None) return 0;
    if ((Py_TYPE(instance)->tp_flags & Py_TPFLAGS_INLINE_VALUES) &&
        _PyObject_GetManagedDict(instance) == NULL &&
        _PyObject_InlineValues(instance)->valid == 2 &&
        PyObject_GC_IsTracked(candidate)) {
        PyErr_SetString(PyExc_AssertionError, "private dictionary header became GC-visible");
        return -1;
    }
    /* MOVE the one observer edge off the owner before reentry. Neither the
     * private dictionary nor an extra receiver argument is exposed to Python. */
    PyList_SET_ITEM(observers, 0, Py_NewRef(Py_None));
    PyObject *result = PyObject_CallNoArgs(hook);
    int ok = result == Py_None;
    if (result != NULL && !ok) PyErr_SetString(PyExc_TypeError, "test hook must return None");
    PyObject *error = PyErr_GetRaisedException();
    Py_XDECREF(result);
    Py_DECREF(hook);
    PyErr_SetRaisedException(error);
    return ok ? 0 : -1;
}

static PyObject *
dict_arm_soac_ordinary_hook(PyObject *self, PyObject *args)
{
    PyObject *type, *hook;
    if (!PyArg_ParseTuple(args, "OO:dict_arm_soac_ordinary_hook", &type, &hook)) return NULL;
    PyObject *owner = soac_test_ordinary_owner(type);
    if (owner == NULL) return NULL;
    PyObject *observers = PyTuple_GET_ITEM(owner, 2);
    if (!PyCallable_Check(hook) || PyList_GET_ITEM(observers, 0) != Py_None) {
        PyErr_SetString(PyExc_TypeError, "ordinary test hook must be callable and unarmed");
        return NULL;
    }
    PyObject *old = PyList_GET_ITEM(observers, 0);
    PyList_SET_ITEM(observers, 0, Py_NewRef(hook));
    Py_DECREF(old);
    Py_RETURN_NONE;
}

static PyObject *
dict_ordinary_inline_state(PyObject *self, PyObject *instance)
{
    if (!(Py_TYPE(instance)->tp_flags & Py_TPFLAGS_INLINE_VALUES)) {
        PyErr_SetString(PyExc_TypeError, "test receiver has no inline values");
        return NULL;
    }
    return Py_BuildValue("(iO)", (int)_PyObject_InlineValues(instance)->valid,
                         _PyObject_GetManagedDict(instance) == NULL ? Py_False : Py_True);
}

static PyObject *
dict_ordinary_clear_managed_probe(PyObject *self, PyObject *args)
{
    PyObject *instance, *primary;
    if (!PyArg_ParseTuple(args, "OO:dict_ordinary_clear_managed_probe", &instance, &primary)) return NULL;
    if (!(Py_TYPE(instance)->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
        (primary != Py_None && !PyExceptionInstance_Check(primary))) {
        PyErr_SetString(PyExc_TypeError, "clear probe requires inline receiver and exception or None");
        return NULL;
    }
    PyDictValues *values = _PyObject_InlineValues(instance);
    int preparing = values->valid == 2;
    PyDictObject *dictionary = _PyObject_GetManagedDict(instance);
    PyObject *saved[SHARED_KEYS_MAX_SIZE];
    unsigned char size = values->size, capacity = values->capacity;
    if (preparing) {
        assert(capacity <= SHARED_KEYS_MAX_SIZE);
        memcpy(saved, values->values, capacity * sizeof(PyObject *));
    }
    if (primary != Py_None) PyErr_SetRaisedException(Py_NewRef(primary));
    PyObject_ClearManagedDict(instance);
    /* Void is NOT a success signal: inspect the exact pending exception
     * immediately, before returning through any other C/Python operation. */
    PyObject *error = PyErr_GetRaisedException();
    if (preparing &&
        (error == NULL || values->valid != 2 || values->size != size ||
         _PyObject_GetManagedDict(instance) != dictionary ||
         memcmp(saved, values->values, capacity * sizeof(PyObject *)) != 0)) {
        Py_XDECREF(error);
        PyErr_SetString(PyExc_AssertionError, "busy clear changed live inline storage or lost its error");
        return NULL;
    }
    return error == NULL ? Py_NewRef(Py_None) : error;
}

static PyObject *
dict_ordinary_replace_detach_oom(PyObject *self, PyObject *args)
{
    PyObject *instance, *candidate;
    if (!PyArg_ParseTuple(args, "OO:dict_ordinary_replace_detach_oom", &instance, &candidate)) return NULL;
    if (!(Py_TYPE(instance)->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
        !PyDict_CheckExact(candidate) || PyDict_Size(candidate) != 1 ||
        PyDict_HasSoacPolicy(candidate)) {
        PyErr_SetString(PyExc_TypeError, "detach fault requires live inline storage and an unprotected one-entry dict");
        return NULL;
    }
    PyDictValues *values = _PyObject_InlineValues(instance);
    PyDictObject *previous = _PyObject_GetManagedDict(instance);
    if (previous == NULL || previous->ma_values != values || values->valid != 1 ||
        Py_REFCNT(previous) < 2) {
        PyErr_SetString(PyExc_TypeError, "detach fault requires the actual retained embedded dictionary");
        return NULL;
    }
    SoacTestOrdinaryDetachFault fault = {
        .instance = instance, .candidate = candidate,
        .previous_dictionary = previous, .previous_values = values,
        /* Exact new_values(capacity) byte size in native copy_values. */
        .expected_size = (values->capacity + 1) * sizeof(PyObject *) +
            _Py_SIZE_ROUND_UP(values->capacity, sizeof(PyObject *)),
    };
    PyObject *observers = NULL;
    if (_PySOAC_HasOrdinaryInstanceWrites(Py_TYPE(instance))) {
        PyObject *owner = soac_test_ordinary_owner((PyObject *)Py_TYPE(instance));
        if (owner == NULL) return NULL;
        observers = PyTuple_GET_ITEM(owner, 2);
        if (PyList_GET_ITEM(observers, 0) != Py_None ||
            PyList_GET_ITEM(observers, 1) != Py_None) {
            PyErr_SetString(PyExc_TypeError, "detach observer is already armed");
            return NULL;
        }
        PyObject *capsule = PyCapsule_New(&fault, "test.soac.ordinary.detach_fault", NULL);
        if (capsule == NULL) return NULL;
        PyObject *old = PyList_GET_ITEM(observers, 1);
        PyList_SET_ITEM(observers, 1, capsule);
        Py_DECREF(old);
        /* The actual INITIAL callback arms only after validating the sole
         * candidate item. No native MEM allocation intervenes before detach. */
    }
    else {
        /* Ordinary control has no factory/INITIAL interval: the same checked
         * native destination enters copy_values directly from this setter. */
        fault.initial_complete = 1;
        soac_test_ordinary_arm_fault(&fault);
    }
    int result = PyObject_GenericSetDict(instance, candidate, NULL);
    soac_test_ordinary_restore_allocator(&fault);
    PyObject *error = PyErr_GetRaisedException();
    if (observers != NULL) {
        PyObject *capsule = PyList_GET_ITEM(observers, 1);
        PyList_SET_ITEM(observers, 1, Py_NewRef(Py_None));
        Py_DECREF(capsule);
    }
    if (!fault.failed || fault.unexpected || result != -1 || error == NULL) {
        Py_XDECREF(error);
        PyErr_SetString(PyExc_AssertionError, "fault did not hit actual post-validation detach allocation");
        return NULL;
    }
    PyErr_SetRaisedException(error);
    return NULL;
}

static int soac_test_type_state_private_write(PyObject *, PyObject *, PyObject *, int);

static int
soac_test_ordinary_instance_policy(PyObject *owner, PyObject *dict, PyObject *key,
                                   PyObject *value, int operation, PyObject *provenance)
{
    if (operation == PyDict_SOAC_TERMINAL_TEARDOWN ||
        operation == PyDict_SOAC_DELETE || operation == PyDict_SOAC_CLEAR) {
        return 0;
    }
    int attribute = operation == PyDict_SOAC_ATTRIBUTE_SET ||
                    operation == PyDict_SOAC_ATTRIBUTE_SET_EXISTING;
    if ((!attribute && provenance != NULL) ||
        (attribute && (provenance == NULL || !PyUnicode_Check(provenance)))) {
        PyErr_SetString(PyExc_TypeError, "ordinary instance has no namespace permit");
        return -1;
    }
    if (PyUnicode_Check(key) &&
        soac_test_ordinary_field(owner, key, value) < 0) {
        return -1;
    }
    /* The original incoming attribute name is not the canonical stored key.
     * Inspect Unicode data, never repeat arbitrary hash/equality or str(). */
    if (attribute && soac_test_ordinary_field(owner, provenance, value) < 0) return -1;
    if (soac_test_type_state_private_write(owner, dict, value, operation) < 0) return -1;
    return operation == PyDict_SOAC_VALIDATE_INITIAL
        ? soac_test_ordinary_after_initial(owner, dict) : 0;
}

static int
soac_test_ordinary_inline_write(PyObject *owner, PyObject *instance,
                                PyObject *name, PyObject *value)
{
    return soac_test_ordinary_field(owner, name, value);
}

static int
soac_test_ordinary_prepare_dictionary(PyObject *owner, PyObject *instance,
                                      PyObject *dictionary,
                                      const PySoacInstanceDictPolicy *existing,
                                      PySoacInstanceDictPolicy *out)
{
    out->owner = NULL;
    out->validate = NULL;
    if (existing != NULL &&
        (existing->owner != owner ||
         existing->validate != soac_test_ordinary_instance_policy)) {
        PyErr_SetString(PyExc_TypeError, "incompatible actual instance dictionary policy");
        return -1;
    }
    if (soac_test_ordinary_invoke_hook(owner, instance, dictionary) < 0) return -1;
    out->owner = Py_NewRef(owner);
    out->validate = soac_test_ordinary_instance_policy;
    return 0;
}

static int
soac_test_ordinary_final_commit(PyObject *owner, PyObject *type,
                                const PySoacTypeContractSpecV4 *contract)
{
    PySoacTypeConstructionInfoV1 info;
    if (PyType_GetSoacConstructionInfoV1(type, &info, sizeof(info)) != 1 ||
        info.phase != Py_SOAC_TYPE_STATE_ADMITTING || info.owner != owner ||
        contract->dictionary_mode != Py_SOAC_INSTANCE_DICT_ORDINARY ||
        contract->fields != PyTuple_GET_ITEM(owner, 0) ||
        contract->protected_names != PyTuple_GET_ITEM(owner, 1) ||
        contract->final_methods != PyTuple_GET_ITEM(owner, 1) ||
        contract->object_slot_fields != PyTuple_GET_ITEM(owner, 1) ||
        contract->check_instance_write != soac_test_ordinary_inline_write ||
        contract->new_instance_dict != NULL ||
        contract->prepare_instance_dictionary_policy != soac_test_ordinary_prepare_dictionary) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "ordinary fixture final policy identity changed");
        }
        return -1;
    }
    return 0;
}

#include "_testinternalcapi/soac_type_state.inc"

static PyObject *
dict_new_soac_ordinary_type_impl(PyObject *args, int direct)
{
    PyObject *name, *bases, *namespace, *fields, *namespace_function;
    if (!PyArg_ParseTuple(args, "OOOOO:dict_new_soac_ordinary_type",
                          &name, &bases, &namespace, &fields, &namespace_function)) {
        return NULL;
    }
    if (!PyTuple_CheckExact(fields)) {
        PyErr_SetString(PyExc_TypeError, "ordinary fixture fields must be an exact tuple");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(fields); ++i) {
        if (!PyUnicode_CheckExact(PyTuple_GET_ITEM(fields, i))) {
            PyErr_SetString(PyExc_TypeError, "ordinary fixture field names must be exact strings");
            return NULL;
        }
    }
    PyObject *empty = PyTuple_New(0);
    PyObject *keywords = PyDict_New();
    PyObject *observers = PyList_New(2);
    if (observers != NULL) {
        PyList_SET_ITEM(observers, 0, Py_NewRef(Py_None));
        PyList_SET_ITEM(observers, 1, Py_NewRef(Py_None));
    }
    PyObject *owner = empty == NULL || observers == NULL
        ? NULL : PyTuple_Pack(3, fields, empty, observers);
    Py_XDECREF(observers);
    if (empty == NULL || keywords == NULL || owner == NULL) {
        Py_XDECREF(empty);
        Py_XDECREF(keywords);
        Py_XDECREF(owner);
        return NULL;
    }
    PySoacTypeConstructionSpec spec = {
        .abi_version = Py_SOAC_TYPE_CONTRACT_ABI,
        .struct_size = sizeof(spec),
        .construction_mode = Py_SOAC_TYPE_CONSTRUCT_PENDING,
        .owner = owner, .namespace_function = namespace_function,
        .name = name, .bases = bases, .namespace_dict = namespace,
        .keywords = keywords, .commit_final = soac_test_ordinary_final_commit,
        .bind_type = direct ? soac_test_type_state_bind : NULL,
    };
    PyObject *handle = PyType_NewSoacConstructionHandle(&spec);
    PyObject *type = handle == NULL ? NULL
        : PyType_FromSoacConstructionHandle(handle, namespace_function);
    PySoacTypeContractSpecV4 contract = {
        .dictionary_mode = Py_SOAC_INSTANCE_DICT_ORDINARY,
        .fields = fields, .protected_names = empty, .final_methods = empty,
        .object_slot_fields = empty,
        .check_instance_write = soac_test_ordinary_inline_write,
        .prepare_instance_dictionary_policy = soac_test_ordinary_prepare_dictionary,
    };
    if (type != NULL &&
        PyType_AdmitSoacPendingV1(type, owner, handle, &contract, sizeof(contract),
                                 soac_test_ordinary_final_commit) < 0) {
        Py_CLEAR(type);
    }
    if (type != NULL && PyType_SealSoacContract(type, owner) < 0) {
        Py_CLEAR(type);
    }
    Py_XDECREF(handle);
    Py_DECREF(owner);
    Py_DECREF(keywords);
    Py_DECREF(empty);
    return type;
}

static PyObject *
dict_new_soac_ordinary_type(PyObject *self, PyObject *args)
{
    return dict_new_soac_ordinary_type_impl(args, 0);
}

static PyObject *
dict_new_soac_type_state_type(PyObject *self, PyObject *args)
{
    return dict_new_soac_ordinary_type_impl(args, 1);
}

static PyObject *
dict_get_indexed_item(PyObject *self, PyObject *args)
{
    PyObject *dict;
    Py_ssize_t index;
    if (!PyArg_ParseTuple(
            args, "On:dict_get_indexed_item", &dict, &index))
    {
        return NULL;
    }
    PyObject *result = NULL;
    int found = _PyDict_GetIndexedItem(dict, index, &result);
    if (found < 0) {
        return NULL;
    }
    if (found == 0) {
        PyErr_SetString(PyExc_KeyError, "indexed dictionary slot is empty");
        return NULL;
    }
    return result;
}

static PyObject *
dict_set_indexed_item(PyObject *self, PyObject *args)
{
    PyObject *dict;
    PyObject *value;
    Py_ssize_t index;
    if (!PyArg_ParseTuple(
            args, "OnO:dict_set_indexed_item", &dict, &index, &value))
    {
        return NULL;
    }
    if (_PyDict_SetIndexedItem(dict, index, value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
dict_watch_split_keys_for_type(PyObject *self, PyObject *type)
{
    if (_PyDict_WatchSplitKeysForType(type) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
dict_get_key_layout_events(PyObject *self, PyObject *Py_UNUSED(args))
{
    return _PyDict_GetKeyLayoutEvents();
}


static int
_init_interp_config_from_object(PyInterpreterConfig *config, PyObject *obj)
{
    if (obj == NULL) {
        *config = (PyInterpreterConfig)_PyInterpreterConfig_INIT;
        return 0;
    }

    PyObject *dict = PyObject_GetAttrString(obj, "__dict__");
    if (dict == NULL) {
        PyErr_Format(PyExc_TypeError, "bad config %R", obj);
        return -1;
    }
    int res = _PyInterpreterConfig_InitFromDict(config, dict);
    Py_DECREF(dict);
    if (res < 0) {
        return -1;
    }
    return 0;
}

static PyInterpreterState *
_new_interpreter(PyInterpreterConfig *config, long whence)
{
    if (whence == _PyInterpreterState_WHENCE_XI) {
        return _PyXI_NewInterpreter(config, &whence, NULL, NULL);
    }
    PyObject *exc = NULL;
    PyInterpreterState *interp = NULL;
    if (whence == _PyInterpreterState_WHENCE_UNKNOWN) {
        assert(config == NULL);
        interp = PyInterpreterState_New();
    }
    else if (whence == _PyInterpreterState_WHENCE_CAPI
             || whence == _PyInterpreterState_WHENCE_LEGACY_CAPI)
    {
        PyThreadState *tstate = NULL;
        PyThreadState *save_tstate = PyThreadState_Swap(NULL);
        if (whence == _PyInterpreterState_WHENCE_LEGACY_CAPI) {
            assert(config == NULL);
            tstate = Py_NewInterpreter();
            PyThreadState_Swap(save_tstate);
        }
        else {
            PyStatus status = Py_NewInterpreterFromConfig(&tstate, config);
            PyThreadState_Swap(save_tstate);
            if (PyStatus_Exception(status)) {
                assert(tstate == NULL);
                _PyErr_SetFromPyStatus(status);
                exc = PyErr_GetRaisedException();
            }
        }
        if (tstate != NULL) {
            interp = PyThreadState_GetInterpreter(tstate);
            // Throw away the initial tstate.
            PyThreadState_Swap(tstate);
            PyThreadState_Clear(tstate);
            PyThreadState_Swap(save_tstate);
            PyThreadState_Delete(tstate);
        }
    }
    else {
        PyErr_Format(PyExc_ValueError,
                     "unsupported whence %ld", whence);
        return NULL;
    }

    if (interp == NULL) {
        PyErr_SetString(PyExc_InterpreterError,
                        "sub-interpreter creation failed");
        if (exc != NULL) {
            _PyErr_ChainExceptions1(exc);
        }
    }
    return interp;
}

// This exists mostly for testing the _interpreters module, as an
// alternative to _interpreters.create()
static PyObject *
create_interpreter(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"config", "whence", NULL};
    PyObject *configobj = NULL;
    long whence = _PyInterpreterState_WHENCE_XI;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "|O$l:create_interpreter", kwlist,
                                     &configobj, &whence))
    {
        return NULL;
    }
    if (configobj == Py_None) {
        configobj = NULL;
    }

    // Resolve the config.
    PyInterpreterConfig *config = NULL;
    PyInterpreterConfig _config;
    if (whence == _PyInterpreterState_WHENCE_UNKNOWN
            || whence == _PyInterpreterState_WHENCE_LEGACY_CAPI)
    {
        if (configobj != NULL) {
            PyErr_SetString(PyExc_ValueError, "got unexpected config");
            return NULL;
        }
    }
    else {
        config = &_config;
        if (_init_interp_config_from_object(config, configobj) < 0) {
            return NULL;
        }
    }

    // Create the interpreter.
    PyInterpreterState *interp = _new_interpreter(config, whence);
    if (interp == NULL) {
        return NULL;
    }

    // Return the ID.
    PyObject *idobj = _PyInterpreterState_GetIDObject(interp);
    if (idobj == NULL) {
        _PyXI_EndInterpreter(interp, NULL, NULL);
        return NULL;
    }

    return idobj;
}

// This exists mostly for testing the _interpreters module, as an
// alternative to _interpreters.destroy()
static PyObject *
destroy_interpreter(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"id", "basic", NULL};
    PyObject *idobj = NULL;
    int basic = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O|p:destroy_interpreter", kwlist,
                                     &idobj, &basic))
    {
        return NULL;
    }

    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        return NULL;
    }

    if (basic)
    {
        // Test the basic Py_EndInterpreter with weird out of order thread states
        PyThreadState *t1, *t2;
        PyThreadState *prev;
        t1 = interp->threads.head;
        if (t1 == NULL) {
            t1 = PyThreadState_New(interp);
        }
        t2 = PyThreadState_New(interp);
        prev = PyThreadState_Swap(t2);
        PyThreadState_Clear(t1);
        PyThreadState_Delete(t1);
        Py_EndInterpreter(t2);
        PyThreadState_Swap(prev);
    }
    else
    {
        // use the cross interpreter _PyXI_EndInterpreter normally
        _PyXI_EndInterpreter(interp, NULL, NULL);
    }
    Py_RETURN_NONE;
}

// This exists mostly for testing the _interpreters module, as an
// alternative to _interpreters.destroy()
static PyObject *
exec_interpreter(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"id", "code", "main", NULL};
    PyObject *idobj;
    const char *code;
    int runningmain = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "Os|$p:exec_interpreter", kwlist,
                                     &idobj, &code, &runningmain))
    {
        return NULL;
    }

    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        return NULL;
    }

    PyObject *res = NULL;
    PyThreadState *tstate =
        _PyThreadState_NewBound(interp, _PyThreadState_WHENCE_EXEC);

    PyThreadState *save_tstate = PyThreadState_Swap(tstate);

    if (runningmain) {
       if (_PyInterpreterState_SetRunningMain(interp) < 0) {
           goto finally;
       }
    }

    /* only initialise 'cflags.cf_flags' to test backwards compatibility */
    PyCompilerFlags cflags = {0};
    int r = PyRun_SimpleStringFlags(code, &cflags);
    if (PyErr_Occurred()) {
        PyErr_PrintEx(0);
    }

    if (runningmain) {
        _PyInterpreterState_SetNotRunningMain(interp);
    }

    res = PyLong_FromLong(r);

finally:
    PyThreadState_Clear(tstate);
    PyThreadState_Swap(save_tstate);
    PyThreadState_Delete(tstate);
    return res;
}


/* To run some code in a sub-interpreter.

Generally you can use the interpreters module,
but we keep this helper as a distinct implementation.
That's especially important for testing the interpreters module.
*/
static PyObject *
run_in_subinterp_with_config(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *code;
    PyObject *configobj;
    int xi = 0;
    static char *kwlist[] = {"code", "config", "xi", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                    "sO|$p:run_in_subinterp_with_config", kwlist,
                    &code, &configobj, &xi))
    {
        return NULL;
    }

    PyInterpreterConfig config;
    if (_init_interp_config_from_object(&config, configobj) < 0) {
        return NULL;
    }

    /* only initialise 'cflags.cf_flags' to test backwards compatibility */
    PyCompilerFlags cflags = {0};

    int r;
    if (xi) {
        PyThreadState *save_tstate;
        PyThreadState *tstate;

        /* Create an interpreter, staying switched to it. */
        PyInterpreterState *interp = \
                _PyXI_NewInterpreter(&config, NULL, &tstate, &save_tstate);
        if (interp == NULL) {
            return NULL;
        }

        /* Exec the code in the new interpreter. */
        r = PyRun_SimpleStringFlags(code, &cflags);

        /* clean up post-exec. */
        _PyXI_EndInterpreter(interp, tstate, &save_tstate);
    }
    else {
        PyThreadState *substate;
        PyThreadState *mainstate = PyThreadState_Swap(NULL);

        /* Create an interpreter, staying switched to it. */
        PyStatus status = Py_NewInterpreterFromConfig(&substate, &config);
        if (PyStatus_Exception(status)) {
            /* Since no new thread state was created, there is no exception to
               propagate; raise a fresh one after swapping in the old thread
               state. */
            PyThreadState_Swap(mainstate);
            _PyErr_SetFromPyStatus(status);
            PyObject *exc = PyErr_GetRaisedException();
            PyErr_SetString(PyExc_InterpreterError,
                            "sub-interpreter creation failed");
            _PyErr_ChainExceptions1(exc);
            return NULL;
        }

        /* Exec the code in the new interpreter. */
        r = PyRun_SimpleStringFlags(code, &cflags);

        /* clean up post-exec. */
        Py_EndInterpreter(substate);
        PyThreadState_Swap(mainstate);
    }

    return PyLong_FromLong(r);
}


static PyObject *
normalize_interp_id(PyObject *self, PyObject *idobj)
{
    int64_t interpid = _PyInterpreterState_ObjectToID(idobj);
    if (interpid < 0) {
        return NULL;
    }
    return PyLong_FromLongLong(interpid);
}

static PyObject *
next_interpreter_id(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int64_t interpid = _PyRuntime.interpreters.next_id;
    return PyLong_FromLongLong(interpid);
}

static PyObject *
unused_interpreter_id(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int64_t interpid = INT64_MAX;
    assert(interpid > _PyRuntime.interpreters.next_id);
    return PyLong_FromLongLong(interpid);
}

static PyObject *
interpreter_exists(PyObject *self, PyObject *idobj)
{
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        if (PyErr_ExceptionMatches(PyExc_InterpreterNotFoundError)) {
            PyErr_Clear();
            Py_RETURN_FALSE;
        }
        assert(PyErr_Occurred());
        return NULL;
    }
    Py_RETURN_TRUE;
}

static PyObject *
get_interpreter_refcount(PyObject *self, PyObject *idobj)
{
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        return NULL;
    }
    return PyLong_FromLongLong(interp->id_refcount);
}

static PyObject *
link_interpreter_refcount(PyObject *self, PyObject *idobj)
{
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        assert(PyErr_Occurred());
        return NULL;
    }
    _PyInterpreterState_RequireIDRef(interp, 1);
    Py_RETURN_NONE;
}

static PyObject *
unlink_interpreter_refcount(PyObject *self, PyObject *idobj)
{
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        assert(PyErr_Occurred());
        return NULL;
    }
    _PyInterpreterState_RequireIDRef(interp, 0);
    Py_RETURN_NONE;
}

static PyObject *
interpreter_refcount_linked(PyObject *self, PyObject *idobj)
{
    PyInterpreterState *interp = _PyInterpreterState_LookUpIDObject(idobj);
    if (interp == NULL) {
        return NULL;
    }
    if (_PyInterpreterState_RequiresIDRef(interp)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}


static void
_xid_capsule_destructor(PyObject *capsule)
{
    _PyXIData_t *xidata = (_PyXIData_t *)PyCapsule_GetPointer(capsule, NULL);
    if (xidata != NULL) {
        assert(_PyXIData_Release(xidata) == 0);
        _PyXIData_Free(xidata);
    }
}

static PyObject *
get_crossinterp_data(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj = NULL;
    PyObject *modeobj = NULL;
    static char *kwlist[] = {"obj", "mode", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                    "O|O:get_crossinterp_data", kwlist,
                    &obj, &modeobj))
    {
        return NULL;
    }
    const char *mode = NULL;
    if (modeobj == NULL || modeobj == Py_None) {
        mode = "xidata";
    }
    else if (!PyUnicode_Check(modeobj)) {
        PyErr_Format(PyExc_TypeError, "expected mode str, got %R", modeobj);
        return NULL;
    }
    else {
        mode = PyUnicode_AsUTF8(modeobj);
        if (strlen(mode) == 0) {
            mode = "xidata";
        }
    }

    PyThreadState *tstate = _PyThreadState_GET();
    _PyXIData_t *xidata = _PyXIData_New();
    if (xidata == NULL) {
        return NULL;
    }
    if (strcmp(mode, "xidata") == 0) {
        if (_PyObject_GetXIDataNoFallback(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "fallback") == 0) {
        xidata_fallback_t fallback = _PyXIDATA_FULL_FALLBACK;
        if (_PyObject_GetXIData(tstate, obj, fallback, xidata) != 0)
        {
            goto error;
        }
    }
    else if (strcmp(mode, "pickle") == 0) {
        if (_PyPickle_GetXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "marshal") == 0) {
        if (_PyMarshal_GetXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "code") == 0) {
        if (_PyCode_GetXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "func") == 0) {
        if (_PyFunction_GetXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "script") == 0) {
        if (_PyCode_GetScriptXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else if (strcmp(mode, "script-pure") == 0) {
        if (_PyCode_GetPureScriptXIData(tstate, obj, xidata) != 0) {
            goto error;
        }
    }
    else {
        PyErr_Format(PyExc_ValueError, "unsupported mode %R", modeobj);
        goto error;
    }
    PyObject *capsule = PyCapsule_New(xidata, NULL, _xid_capsule_destructor);
    if (capsule == NULL) {
        assert(_PyXIData_Release(xidata) == 0);
        goto error;
    }
    return capsule;

error:
    _PyXIData_Free(xidata);
    return NULL;
}

static PyObject *
restore_crossinterp_data(PyObject *self, PyObject *args)
{
    PyObject *capsule = NULL;
    if (!PyArg_ParseTuple(args, "O:restore_crossinterp_data", &capsule)) {
        return NULL;
    }

    _PyXIData_t *xidata = (_PyXIData_t *)PyCapsule_GetPointer(capsule, NULL);
    if (xidata == NULL) {
        return NULL;
    }
    return _PyXIData_NewObject(xidata);
}


static PyObject *
raiseTestError(const char* test_name, const char* msg)
{
    PyErr_Format(PyExc_AssertionError, "%s: %s", test_name, msg);
    return NULL;
}


/*[clinic input]
_testinternalcapi.test_long_numbits
[clinic start generated code]*/

static PyObject *
_testinternalcapi_test_long_numbits_impl(PyObject *module)
/*[clinic end generated code: output=745d62d120359434 input=f14ca6f638e44dad]*/
{
    struct triple {
        long input;
        uint64_t nbits;
        int sign;
    } testcases[] = {{0, 0, 0},
                     {1L, 1, 1},
                     {-1L, 1, -1},
                     {2L, 2, 1},
                     {-2L, 2, -1},
                     {3L, 2, 1},
                     {-3L, 2, -1},
                     {4L, 3, 1},
                     {-4L, 3, -1},
                     {0x7fffL, 15, 1},          /* one Python int digit */
             {-0x7fffL, 15, -1},
             {0xffffL, 16, 1},
             {-0xffffL, 16, -1},
             {0xfffffffL, 28, 1},
             {-0xfffffffL, 28, -1}};
    size_t i;

    for (i = 0; i < Py_ARRAY_LENGTH(testcases); ++i) {
        uint64_t nbits;
        int sign = -7;
        PyObject *plong;

        plong = PyLong_FromLong(testcases[i].input);
        if (plong == NULL)
            return NULL;
        nbits = _PyLong_NumBits(plong);
        (void)PyLong_GetSign(plong, &sign);

        Py_DECREF(plong);
        if (nbits != testcases[i].nbits)
            return raiseTestError("test_long_numbits",
                            "wrong result for _PyLong_NumBits");
        if (sign != testcases[i].sign)
            return raiseTestError("test_long_numbits",
                            "wrong result for PyLong_GetSign()");
    }
    Py_RETURN_NONE;
}

static PyObject *
compile_perf_trampoline_entry(PyObject *self, PyObject *args)
{
    PyObject *co;
    if (!PyArg_ParseTuple(args, "O!", &PyCode_Type, &co)) {
        return NULL;
    }
    int ret = PyUnstable_PerfTrampoline_CompileCode((PyCodeObject *)co);
    if (ret != 0) {
        PyErr_SetString(PyExc_AssertionError, "Failed to compile trampoline");
        return NULL;
    }
    return PyLong_FromLong(ret);
}

static PyObject *
perf_trampoline_set_persist_after_fork(PyObject *self, PyObject *args)
{
    int enable;
    if (!PyArg_ParseTuple(args, "i", &enable)) {
        return NULL;
    }
    int ret = PyUnstable_PerfTrampoline_SetPersistAfterFork(enable);
    if (ret == 0) {
        PyErr_SetString(PyExc_AssertionError, "Failed to set persist_after_fork");
        return NULL;
    }
    return PyLong_FromLong(ret);
}


static PyObject *
get_rare_event_counters(PyObject *self, PyObject *type)
{
    PyInterpreterState *interp = PyInterpreterState_Get();

    return Py_BuildValue(
        "{sksksksksk}",
        "set_class", (unsigned long)interp->rare_events.set_class,
        "set_bases", (unsigned long)interp->rare_events.set_bases,
        "set_eval_frame_func", (unsigned long)interp->rare_events.set_eval_frame_func,
        "builtin_dict", (unsigned long)interp->rare_events.builtin_dict,
        "func_modification", (unsigned long)interp->rare_events.func_modification
    );
}

static PyObject *
reset_rare_event_counters(PyObject *self, PyObject *Py_UNUSED(type))
{
    PyInterpreterState *interp = PyInterpreterState_Get();

    interp->rare_events.set_class = 0;
    interp->rare_events.set_bases = 0;
    interp->rare_events.set_eval_frame_func = 0;
    interp->rare_events.builtin_dict = 0;
    interp->rare_events.func_modification = 0;

    return Py_None;
}


#ifdef Py_GIL_DISABLED
static PyObject *
get_py_thread_id(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    uintptr_t tid = _Py_ThreadId();
    Py_BUILD_ASSERT(sizeof(unsigned long long) >= sizeof(tid));
    return PyLong_FromUnsignedLongLong(tid);
}

static PyCodeObject *
get_code(PyObject *obj)
{
    if (PyCode_Check(obj)) {
        return (PyCodeObject *)obj;
    }
    else if (PyFunction_Check(obj)) {
        return (PyCodeObject *)PyFunction_GetCode(obj);
    }
    return (PyCodeObject *)PyErr_Format(
        PyExc_TypeError, "expected function or code object, got %T", obj);
}

static PyObject *
get_tlbc(PyObject *Py_UNUSED(module), PyObject *obj)
{
    PyCodeObject *code = get_code(obj);
    if (code == NULL) {
        return NULL;
    }
    _Py_CODEUNIT *bc = _PyCode_GetTLBCFast(PyThreadState_GET(), code);
    if (bc == NULL) {
        Py_RETURN_NONE;
    }
    return PyBytes_FromStringAndSize((const char *)bc, _PyCode_NBYTES(code));
}

static PyObject *
get_tlbc_id(PyObject *Py_UNUSED(module), PyObject *obj)
{
    PyCodeObject *code = get_code(obj);
    if (code == NULL) {
        return NULL;
    }
    _Py_CODEUNIT *bc = _PyCode_GetTLBCFast(PyThreadState_GET(), code);
    if (bc == NULL) {
        Py_RETURN_NONE;
    }
    return PyLong_FromVoidPtr(bc);
}

static PyObject *
get_long_lived_total(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromInt64(PyInterpreterState_Get()->gc.long_lived_total);
}

#endif

static PyObject *
has_inline_values(PyObject *self, PyObject *obj)
{
    if ((Py_TYPE(obj)->tp_flags & Py_TPFLAGS_INLINE_VALUES) &&
        _PyObject_InlineValues(obj)->valid) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *
has_split_table(PyObject *self, PyObject *obj)
{
    if (PyDict_Check(obj) && _PyDict_HasSplitTable((PyDictObject *)obj)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// Circumvents standard version assignment machinery - use with caution and only on
// short-lived heap types
static PyObject *
type_assign_specific_version_unsafe(PyObject *self, PyObject *args)
{
    PyTypeObject *type;
    unsigned int version;
    if (!PyArg_ParseTuple(args, "Oi:type_assign_specific_version_unsafe", &type, &version)) {
        return NULL;
    }
    assert(!PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE));
    _PyType_SetVersion(type, version);
    Py_RETURN_NONE;
}

/*[clinic input]
gh_119213_getargs

    spam: object = None

Test _PyArg_Parser.kwtuple
[clinic start generated code]*/

static PyObject *
gh_119213_getargs_impl(PyObject *module, PyObject *spam)
/*[clinic end generated code: output=d8d9c95d5b446802 input=65ef47511da80fc2]*/
{
    // It must never have been called in the main interprer
    assert(!_Py_IsMainInterpreter(PyInterpreterState_Get()));
    return Py_NewRef(spam);
}

/*[clinic input]
get_next_dict_keys_version
[clinic start generated code]*/

static PyObject *
get_next_dict_keys_version_impl(PyObject *module)
/*[clinic end generated code: output=e5405a509cf9d423 input=bd1cee7c6b9d3a3c]*/
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    uint32_t keys_version = interp->dict_state.next_keys_version;
    return PyLong_FromLong(keys_version);
}

static PyObject *
get_static_builtin_types(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return _PyStaticType_GetBuiltins();
}


static PyObject *
identify_type_slot_wrappers(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return _PyType_GetSlotWrapperNames();
}


static PyObject *
has_deferred_refcount(PyObject *self, PyObject *op)
{
    return PyBool_FromLong(_PyObject_HasDeferredRefcount(op));
}

static PyObject *
get_tracked_heap_size(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromInt64(PyInterpreterState_Get()->gc.heap_size);
}

static PyObject *
is_static_immortal(PyObject *self, PyObject *op)
{
    if (_Py_IsStaticImmortal(op)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *
incref_decref_delayed(PyObject *self, PyObject *op)
{
    _PyObject_XDecRefDelayed(Py_NewRef(op));
    Py_RETURN_NONE;
}

#ifdef __EMSCRIPTEN__
#include "emscripten.h"

EM_JS(int, emscripten_set_up_async_input_device_js, (void), {
    let idx = 0;
    const encoder = new TextEncoder();
    const bufs = [
        encoder.encode("ab\n"),
        encoder.encode("fi\n"),
        encoder.encode("xy\n"),
    ];
    function sleep(t) {
        return new Promise(res => setTimeout(res, t));
    }
    FS.createAsyncInputDevice("/dev", "blah", async () => {
        await sleep(5);
        return bufs[(idx ++) % 3];
    });
    return !!WebAssembly.promising;
});

static PyObject *
emscripten_set_up_async_input_device(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    if (emscripten_set_up_async_input_device_js()) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}
#endif

static PyObject *
simple_pending_call(PyObject *self, PyObject *callable)
{
    if (_PyEval_AddPendingCall(_PyInterpreterState_GET(), _pending_callback, Py_NewRef(callable), 0) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *
vectorcall_nop(PyObject *callable, PyObject *const *args,
               size_t nargsf, PyObject *kwnames)
{
    Py_RETURN_NONE;
}

static PyObject *
set_vectorcall_nop(PyObject *self, PyObject *func)
{
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "expected function");
        return NULL;
    }

    ((PyFunctionObject*)func)->vectorcall = vectorcall_nop;
    Py_RETURN_NONE;
}

/* Trusted C fixtures for the replay mechanism, not the production Rust
 * artifact/role/closure authentication boundary. No hidden Python globals. */
static PyObject *
soac_test_annotation_replay_resolver(PyObject *provider, PyObject *owner, int format)
{
    if (!PyFunction_Check(provider)) {
        PyErr_SetString(PyExc_TypeError, "annotation replay fixture needs a function");
        return NULL;
    }
    return PySoac_CloneAnnotationReplayCode(provider, Py_None,
                                          PyFunction_GET_CODE(provider));
}

static PyObject *
soac_test_other_replay_resolver(PyObject *provider, PyObject *owner, int format)
{
    PyErr_SetString(PyExc_AssertionError, "replacement replay resolver was called");
    return NULL;
}

static PyObject *
soac_install_annotation_replay_resolver(PyObject *self, PyObject *alternate)
{
    PySoacAnnotationReplayResolver resolver;
    if (alternate == Py_False) {
        resolver = soac_test_annotation_replay_resolver;
    }
    else if (alternate == Py_True) {
        resolver = soac_test_other_replay_resolver;
    }
    else if (alternate == Py_None) {
        resolver = NULL;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "expected bool or None");
        return NULL;
    }
    if (PySoac_SetAnnotationReplayResolver(resolver) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
soac_test_annotation_vectorcall(PyObject *provider, PyObject *const *args,
                               size_t nargsf, PyObject *kwnames)
{
    PyObject *code = PySoac_CloneAnnotationReplayCode(
        provider, Py_None, PyFunction_GET_CODE(provider));
    if (code == NULL) {
        return NULL;
    }
    PyObject *plain = PyFunction_New(code, PyFunction_GET_GLOBALS(provider));
    Py_DECREF(code);
    if (plain == NULL) {
        return NULL;
    }
    PyFunctionObject *original = (PyFunctionObject *)provider;
    Py_SETREF(((PyFunctionObject *)plain)->func_builtins,
              Py_NewRef(original->func_builtins));
    if ((original->func_closure != NULL &&
         PyFunction_SetClosure(plain, original->func_closure) < 0) ||
        (original->func_defaults != NULL &&
         PyFunction_SetDefaults(plain, original->func_defaults) < 0) ||
        (original->func_kwdefaults != NULL &&
         PyFunction_SetKwDefaults(plain, original->func_kwdefaults) < 0)) {
        Py_DECREF(plain);
        return NULL;
    }
    PyObject *result = PyObject_Vectorcall(plain, args, nargsf, kwnames);
    Py_DECREF(plain);
    return result;
}

static PyObject *
soac_prepare_annotation_replay_fixture(PyObject *self, PyObject *provider)
{
    if (!PyFunction_Check(provider)) {
        PyErr_SetString(PyExc_TypeError, "annotation replay fixture needs an exact function");
        return NULL;
    }
    PyObject *code = PyFunction_GET_CODE(provider);
    if (code == NULL || !PyCode_Check(code) ||
        ((PyCodeObject *)code)->_co_soac_strict_source_id == 0 ||
        !(((PyCodeObject *)code)->co_flags & CO_FUTURE_STRICT)) {
        PyErr_SetString(PyExc_ValueError, "annotation replay fixture needs verified strict code");
        return NULL;
    }
    if (PyFunction_SetSoacStrictOwner(provider, Py_None) < 0) {
        return NULL;
    }
    PyFunction_SetVectorcall((PyFunctionObject *)provider, soac_test_annotation_vectorcall);
    Py_RETURN_NONE;
}

static PyObject *
module_get_gc_hooks(PyObject *self, PyObject *arg)
{
    PyModuleObject *mod = (PyModuleObject *)arg;
    PyObject *traverse = NULL;
    PyObject *clear = NULL;
    PyObject *free = NULL;
    PyObject *result = NULL;
    traverse = PyLong_FromVoidPtr(mod->md_state_traverse);
    if (!traverse) {
        goto finally;
    }
    clear = PyLong_FromVoidPtr(mod->md_state_clear);
    if (!clear) {
        goto finally;
    }
    free = PyLong_FromVoidPtr(mod->md_state_free);
    if (!free) {
        goto finally;
    }
    result = PyTuple_FromArray((PyObject*[]){ traverse, clear, free }, 3);
finally:
    Py_XDECREF(traverse);
    Py_XDECREF(clear);
    Py_XDECREF(free);
    return result;
}


static void
check_threadstate_set_stack_protection(PyThreadState *tstate,
                                       void *start, size_t size)
{
    assert(PyUnstable_ThreadState_SetStackProtection(tstate, start, size) == 0);
    assert(!PyErr_Occurred());

    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    assert(ts->c_stack_top == (uintptr_t)start + size);
    assert(ts->c_stack_hard_limit <= ts->c_stack_soft_limit);
    assert(ts->c_stack_soft_limit < ts->c_stack_top);
}


static PyObject *
test_threadstate_set_stack_protection(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyThreadState *tstate = PyThreadState_GET();
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    assert(!PyErr_Occurred());

    uintptr_t init_base = ts->c_stack_init_base;
    size_t init_top = ts->c_stack_init_top;

    // Test the minimum stack size
    size_t size = _PyOS_MIN_STACK_SIZE;
    void *start = (void*)(_Py_get_machine_stack_pointer() - size);
    check_threadstate_set_stack_protection(tstate, start, size);

    // Test a larger size
    size = 7654321;
    assert(size > _PyOS_MIN_STACK_SIZE);
    start = (void*)(_Py_get_machine_stack_pointer() - size);
    check_threadstate_set_stack_protection(tstate, start, size);

    // Test invalid size (too small)
    size = 5;
    start = (void*)(_Py_get_machine_stack_pointer() - size);
    assert(PyUnstable_ThreadState_SetStackProtection(tstate, start, size) == -1);
    assert(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();

    // Test PyUnstable_ThreadState_ResetStackProtection()
    PyUnstable_ThreadState_ResetStackProtection(tstate);
    assert(ts->c_stack_init_base == init_base);
    assert(ts->c_stack_init_top == init_top);

    Py_RETURN_NONE;
}


#include "_testinternalcapi/soac_function_watch.inc"
#include "_testinternalcapi/soac_interpreter.inc"
#include "_testinternalcapi/soac_pending_type.inc"
#include "_testinternalcapi/soac_slots.inc"
#include "_testinternalcapi/soac_dataclass.inc"

/* A direct, allocation-free argument bridge for descriptor fault injection.
 * Unlike ctypes marshalling, all allocation failures after this boundary are
 * in the actual native constructor. This fixture grants no source authority. */
static PyObject *
soac_new_builtin_descriptor(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 5) {
        PyErr_SetString(PyExc_TypeError, "descriptor fixture needs five operands");
        return NULL;
    }
    return PySoac_NewBuiltinDescriptor(args[0], args[1], args[2], args[3], args[4]);
}

static PyObject *
soac_descriptor_birth_exhaustion(PyObject *self, PyObject *const *args,
                                  Py_ssize_t nargs)
{
    if (nargs != 5) {
        PyErr_SetString(PyExc_TypeError, "descriptor fixture needs five operands");
        return NULL;
    }
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "counter fault injection requires the GIL build");
    return NULL;
#else
    PyInterpreterState *interp = _PyInterpreterState_GET();
    uint64_t previous = interp->soac.descriptor_birth_counter;
    /* Expose no reset operation and never mint an ID in the altered range.
     * Allocation/finalizer reentry also sees exhaustion, not a reusable ID. */
    interp->soac.descriptor_birth_counter = UINT64_MAX;
    PyObject *result = PySoac_NewBuiltinDescriptor(
        args[0], args[1], args[2], args[3], args[4]);
    assert(result == NULL);
    assert(PyErr_Occurred());
    assert(interp->soac.descriptor_birth_counter == UINT64_MAX);
    interp->soac.descriptor_birth_counter = previous;
    return NULL;
#endif
}

static PyObject *
soac_descriptor_birth_foreign(PyObject *self, PyObject *const *args,
                              Py_ssize_t nargs)
{
    if (nargs != 5) {
        PyErr_SetString(PyExc_TypeError, "descriptor fixture needs five operands");
        return NULL;
    }
    PyThreadState *saved = PyThreadState_Swap(NULL);
    PyThreadState *foreign;
    PyInterpreterConfig config = _PyInterpreterConfig_LEGACY_INIT;
    PyStatus status = Py_NewInterpreterFromConfig(&foreign, &config);
    if (PyStatus_Exception(status)) {
        PyThreadState_Swap(saved);
        _PyErr_SetFromPyStatus(status);
        return NULL;
    }
    /* The original caller pins all operands. We neither refcount nor use the
     * foreign function/environment: each API must reject the birth's origin
     * first and set an exception belonging to this current interpreter. */
    int rejected = 0;
    PyObject *error = PySoac_GetStrictRuntimeUnavailableError();
    if (error != NULL) {
        uint64_t identity = PySoac_GetDescriptorBirthId(args[0]);
        rejected += identity == 0 && PyErr_ExceptionMatches(error);
        PyErr_Clear();
        PyObject *owner = PySoac_GetDescriptorBirthOwner(args[0]);
        rejected += owner == NULL && PyErr_ExceptionMatches(error);
        PyErr_Clear();
        int matches = PySoac_MatchesDescriptorBirth(
            args[0], args[1], args[2], args[3], args[4]);
        rejected += matches == -1 && PyErr_ExceptionMatches(error);
        PyErr_Clear();
        int adopted = PySoac_AdoptBuiltinDescriptor(
            args[0], args[1], args[2], args[3], args[4]);
        rejected += adopted == -1 && PyErr_ExceptionMatches(error);
        PyErr_Clear();
    }
    else {
        PyErr_Clear();
    }
    Py_EndInterpreter(foreign);
    PyThreadState_Swap(saved);
    return PyLong_FromLong(rejected);
}

static PyMethodDef module_functions[] = {
    {"soac_pending_type_construct", soac_pending_type_construct, METH_VARARGS},
    {"soac_pending_type_admit", soac_pending_type_admit, METH_VARARGS},
    {"soac_pending_type_admit_and_retire", soac_pending_type_admit_and_retire, METH_VARARGS},
    {"soac_pending_type_try_weaker_admission", soac_pending_type_try_weaker_admission, METH_VARARGS},
    {"soac_pending_type_preserve_error", soac_pending_type_preserve_error, METH_VARARGS},
    {"soac_pending_type_allocate", soac_pending_type_allocate, METH_VARARGS},
    {"soac_pending_type_init_buffer", soac_pending_type_init_buffer, METH_VARARGS},
    {"soac_pending_type_from_spec", soac_pending_type_from_spec, METH_O},
    {"soac_slot_type", soac_slot_type, METH_VARARGS},
    {"soac_slot_definition", soac_slot_definition, METH_O},
    {"soac_slot_view_get", _PyCFunction_CAST(soac_slot_view_get), METH_VARARGS | METH_KEYWORDS},
    {"soac_slot_view_set", _PyCFunction_CAST(soac_slot_view_set), METH_VARARGS | METH_KEYWORDS},
    {"soac_type_construction_layout", soac_type_construction_layout, METH_NOARGS},
    {"soac_dataclass_boundary_owner", soac_dataclass_boundary_owner, METH_VARARGS},
    {"soac_dataclass_boundary_state", soac_dataclass_boundary_state, METH_O},
    {"soac_dataclass_configure_boundary", soac_dataclass_configure_boundary, METH_VARARGS},
    {"soac_dataclass_forward_checked_entry", soac_dataclass_forward_checked_entry, METH_O},
    {"soac_new_builtin_descriptor", _PyCFunction_CAST(soac_new_builtin_descriptor), METH_FASTCALL},
    {"soac_descriptor_birth_exhaustion", _PyCFunction_CAST(soac_descriptor_birth_exhaustion), METH_FASTCALL},
    {"soac_descriptor_birth_foreign", _PyCFunction_CAST(soac_descriptor_birth_foreign), METH_FASTCALL},
    {"soac_dataclass_fixture", soac_dataclass_fixture, METH_VARARGS},
    {"soac_dataclass_fixture_call", soac_dataclass_fixture_call, METH_VARARGS},
    {"soac_dataclass_fixture_c_proxy", soac_dataclass_fixture_c_proxy, METH_O},
    {"soac_dataclass_fixture_c_forward", _PyCFunction_CAST(soac_dataclass_fixture_c_forward), METH_FASTCALL},
    {"soac_dataclass_fixture_copy_builtin", soac_dataclass_fixture_copy_builtin, METH_VARARGS},
    {"soac_code_view", soac_code_view, METH_VARARGS},
    {"soac_dataclass_frame_offsets", soac_dataclass_frame_offsets, METH_NOARGS},
    {"get_configs", get_configs, METH_NOARGS},
    {"get_eval_frame_stats", get_eval_frame_stats, METH_NOARGS, NULL},
    {"get_recursion_depth", get_recursion_depth, METH_NOARGS},
    {"get_c_recursion_remaining", get_c_recursion_remaining, METH_NOARGS},
    {"get_stack_pointer", get_stack_pointer, METH_NOARGS},
    {"get_stack_margin", get_stack_margin, METH_NOARGS},
    {"classify_stack_addresses", classify_stack_addresses, METH_VARARGS},
    {"get_jit_code_ranges", get_jit_code_ranges, METH_NOARGS},
    {"get_jit_backend", get_jit_backend, METH_NOARGS},
    {"manual_frame_pointer_unwind", manual_frame_pointer_unwind, METH_NOARGS},
    {"test_bswap", test_bswap, METH_NOARGS},
    {"test_popcount", test_popcount, METH_NOARGS},
    {"test_bit_length", test_bit_length, METH_NOARGS},
    {"test_hashtable", test_hashtable, METH_NOARGS},
    {"reset_path_config", test_reset_path_config, METH_NOARGS},
    {"test_edit_cost", test_edit_cost, METH_NOARGS},
    {"test_bytes_find", test_bytes_find, METH_NOARGS},
    {"normalize_path", normalize_path, METH_O, NULL},
    {"get_getpath_codeobject", get_getpath_codeobject, METH_NOARGS, NULL},
    {"EncodeLocaleEx", encode_locale_ex, METH_VARARGS},
    {"DecodeLocaleEx", decode_locale_ex, METH_VARARGS},
    {"set_eval_frame_default", set_eval_frame_default, METH_NOARGS, NULL},
    {"set_eval_frame_interp", set_eval_frame_interp, METH_NOARGS, NULL},
    {"set_eval_frame_record", set_eval_frame_record, METH_O, NULL},
    _TESTINTERNALCAPI_COMPILER_CLEANDOC_METHODDEF
    _TESTINTERNALCAPI_NEW_INSTRUCTION_SEQUENCE_METHODDEF
    _TESTINTERNALCAPI_COMPILER_CODEGEN_METHODDEF
    _TESTINTERNALCAPI_OPTIMIZE_CFG_METHODDEF
    _TESTINTERNALCAPI_ASSEMBLE_CODE_OBJECT_METHODDEF
    {"get_interp_settings", get_interp_settings, METH_VARARGS, NULL},
    {"clear_extension", clear_extension, METH_VARARGS, NULL},
    {"write_perf_map_entry", write_perf_map_entry, METH_VARARGS},
    {"perf_map_state_teardown", perf_map_state_teardown, METH_NOARGS},
    {"iframe_getcode", iframe_getcode, METH_O, NULL},
    {"iframe_getline", iframe_getline, METH_O, NULL},
    {"iframe_getlasti", iframe_getlasti, METH_O, NULL},
    {"code_returns_only_none", code_returns_only_none, METH_O, NULL},
    {"get_co_framesize", get_co_framesize, METH_O, NULL},
    {"get_co_localskinds", get_co_localskinds, METH_O, NULL},
    {"get_code_var_counts", _PyCFunction_CAST(get_code_var_counts),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"verify_stateless_code", _PyCFunction_CAST(verify_stateless_code),
     METH_VARARGS | METH_KEYWORDS, NULL},
#ifdef _Py_TIER2
    {"add_executor_dependency", add_executor_dependency, METH_VARARGS, NULL},
    {"invalidate_executors", invalidate_executors, METH_O, NULL},
    {"clear_executor_deletion_list", clear_executor_deletion_list, METH_NOARGS, NULL},
    {"get_exit_executor", get_exit_executor, METH_O, NULL},
#endif
    {"pending_threadfunc", _PyCFunction_CAST(pending_threadfunc),
     METH_VARARGS | METH_KEYWORDS},
    {"pending_identify", pending_identify, METH_VARARGS, NULL},
    {"_PyTraceMalloc_GetTraceback", tracemalloc_get_traceback, METH_VARARGS},
    {"test_tstate_capi", test_tstate_capi, METH_NOARGS, NULL},
    {"_PyUnicode_TransformDecimalAndSpaceToASCII", unicode_transformdecimalandspacetoascii, METH_O},
    {"check_pyobject_forbidden_bytes_is_freed",
                            check_pyobject_forbidden_bytes_is_freed, METH_NOARGS},
    {"check_pyobject_freed_is_freed", check_pyobject_freed_is_freed, METH_NOARGS},
    {"check_pyobject_null_is_freed",  check_pyobject_null_is_freed,  METH_NOARGS},
    {"check_pyobject_uninitialized_is_freed",
                              check_pyobject_uninitialized_is_freed, METH_NOARGS},
    {"pymem_getallocatorsname", test_pymem_getallocatorsname, METH_NOARGS},
    {"get_object_dict_values", get_object_dict_values, METH_O},
    {"hamt", new_hamt, METH_NOARGS},
    {"dict_getitem_knownhash",  dict_getitem_knownhash,          METH_VARARGS},
    {"dict_new_indexed", dict_new_indexed, METH_O},
    {"dict_has_indexed_keys", dict_has_indexed_keys, METH_O},
    {"dict_has_no_lookup_aliases", dict_has_no_lookup_aliases, METH_O},
    {"dict_reserve_soac_namespace_keys", dict_reserve_soac_namespace_keys, METH_VARARGS},
    {"dict_setitem_and_delete_for_module", dict_setitem_and_delete_for_module, METH_VARARGS},
    {"dict_new_soac_type", dict_new_soac_type, METH_VARARGS},
    {"dict_new_soac_ordinary_type", dict_new_soac_ordinary_type, METH_VARARGS},
    {"dict_new_soac_type_state_type", dict_new_soac_type_state_type, METH_VARARGS},
    {"slot_new_soac_type_state_type", slot_new_soac_type_state_type, METH_VARARGS},
    {"get_soac_type_state_info", get_soac_type_state_info, METH_O},
    {"capture_soac_type_state_allocation", capture_soac_type_state_allocation, METH_VARARGS},
    {"soac_type_state_alloc_pending_error", soac_type_state_alloc_pending_error, METH_VARARGS},
    {"check_soac_type_state_reftracer", check_soac_type_state_reftracer, METH_O},
    {"probe_soac_type_state_lookups", probe_soac_type_state_lookups, METH_VARARGS},
    {"probe_soac_type_state_private_escape", probe_soac_type_state_private_escape, METH_VARARGS},
    {"soac_type_state_clear", soac_type_state_clear, METH_O},
    {"soac_type_state_negative_refcount", soac_type_state_negative_refcount, METH_O},
    {"soac_type_state_write_member", soac_type_state_write_member, METH_VARARGS},
    {"dict_arm_soac_ordinary_hook", dict_arm_soac_ordinary_hook, METH_VARARGS},
    {"dict_ordinary_inline_state", dict_ordinary_inline_state, METH_O},
    {"dict_ordinary_clear_managed_probe", dict_ordinary_clear_managed_probe, METH_VARARGS},
    {"dict_ordinary_replace_detach_oom", dict_ordinary_replace_detach_oom, METH_VARARGS},
    {"soac_function_create_watch", soac_function_create_watch, METH_VARARGS},
    {"soac_function_create_unwatch", soac_function_create_unwatch, METH_O},
    {"soac_interpreter_fixture", soac_interpreter_fixture, METH_VARARGS},
    {"soac_interpreter_eval", soac_interpreter_eval, METH_VARARGS},
    {"soac_interpreter_events", soac_interpreter_events, METH_O},
    {"soac_interpreter_forward_entry", soac_interpreter_forward_entry, METH_O},
    {"dict_new_from_indexed_schema", dict_new_from_indexed_schema, METH_O},
    {"dict_setitem_knownhash", dict_setitem_knownhash, METH_VARARGS},
    {"dict_delitem_knownhash", dict_delitem_knownhash, METH_VARARGS},
    {"dict_indexed_key_index", dict_indexed_key_index, METH_VARARGS},
    {"dict_get_indexed_item", dict_get_indexed_item, METH_VARARGS},
    {"dict_set_indexed_item", dict_set_indexed_item, METH_VARARGS},
    {"dict_watch_split_keys_for_type", dict_watch_split_keys_for_type, METH_O},
    {"dict_get_key_layout_events", dict_get_key_layout_events, METH_NOARGS},
    {"create_interpreter", _PyCFunction_CAST(create_interpreter),
     METH_VARARGS | METH_KEYWORDS},
    {"destroy_interpreter", _PyCFunction_CAST(destroy_interpreter),
     METH_VARARGS | METH_KEYWORDS},
    {"exec_interpreter", _PyCFunction_CAST(exec_interpreter),
     METH_VARARGS | METH_KEYWORDS},
    {"run_in_subinterp_with_config",
     _PyCFunction_CAST(run_in_subinterp_with_config),
     METH_VARARGS | METH_KEYWORDS},
    {"normalize_interp_id", normalize_interp_id, METH_O},
    {"next_interpreter_id", next_interpreter_id, METH_NOARGS},
    {"unused_interpreter_id", unused_interpreter_id, METH_NOARGS},
    {"interpreter_exists", interpreter_exists, METH_O},
    {"get_interpreter_refcount", get_interpreter_refcount, METH_O},
    {"link_interpreter_refcount", link_interpreter_refcount,     METH_O},
    {"unlink_interpreter_refcount", unlink_interpreter_refcount, METH_O},
    {"interpreter_refcount_linked", interpreter_refcount_linked, METH_O},
    {"compile_perf_trampoline_entry", compile_perf_trampoline_entry, METH_VARARGS},
    {"perf_trampoline_set_persist_after_fork", perf_trampoline_set_persist_after_fork, METH_VARARGS},
    {"get_crossinterp_data",    _PyCFunction_CAST(get_crossinterp_data),
     METH_VARARGS | METH_KEYWORDS},
    {"restore_crossinterp_data", restore_crossinterp_data,       METH_VARARGS},
    _TESTINTERNALCAPI_TEST_LONG_NUMBITS_METHODDEF
    {"get_rare_event_counters", get_rare_event_counters, METH_NOARGS},
    {"reset_rare_event_counters", reset_rare_event_counters, METH_NOARGS},
    {"has_inline_values", has_inline_values, METH_O},
    {"has_split_table", has_split_table, METH_O},
    {"type_assign_specific_version_unsafe", type_assign_specific_version_unsafe, METH_VARARGS,
     PyDoc_STR("forcefully assign type->tp_version_tag")},

#ifdef Py_GIL_DISABLED
    {"py_thread_id", get_py_thread_id, METH_NOARGS},
    {"get_tlbc", get_tlbc, METH_O, NULL},
    {"get_tlbc_id", get_tlbc_id, METH_O, NULL},
    {"get_long_lived_total", get_long_lived_total, METH_NOARGS},
#endif
#ifdef _Py_TIER2
    {"uop_symbols_test", _Py_uop_symbols_test, METH_NOARGS},
#endif
    GH_119213_GETARGS_METHODDEF
    {"get_static_builtin_types", get_static_builtin_types, METH_NOARGS},
    {"identify_type_slot_wrappers", identify_type_slot_wrappers, METH_NOARGS},
    {"has_deferred_refcount", has_deferred_refcount, METH_O},
    {"get_tracked_heap_size", get_tracked_heap_size, METH_NOARGS},
    {"is_static_immortal", is_static_immortal, METH_O},
    {"incref_decref_delayed", incref_decref_delayed, METH_O},
    GET_NEXT_DICT_KEYS_VERSION_METHODDEF
#ifdef __EMSCRIPTEN__
    {"emscripten_set_up_async_input_device", emscripten_set_up_async_input_device, METH_NOARGS},
#endif
    {"simple_pending_call", simple_pending_call, METH_O},
    {"set_vectorcall_nop", set_vectorcall_nop, METH_O},
    {"soac_prepare_annotation_replay_fixture", soac_prepare_annotation_replay_fixture, METH_O},
    {"soac_install_annotation_replay_resolver", soac_install_annotation_replay_resolver, METH_O},
    {"module_get_gc_hooks", module_get_gc_hooks, METH_O},
    {"test_threadstate_set_stack_protection",
     test_threadstate_set_stack_protection, METH_NOARGS},
    {NULL, NULL} /* sentinel */
};


/* initialization function */

static int
module_exec(PyObject *module)
{
    if (_PyTestInternalCapi_Init_Lock(module) < 0) {
        return 1;
    }
    if (_PyTestInternalCapi_Init_PyTime(module) < 0) {
        return 1;
    }
    if (_PyTestInternalCapi_Init_Set(module) < 0) {
        return 1;
    }
    if (_PyTestInternalCapi_Init_Complex(module) < 0) {
        return 1;
    }
    if (_PyTestInternalCapi_Init_CriticalSection(module) < 0) {
        return 1;
    }

    Py_ssize_t sizeof_gc_head = 0;
#ifndef Py_GIL_DISABLED
    sizeof_gc_head = sizeof(PyGC_Head);
#endif

    if (PyModule_Add(module, "SIZEOF_PYGC_HEAD",
                        PyLong_FromSsize_t(sizeof_gc_head)) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SIZEOF_MANAGED_PRE_HEADER",
                        PyLong_FromSsize_t(2 * sizeof(PyObject*))) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SIZEOF_PYOBJECT",
                        PyLong_FromSsize_t(sizeof(PyObject))) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SIZEOF_TIME_T",
                        PyLong_FromSsize_t(sizeof(time_t))) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "TIER2_THRESHOLD",
        // + 1 more due to one loop spent on tracing.
                        PyLong_FromLong(JUMP_BACKWARD_INITIAL_VALUE + 2)) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SPECIALIZATION_THRESHOLD",
                        PyLong_FromLong(ADAPTIVE_WARMUP_VALUE + 1)) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SPECIALIZATION_COOLDOWN",
                        PyLong_FromLong(ADAPTIVE_COOLDOWN_VALUE + 1)) < 0) {
        return 1;
    }

    if (PyModule_Add(module, "SHARED_KEYS_MAX_SIZE",
                        PyLong_FromLong(SHARED_KEYS_MAX_SIZE)) < 0) {
        return 1;
    }

    if (PyModule_AddIntMacro(module, _PY_NSMALLPOSINTS) < 0) {
        return 1;
    }

    return 0;
}

PyABIInfo_VAR(abi_info);

static struct PyModuleDef_Slot module_slots[] = {
    {Py_mod_abi, &abi_info},
    {Py_mod_exec, module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static int
module_traverse(PyObject *module, visitproc visit, void *arg)
{
    module_state *state = get_module_state(module);
    assert(state != NULL);
    traverse_module_state(state, visit, arg);
    return 0;
}

static int
module_clear(PyObject *module)
{
    module_state *state = get_module_state(module);
    assert(state != NULL);
    (void)clear_module_state(state);
    return 0;
}

static void
module_free(void *module)
{
    module_state *state = get_module_state(module);
    assert(state != NULL);
    (void)clear_module_state(state);
}

static struct PyModuleDef _testcapimodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = MODULE_NAME,
    .m_doc = NULL,
    .m_size = sizeof(module_state),
    .m_methods = module_functions,
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
    .m_free = module_free,
};


PyMODINIT_FUNC
PyInit__testinternalcapi(void)
{
    return PyModuleDef_Init(&_testcapimodule);
}
