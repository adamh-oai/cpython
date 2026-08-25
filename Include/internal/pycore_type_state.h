#ifndef Py_INTERNAL_TYPE_STATE_H
#define Py_INTERNAL_TYPE_STATE_H
#ifdef __cplusplus
extern "C" {
#endif
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_dict.h"

#if !defined(Py_GIL_DISABLED) && SIZEOF_VOID_P == 8 && !PY_BIG_ENDIAN
#  define _Py_TYPE_STATE_SUPPORTED 1
#else
#  define _Py_TYPE_STATE_SUPPORTED 0
#endif

#define _Py_DICT_TYPE_STATE_MUTATING (UINT64_C(1) << 15)
#define _Py_DICT_TYPE_STATE_TERMINAL (UINT64_C(1) << 16)
#define _Py_DICT_TYPE_STATE_INSTALLING (UINT64_C(1) << 17)

typedef struct SoacSplitClearFrame SoacSplitClearFrame;

/* Existing dictionary rule representation. Direct states publish its rule
 * fields once; their mutable attachment flags live on the actual dictionary.
 * Only legacy policies use the remaining local clear/installation fields. */
typedef struct {
    PyObject *owner;
    PyDict_SoacPolicyCallback validate;
    unsigned int flags;
    unsigned char dictionary_mode;
    unsigned char direct;
    unsigned char installing;
    unsigned char sealed;
    unsigned char terminal;
    unsigned char mutating;
    PyDictKeysObject *baseline_keys;
    uint8_t baseline_capacity;
    unsigned char baseline_embedded;
    unsigned char baseline_promoted;
    unsigned char instance_bound;
    SoacSplitClearFrame *split_clear;
    uint32_t split_clear_pending;
} SoacDictPolicy;

typedef struct {
    Py_ssize_t offset;
    PyObject *canonical_name;
    PyObject *owner;
    PyTypeStateFieldCheckV1 validate;
} _PyTypeStateSlot;

enum {
    _Py_TYPE_STATE_INSTANCE = 1,
    _Py_TYPE_STATE_DICTIONARY = 2,
};

struct _PyTypeState {
    PyObject_HEAD
    PyInterpreterState *interpreter;
    unsigned char kind;
    unsigned char terminal;
    /* Comparison-only cache receipt. An instance already owns its actual
     * type; dictionary projections carry neither this address nor its slots. */
    PyTypeObject *allocation_type;
    unsigned int type_version;
    /* Only instance states retain native class-liveness metadata. Escaped
     * dictionary projections never acquire this edge. */
    PyObject *class_contracts;
    PyTypeState *dictionary;
    SoacDictPolicy dictionary_policy;
    PyTypeStateFieldCheckV1 validate_inline;
    Py_ssize_t slot_count;
    _PyTypeStateSlot *slots;
};

static inline int
_PyObject_HasTypeStateSlot(PyObject *object)
{
#if _Py_TYPE_STATE_SUPPORTED
    return (object->ob_flags & _Py_HAS_TYPE_STATE_SLOT_FLAG) != 0;
#else
    return 0;
#endif
}

extern PyTypeObject _PyTypeState_Type;
extern int _PyTypeState_CheckLive(PyTypeState *state);
PyAPI_FUNC(PyTypeState **) _PyObject_TypeStateSlot(PyObject *object);
extern int _PyObject_TypeStateTraverse(PyObject *, visitproc, void *);
extern void _PyObject_ClearTypeState(PyObject *);
extern void _PyObject_InitWithTypeState(PyObject *, PyTypeObject *, PyTypeState *);
extern int _PyTypeState_AllocationSize(PyTypeObject *, size_t, size_t *);
extern int _PyTypeState_BindSpec(PyTypeObject *, const PyTypeStateSpecV1 *, PyTypeState *);
extern int _PyTypeState_CheckInstanceContracts(PyTypeState *);
/* Returns an owned state or NULL/no-error for a legacy/ordinary allocation. */
extern PyTypeState *_PyTypeState_ForAllocation(PyTypeObject *);
extern int _PyTypeState_SupportedInstanceType(PyTypeObject *);
extern int _PyTypeState_CheckInlineWrite(PyObject *, PyObject *, PyObject *);
extern int _PyTypeState_CheckMemberAccess(PyObject *, const PyMemberDef *);
extern int _PyTypeState_CheckMemberWrite(PyObject *, Py_ssize_t, PyObject *);

#ifdef __cplusplus
}
#endif
#endif /* Py_INTERNAL_TYPE_STATE_H */
