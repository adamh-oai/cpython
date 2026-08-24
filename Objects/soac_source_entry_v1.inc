/* Function-owned source-entry registration and invalidation.
 * This C-only record owns NO Python references. Trusted
 * registrar precondition: the actual catalogue original, CallableSourceRole,
 * all native parameters and executable token-body/read plan have already been
 * checked together. C does not infer that proof from names, flags or metadata.
 */

struct _PySoacSourceEntryRecordV1 {
    PyFunctionObject *function;       /* containing function, borrowed */
    PyInterpreterState *interpreter;
    PySoacSourceEntrySpecV1 spec;      /* all Python operands borrowed */
};

void
_PySoacSourceEntry_InvalidateV1(PyFunctionObject *function)
{
    struct _PySoacSourceEntryRecordV1 *record = function->func_soac_source_entry;
    function->func_soac_source_entry = NULL;
    if (record != NULL) {
        PyMem_Free(record);
    }
}

static int
soac_source_entry_matches(PyFunctionObject *function,
                          const PySoacSourceEntrySpecV1 *spec)
{
    /* Check borrowed identities against their live owning fields before
     * following code. Code's verified-source stamp proves original native
     * provenance, not Rust-body correspondence or execution authority. */
    if (function->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        function->func_soac_strict_owner == NULL ||
        function->func_soac_strict_owner != spec->expected_owner ||
        function->func_soac_metadata == NULL ||
        function->func_soac_metadata != spec->expected_metadata ||
        function->func_soac_function_id == 0 ||
        function->func_soac_function_id != spec->expected_function_id ||
        function->func_soac_strict_id != spec->expected_strict_id ||
        function->func_code != (PyObject *)spec->expected_code ||
        function->vectorcall == NULL ||
        function->vectorcall == _PyFunction_Vectorcall ||
        function->vectorcall != spec->expected_vectorcall ||
        function->func_globals == NULL || function->func_builtins == NULL) {
        return 0;
    }
    PyCodeObject *code = (PyCodeObject *)function->func_code;
    return PyCode_Check(code) && code->_co_soac_strict_source_id != 0;
}

int
_PySoacSourceEntry_CopyV1(PyFunctionObject *function,
                         PySoacSourceEntrySpecV1 *out)
{
    assert(PyFunction_Check(function) && out != NULL);
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    return 0;
#else
    assert(PyGILState_Check());
    struct _PySoacSourceEntryRecordV1 *record = function->func_soac_source_entry;
    if (record == NULL) {
        return 0;
    }
    if (record->function != function ||
        record->interpreter != _PyInterpreterState_GET() ||
        !soac_source_entry_matches(function, &record->spec)) {
        _PySoacSourceEntry_InvalidateV1(function);
        return 0;
    }
    *out = record->spec;
    return 1;
#endif
}

int
PyFunction_SetSoacSourceEntryV1(PyObject *object,
                               const PySoacSourceEntrySpecV1 *spec,
                               size_t spec_size)
{
    if (object == NULL || !PyFunction_Check(object) || spec == NULL ||
        spec_size != sizeof(*spec) ||
        (uintptr_t)spec % _Alignof(PySoacSourceEntrySpecV1) != 0) {
        PyErr_SetString(PyExc_ValueError, "invalid SOAC source-entry registration shape");
        return -1;
    }
    PySoacReferenceAbiV1 abi;
    if (PySoacRef_GetAbiV1(&abi, sizeof(abi)) < 0) {
        return -1;
    }
    /* Copy before allocation/replacement: input may itself be a caller-owned
     * snapshot. Never keep a pointer to a caller's spec or use struct padding
     * as an identity check. */
    PySoacSourceEntrySpecV1 captured = *spec;
    if (captured.abi_version != Py_SOAC_SOURCE_ENTRY_ABI_V1 ||
        captured.reserved != 0 || captured.pin == NULL ||
        captured.release == NULL || captured.execute == NULL) {
        PyErr_SetString(PyExc_ValueError, "unsupported SOAC source-entry spec");
        return -1;
    }
    PyFunctionObject *function = (PyFunctionObject *)object;
    if (!soac_source_entry_matches(function, &captured)) {
        func_soac_runtime_error("SOAC source-entry identities or original code do not match");
        return -1;
    }
    struct _PySoacSourceEntryRecordV1 *record = PyMem_Malloc(sizeof(*record));
    if (record == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    if (!soac_source_entry_matches(function, &captured)) {
        PyMem_Free(record);
        func_soac_runtime_error("SOAC source-entry identities changed during allocation");
        return -1;
    }
    *record = (struct _PySoacSourceEntryRecordV1){
        .function = function,
        .interpreter = _PyInterpreterState_GET(),
        .spec = captured,
    };
    _PySoacSourceEntry_InvalidateV1(function);
    function->func_soac_source_entry = record;
    return 0;
}

int
PyFunction_ClearSoacSourceEntryV1(PyObject *object)
{
    if (object == NULL || !PyFunction_Check(object)) {
        PyErr_SetString(PyExc_TypeError, "SOAC source-entry clearing needs an exact function");
        return -1;
    }
    _PySoacSourceEntry_InvalidateV1((PyFunctionObject *)object);
    return 0;
}
