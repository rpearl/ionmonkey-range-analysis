/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79:
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
 *   David Anderson <dvander@alliedmods.net>
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

#ifndef jsion_macro_assembler_x64_h__
#define jsion_macro_assembler_x64_h__

#include "ion/shared/MacroAssembler-x86-shared.h"
#include "ion/MoveResolver.h"
#include "ion/IonFrames.h"
#include "jsnum.h"

namespace js {
namespace ion {

struct ImmShiftedTag : public ImmWord
{
    ImmShiftedTag(JSValueShiftedTag shtag)
      : ImmWord((uintptr_t)shtag)
    { }

    ImmShiftedTag(JSValueType type)
      : ImmWord(uintptr_t(JSValueShiftedTag(JSVAL_TYPE_TO_SHIFTED_TAG(type))))
    { }
};

struct ImmTag : public Imm32
{
    ImmTag(JSValueTag tag)
      : Imm32(tag)
    { }
};

class MacroAssemblerX64 : public MacroAssemblerX86Shared
{
    // Number of bytes the stack is adjusted inside a call to C. Calls to C may
    // not be nested.
    bool inCall_;
    uint32 args_;
    uint32 passedIntArgs_;
    uint32 passedFloatArgs_;
    uint32 stackForCall_;
    bool dynamicAlignment_;
    bool enoughMemory_;

    void setupABICall(uint32 arg);

  protected:
    MoveResolver moveResolver_;

  public:
    using MacroAssemblerX86Shared::call;
    using MacroAssemblerX86Shared::Push;

    enum Result {
        GENERAL,
        DOUBLE
    };

    typedef MoveResolver::MoveOperand MoveOperand;
    typedef MoveResolver::Move Move;

    MacroAssemblerX64()
      : inCall_(false),
        enoughMemory_(true)
    {
    }

    bool oom() const {
        return MacroAssemblerX86Shared::oom() || !enoughMemory_;
    }

    /////////////////////////////////////////////////////////////////
    // X64 helpers.
    /////////////////////////////////////////////////////////////////
    void call(ImmWord target) {
        movq(target, rax);
        call(rax);
    }

    /////////////////////////////////////////////////////////////////
    // X86/X64-common interface.
    /////////////////////////////////////////////////////////////////
    void storeValue(ValueOperand val, Operand dest) {
        movq(val.valueReg(), dest);
    }
    void storeValue(ValueOperand val, const Address &dest) {
        storeValue(val, Operand(dest));
    }
    template <typename T>
    void storeValue(JSValueType type, Register reg, const T &dest) {
        boxValue((JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type),
                 Operand(reg), ScratchReg);
        movq(ScratchReg, Operand(dest));
    }
    template <typename T>
    void storeValue(const Value &val, const T &dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        movq(ImmWord(jv.asBits), ScratchReg);
        writeDataRelocation(val);
        movq(ScratchReg, Operand(dest));
    }
    void storeValue(ValueOperand val, BaseIndex dest) {
        storeValue(val, Operand(dest));
    }
    void loadValue(Operand src, ValueOperand val) {
        movq(src, val.valueReg());
    }
    void loadValue(Address src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void loadValue(const BaseIndex &src, ValueOperand val) {
        loadValue(Operand(src), val);
    }
    void tagValue(JSValueType type, Register payload, ValueOperand dest) {
        JS_ASSERT(dest.valueReg() != ScratchReg);
        if (payload != dest.valueReg())
            movq(payload, dest.valueReg());
        movq(ImmShiftedTag(type), ScratchReg);
        orq(Operand(ScratchReg), dest.valueReg());
    }
    void pushValue(ValueOperand val) {
        push(val.valueReg());
    }
    void Push(const ValueOperand &val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }
    void popValue(ValueOperand val) {
        pop(val.valueReg());
    }
    void pushValue(const Value &val) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        push(ImmWord(jv.asBits));
    }
    void pushValue(JSValueType type, Register reg) {
        boxValue((JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type),
                 Operand(reg), ScratchReg);
        push(ScratchReg);
    }

    void movePtr(Operand op, const Register &dest) {
        movq(op, dest);
    }
    void moveValue(const Value &val, const Register &dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        movq(ImmWord(jv.asPtr), dest);
        writeDataRelocation(val);
    }
    void moveValue(const Value &src, const ValueOperand &dest) {
        moveValue(src, dest.valueReg());
    }
    void boxValue(JSValueShiftedTag tag, const Operand &src, const Register &dest) {
        movq(ImmShiftedTag(tag), dest);
        orq(src, dest);
    }

    Condition testUndefined(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_UNDEFINED));
        return cond;
    }
    Condition testInt32(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_INT32));
        return cond;
    }
    Condition testBoolean(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_BOOLEAN));
        return cond;
    }
    Condition testNull(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_NULL));
        return cond;
    }
    Condition testString(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_STRING));
        return cond;
    }
    Condition testObject(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_OBJECT));
        return cond;
    }
    Condition testDouble(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, Imm32(JSVAL_TAG_MAX_DOUBLE));
        return cond == Equal ? BelowOrEqual : Above;
    }
    Condition testNumber(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, Imm32(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET));
        return cond == Equal ? BelowOrEqual : Above;
    }
    Condition testGCThing(Condition cond, Register tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, Imm32(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
        return cond == Equal ? AboveOrEqual : Below;
    }

    Condition testMagic(Condition cond, const Register &tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_TAG_MAGIC));
        return cond;
    }
    Condition testError(Condition cond, const Register &tag) {
        return testMagic(cond, tag);
    }
    Condition testPrimitive(Condition cond, const Register &tag) {
        JS_ASSERT(cond == Equal || cond == NotEqual);
        cmpl(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET));
        return cond == Equal ? Below : AboveOrEqual;
    }

    Condition testUndefined(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testUndefined(cond, ScratchReg);
    }
    Condition testInt32(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testInt32(cond, ScratchReg);
    }
    Condition testBoolean(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testBoolean(cond, ScratchReg);
    }
    Condition testDouble(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testDouble(cond, ScratchReg);
    }
    Condition testNull(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testNull(cond, ScratchReg);
    }
    Condition testString(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testString(cond, ScratchReg);
    }
    Condition testObject(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testObject(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const Address &src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }
    Condition testGCThing(Condition cond, const BaseIndex &src) {
        splitTag(src, ScratchReg);
        return testGCThing(cond, ScratchReg);
    }
    Condition testMagic(Condition cond, const Address &src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }
    Condition testMagic(Condition cond, const BaseIndex &src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }
    Condition testPrimitive(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testPrimitive(cond, ScratchReg);
    }

    void cmpPtr(const Register &lhs, const ImmWord rhs) {
        JS_ASSERT(lhs != ScratchReg);
        movq(rhs, ScratchReg);
        cmpq(lhs, ScratchReg);
    }
    void cmpPtr(const Register &lhs, const ImmGCPtr rhs) {
        JS_ASSERT(lhs != ScratchReg);
        movq(rhs, ScratchReg);
        cmpq(lhs, ScratchReg);
    }
    void cmpPtr(const Operand &lhs, const ImmGCPtr rhs) {
        movq(rhs, ScratchReg);
        cmpq(lhs, ScratchReg);
    }
    void cmpPtr(const Operand &lhs, const ImmWord rhs) {
        movq(rhs, ScratchReg);
        cmpq(lhs, ScratchReg);
    }
    void cmpPtr(const Address &lhs, const ImmGCPtr rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Operand &lhs, const Register &rhs) {
        cmpq(lhs, rhs);
    }
    void cmpPtr(const Address &lhs, const Register &rhs) {
        cmpPtr(Operand(lhs), rhs);
    }
    void cmpPtr(const Register &lhs, const Register &rhs) {
        return cmpq(lhs, rhs);
    }
    void testPtr(const Register &lhs, const Register &rhs) {
        testq(lhs, rhs);
    }

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
    void reserveStack(uint32 amount) {
        if (amount)
            subq(Imm32(amount), StackPointer);
        framePushed_ += amount;
    }
    void freeStack(uint32 amount) {
        JS_ASSERT(amount <= framePushed_);
        if (amount)
            addq(Imm32(amount), StackPointer);
        framePushed_ -= amount;
    }

    void addPtr(const Register &src, const Register &dest) {
        addq(src, dest);
    }
    void addPtr(Imm32 imm, const Register &dest) {
        addq(imm, dest);
    }
    void addPtr(Imm32 imm, const Address &dest) {
        addq(imm, Operand(dest));
    }
    void subPtr(Imm32 imm, const Register &dest) {
        subq(imm, dest);
    }

    // Specialization for AbsoluteAddress.
    void branchPtr(Condition cond, const AbsoluteAddress &addr, const Register &ptr, Label *label) {
        JS_ASSERT(ptr != ScratchReg);
        movq(ImmWord(addr.addr), ScratchReg);
        branchPtr(cond, Operand(ScratchReg, 0x0), ptr, label);
    }

    template <typename T, typename S>
    void branchPtr(Condition cond, T lhs, S ptr, Label *label) {
        cmpPtr(Operand(lhs), ptr);
        j(cond, label);
    }

    CodeOffsetJump jumpWithPatch(Label *label) {
        JmpSrc src = jmpSrc(label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }
    template <typename S, typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, S lhs, T ptr, Label *label) {
        cmpPtr(lhs, ptr);
        JmpSrc src = jSrc(cond, label);
        return CodeOffsetJump(size(), addPatchableJump(src, Relocation::HARDCODED));
    }
    void branchPtr(Condition cond, Register lhs, Register rhs, Label *label) {
        cmpPtr(lhs, rhs);
        j(cond, label);
    }
    void branchTestPtr(Condition cond, Register lhs, Register rhs, Label *label) {
        testq(lhs, rhs);
        j(cond, label);
    }

    void movePtr(const Register &src, const Register &dest) {
        movq(src, dest);
    }
    void movePtr(ImmWord imm, Register dest) {
        movq(imm, dest);
    }
    void movePtr(ImmGCPtr imm, Register dest) {
        movq(imm, dest);
    }
    void movePtr(const Address &address, const Register &dest) {
        movq(Operand(address), dest);
    }
    void loadPtr(const AbsoluteAddress &address, Register dest) {
        movq(ImmWord(address.addr), ScratchReg);
        movq(Operand(ScratchReg, 0x0), dest);
    }
    void loadPtr(const Address &address, Register dest) {
        movq(Operand(address), dest);
    }
    void loadPtr(const BaseIndex &src, Register dest) {
        movq(Operand(src), dest);
	}
    void loadPrivate(const Address &src, Register dest) {
        loadPtr(src, dest);
        shlq(Imm32(1), dest);
    }
    void storePtr(ImmWord imm, const Address &address) {
        movq(imm, ScratchReg);
        movq(ScratchReg, Operand(address));
    }
    void storePtr(ImmGCPtr imm, const Address &address) {
        movq(imm, ScratchReg);
        movq(ScratchReg, Operand(address));
    }
    void storePtr(Register src, const Address &address) {
        movq(src, Operand(address));
    }
    void storePtr(const Register &src, const AbsoluteAddress &address) {
        movq(ImmWord(address.addr), ScratchReg);
        movq(src, Operand(ScratchReg, 0x0));
    }
    void rshiftPtr(Imm32 imm, const Register &dest) {
        shrq(imm, dest);
    }
    void lshiftPtr(Imm32 imm, const Register &dest) {
        shlq(imm, dest);
    }

    void splitTag(Register src, Register dest) {
        if (src != dest)
            movq(src, dest);
        shrq(Imm32(JSVAL_TAG_SHIFT), dest);
    }
    void splitTag(const ValueOperand &operand, const Register &dest) {
        splitTag(operand.valueReg(), dest);
    }
    void splitTag(const Address &operand, const Register &dest) {
        movq(Operand(operand), dest);
        shrq(Imm32(JSVAL_TAG_SHIFT), dest);
    }
    void splitTag(const BaseIndex &operand, const Register &dest) {
        movq(Operand(operand), dest);
        shrq(Imm32(JSVAL_TAG_SHIFT), dest);
    }

    // Extracts the tag of a value and places it in ScratchReg.
    Register splitTagForTest(const ValueOperand &value) {
        splitTag(value, ScratchReg);
        return ScratchReg;
    }
    void cmpTag(const ValueOperand &operand, ImmTag tag) {
        Register reg = splitTagForTest(operand);
        cmpl(Operand(reg), tag);
    }

    void branchTestUndefined(Condition cond, Register tag, Label *label) {
        cond = testUndefined(cond, tag);
        j(cond, label);
    }
    void branchTestInt32(Condition cond, Register tag, Label *label) {
        cond = testInt32(cond, tag);
        j(cond, label);
    }
    void branchTestDouble(Condition cond, Register tag, Label *label) {
        cond = testDouble(cond, tag);
        j(cond, label);
    }
    void branchTestBoolean(Condition cond, Register tag, Label *label) {
        cond = testBoolean(cond, tag);
        j(cond, label);
    }
    void branchTestNull(Condition cond, Register tag, Label *label) {
        cond = testNull(cond, tag);
        j(cond, label);
    }
    void branchTestString(Condition cond, Register tag, Label *label) {
        cond = testString(cond, tag);
        j(cond, label);
    }
    void branchTestObject(Condition cond, Register tag, Label *label) {
        cond = testObject(cond, tag);
        j(cond, label);
    }
    void branchTestNumber(Condition cond, Register tag, Label *label) {
        cond = testNumber(cond, tag);
        j(cond, label);
    }

    // Type-testing instructions on x64 will clobber ScratchReg, when used on
    // ValueOperands.
    void branchTestUndefined(Condition cond, const ValueOperand &src, Label *label) {
        cond = testUndefined(cond, src);
        j(cond, label);
    }
    void branchTestInt32(Condition cond, const ValueOperand &src, Label *label) {
        splitTag(src, ScratchReg);
        branchTestInt32(cond, ScratchReg, label);
    }
    void branchTestBoolean(Condition cond, const ValueOperand &src, Label *label) {
        splitTag(src, ScratchReg);
        branchTestBoolean(cond, ScratchReg, label);
    }
    void branchTestDouble(Condition cond, const ValueOperand &src, Label *label) {
        cond = testDouble(cond, src);
        j(cond, label);
    }
    void branchTestNull(Condition cond, const ValueOperand &src, Label *label) {
        cond = testNull(cond, src);
        j(cond, label);
    }
    void branchTestString(Condition cond, const ValueOperand &src, Label *label) {
        cond = testString(cond, src);
        j(cond, label);
    }
    void branchTestObject(Condition cond, const ValueOperand &src, Label *label) {
        cond = testObject(cond, src);
        j(cond, label);
    }
    template <typename T>
    void branchTestGCThing(Condition cond, const T &src, Label *label) {
        cond = testGCThing(cond, src);
        j(cond, label);
    }
    template <typename T>
    void branchTestPrimitive(Condition cond, const T &t, Label *label) {
        cond = testPrimitive(cond, t);
        j(cond, label);
    }
    template <typename T>
    void branchTestMagic(Condition cond, const T &t, Label *label) {
        cond = testMagic(cond, t);
        j(cond, label);
    }
    Condition testMagic(Condition cond, const ValueOperand &src) {
        splitTag(src, ScratchReg);
        return testMagic(cond, ScratchReg);
    }
    Condition testError(Condition cond, const ValueOperand &src) {
        return testMagic(cond, src);
    }
    void branchTestValue(Condition cond, const ValueOperand &value, const Value &v, Label *label) {
        JS_ASSERT(value.valueReg() != ScratchReg);
        moveValue(v, ScratchReg);
        cmpq(value.valueReg(), ScratchReg);
        j(cond, label);
    }

    void boxDouble(const FloatRegister &src, const ValueOperand &dest) {
        movqsd(src, dest.valueReg());
    }

    // Note that the |dest| register here may be ScratchReg, so we shouldn't
    // use it.
    void unboxInt32(const ValueOperand &src, const Register &dest) {
        movl(Operand(src.valueReg()), dest);
    }
    void unboxInt32(const Operand &src, const Register &dest) {
        movl(src, dest);
    }
    void unboxInt32(const Address &src, const Register &dest) {
        unboxInt32(Operand(src), dest);
    }
    void unboxBoolean(const ValueOperand &src, const Register &dest) {
        movl(Operand(src.valueReg()), dest);
    }
    void unboxBoolean(const Operand &src, const Register &dest) {
        movl(src, dest);
    }
    void unboxDouble(const ValueOperand &src, const FloatRegister &dest) {
        movqsd(src.valueReg(), dest);
    }
    void unboxDouble(const Operand &src, const FloatRegister &dest) {
        lea(src, ScratchReg);
        movqsd(ScratchReg, dest);
    }

    // Unbox any non-double value into dest. Prefer unboxInt32 or unboxBoolean
    // instead if the source type is known.
    void unboxNonDouble(const ValueOperand &src, const Register &dest) {
        if (src.valueReg() == dest) {
            movq(ImmWord(JSVAL_PAYLOAD_MASK), ScratchReg);
            andq(ScratchReg, dest);
        } else {
            movq(ImmWord(JSVAL_PAYLOAD_MASK), dest);
            andq(src.valueReg(), dest);
        }
    }
    void unboxNonDouble(const Operand &src, const Register &dest) {
        // Explicitly permits |dest| to be used in |src|.
        JS_ASSERT(dest != ScratchReg);
        movq(ImmWord(JSVAL_PAYLOAD_MASK), ScratchReg);
        movq(src, dest);
        andq(ScratchReg, dest);
    }

    void unboxString(const ValueOperand &src, const Register &dest) { unboxNonDouble(src, dest); }
    void unboxString(const Operand &src, const Register &dest) { unboxNonDouble(src, dest); }

    void unboxObject(const ValueOperand &src, const Register &dest) { unboxNonDouble(src, dest); }
    void unboxObject(const Operand &src, const Register &dest) { unboxNonDouble(src, dest); }

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address &address, Register scratch) {
        JS_ASSERT(scratch != ScratchReg);
        loadPtr(address, ScratchReg);
        unboxObject(ValueOperand(ScratchReg), scratch);
        return scratch;
    }
    Register extractObject(const ValueOperand &value, Register scratch) {
        JS_ASSERT(scratch != ScratchReg);
        unboxObject(value, scratch);
        return scratch;
    }
    Register extractTag(const Address &address, Register scratch) {
        JS_ASSERT(scratch != ScratchReg);
        loadPtr(address, scratch);
        splitTag(scratch, scratch);
        return scratch;
    }
    Register extractTag(const ValueOperand &value, Register scratch) {
        JS_ASSERT(scratch != ScratchReg);
        splitTag(value, scratch);
        return scratch;
    }

    void unboxValue(const ValueOperand &src, AnyRegister dest) {
        if (dest.isFloat()) {
            Label notInt32, end;
            branchTestInt32(Assembler::NotEqual, src, &notInt32);
            cvtsi2sd(src.valueReg(), dest.fpu());
            jump(&end);
            bind(&notInt32);
            unboxDouble(src, dest.fpu());
            bind(&end);
        } else {
            unboxNonDouble(src, dest.gpr());
        }
    }

    // These two functions use the low 32-bits of the full value register.
    void boolValueToDouble(const ValueOperand &operand, const FloatRegister &dest) {
        cvtsi2sd(Operand(operand.valueReg()), dest);
    }
    void int32ValueToDouble(const ValueOperand &operand, const FloatRegister &dest) {
        cvtsi2sd(Operand(operand.valueReg()), dest);
    }

    void loadConstantDouble(double d, const FloatRegister &dest) {
        union DoublePun {
            uint64_t u;
            double d;
        } pun;
        pun.d = d;
        if (pun.u == 0) {
            xorpd(dest, dest);
        } else {
            movq(ImmWord(pun.u), ScratchReg);
            movqsd(ScratchReg, dest);
        }
    }
    void loadStaticDouble(const double *dp, const FloatRegister &dest) {
        loadConstantDouble(*dp, dest);
    }

    Condition testInt32Truthy(bool truthy, const ValueOperand &operand) {
        testl(operand.valueReg(), operand.valueReg());
        return truthy ? NonZero : Zero;
    }
    void branchTestBooleanTruthy(bool truthy, const ValueOperand &operand, Label *label) {
        testl(operand.valueReg(), operand.valueReg());
        j(truthy ? NonZero : Zero, label);
    }
    // This returns the tag in ScratchReg.
    Condition testStringTruthy(bool truthy, const ValueOperand &value) {
        unboxString(value, ScratchReg);

        Operand lengthAndFlags(ScratchReg, JSString::offsetOfLengthAndFlags());
        movq(lengthAndFlags, ScratchReg);
        shrq(Imm32(JSString::LENGTH_SHIFT), ScratchReg);
        testq(ScratchReg, ScratchReg);
        return truthy ? Assembler::NonZero : Assembler::Zero;
    }


    void loadInt32OrDouble(const Operand &operand, const FloatRegister &dest) {
        Label notInt32, end;
        movq(operand, ScratchReg);
        branchTestInt32(Assembler::NotEqual, ValueOperand(ScratchReg), &notInt32);
        cvtsi2sd(operand, dest);
        jump(&end);
        bind(&notInt32);
        movsd(operand, dest);
        bind(&end);
    }

    template <typename T>
    void loadUnboxedValue(const T &src, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(Operand(src), dest.fpu());
        else
            unboxNonDouble(Operand(src), dest.gpr());
    }

    void loadInstructionPointerAfterCall(const Register &dest) {
        movq(Operand(StackPointer, 0x0), dest);
    }

    void convertUInt32ToDouble(const Register &src, const FloatRegister &dest) {
        cvtsq2sd(src, dest);
    }

    // Setup a call to C/C++ code, given the number of general arguments it
    // takes. Note that this only supports cdecl.
    //
    // In order for alignment to work correctly, the MacroAssembler must have a
    // consistent view of the stack displacement. It is okay to call "push"
    // manually, however, if the stack alignment were to change, the macro
    // assembler should be notified before starting a call.
    void setupAlignedABICall(uint32 args);

    // Sets up an ABI call for when the alignment is not known. This may need a
    // scratch register.
    void setupUnalignedABICall(uint32 args, const Register &scratch);

    // Arguments must be assigned to a C/C++ call in order. They are moved
    // in parallel immediately before performing the call. This process may
    // temporarily use more stack, in which case esp-relative addresses will be
    // automatically adjusted. It is extremely important that esp-relative
    // addresses are computed *after* setupABICall(). Furthermore, no
    // operations should be emitted while setting arguments.
    void passABIArg(const MoveOperand &from);
    void passABIArg(const Register &reg);
    void passABIArg(const FloatRegister &reg);

    // Emits a call to a C/C++ function, resolving all argument moves.
    void callWithABI(void *fun, Result result = GENERAL);

    void handleException();

    void makeFrameDescriptor(Register frameSizeReg, FrameType type) {
        shlq(Imm32(FRAMESIZE_SHIFT), frameSizeReg);
        orq(Imm32(type), frameSizeReg);
    }

    // Save an exit frame (which must be aligned to the stack pointer) to
    // ThreadData::ionTop.
    void linkExitFrame() {
        mov(ImmWord(GetIonContext()->cx->runtime), ScratchReg);
        mov(StackPointer, Operand(ScratchReg, offsetof(JSRuntime, ionTop)));
    }
};

typedef MacroAssemblerX64 MacroAssemblerSpecific;

} // namespace ion
} // namespace js

#endif // jsion_macro_assembler_x64_h__

