/* Generated-member construction safety and metadata adoption.
 * Included by soac_dataclass.inc; no argument or return type predicates. */

static int
soac_dataclass_closure_complete(PyFunctionObject *function, PyCodeObject *code)
{
    if ((code->co_nfreevars != 0 && function->func_closure == NULL) ||
        (function->func_closure != NULL &&
         (!PyTuple_CheckExact(function->func_closure) ||
          PyTuple_GET_SIZE(function->func_closure) != code->co_nfreevars))) {
        return soac_dataclass_error("generated function construction is incomplete");
    }
    for (int i = 0; i < code->co_nfreevars; i++) {
        if (!PyCell_Check(PyTuple_GET_ITEM(function->func_closure, i))) {
            return soac_dataclass_error("generated closure has an invalid cell");
        }
    }
    return 0;
}

int
_PySOAC_DataclassCheckFrameConstruction(_PyInterpreterFrame *frame)
{
    PyObject *object = PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
    if (object == NULL || !PyFunction_Check(object)) {
        return 0;
    }
    PyFunctionObject *function = (PyFunctionObject *)object;
    PyObject *owner = function->func_soac_strict_owner;
    if (owner == NULL || !Py_IS_TYPE(owner, &SoacDataclassRecord_Type)) {
        return 0;
    }
    if (soac_dataclass_record(function) == NULL) {
        return -1;
    }
    /* MAKE_FUNCTION exposes CREATE before ordinary closure attributes arrive.
     * This is a structural memory-safety check, not a type predicate or an
     * execution capability. Expired unsealed birth provenance stays ordinary.
     */
    return soac_dataclass_closure_complete(function, _PyFrame_GetCode(frame));
}

static int
soac_dataclass_component_valid(SoacDataclassInvocation *invocation,
                                PyFunctionObject *method, SoacDataclassRecord *method_record,
                                PyFunctionObject *component, SoacDataclassRecord *component_record,
                                unsigned int kind, Py_ssize_t index,
                                SoacDataclassRecordPhase expected_phase)
{
    unsigned int expected_role = kind == Py_SOAC_DATACLASS_COMPONENT_ANNOTATE
        ? Py_SOAC_DATACLASS_ANNOTATION_PROVIDER : Py_SOAC_DATACLASS_REPR_IMPLEMENTATION;
    if (invocation->phase != SOAC_DC_APPLY_ACTIVE || invocation->owner == NULL ||
        method == component || method_record == NULL || component_record == NULL ||
        method_record->invocation != invocation || component_record->invocation != invocation ||
        method_record->function != method || component_record->function != component ||
        method->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        component->func_soac_strict_owner_state != FUNC_SOAC_OWNER_ATTACHED ||
        method->func_soac_strict_owner != (PyObject *)method_record ||
        component->func_soac_strict_owner != (PyObject *)component_record ||
        method_record->phase != SOAC_DC_RECORD_FRESH ||
        component_record->phase != expected_phase ||
        method_record->role != Py_SOAC_DATACLASS_MEMBER || component_record->role != expected_role ||
        !soac_dataclass_original_code(method_record, method) ||
        !soac_dataclass_original_code(component_record, component) ||
        (kind != Py_SOAC_DATACLASS_COMPONENT_ANNOTATE && kind != Py_SOAC_DATACLASS_COMPONENT_REPR)) {
        return soac_dataclass_fail(invocation, "generated component roles are not authentic");
    }
    if (kind == Py_SOAC_DATACLASS_COMPONENT_ANNOTATE) {
        if (index != -1 || method->func_annotate != (PyObject *)component) {
            return soac_dataclass_fail(invocation, "generated annotation component is not attached");
        }
    }
    else if (method->func_closure == NULL || !PyTuple_CheckExact(method->func_closure) ||
             index < 0 || index >= PyTuple_GET_SIZE(method->func_closure) ||
             !PyCell_Check(PyTuple_GET_ITEM(method->func_closure, index)) ||
             PyCell_GET(PyTuple_GET_ITEM(method->func_closure, index)) != (PyObject *)component) {
        return soac_dataclass_fail(invocation, "generated repr component is not the exact closure value");
    }
    int status = _PyInterpreterState_GET()->soac.dataclass_callbacks.validate_component(
        invocation->owner, (PyObject *)method, (PyObject *)component, kind, index);
    if (status != 0 || PyErr_Occurred() || invocation->phase != SOAC_DC_APPLY_ACTIVE) {
        return soac_dataclass_fail(invocation, "generated component policy did not authenticate");
    }
    return 0;
}

int
PyFunction_AdoptSoacDataclassComponent(PyObject *object, PyObject *method_object,
                                      PyObject *component_object, unsigned int kind,
                                      Py_ssize_t closure_index)
{
    SoacDataclassInvocation *invocation = soac_dataclass_invocation(object);
    if (invocation == NULL) {
        return -1;
    }
    if (method_object == NULL || !PyFunction_Check(method_object) ||
        component_object == NULL || !PyFunction_Check(component_object)) {
        return soac_dataclass_fail(invocation, "generated component adoption needs exact functions");
    }
    PyFunctionObject *method = (PyFunctionObject *)method_object;
    PyFunctionObject *component = (PyFunctionObject *)component_object;
    SoacDataclassRecord *method_record = soac_dataclass_record(method);
    SoacDataclassRecord *component_record = soac_dataclass_record(component);
    if (soac_dataclass_component_valid(invocation, method, method_record,
                                      component, component_record, kind, closure_index,
                                      SOAC_DC_RECORD_FRESH) < 0) {
        return -1;
    }
    component_record->phase = SOAC_DC_RECORD_CONSUMING;
    if (PyFunction_SealSoacStrict(component_object, component_record->identity) < 0 ||
        soac_dataclass_component_valid(invocation, method, method_record,
                                      component, component_record, kind, closure_index,
                                      SOAC_DC_RECORD_CONSUMING) < 0) {
        return soac_dataclass_fail(invocation, "generated component changed during adoption");
    }
    component_record->phase = SOAC_DC_RECORD_COMPONENT;
    return 0;
}
