/* Native opaque-reference primitives, included by the CPython core.
 * Call transactions use the native binding and dispatch producers.
 * A C probe must link these symbols from the matching native library,
 * never compile a substitute implementation into its extension.
 */
#include "Python.h"
#include "pycore_cell.h"
#include "pycore_gc.h"
#include "pycore_stackref.h"
#include "native_reference_v1.h"

#include <assert.h>
#include <string.h>

_Static_assert(sizeof(PySoacRefV1) == sizeof(_PyStackRef),
               "opaque reference transport must preserve the native token");
_Static_assert(_Alignof(PySoacRefV1) == _Alignof(_PyStackRef),
               "opaque reference alignment must match the native token");

static inline void
soac_ref_require_supported(void)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    Py_FatalError("SOAC reference V1 used without a supported native ABI query");
#else
    assert(PyGILState_Check());
#endif
}

/* These are transport copies, not DUPs. The caller owns exactly the same one
 * close obligation before and after a consuming operation. Inspection passes
 * a temporary native view and must not close that temporary independently.
 * V1 carries boxed objects or Empty, not ERROR, wrapped pointers or tagged ints.
 */
static inline _PyStackRef
soac_ref_native(PySoacRefV1 value)
{
    soac_ref_require_supported();
    _PyStackRef native;
    memcpy(&native, &value, sizeof(native));
    assert(!PyStackRef_IsError(native));
    assert(!PyStackRef_IsMalformed(native));
    assert(!PyStackRef_IsTaggedInt(native));
    assert(PyStackRef_IsNull(native) || PyStackRef_IsValid(native));
    return native;
}

static inline PySoacRefV1
soac_ref_public(_PyStackRef native)
{
    soac_ref_require_supported();
    PySoacRefV1 value;
    memcpy(&value, &native, sizeof(value));
    return value;
}

int
PySoacRef_GetAbiV1(PySoacReferenceAbiV1 *out, size_t out_size)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)out;
    (void)out_size;
    PyErr_SetString(PyExc_NotImplementedError,
                    "SOAC reference V1 requires the selected 64-bit GIL build");
    return -1;
#else
    soac_ref_require_supported();
    if (out == NULL || out_size != sizeof(*out) ||
        (uintptr_t)out % _Alignof(PySoacReferenceAbiV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid SOAC reference V1 output size or alignment");
        return -1;
    }
    uint32_t capabilities = Py_SOAC_REFERENCE_GIL |
                            Py_SOAC_REFERENCE_NATIVE_GC_VISIT;
#ifdef Py_STACKREF_DEBUG
    capabilities |= Py_SOAC_REFERENCE_DEBUG_HANDLES;
#endif
    *out = (PySoacReferenceAbiV1) {
        .abi_version = Py_SOAC_REFERENCE_ABI_V1,
        .capabilities = capabilities,
        .reference_size = sizeof(PySoacRefV1),
        .reference_alignment = _Alignof(PySoacRefV1),
        .empty = soac_ref_public(PyStackRef_NULL),
    };
    return 0;
#endif
}

PySoacRefV1
PySoacRef_EmptyV1(void)
{
    return soac_ref_public(PyStackRef_NULL);
}

void
PySoacRef_InitSlotsV1(PySoacRefV1 *slots, Py_ssize_t count)
{
    soac_ref_require_supported();
    assert(count >= 0 && (count == 0 || slots != NULL));
    for (Py_ssize_t i = 0; i < count; i++) {
        slots[i] = PySoacRef_EmptyV1();
    }
}

int
PySoacRef_IsEmptyV1(PySoacRefV1 value)
{
    return PyStackRef_IsNull(soac_ref_native(value));
}

int
PySoacRef_IsHeapSafeV1(PySoacRefV1 value)
{
    return PyStackRef_IsHeapSafe(soac_ref_native(value));
}

int
PySoacRef_IsV1(PySoacRefV1 left, PySoacRefV1 right)
{
    return PyStackRef_Is(soac_ref_native(left), soac_ref_native(right));
}

PyObject *
PySoacRef_ObjectViewV1(PySoacRefV1 value)
{
    _PyStackRef native = soac_ref_native(value);
    if (PyStackRef_IsNull(native)) {
        return NULL;
    }
    return PyStackRef_AsPyObjectBorrow(native);
}

PySoacRefV1
PySoacRef_DupV1(PySoacRefV1 value)
{
    return soac_ref_public(PyStackRef_DUP(soac_ref_native(value)));
}

PySoacRefV1
PySoacRef_BorrowV1(PySoacRefV1 value)
{
    return soac_ref_public(PyStackRef_Borrow(soac_ref_native(value)));
}

PySoacRefV1
PySoacRef_FromOwnedV1(PyObject *value)
{
    soac_ref_require_supported();
    return soac_ref_public(PyStackRef_FromPyObjectSteal(value));
}

PySoacRefV1
PySoacRef_FromBorrowedV1(PyObject *value)
{
    soac_ref_require_supported();
    return soac_ref_public(PyStackRef_FromPyObjectNew(value));
}

PyObject *
PySoacRef_IntoOwnedV1(PySoacRefV1 value)
{
    return PyStackRef_AsPyObjectSteal(soac_ref_native(value));
}

PySoacRefV1
PySoacRef_MakeHeapSafeV1(PySoacRefV1 value)
{
    return soac_ref_public(PyStackRef_MakeHeapSafe(soac_ref_native(value)));
}

PySoacRefV1
PySoacRef_TakeV1(PySoacRefV1 *slot)
{
    assert(slot != NULL);
    (void)soac_ref_native(*slot);
    PySoacRefV1 value = *slot;
    *slot = PySoacRef_EmptyV1();
    return value;
}

PySoacRefV1
PySoacRef_ExchangeV1(PySoacRefV1 *slot, PySoacRefV1 incoming)
{
    assert(slot != NULL);
    (void)soac_ref_native(*slot);
    (void)soac_ref_native(incoming);
    PySoacRefV1 displaced = *slot;
    *slot = incoming;
    return displaced;
}

void
PySoacRef_CloseV1(PySoacRefV1 value)
{
    PyStackRef_XCLOSE(soac_ref_native(value));
}

int
PySoacRef_VisitOwnerV1(PySoacRefV1 value, visitproc visit, void *arg)
{
    assert(visit != NULL);
    _PyStackRef native = soac_ref_native(value);
    if (PyStackRef_IsNull(native)) {
        return 0;
    }
    /* The selected visitor reads this token; it never mutates its storage.
     * In particular, do not replace it with an unconditional Py_VISIT or skip
     * every borrowed token. Only the native subtract visitor omits that edge.
     */
    return _PyGC_VisitStackRef(&native, visit, arg);
}

PySoacRefV1
PySoacRef_CellGetV1(PyObject *cell)
{
    soac_ref_require_supported();
    assert(cell != NULL && PyCell_Check(cell));
    return soac_ref_public(_PyCell_GetStackRef((PyCellObject *)cell));
}

int
PySoacRef_MakeCellV1(PySoacRefV1 *slot)
{
    assert(slot != NULL);
    PyObject *initial = PySoacRef_ObjectViewV1(*slot);
    PyObject *cell = PyCell_New(initial);
    if (cell == NULL) {
        return -1;
    }
    PySoacRefV1 displaced = PySoacRef_ExchangeV1(
        slot, PySoacRef_FromOwnedV1(cell));
    PySoacRef_CloseV1(displaced);
    return 0;
}
