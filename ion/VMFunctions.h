/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsion_vm_functions_h__
#define jsion_vm_functions_h__

#include "jspubtd.h"

namespace js {
namespace ion {

enum DataType {
    Type_Void,
    Type_Bool,
    Type_Int32,
    Type_Object,
    Type_Value,
    Type_Handle
};

// Contains information about a virtual machine function that can be called
// from JIT code. Functions described in this manner must conform to a simple
// protocol: the return type must have a special "failure" value (for example,
// false for bool, or NULL for Objects). If the function is designed to return
// a value that does not meet this requirement - such as object-or-NULL, or an
// integer, an optional, final outParam can be specified. In this case, the
// return type must be boolean to indicate failure.
//
// All functions described by VMFunction take a JSContext * as a first
// argument, and are treated as re-entrant into the VM and therefore fallible.
struct VMFunction
{
    // Address of the C function.
    void *wrapped;

    // Number of arguments expected, excluding JSContext * as an implicit
    // first argument and an outparam as a possible implicit final argument.
    uint32 explicitArgs;

    enum ArgProperties {
        WordByValue = 0,
        DoubleByValue = 1,
        WordByRef = 2,
        DoubleByRef = 3,
        // BitMask version.
        Word = 0,
        Double = 1,
        ByRef = 2
    };

    // Contains properties about the first 16 arguments.
    uint32 argumentProperties;

    // The outparam may be any Type_*, and must be the final argument to the
    // function, if not Void. outParam != Void implies that the return type
    // has a boolean failure mode.
    DataType outParam;

    // Type returned by the C function and used by the VMFunction wrapper to
    // check for failures of the C function.  Valid failure/return types are
    // boolean and object pointers which are asserted inside the VMFunction
    // constructor. If the C function use an outparam (!= Type_Void), then
    // the only valid failure/return type is boolean -- object pointers are
    // pointless because the wrapper will only use it to compare it against
    // NULL before discarding its value.
    DataType returnType;

    uint32 argc() const {
        // JSContext * + args + (OutParam? *)
        return 1 + explicitArgc() + ((outParam == Type_Void) ? 0 : 1);
    }

    DataType failType() const {
        return returnType;
    }

    ArgProperties argProperties(uint32 explicitArg) const {
        return ArgProperties((argumentProperties >> (2 * explicitArg)) & 3);
    }

    // Return the stack size consumed by explicit arguments.
    size_t explicitStackSlots() const {
        size_t stackSlots = explicitArgs;

        // Fetch all double-word flags of explicit arguments.
        uint32 n =
            ((1 << (explicitArgs * 2)) - 1) // = Explicit argument mask.
            & 0x55555555                    // = Mask double-size args.
            & argumentProperties;

        // Add the number of double-word flags. (expect a few loop
        // iteration)
        while (n) {
            stackSlots++;
            n &= n - 1;
        }
        return stackSlots;
    }

    // Double-size argument which are passed by value are taking the space
    // of 2 C arguments.  This function is used to compute the number of
    // argument expected by the C function.  This is not the same as
    // explicitStackSlots because reference to stack slots may take one less
    // register in the total count.
    size_t explicitArgc() const {
        size_t stackSlots = explicitArgs;

        // Fetch all explicit arguments.
        uint32 n =
            ((1 << (explicitArgs * 2)) - 1) // = Explicit argument mask.
            & argumentProperties;

        // Filter double-size arguments (0x5 = 0b0101) and remove (& ~)
        // arguments passed by reference (0b1010 >> 1 == 0b0101).
        n = (n & 0x55555555) & ~(n >> 1);

        // Add the number of double-word transfered by value. (expect a few
        // loop iteration)
        while (n) {
            stackSlots++;
            n &= n - 1;
        }
        return stackSlots;
    }

    VMFunction()
      : wrapped(NULL),
        explicitArgs(0),
        argumentProperties(0),
        outParam(Type_Void),
        returnType(Type_Void)
    {
    }

    VMFunction(void *wrapped, uint32 explicitArgs, uint32 argumentProperties, DataType outParam, DataType returnType)
      : wrapped(wrapped),
        explicitArgs(explicitArgs),
        argumentProperties(argumentProperties),
        outParam(outParam),
        returnType(returnType)
    {
        // Check for valid failure/return type.
        JS_ASSERT_IF(outParam != Type_Void, returnType == Type_Bool);
        JS_ASSERT(returnType == Type_Bool || returnType == Type_Object);
    }
};

template <class> struct TypeToDataType { /* Unexpected return type for a VMFunction. */ };
template <> struct TypeToDataType<bool> { static const DataType result = Type_Bool; };
template <> struct TypeToDataType<JSObject *> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<JSString *> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<JSFixedString *> { static const DataType result = Type_Object; };
template <> struct TypeToDataType<HandleObject> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleString> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandlePropertyName> { static const DataType result = Type_Handle; };
template <> struct TypeToDataType<HandleFunction> { static const DataType result = Type_Handle; };

// Convert argument types to properties of the argument known by the jit.
template <class T> struct TypeToArgProperties {
    static const uint32 result =
        (sizeof(T) <= sizeof(void *) ? VMFunction::Word : VMFunction::Double);
};
template <> struct TypeToArgProperties<const Value &> {
    static const uint32 result = TypeToArgProperties<Value>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleObject> {
    static const uint32 result = TypeToArgProperties<JSObject *>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleString> {
    static const uint32 result = TypeToArgProperties<JSString *>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandlePropertyName> {
    static const uint32 result = TypeToArgProperties<PropertyName *>::result | VMFunction::ByRef;
};
template <> struct TypeToArgProperties<HandleFunction> {
    static const uint32 result = TypeToArgProperties<JSFunction *>::result | VMFunction::ByRef;
};

template <class> struct OutParamToDataType { static const DataType result = Type_Void; };
template <> struct OutParamToDataType<Value *> { static const DataType result = Type_Value; };
template <> struct OutParamToDataType<int *> { static const DataType result = Type_Int32; };
template <> struct OutParamToDataType<uint32_t *> { static const DataType result = Type_Int32; };

#define FOR_EACH_ARGS_1(Macro, Sep, Last) Macro(1) Last(1)
#define FOR_EACH_ARGS_2(Macro, Sep, Last) FOR_EACH_ARGS_1(Macro, Sep, Sep) Macro(2) Last(2)
#define FOR_EACH_ARGS_3(Macro, Sep, Last) FOR_EACH_ARGS_2(Macro, Sep, Sep) Macro(3) Last(3)
#define FOR_EACH_ARGS_4(Macro, Sep, Last) FOR_EACH_ARGS_3(Macro, Sep, Sep) Macro(4) Last(4)

#define COMPUTE_INDEX(NbArg) NbArg
#define COMPUTE_OUTPARAM_RESULT(NbArg) OutParamToDataType<A ## NbArg>::result
#define COMPUTE_ARG_PROP(NbArg) (TypeToArgProperties<A ## NbArg>::result << (2 * (NbArg - 1)))
#define SEP_OR(_) |
#define NOTHING(_)

#define FUNCTION_INFO_STRUCT_BODY(ForEachNb)                                            \
    static inline DataType returnType() {                                               \
        return TypeToDataType<R>::result;                                               \
    }                                                                                   \
    static inline DataType outParam() {                                                 \
        return ForEachNb(NOTHING, NOTHING, COMPUTE_OUTPARAM_RESULT);                    \
    }                                                                                   \
    static inline size_t NbArgs() {                                                     \
        return ForEachNb(NOTHING, NOTHING, COMPUTE_INDEX);                              \
    }                                                                                   \
    static inline size_t explicitArgs() {                                               \
        return NbArgs() - (outParam() != Type_Void ? 1 : 0);                            \
    }                                                                                   \
    static inline uint32 argumentProperties() {                                         \
        return ForEachNb(COMPUTE_ARG_PROP, SEP_OR, NOTHING);                            \
    }                                                                                   \
    FunctionInfo(pf fun)                                                                \
        : VMFunction(JS_FUNC_TO_DATA_PTR(void *, fun), explicitArgs(),                  \
                     argumentProperties(), outParam(), returnType())                    \
    { }

template <typename Fun>
struct FunctionInfo {
};

// VMFunction wrapper with no explicit arguments.
template <class R>
struct FunctionInfo<R (*)(JSContext *)> : public VMFunction {
    typedef R (*pf)(JSContext *);

    static inline DataType returnType() {
        return TypeToDataType<R>::result;
    }
    static inline DataType outParam() {
        return Type_Void;
    }
    static inline size_t explicitArgs() {
        return 0;
    }
    static inline uint32 argumentProperties() {
        return 0;
    }
    FunctionInfo(pf fun)
      : VMFunction(JS_FUNC_TO_DATA_PTR(void *, fun), explicitArgs(),
                   argumentProperties(), outParam(), returnType())
    { }
};

// Specialize the class for each number of argument used by VMFunction.
// Keep it verbose unless you find a readable macro for it.
template <class R, class A1>
struct FunctionInfo<R (*)(JSContext *, A1)> : public VMFunction {
    typedef R (*pf)(JSContext *, A1);
    FUNCTION_INFO_STRUCT_BODY(FOR_EACH_ARGS_1);
};

template <class R, class A1, class A2>
struct FunctionInfo<R (*)(JSContext *, A1, A2)> : public VMFunction {
    typedef R (*pf)(JSContext *, A1, A2);
    FUNCTION_INFO_STRUCT_BODY(FOR_EACH_ARGS_2);
};

template <class R, class A1, class A2, class A3>
struct FunctionInfo<R (*)(JSContext *, A1, A2, A3)> : public VMFunction {
    typedef R (*pf)(JSContext *, A1, A2, A3);
    FUNCTION_INFO_STRUCT_BODY(FOR_EACH_ARGS_3);
};

template <class R, class A1, class A2, class A3, class A4>
struct FunctionInfo<R (*)(JSContext *, A1, A2, A3, A4)> : public VMFunction {
    typedef R (*pf)(JSContext *, A1, A2, A3, A4);
    FUNCTION_INFO_STRUCT_BODY(FOR_EACH_ARGS_4);
};

#undef FUNCTION_INFO_STRUCT_BODY

#undef FOR_EACH_ARGS_4
#undef FOR_EACH_ARGS_3
#undef FOR_EACH_ARGS_2
#undef FOR_EACH_ARGS_1

#undef COMPUTE_INDEX
#undef COMPUTE_OUTPARAM_RESULT
#undef COMPUTE_ARG_PROP
#undef SEP_OR
#undef NOTHING

class AutoDetectInvalidation
{
    JSContext *cx_;
    JSScript *script_;
    IonScript *ion_;
    Value *rval_;

  public:
    AutoDetectInvalidation(JSContext *cx, Value *rval, JSScript *script = NULL)
      : cx_(cx),
        script_(script ? script : GetTopIonJSScript(cx)),
        ion_(script_->ionScript()),
        rval_(rval)
    { }

    ~AutoDetectInvalidation() {
        if (script_->ion != ion_)
            cx_->runtime->setIonReturnOverride(*rval_);
    }
};

bool InvokeFunction(JSContext *cx, JSFunction *fun, uint32 argc, Value *argv, Value *rval);
bool InvokeConstructorFunction(JSContext *cx, JSFunction *fun, uint32 argc, Value *argv, Value *rval);
bool ReportOverRecursed(JSContext *cx);

bool DefVarOrConst(JSContext *cx, HandlePropertyName dn, unsigned attrs, HandleObject scopeChain);
bool InitProp(JSContext *cx, HandleObject obj, HandlePropertyName name, const Value &value);

template<bool Equal>
bool LooselyEqual(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);

template<bool Equal>
bool StrictlyEqual(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);

bool LessThan(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);
bool LessThanOrEqual(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);
bool GreaterThan(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);
bool GreaterThanOrEqual(JSContext *cx, const Value &lhs, const Value &rhs, JSBool *res);

bool ValueToBooleanComplement(JSContext *cx, const Value &input, JSBool *output);

bool IteratorMore(JSContext *cx, HandleObject obj, JSBool *res);

// Allocation functions for JSOP_NEWARRAY and JSOP_NEWOBJECT
JSObject *NewInitArray(JSContext *cx, uint32_t count, types::TypeObject *type);
JSObject *NewInitObject(JSContext *cx, HandleObject baseObj, types::TypeObject *type);

bool ArrayPopDense(JSContext *cx, JSObject *obj, Value *rval);
bool ArrayPushDense(JSContext *cx, JSObject *obj, const Value &v, uint32_t *length);
bool ArrayShiftDense(JSContext *cx, JSObject *obj, Value *rval);

} // namespace ion
} // namespace js

#endif // jsion_vm_functions_h_

