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
 *  Marty Rosenberg <mrosenberg@mozilla.com>
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
#include "ion/arm/MacroAssembler-arm.h"
#include "ion/MoveEmitter.h"

using namespace js;
using namespace ion;
bool
isValueDTRDCandidate(ValueOperand &val)
{
    // In order to be used for a DTRD memory function, the two target registers
    // need to be a) Adjacent, with the tag larger than the payload, and
    // b) Aligned to a multiple of two.
    if ((val.typeReg().code() != (val.payloadReg().code() + 1)))
        return false;
    else if ((val.payloadReg().code() & 1) != 0)
        return false;
    return true;
}

void
MacroAssemblerARM::convertInt32ToDouble(const Register &src, const FloatRegister &dest_)
{
    // direct conversions aren't possible.
    VFPRegister dest = VFPRegister(dest_);
    as_vxfer(src, InvalidReg, dest.sintOverlay(),
             CoreToFloat);
    as_vcvt(dest, dest.sintOverlay());
}

void
MacroAssemblerARM::convertUInt32ToDouble(const Register &src, const FloatRegister &dest_)
{
    // direct conversions aren't possible.
    VFPRegister dest = VFPRegister(dest_);
    as_vxfer(src, InvalidReg, dest.uintOverlay(),
             CoreToFloat);
    as_vcvt(dest, dest.uintOverlay());
}

// there are two options for implementing emitTruncateDouble.
// 1) convert the floating point value to an integer, if it did not fit,
//        then it was clamped to INT_MIN/INT_MAX, and we can test it.
//        NOTE: if the value really was supposed to be INT_MAX / INT_MIN
//        then it will be wrong.
// 2) convert the floating point value to an integer, if it did not fit,
//        then it set one or two bits in the fpcsr.  Check those.
void
MacroAssemblerARM::branchTruncateDouble(const FloatRegister &src, const Register &dest, Label *fail)
{
    ma_vcvt_F64_I32(src, ScratchFloatReg);
    ma_vxfer(ScratchFloatReg, dest);
    ma_cmp(dest, Imm32(0x7fffffff));
    ma_cmp(dest, Imm32(0x80000000), Assembler::NotEqual);
    ma_b(fail, Assembler::Equal);
}

bool
MacroAssemblerARM::alu_dbl(Register src1, Imm32 imm, Register dest, ALUOp op,
                           SetCond_ sc, Condition c)
{
    if ((sc == SetCond && ! condsAreSafe(op)) || !can_dbl(op)) {
        return false;
    }
    ALUOp interop = getDestVariant(op);
    Imm8::TwoImm8mData both = Imm8::encodeTwoImms(imm.value);
    if (both.fst.invalid) {
        return false;
    }
    // for the most part, there is no good reason to set the condition
    // codes for the first instruction.
    // we can do better things if the second instruction doesn't
    // have a dest, such as check for overflow by doing first operation
    // don't do second operation if first operation overflowed.
    // this preserves the overflow condition code.
    // unfortunately, it is horribly brittle.
    as_alu(ScratchRegister, src1, both.fst, interop, NoSetCond, c);
    as_alu(dest, ScratchRegister, both.snd, op, sc, c);
    // we succeeded!
    return true;
}


void
MacroAssemblerARM::ma_alu(Register src1, Imm32 imm, Register dest,
                          ALUOp op,
                          SetCond_ sc, Condition c)
{
    // As it turns out, if you ask for a compare-like instruction
    // you *probably* want it to set condition codes.
    if (dest == InvalidReg) {
        JS_ASSERT(sc == SetCond);
    }

    // The operator gives us the ability to determine how
    // this can be used.
    Imm8 imm8 = Imm8(imm.value);
    // ONE INSTRUCTION:
    // If we can encode it using an imm8m, then do so.
    if (!imm8.invalid) {
        as_alu(dest, src1, imm8, op, sc, c);
        return;
    }
    // ONE INSTRUCTION, NEGATED:
    Imm32 negImm = imm;
    Register negDest;
    ALUOp negOp = ALUNeg(op, dest, &negImm, &negDest);
    Imm8 negImm8 = Imm8(negImm.value);
    // add r1, r2, -15 can be replaced with
    // sub r1, r2, 15
    // for bonus points, dest can be replaced (nearly always invalid => ScratchRegister)
    // This is useful if we wish to negate tst.  tst has an invalid (aka not used) dest,
    // but its negation is bic *requires* a dest.  We can accomodate, but it will need to clobber
    // *something*, and the scratch register isn't being used, so...
    if (negOp != op_invalid && !negImm8.invalid) {
        as_alu(negDest, src1, negImm8, negOp, sc, c);
        return;
    }

    if (hasMOVWT()) {
        // If the operation is a move-a-like then we can try to use movw to
        // move the bits into the destination.  Otherwise, we'll need to
        // fall back on a multi-instruction format :(
        // movw/movt don't set condition codes, so don't hold your breath.
        if (sc == NoSetCond && (op == op_mov || op == op_mvn)) {
            // ARMv7 supports movw/movt. movw zero-extends
            // its 16 bit argument, so we can set the register
            // this way.
            // movt leaves the bottom 16 bits in tact, so
            // it is unsuitable to move a constant that
            if (op == op_mov && ((imm.value & ~ 0xffff) == 0)) {
                JS_ASSERT(src1 == InvalidReg);
                as_movw(dest, (uint16)imm.value, c);
                return;
            }
            // If they asked for a mvn rfoo, imm, where ~imm fits into 16 bits
            // then do it.
            if (op == op_mvn && (((~imm.value) & ~ 0xffff) == 0)) {
                JS_ASSERT(src1 == InvalidReg);
                as_movw(dest, (uint16)~imm.value, c);
                return;
            }
            // TODO: constant dedup may enable us to add dest, r0, 23 *if*
            // we are attempting to load a constant that looks similar to one
            // that already exists
            // If it can't be done with a single movw
            // then we *need* to use two instructions
            // since this must be some sort of a move operation, we can just use
            // a movw/movt pair and get the whole thing done in two moves.  This
            // does not work for ops like add, sinc we'd need to do
            // movw tmp; movt tmp; add dest, tmp, src1
            if (op == op_mvn)
                imm.value = ~imm.value;
            as_movw(dest, imm.value & 0xffff, c);
            as_movt(dest, (imm.value >> 16) & 0xffff, c);
            return;
        }
        // If we weren't doing a movalike, a 16 bit immediate
        // will require 2 instructions.  With the same amount of
        // space and (less)time, we can do two 8 bit operations, reusing
        // the dest register.  e.g.
        // movw tmp, 0xffff; add dest, src, tmp ror 4
        // vs.
        // add dest, src, 0xff0; add dest, dest, 0xf000000f
        // it turns out that there are some immediates that we miss with the
        // second approach.  A sample value is: add dest, src, 0x1fffe
        // this can be done by movw tmp, 0xffff; add dest, src, tmp lsl 1
        // since imm8m's only get even offsets, we cannot encode this.
        // I'll try to encode as two imm8's first, since they are faster.
        // Both operations should take 1 cycle, where as add dest, tmp ror 4
        // takes two cycles to execute.
    }
    // Either a) this isn't ARMv7 b) this isn't a move
    // start by attempting to generate a two instruction form.
    // Some things cannot be made into two-inst forms correctly.
    // namely, adds dest, src, 0xffff.
    // Since we want the condition codes (and don't know which ones will
    // be checked), we need to assume that the overflow flag will be checked
    // and add{,s} dest, src, 0xff00; add{,s} dest, dest, 0xff is not
    // guaranteed to set the overflow flag the same as the (theoretical)
    // one instruction variant.
    if (alu_dbl(src1, imm, dest, op, sc, c))
        return;
    // And try with its negative.
    if (negOp != op_invalid &&
        alu_dbl(src1, negImm, dest, negOp, sc, c))
        return;
    // Well, damn. We can use two 16 bit mov's, then do the op
    // or we can do a single load from a pool then op.
    if (hasMOVWT()) {
        // Try to load the immediate into a scratch register
        // then use that
        as_movw(ScratchRegister, imm.value & 0xffff, c);
        if ((imm.value >> 16) != 0)
            as_movt(ScratchRegister, (imm.value >> 16) & 0xffff, c);
    } else {
        JS_NOT_REACHED("non-ARMv7 loading of immediates NYI.");
    }
    as_alu(dest, src1, O2Reg(ScratchRegister), op, sc, c);
    // done!
}

void
MacroAssemblerARM::ma_alu(Register src1, Operand op2, Register dest, ALUOp op,
            SetCond_ sc, Assembler::Condition c)
{
    JS_ASSERT(op2.getTag() == Operand::OP2);
    as_alu(dest, src1, op2.toOp2(), op, sc, c);
}

void
MacroAssemblerARM::ma_alu(Register src1, Operand2 op2, Register dest, ALUOp op, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, op2, op, sc, c);
}

void
MacroAssemblerARM::ma_nop()
{
    as_nop();
}

Instruction *
NextInst(Instruction *i)
{
    if (i == NULL)
        return NULL;
    return i->next();
}

void
MacroAssemblerARM::ma_movPatchable(Imm32 imm_, Register dest,
                                   Assembler::Condition c, RelocStyle rs, Instruction *i)
{
    int32 imm = imm_.value;
    switch(rs) {
      case L_MOVWT:
        as_movw(dest, Imm16(imm & 0xffff), c, i);
        i = NextInst(i);
        as_movt(dest, Imm16(imm >> 16 & 0xffff), c, i);
        break;
      case L_LDR:
        //as_Imm32Pool(dest, imm, c, i);
        break;
    }
}

void
MacroAssemblerARM::ma_mov(Register src, Register dest,
            SetCond_ sc, Assembler::Condition c)
{
    if (sc == SetCond || dest != src)
        as_mov(dest, O2Reg(src), sc, c);
}

void
MacroAssemblerARM::ma_mov(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(InvalidReg, imm, dest, op_mov, sc, c);
}

void
MacroAssemblerARM::ma_mov(const ImmGCPtr &ptr, Register dest)
{
    // As opposed to x86/x64 version, the data relocation has to be executed
    // before to recover the pointer, and not after.
    writeDataRelocation(ptr);
    ma_movPatchable(Imm32(ptr.value), dest, Always, L_MOVWT);
}

    // Shifts (just a move with a shifting op2)
void
MacroAssemblerARM::ma_lsl(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, lsl(src, shift.value));
}
void
MacroAssemblerARM::ma_lsr(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, lsr(src, shift.value));
}
void
MacroAssemblerARM::ma_asr(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, asr(src, shift.value));
}
void
MacroAssemblerARM::ma_ror(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, ror(src, shift.value));
}
void
MacroAssemblerARM::ma_rol(Imm32 shift, Register src, Register dst)
{
    as_mov(dst, rol(src, shift.value));
}
    // Shifts (just a move with a shifting op2)
void
MacroAssemblerARM::ma_lsl(Register shift, Register src, Register dst)
{
    as_mov(dst, lsl(src, shift));
}
void
MacroAssemblerARM::ma_lsr(Register shift, Register src, Register dst)
{
    as_mov(dst, lsr(src, shift));
}
void
MacroAssemblerARM::ma_asr(Register shift, Register src, Register dst)
{
    as_mov(dst, asr(src, shift));
}
void
MacroAssemblerARM::ma_ror(Register shift, Register src, Register dst)
{
    as_mov(dst, ror(src, shift));
}
void
MacroAssemblerARM::ma_rol(Register shift, Register src, Register dst)
{
    ma_rsb(shift, Imm32(32), ScratchRegister);
    as_mov(dst, ror(src, ScratchRegister));
}

    // Move not (dest <- ~src)

void
MacroAssemblerARM::ma_mvn(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(InvalidReg, imm, dest, op_mvn, sc, c);
}

void
MacroAssemblerARM::ma_mvn(Register src1, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_alu(dest, InvalidReg, O2Reg(src1), op_mvn, sc, c);
}

    // and
void
MacroAssemblerARM::ma_and(Register src, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_and(dest, src, dest);
}
void
MacroAssemblerARM::ma_and(Register src1, Register src2, Register dest,
            SetCond_ sc, Assembler::Condition c)
{
    as_and(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_and(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, op_and, sc, c);
}
void
MacroAssemblerARM::ma_and(Imm32 imm, Register src1, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, op_and, sc, c);
}


    // bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2)
void
MacroAssemblerARM::ma_bic(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, op_bic, sc, c);
}

    // exclusive or
void
MacroAssemblerARM::ma_eor(Register src, Register dest,
            SetCond_ sc, Assembler::Condition c)
{
    ma_eor(dest, src, dest, sc, c);
}
void
MacroAssemblerARM::ma_eor(Register src1, Register src2, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_eor(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_eor(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, op_eor, sc, c);
}
void
MacroAssemblerARM::ma_eor(Imm32 imm, Register src1, Register dest,
       SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, op_eor, sc, c);
}

    // or
void
MacroAssemblerARM::ma_orr(Register src, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_orr(dest, src, dest, sc, c);
}
void
MacroAssemblerARM::ma_orr(Register src1, Register src2, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    as_orr(dest, src1, O2Reg(src2), sc, c);
}
void
MacroAssemblerARM::ma_orr(Imm32 imm, Register dest,
                          SetCond_ sc, Assembler::Condition c)
{
    ma_alu(dest, imm, dest, op_orr, sc, c);
}
void
MacroAssemblerARM::ma_orr(Imm32 imm, Register src1, Register dest,
                SetCond_ sc, Assembler::Condition c)
{
    ma_alu(src1, imm, dest, op_orr, sc, c);
}

    // arithmetic based ops
    // add with carry
void
MacroAssemblerARM::ma_adc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_adc, sc, c);
}
void
MacroAssemblerARM::ma_adc(Register src, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src), op_adc, sc, c);
}
void
MacroAssemblerARM::ma_adc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), op_adc, sc, c);
}

    // add
void
MacroAssemblerARM::ma_add(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_add, sc, c);
}

void
MacroAssemblerARM::ma_add(Register src1, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, O2Reg(src1), dest, op_add, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), op_add, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Operand op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, op_add, sc, c);
}
void
MacroAssemblerARM::ma_add(Register src1, Imm32 op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, op_add, sc, c);
}

    // subtract with carry
void
MacroAssemblerARM::ma_sbc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_sbc, sc, c);
}
void
MacroAssemblerARM::ma_sbc(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), op_sbc, sc, c);
}
void
MacroAssemblerARM::ma_sbc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), op_sbc, sc, c);
}

    // subtract
void
MacroAssemblerARM::ma_sub(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_sub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, Operand(src1), dest, op_sub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, Operand(src2), dest, op_sub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Operand op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, op_sub, sc, c);
}
void
MacroAssemblerARM::ma_sub(Register src1, Imm32 op, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op, dest, op_sub, sc, c);
}

    // reverse subtract
void
MacroAssemblerARM::ma_rsb(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_rsb, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), op_add, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), op_rsb, sc, c);
}
void
MacroAssemblerARM::ma_rsb(Register src1, Imm32 op2, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(src1, op2, dest, op_rsb, sc, c);
}

    // reverse subtract with carry
void
MacroAssemblerARM::ma_rsc(Imm32 imm, Register dest, SetCond_ sc, Condition c)
{
    ma_alu(dest, imm, dest, op_rsc, sc, c);
}
void
MacroAssemblerARM::ma_rsc(Register src1, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, dest, O2Reg(src1), op_rsc, sc, c);
}
void
MacroAssemblerARM::ma_rsc(Register src1, Register src2, Register dest, SetCond_ sc, Condition c)
{
    as_alu(dest, src1, O2Reg(src2), op_rsc, sc, c);
}

    // compares/tests
    // compare negative (sets condition codes as src1 + src2 would)
void
MacroAssemblerARM::ma_cmn(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, op_cmn, SetCond, c);
}
void
MacroAssemblerARM::ma_cmn(Register src1, Register src2, Condition c)
{
    as_alu(InvalidReg, src2, O2Reg(src1), op_cmn, SetCond, c);
}
void
MacroAssemblerARM::ma_cmn(Register src1, Operand op, Condition c)
{
    JS_NOT_REACHED("Feature NYI");
}

// compare (src - src2)
void
MacroAssemblerARM::ma_cmp(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, op_cmp, SetCond, c);
}

void
MacroAssemblerARM::ma_cmp(Register src1, ImmWord ptr, Condition c)
{
    ma_cmp(src1, Imm32(ptr.value), c);
}

void
MacroAssemblerARM::ma_cmp(Register src1, ImmGCPtr ptr, Condition c)
{
    ma_mov(ptr, ScratchRegister);
    ma_cmp(src1, ScratchRegister, c);
}
void
MacroAssemblerARM::ma_cmp(Register src1, Operand op, Condition c)
{
    switch (op.getTag()) {
      case Operand::OP2:
        as_cmp(src1, op.toOp2(), c);
        break;
      case Operand::MEM:
        ma_ldr(op, ScratchRegister);
        as_cmp(src1, O2Reg(ScratchRegister), c);
        break;
      default:
        JS_NOT_REACHED("trying to compare FP and integer registers");
        break;
    }
}
void
MacroAssemblerARM::ma_cmp(Register src1, Register src2, Condition c)
{
    as_cmp(src1, O2Reg(src2), c);
}

    // test for equality, (src1^src2)
void
MacroAssemblerARM::ma_teq(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, op_teq, SetCond, c);
}
void
MacroAssemblerARM::ma_teq(Register src1, Register src2, Condition c)
{
    as_tst(src1, O2Reg(src2), c);
}
void
MacroAssemblerARM::ma_teq(Register src1, Operand op, Condition c)
{
    as_teq(src1, op.toOp2(), c);
}


// test (src1 & src2)
void
MacroAssemblerARM::ma_tst(Register src1, Imm32 imm, Condition c)
{
    ma_alu(src1, imm, InvalidReg, op_tst, SetCond, c);
}
void
MacroAssemblerARM::ma_tst(Register src1, Register src2, Condition c)
{
    as_tst(src1, O2Reg(src2), c);
}
void
MacroAssemblerARM::ma_tst(Register src1, Operand op, Condition c)
{
    as_tst(src1, op.toOp2(), c);
}

void
MacroAssemblerARM::ma_mul(Register src1, Register src2, Register dest)
{
    as_mul(dest, src1, src2);
}
void
MacroAssemblerARM::ma_mul(Register src1, Imm32 imm, Register dest)
{

    ma_mov(imm, ScratchRegister);
    as_mul( dest, src1, ScratchRegister);
}

Assembler::Condition
MacroAssemblerARM::ma_check_mul(Register src1, Register src2, Register dest, Condition cond)
{
    // TODO: this operation is illegal on armv6 and earlier if src2 == ScratchRegister
    //       or src2 == dest.
    if (cond == Equal || cond == NotEqual) {
        as_smull(ScratchRegister, dest, src1, src2, SetCond);
        return cond;
    } else if (cond == Overflow) {
        as_smull(ScratchRegister, dest, src1, src2);
        as_cmp(ScratchRegister, asr(dest, 31));
        return NotEqual;
    }
    JS_NOT_REACHED("Condition NYI");
    return Always;

}

Assembler::Condition
MacroAssemblerARM::ma_check_mul(Register src1, Imm32 imm, Register dest, Condition cond)
{
    ma_mov(imm, ScratchRegister);
    if (cond == Equal || cond == NotEqual) {
        as_smull(ScratchRegister, dest, ScratchRegister, src1, SetCond);
        return cond;
    } else if (cond == Overflow) {
        as_smull(ScratchRegister, dest, ScratchRegister, src1);
        as_cmp(ScratchRegister, asr(dest, 31));
        return NotEqual;
    }
    JS_NOT_REACHED("Condition NYI");
    return Always;
}

void
MacroAssemblerARM::ma_mod_mask(Register src, Register dest, Register hold, int32 shift)
{
    // MATH:
    // We wish to compute x % (1<<y) - 1 for a known constant, y.
    // first, let b = (1<<y) and C = (1<<y)-1, then think of the 32 bit dividend as
    // a number in base b, namely c_0*1 + c_1*b + c_2*b^2 ... c_n*b^n
    // now, since both addition and multiplication commute with modulus,
    // x % C == (c_0 + c_1*b + ... + c_n*b^n) % C ==
    // (c_0 % C) + (c_1%C) * (b % C) + (c_2 % C) * (b^2 % C)...
    // now, since b == C + 1, b % C == 1, and b^n % C == 1
    // this means that the whole thing simplifies to:
    // c_0 + c_1 + c_2 ... c_n % C
    // each c_n can easily be computed by a shift/bitextract, and the modulus can be maintained
    // by simply subtracting by C whenever the number gets over C.
    int32 mask = (1 << shift) - 1;
    Label head;

    // hold holds -1 if the value was negative, 1 otherwise.
    // ScratchRegister holds the remaining bits that have not been processed
    // lr serves as a temporary location to store extracted bits into as well
    //    as holding the trial subtraction as a temp value
    // dest is the accumulator (and holds the final result)

    // move the whole value into the scratch register, setting the codition codes so
    // we can muck with them later
    as_mov(ScratchRegister, O2Reg(src), SetCond);
    // Zero out the dest.
    ma_mov(Imm32(0), dest);
    // Set the hold appropriately.
    ma_mov(Imm32(1), hold);
    ma_mov(Imm32(-1), hold, NoSetCond, Signed);
    ma_rsb(Imm32(0), ScratchRegister, SetCond, Signed);
    // Begin the main loop.
    bind(&head);

    // Extract the bottom bits into lr.
    ma_and(Imm32(mask), ScratchRegister, lr);
    // Add those bits to the accumulator.
    ma_add(lr, dest, dest);
    // Do a trial subtraction, this is the same operation as cmp, but we store the dest
    ma_sub(dest, Imm32(mask), lr, SetCond);
    // If (sum - C) > 0, store sum - C back into sum, thus performing a modulus.
    ma_mov(lr, dest, NoSetCond, Unsigned);
    // Get rid of the bits that we extracted before, and set the condition codes
    as_mov(ScratchRegister, lsr(ScratchRegister, shift), SetCond);
    // If the shift produced zero, finish, otherwise, continue in the loop.
    ma_b(&head, NonZero);
    // Check the hold to see if we need to negate the result.  Hold can only be 1 or -1,
    // so this will never set the 0 flag.
    ma_cmp(hold, Imm32(0));
    // If the hold was non-zero, negate the result to be in line with what JS wants
    // this will set the condition codes if we try to negate
    ma_rsb(Imm32(0), dest, SetCond, Signed);
    // Since the Zero flag is not set by the compare, we can *only* set the Zero flag
    // in the rsb, so Zero is set iff we negated zero (e.g. the result of the computation was -0.0).

}

// memory
// shortcut for when we know we're transferring 32 bits of data
void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rn, Imm32 offset, Register rt,
                          Index mode, Assembler::Condition cc)
{
    ma_dataTransferN(ls, 32, true, rn, offset, rt, mode, cc);
}

void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rn, Register rm, Register rt,
                          Index mode, Assembler::Condition cc)
{
    JS_NOT_REACHED("Feature NYI");
}

void
MacroAssemblerARM::ma_str(Register rt, DTRAddr addr, Index mode, Condition cc)
{
    as_dtr(IsStore, 32, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_dtr(LoadStore ls, Register rt, const Operand &addr, Index mode, Condition cc)
{
    ma_dataTransferN(ls, 32, true,
                     Register::FromCode(addr.base()), Imm32(addr.disp()),
                     rt, mode, cc);
}

void
MacroAssemblerARM::ma_str(Register rt, const Operand &addr, Index mode, Condition cc)
{
    ma_dtr(IsStore, rt, addr, mode, cc);
}
void
MacroAssemblerARM::ma_strd(Register rt, DebugOnly<Register> rt2, EDtrAddr addr, Index mode, Condition cc)
{
    JS_ASSERT((rt.code() & 1) == 0);
    JS_ASSERT(rt2.value.code() == rt.code() + 1);
    as_extdtr(IsStore, 64, true, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldr(DTRAddr addr, Register rt, Index mode, Condition cc)
{
    as_dtr(IsLoad, 32, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldr(const Operand &addr, Register rt, Index mode, Condition cc)
{
    ma_dtr(IsLoad, rt, addr, mode, cc);
}

void
MacroAssemblerARM::ma_ldrb(DTRAddr addr, Register rt, Index mode, Condition cc)
{
    as_dtr(IsLoad, 8, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldrsh(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 16, true, mode, rt, addr, cc);
}

void
MacroAssemblerARM::ma_ldrh(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 16, false, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldrsb(EDtrAddr addr, Register rt, Index mode, Condition cc)
{
    as_extdtr(IsLoad, 8, true, mode, rt, addr, cc);
}
void
MacroAssemblerARM::ma_ldrd(EDtrAddr addr, Register rt, DebugOnly<Register> rt2, Index mode, Condition cc)
{
    JS_ASSERT((rt.code() & 1) == 0);
    JS_ASSERT(rt2.value.code() == rt.code() + 1);
    as_extdtr(IsLoad, 64, true, mode, rt, addr, cc);
}

// specialty for moving N bits of data, where n == 8,16,32,64
void
MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                          Register rn, Register rm, Register rt,
                          Index mode, Assembler::Condition cc)
{
    JS_NOT_REACHED("Feature NYI");
}

void
MacroAssemblerARM::ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                                    Register rn, Imm32 offset, Register rt,
                                    Index mode, Assembler::Condition cc)
{
    int off = offset.value;
    // we can encode this as a standard ldr... MAKE IT SO
    if (size == 32 || (size == 8 && !IsSigned) ) {
        if (off < 4096 && off > -4096) {
            as_dtr(ls, size, mode, rt, DTRAddr(rn, DtrOffImm(off)), cc);
            return;
        }
        // We cannot encode this offset in a a single ldr.  Try to encode it as
        // an add scratch, base, imm; ldr dest, [scratch, +offset].
        int bottom = off & 0xfff;
        int neg_bottom = 0x1000 - bottom;
        // at this point, both off - bottom and off + neg_bottom will be reasonable-ish
        // quantities.
        if (off < 0) {
            Operand2 sub_off = Imm8(-(off-bottom)); // sub_off = bottom - off
            if (!sub_off.invalid) {
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc); // - sub_off = off - bottom
                as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(bottom)), cc);
                return;
            }
            sub_off = Imm8(-(off+neg_bottom));// sub_off = -neg_bottom - off
            if (!sub_off.invalid) {
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc); // - sub_off = neg_bottom + off
                as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(-neg_bottom)), cc);
                return;
            }
        } else {
            Operand2 sub_off = Imm8(off-bottom); // sub_off = off - bottom
            if (!sub_off.invalid) {
                as_add(ScratchRegister, rn, sub_off, NoSetCond, cc); //  sub_off = off - bottom
                as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(bottom)), cc);
                return;
            }
            sub_off = Imm8(off+neg_bottom);// sub_off = neg_bottom + off
            if (!sub_off.invalid) {
                as_add(ScratchRegister, rn, sub_off, NoSetCond,  cc); // sub_off = neg_bottom + off
                as_dtr(ls, size, Offset, rt, DTRAddr(ScratchRegister, DtrOffImm(-neg_bottom)), cc);
                return;
            }
        }
        JS_NOT_REACHED("TODO: implement bigger offsets :(");

    } else {
        // should attempt to use the extended load/store instructions
        if (off < 256 && off > -256) {
            as_extdtr(ls, size, IsSigned, mode, rt, EDtrAddr(rn, EDtrOffImm(off)), cc);
        }
        // We cannot encode this offset in a a single extldr.  Try to encode it as
        // an add scratch, base, imm; extldr dest, [scratch, +offset].
        int bottom = off & 0xff;
        int neg_bottom = 0x100 - bottom;
        // at this point, both off - bottom and off + neg_bottom will be reasonable-ish
        // quantities.
        if (off < 0) {
            Operand2 sub_off = Imm8(-(off-bottom)); // sub_off = bottom - off
            if (!sub_off.invalid) {
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc); // - sub_off = off - bottom
                as_extdtr(ls, size, IsSigned, Offset, rt,
                          EDtrAddr(ScratchRegister, EDtrOffImm(bottom)),
                          cc);
                return;
            }
            sub_off = Imm8(-(off+neg_bottom));// sub_off = -neg_bottom - off
            if (!sub_off.invalid) {
                as_sub(ScratchRegister, rn, sub_off, NoSetCond, cc); // - sub_off = neg_bottom + off
                as_extdtr(ls, size, IsSigned, Offset, rt,
                          EDtrAddr(ScratchRegister, EDtrOffImm(-neg_bottom)),
                          cc);
                return;
            }
        } else {
            Operand2 sub_off = Imm8(off-bottom); // sub_off = off - bottom
            if (!sub_off.invalid) {
                as_add(ScratchRegister, rn, sub_off, NoSetCond, cc); //  sub_off = off - bottom
                as_extdtr(ls, size, IsSigned, Offset, rt,
                          EDtrAddr(ScratchRegister, EDtrOffImm(bottom)),
                          cc);
                return;
            }
            sub_off = Imm8(off+neg_bottom);// sub_off = neg_bottom + off
            if (!sub_off.invalid) {
                as_add(ScratchRegister, rn, sub_off, NoSetCond,  cc); // sub_off = neg_bottom + off
                as_extdtr(ls, size, IsSigned, Offset, rt,
                          EDtrAddr(ScratchRegister, EDtrOffImm(-neg_bottom)),
                          cc);
                return;
            }
        }
        JS_NOT_REACHED("TODO: implement bigger offsets :(");
    }
}
void
MacroAssemblerARM::ma_pop(Register r)
{
    ma_dtr(IsLoad, sp, Imm32(4), r, PostIndex);
    if (r == pc)
        m_buffer.markGuard();
}
void
MacroAssemblerARM::ma_push(Register r)
{
    ma_dtr(IsStore, sp,Imm32(-4), r, PreIndex);
}

void
MacroAssemblerARM::ma_vpop(VFPRegister r)
{
    startFloatTransferM(IsLoad, sp, IA, WriteBack);
    transferFloatReg(r);
    finishFloatTransfer();
}
void
MacroAssemblerARM::ma_vpush(VFPRegister r)
{
    startFloatTransferM(IsStore, sp, DB, WriteBack);
    transferFloatReg(r);
    finishFloatTransfer();
}

// branches when done from within arm-specific code
void
MacroAssemblerARM::ma_b(Label *dest, Assembler::Condition c)
{
    as_b(dest, c);
}

void
MacroAssemblerARM::ma_bx(Register dest, Assembler::Condition c)
{
    as_bx(dest, c);
}

Assembler::RelocBranchStyle
b_type()
{
    return Assembler::B_LDR;
}
void
MacroAssemblerARM::ma_b(void *target, Relocation::Kind reloc, Assembler::Condition c)
{
    // we know the absolute address of the target, but not our final
    // location (with relocating GC, we *can't* know our final location)
    // for now, I'm going to be conservative, and load this with an
    // absolute address
    uint32 trg = (uint32)target;
    switch (b_type()) {
      case Assembler::B_MOVWT:
        as_movw(ScratchRegister, Imm16(trg & 0xffff), c);
        as_movt(ScratchRegister, Imm16(trg >> 16), c);
        // this is going to get the branch predictor pissed off.
        as_bx(ScratchRegister, c);
        break;
      case Assembler::B_LDR_BX:
        as_Imm32Pool(ScratchRegister, trg, c);
        as_bx(ScratchRegister, c);
        break;
      case Assembler::B_LDR:
        as_Imm32Pool(pc, trg, c);
        if (c == Always)
            m_buffer.markGuard();
        break;
      default:
        JS_NOT_REACHED("Other methods of generating tracable jumps NYI");
    }
}

// this is almost NEVER necessary, we'll basically never be calling a label
// except, possibly in the crazy bailout-table case.
void
MacroAssemblerARM::ma_bl(Label *dest, Assembler::Condition c)
{
    as_bl(dest, c);
}

//VFP/ALU
void
MacroAssemblerARM::ma_vadd(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vadd(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vsub(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vsub(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vmul(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vmul(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vdiv(FloatRegister src1, FloatRegister src2, FloatRegister dst)
{
    as_vdiv(VFPRegister(dst), VFPRegister(src1), VFPRegister(src2));
}

void
MacroAssemblerARM::ma_vmov(FloatRegister src, FloatRegister dest)
{
    as_vmov(dest, src);
}

void
MacroAssemblerARM::ma_vneg(FloatRegister src, FloatRegister dest)
{
    as_vneg(dest, src);
}

void
MacroAssemblerARM::ma_vimm(double value, FloatRegister dest)
{
    jsdpun dpun;
    dpun.d = value;
    if ((dpun.s.lo) == 0) {
        if (dpun.s.hi == 0) {
            // to zero a register, load 1.0, then execute dN <- dN - dN
            VFPImm dblEnc(0x3FF00000);
            as_vimm(dest, dblEnc);
            as_vsub(dest, dest, dest);
            return;
        }
        VFPImm dblEnc(dpun.s.hi);
        if (dblEnc.isValid()) {
            as_vimm(dest, dblEnc);
            return;
        }

    }
    // fall back to putting the value in a pool.
    as_FImm64Pool(dest, value);
}

void
MacroAssemblerARM::ma_vcmp(FloatRegister src1, FloatRegister src2)
{
    as_vcmp(VFPRegister(src1), VFPRegister(src2));
}
void
MacroAssemblerARM::ma_vcmpz(FloatRegister src1)
{
    as_vcmpz(VFPRegister(src1));
}

void
MacroAssemblerARM::ma_vcvt_F64_I32(FloatRegister src, FloatRegister dest)
{
    as_vcvt(VFPRegister(dest).sintOverlay(), VFPRegister(src));
}
void
MacroAssemblerARM::ma_vcvt_I32_F64(FloatRegister dest, FloatRegister src)
{
    as_vcvt(VFPRegister(dest), VFPRegister(src).sintOverlay());
}

void
MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest)
{
    as_vxfer(dest, InvalidReg, VFPRegister(src).singleOverlay(), FloatToCore);
}

void
MacroAssemblerARM::ma_vxfer(FloatRegister src, Register dest1, Register dest2)
{
    as_vxfer(dest1, dest2, VFPRegister(src), FloatToCore);
}

void
MacroAssemblerARM::ma_vxfer(VFPRegister src, Register dest)
{
    as_vxfer(dest, InvalidReg, src, FloatToCore);
}

void
MacroAssemblerARM::ma_vxfer(VFPRegister src, Register dest1, Register dest2)
{
    as_vxfer(dest1, dest2, src, FloatToCore);
}

void
MacroAssemblerARM::ma_vdtr(LoadStore ls, const Operand &addr, VFPRegister rt, Condition cc)
{
    int off = addr.disp();
    JS_ASSERT((off & 3) == 0);
    Register base = Register::FromCode(addr.base());
    if (off > -1024 && off < 1024) {
        as_vdtr(ls, rt, addr.toVFPAddr(), cc);
        return;
    }
    // We cannot encode this offset in a a single ldr.  Try to encode it as
    // an add scratch, base, imm; ldr dest, [scratch, +offset].
    int bottom = off & (0xff << 2);
    int neg_bottom = (0x100 << 2) - bottom;
    // at this point, both off - bottom and off + neg_bottom will be reasonable-ish
    // quantities.
    if (off < 0) {
        Operand2 sub_off = Imm8(-(off-bottom)); // sub_off = bottom - off
        if (!sub_off.invalid) {
            as_sub(ScratchRegister, base, sub_off, NoSetCond, cc); // - sub_off = off - bottom
            as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(bottom)), cc);
            return;
        }
        sub_off = Imm8(-(off+neg_bottom));// sub_off = -neg_bottom - off
        if (!sub_off.invalid) {
            as_sub(ScratchRegister, base, sub_off, NoSetCond, cc); // - sub_off = neg_bottom + off
            as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(-neg_bottom)), cc);
            return;
        }
    } else {
        Operand2 sub_off = Imm8(off-bottom); // sub_off = off - bottom
        if (!sub_off.invalid) {
            as_add(ScratchRegister, base, sub_off, NoSetCond, cc); //  sub_off = off - bottom
            as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(bottom)), cc);
            return;
        }
        sub_off = Imm8(off+neg_bottom);// sub_off = neg_bottom + off
        if (!sub_off.invalid) {
            as_add(ScratchRegister, base, sub_off, NoSetCond,  cc); // sub_off = neg_bottom + off
            as_vdtr(ls, rt, VFPAddr(ScratchRegister, VFPOffImm(-neg_bottom)), cc);
            return;
        }
    }
    JS_NOT_REACHED("TODO: implement bigger offsets :(");

}

void
MacroAssemblerARM::ma_vldr(VFPAddr addr, FloatRegister dest)
{
    as_vdtr(IsLoad, dest, addr);
}
void
MacroAssemblerARM::ma_vldr(const Operand &addr, FloatRegister dest)
{
    ma_vdtr(IsLoad, addr, dest);
}

void
MacroAssemblerARM::ma_vstr(FloatRegister src, VFPAddr addr)
{
    as_vdtr(IsStore, src, addr);
}

void
MacroAssemblerARM::ma_vstr(FloatRegister src, const Operand &addr)
{
    ma_vdtr(IsStore, addr, src);
}
void
MacroAssemblerARM::ma_vstr(FloatRegister src, Register base, Register index, int32 shift)
{
    as_add(ScratchRegister, base, lsl(index, shift));
    ma_vstr(src, Operand(ScratchRegister, 0));
}

uint32
MacroAssemblerARMCompat::buildFakeExitFrame(const Register &scratch)
{
    DebugOnly<uint32> initialDepth = framePushed();
    uint32 descriptor = MakeFrameDescriptor(framePushed(), IonFrame_JS);

    Push(scratch); // padding2
    Push(Imm32(descriptor)); // descriptor_
    Push(scratch); // padding

    DebugOnly<uint32> offsetBeforePush = currentOffset();
    Push(pc); // actually pushes $pc + 8.

    // Consume an additional 4 bytes. The start of the next instruction will
    // then be 8 bytes after the instruction for Push(pc); this offset can
    // therefore be fed to the safepoint.
    ma_nop();
    uint32 pseudoReturnOffset = currentOffset();

    JS_ASSERT(framePushed() == initialDepth + IonExitFrameLayout::Size());
    JS_ASSERT(pseudoReturnOffset - offsetBeforePush == 8);
    return pseudoReturnOffset;
}

void
MacroAssemblerARMCompat::callWithExitFrame(IonCode *target)
{
    uint32 descriptor = MakeFrameDescriptor(framePushed(), IonFrame_JS);
#ifdef DEBUG
    ma_mov(Imm32(0xdeadbeef), ScratchRegister);
#endif
    Push(ScratchRegister); // padding
    Push(Imm32(descriptor)); // descriptor

    addPendingJump(m_buffer.nextOffset(), target->raw(), Relocation::IONCODE);
    ma_mov(Imm32((int) target->raw()), ScratchRegister);
    callIon(ScratchRegister);
}

void
MacroAssemblerARMCompat::callIon(const Register &callee)
{
    JS_ASSERT((framePushed() & 3) == 0);
    if ((framePushed() & 7) == 4) {
        ma_callIonHalfPush(callee);
    } else {
        adjustFrame(sizeof(void*));
        ma_callIon(callee);
    }
}

void
MacroAssemblerARMCompat::reserveStack(uint32 amount)
{
    if (amount)
        ma_sub(Imm32(amount), sp);
    adjustFrame(amount);
}
void
MacroAssemblerARMCompat::freeStack(uint32 amount)
{
    JS_ASSERT(amount <= framePushed_);
    if (amount)
        ma_add(Imm32(amount), sp);
    adjustFrame(-amount);
}

void
MacroAssemblerARMCompat::add32(const Imm32 &imm, const Register &dest)
{
    ma_add(imm, dest);
}

void
MacroAssemblerARMCompat::sub32(const Imm32 &imm, const Register &dest)
{
    ma_sub(imm, dest);
}

void
MacroAssemblerARMCompat::and32(const Imm32 &imm, const Address &dest)
{
    load32(dest, ScratchRegister);
    ma_and(imm, ScratchRegister);
    store32(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::or32(const Imm32 &imm, const Address &dest)
{
    load32(dest, ScratchRegister);
    ma_orr(imm, ScratchRegister);
    store32(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::move32(const Imm32 &imm, const Register &dest)
{
    ma_mov(imm, dest);
}
void
MacroAssemblerARMCompat::move32(const Address &src, const Register &dest)
{
    movePtr(src, dest);
}
void
MacroAssemblerARMCompat::movePtr(const Register &src, const Register &dest)
{
    ma_mov(src, dest);
}
void
MacroAssemblerARMCompat::movePtr(const ImmWord &imm, const Register &dest)
{
    ma_mov(Imm32(imm.value), dest);
}
void
MacroAssemblerARMCompat::movePtr(const ImmGCPtr &imm, const Register &dest)
{
    ma_mov(imm, dest);
}
void
MacroAssemblerARMCompat::movePtr(const Address &src, const Register &dest)
{
    loadPtr(src, dest);
}
void
MacroAssemblerARMCompat::load8ZeroExtend(const Address &address, const Register &dest)
{
    ma_dataTransferN(IsLoad, 8, false, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load8ZeroExtend(const BaseIndex &src, const Register &dest)
{
    Register base = src.base;
    uint32 scale = Imm32::ShiftOf(src.scale).value;

    if (src.offset != 0) {
        ma_mov(base, ScratchRegister);
        base = ScratchRegister;
        ma_add(base, Imm32(src.offset), base);
    }
    ma_ldrb(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);

}

void
MacroAssemblerARMCompat::load8SignExtend(const Address &address, const Register &dest)
{
    ma_dataTransferN(IsLoad, 8, true, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load8SignExtend(const BaseIndex &src, const Register &dest)
{
    Register index = src.index;

    // ARMv7 does not have LSL on an index register with an extended load.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }
    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrsb(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load16ZeroExtend(const Address &address, const Register &dest)
{
    ma_dataTransferN(IsLoad, 16, false, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load16ZeroExtend_mask(const Address &address, Imm32 mask, const Register &dest)
{
    load16ZeroExtend(address, dest);
    ma_and(mask, dest, dest);
}

void
MacroAssemblerARMCompat::load16ZeroExtend(const BaseIndex &src, const Register &dest)
{
    Register index = src.index;

    // ARMv7 does not have LSL on an index register with an extended load.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }
    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load16SignExtend(const Address &address, const Register &dest)
{
    ma_dataTransferN(IsLoad, 16, true, address.base, Imm32(address.offset), dest);
}

void
MacroAssemblerARMCompat::load16SignExtend(const BaseIndex &src, const Register &dest)
{
    Register index = src.index;

    // We don't have LSL on index register yet.
    if (src.scale != TimesOne) {
        ma_lsl(Imm32::ShiftOf(src.scale), index, ScratchRegister);
        index = ScratchRegister;
    }
    if (src.offset != 0) {
        if (index != ScratchRegister) {
            ma_mov(index, ScratchRegister);
            index = ScratchRegister;
        }
        ma_add(Imm32(src.offset), index);
    }
    ma_ldrsh(EDtrAddr(src.base, EDtrOffReg(index)), dest);
}

void
MacroAssemblerARMCompat::load32(const Address &address, const Register &dest)
{
    loadPtr(address, dest);
}

void
MacroAssemblerARMCompat::load32(const BaseIndex &address, const Register &dest)
{
    loadPtr(address, dest);
}

void
MacroAssemblerARMCompat::load32(const AbsoluteAddress &address, const Register &dest)
{
    loadPtr(address, dest);
}
void
MacroAssemblerARMCompat::loadPtr(const Address &address, const Register &dest)
{
    ma_ldr(Operand(address), dest);
}

void
MacroAssemblerARMCompat::loadPtr(const BaseIndex &src, const Register &dest)
{
    Register base = src.base;
    uint32 scale = Imm32::ShiftOf(src.scale).value;

    if (src.offset != 0) {
        ma_mov(base, ScratchRegister);
        base = ScratchRegister;
        ma_add(Imm32(src.offset), base);
    }
    ma_ldr(DTRAddr(base, DtrRegImmShift(src.index, LSL, scale)), dest);
}
void
MacroAssemblerARMCompat::loadPtr(const AbsoluteAddress &address, const Register &dest)
{
    movePtr(ImmWord(address.addr), ScratchRegister);
    loadPtr(Address(ScratchRegister, 0x0), dest);
}

Operand payloadOf(const Address &address) {
    return Operand(address.base, address.offset);
}
Operand tagOf(const Address &address) {
    return Operand(address.base, address.offset + 4);
}

void
MacroAssemblerARMCompat::loadPrivate(const Address &address, const Register &dest)
{
    ma_ldr(payloadOf(address), dest);
}

void
MacroAssemblerARMCompat::loadDouble(const Address &address, const FloatRegister &dest)
{
    ma_vldr(Operand(address), dest);
}

void
MacroAssemblerARMCompat::loadDouble(const BaseIndex &src, const FloatRegister &dest)
{
    // VFP instructions don't even support register Base + register Index modes, so
    // just add the index, then handle the offset like normal
    Register base = src.base;
    Register index = src.index;
    uint32 scale = Imm32::ShiftOf(src.scale).value;
    int32 offset = src.offset;
    as_add(ScratchRegister, base, lsl(index, scale));

    ma_vldr(Operand(ScratchRegister, offset), dest);
}

void
MacroAssemblerARMCompat::loadFloatAsDouble(const Address &address, const FloatRegister &dest)
{
    VFPRegister rt = dest;
    ma_vdtr(IsLoad, address, rt.singleOverlay());
    as_vcvt(rt, rt.singleOverlay());
}

void
MacroAssemblerARMCompat::loadFloatAsDouble(const BaseIndex &src, const FloatRegister &dest)
{
    // VFP instructions don't even support register Base + register Index modes, so
    // just add the index, then handle the offset like normal
    Register base = src.base;
    Register index = src.index;
    uint32 scale = Imm32::ShiftOf(src.scale).value;
    int32 offset = src.offset;
    VFPRegister rt = dest;
    as_add(ScratchRegister, base, lsl(index, scale));

    ma_vdtr(IsLoad, Operand(ScratchRegister, offset), rt.singleOverlay());
    as_vcvt(rt, rt.singleOverlay());
}

void
MacroAssemblerARMCompat::store16(const Register &src, const Address &address)
{
    ma_dataTransferN(IsStore, 16, false, address.base, Imm32(address.offset), src);
}

void
MacroAssemblerARMCompat::store32(Register src, const AbsoluteAddress &address)
{
    storePtr(src, address);
}

void
MacroAssemblerARMCompat::store32(Register src, const Address &address)
{
    storePtr(src, address);
}

void
MacroAssemblerARMCompat::store32(Imm32 src, const Address &address)
{
    move32(src, ScratchRegister);
    storePtr(ScratchRegister, address);
}

void
MacroAssemblerARMCompat::storePtr(ImmWord imm, const Address &address)
{
    movePtr(imm, ScratchRegister);
    storePtr(ScratchRegister, address);
}
    
void
MacroAssemblerARMCompat::storePtr(ImmGCPtr imm, const Address &address)
{
    movePtr(imm, ScratchRegister);
    storePtr(ScratchRegister, address);
}

void
MacroAssemblerARMCompat::storePtr(Register src, const Address &address)
{
    ma_str(src, Operand(address));
}

void
MacroAssemblerARMCompat::storePtr(const Register &src, const AbsoluteAddress &dest)
{
    movePtr(ImmWord(dest.addr), ScratchRegister);
    storePtr(src, Address(ScratchRegister, 0x0));
}

void
MacroAssemblerARMCompat::cmp32(const Register &lhs, const Imm32 &rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmp32(const Register &lhs, const Register &rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(const Register &lhs, const ImmWord &rhs)
{
    ma_cmp(lhs, Imm32(rhs.value));
}

void
MacroAssemblerARMCompat::cmpPtr(const Register &lhs, const Register &rhs)
{
    ma_cmp(lhs, rhs);
}

void
MacroAssemblerARMCompat::cmpPtr(const Address &lhs, const Register &rhs)
{
    loadPtr(lhs, ScratchRegister);
    cmpPtr(ScratchRegister, rhs);
}

void
MacroAssemblerARMCompat::setStackArg(const Register &reg, uint32 arg)
{
    ma_dataTransferN(IsStore, 32, true, sp, Imm32(arg * STACK_SLOT_SIZE), reg);

}

void
MacroAssemblerARMCompat::subPtr(Imm32 imm, const Register dest)
{
    ma_sub(imm, dest);
}

void
MacroAssemblerARMCompat::addPtr(Imm32 imm, const Register dest)
{
    ma_add(imm, dest);
}

void
MacroAssemblerARMCompat::addPtr(Imm32 imm, const Address &dest)
{
    loadPtr(dest, ScratchRegister);
    addPtr(imm, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

void
MacroAssemblerARMCompat::compareDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs)
{
    if (rhs == InvalidFloatReg)
        ma_vcmpz(lhs);
    else
        ma_vcmp(lhs, rhs);
    as_vmrs(pc);
}

void
MacroAssemblerARMCompat::branchDouble(DoubleCondition cond, const FloatRegister &lhs,
                                      const FloatRegister &rhs, Label *label)
{
    compareDouble(cond, lhs, rhs);

    if (cond == DoubleNotEqual) {
        // Force the unordered cases not to jump.
        Label unordered;
        ma_b(&unordered, VFP_Unordered);
        ma_b(label, VFP_NotEqualOrUnordered);
        bind(&unordered);
        return;
    }
    if (cond == DoubleEqualOrUnordered) {
        ma_b(label, VFP_Unordered);
        ma_b(label, VFP_Equal);
        return;
    }

    ma_b(label, ConditionFromDoubleCondition(cond));
}

// higher level tag testing code
Operand ToPayload(Operand base) {
    return Operand(Register::FromCode(base.base()),
                   base.disp());
}
Operand ToType(Operand base) {
    return Operand(Register::FromCode(base.base()),
                   base.disp() + sizeof(void *));
}

Assembler::Condition
MacroAssemblerARMCompat::testInt32(Assembler::Condition cond, const ValueOperand &value)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Assembler::Condition cond, const ValueOperand &value)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN));
    return cond;
}
Assembler::Condition
MacroAssemblerARMCompat::testDouble(Assembler::Condition cond, const ValueOperand &value)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Assembler::Condition actual = (cond == Equal)
        ? Below
        : AboveOrEqual;
    ma_cmp(value.typeReg(), ImmTag(JSVAL_TAG_CLEAR));
    return actual;
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Assembler::Condition cond, const ValueOperand &value)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_NULL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Assembler::Condition cond, const ValueOperand &value)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_cmp(value.typeReg(), ImmType(JSVAL_TYPE_UNDEFINED));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Assembler::Condition cond, const ValueOperand &value)
{
    return testString(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Assembler::Condition cond, const ValueOperand &value)
{
    return testObject(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const ValueOperand &value)
{
    return testMagic(cond, value.typeReg());
}

Assembler::Condition
MacroAssemblerARMCompat::testPrimitive(Assembler::Condition cond, const ValueOperand &value)
{
    return testPrimitive(cond, value.typeReg());
}

// Register-based tests.
Assembler::Condition
MacroAssemblerARMCompat::testInt32(Assembler::Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_INT32));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testBoolean(Assembler::Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testNull(Assembler::Condition cond, const Register &tag) {
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_NULL));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testUndefined(Assembler::Condition cond, const Register &tag) {
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testString(Assembler::Condition cond, const Register &tag) {
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_STRING));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testObject(Assembler::Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_OBJECT));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testPrimitive(Assembler::Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET));
    return cond == Equal ? Below : AboveOrEqual;
}

Assembler::Condition
MacroAssemblerARMCompat::testGCThing(Assembler::Condition cond, const Address &address)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testGCThing(Assembler::Condition cond, const BaseIndex &address)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const Address &address)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testMagic(Assembler::Condition cond, const BaseIndex &address)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_cmp(ScratchRegister, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
}

Assembler::Condition
MacroAssemblerARMCompat::testDouble(Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    ma_cmp(tag, ImmTag(JSVAL_TAG_CLEAR));
    return actual;
}

Assembler::Condition
MacroAssemblerARMCompat::testNumber(Condition cond, const Register &tag)
{
    JS_ASSERT(cond == Equal || cond == NotEqual);
    ma_cmp(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET));
    return cond == Equal ? BelowOrEqual : Above;
}

void
MacroAssemblerARMCompat::branchTestValue(Condition cond, const ValueOperand &value, const Value &v,
                                         Label *label)
{
    // If cond == NotEqual, branch when a.payload != b.payload || a.tag != b.tag.
    // If the payloads are equal, compare the tags. If the payloads are not equal,
    // short circuit true (NotEqual).
    //
    // If cand == Equal, branch when a.payload == b.payload && a.tag == b.tag.
    // If the payloads are equal, compare the tags. If the payloads are not equal,
    // short circuit false (NotEqual).
    jsval_layout jv = JSVAL_TO_IMPL(v);
    if (v.isMarkable())
        ma_cmp(value.payloadReg(), ImmGCPtr(reinterpret_cast<gc::Cell *>(v.toGCThing())));
    else
        ma_cmp(value.payloadReg(), Imm32(jv.s.payload.i32));
    ma_cmp(value.typeReg(), Imm32(jv.s.tag), Equal);
    ma_b(label, cond);
}

// unboxing code
void
MacroAssemblerARMCompat::unboxInt32(const ValueOperand &operand, const Register &dest)
{
    ma_mov(operand.payloadReg(), dest);
}

void
MacroAssemblerARMCompat::unboxInt32(const Address &src, const Register &dest)
{
    ma_ldr(payloadOf(src), dest);
}

void
MacroAssemblerARMCompat::unboxBoolean(const ValueOperand &operand, const Register &dest)
{
    ma_mov(operand.payloadReg(), dest);
}

void
MacroAssemblerARMCompat::unboxDouble(const ValueOperand &operand, const FloatRegister &dest)
{
    JS_ASSERT(dest != ScratchFloatReg);
    as_vxfer(operand.payloadReg(), operand.typeReg(),
             VFPRegister(dest), CoreToFloat);
}

void
MacroAssemblerARMCompat::unboxValue(const ValueOperand &src, AnyRegister dest)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.payloadReg(), dest.fpu());
        ma_b(&end);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else {
        if (src.payloadReg() != dest.gpr())
            as_mov(dest.gpr(), O2Reg(src.payloadReg()));
    }
}

void
MacroAssemblerARMCompat::boxDouble(const FloatRegister &src, const ValueOperand &dest)
{
    as_vxfer(dest.payloadReg(), dest.typeReg(),
             VFPRegister(src), FloatToCore);
}


void
MacroAssemblerARMCompat::boolValueToDouble(const ValueOperand &operand, const FloatRegister &dest)
{
    VFPRegister d = VFPRegister(dest);
    ma_vimm(1.0, dest);
    ma_cmp(operand.payloadReg(), Imm32(0));
    as_vsub(d, d, d, NotEqual);
}

void
MacroAssemblerARMCompat::int32ValueToDouble(const ValueOperand &operand, const FloatRegister &dest)
{
    // transfer the integral value to a floating point register
    VFPRegister vfpdest = VFPRegister(dest);
    as_vxfer(operand.payloadReg(), InvalidReg,
             vfpdest.sintOverlay(), CoreToFloat);
    // convert the value to a double.
    as_vcvt(vfpdest, vfpdest.sintOverlay());
}

void
MacroAssemblerARMCompat::loadInt32OrDouble(const Operand &src, const FloatRegister &dest)
{
    Label notInt32, end;
    // If it's an int, convert it to double.
    ma_ldr(ToType(src), ScratchRegister);
    branchTestInt32(Assembler::NotEqual, ScratchRegister, &notInt32);
    ma_ldr(ToPayload(src), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    ma_b(&end);

    // Not an int, just load as double.
    bind(&notInt32);
    ma_vldr(src, dest);
    bind(&end);
}

void
MacroAssemblerARMCompat::loadInt32OrDouble(Register base, Register index, const FloatRegister &dest, int32 shift)
{
    Label notInt32, end;

    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);

    // If it's an int, convert it to double.
    ma_alu(base, lsl(index, shift), ScratchRegister, op_add);

    // Since we only have one scratch register, we need to stomp over it with the tag
    ma_ldr(Address(ScratchRegister, NUNBOX32_TYPE_OFFSET), ScratchRegister);
    branchTestInt32(Assembler::NotEqual, ScratchRegister, &notInt32);

    // Implicitly requires NUNBOX32_PAYLOAD_OFFSET == 0: no offset provided
    ma_ldr(DTRAddr(base, DtrRegImmShift(index, LSL, shift)), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    ma_b(&end);

    // Not an int, just load as double.
    bind(&notInt32);
    // First, recompute the offset that had been stored in the scratch register
    // since the scratch register was overwritten loading in the type.
    ma_alu(base, lsl(index, shift), ScratchRegister, op_add);
    ma_vldr(Address(ScratchRegister, 0), dest);
    bind(&end);
}

void
MacroAssemblerARMCompat::loadConstantDouble(double dp, const FloatRegister &dest)
{
    as_FImm64Pool(dest, dp);
}

void
MacroAssemblerARMCompat::loadStaticDouble(const double *dp, const FloatRegister &dest)
{
    loadConstantDouble(*dp, dest);
}
    // treat the value as a boolean, and set condition codes accordingly

Assembler::Condition
MacroAssemblerARMCompat::testInt32Truthy(bool truthy, const ValueOperand &operand)
{
    ma_tst(operand.payloadReg(), operand.payloadReg());
    return truthy ? NonZero : Zero;
}

Assembler::Condition
MacroAssemblerARMCompat::testBooleanTruthy(bool truthy, const ValueOperand &operand)
{
    ma_tst(operand.payloadReg(), operand.payloadReg());
    return truthy ? NonZero : Zero;
}

Assembler::Condition
MacroAssemblerARMCompat::testDoubleTruthy(bool truthy, const FloatRegister &reg)
{
    as_vcmpz(VFPRegister(reg));
    as_vmrs(pc);
    as_cmp(r0, O2Reg(r0), Overflow);
    return truthy ? NonZero : Zero;
}

Register
MacroAssemblerARMCompat::extractObject(const Address &address, Register scratch)
{
    ma_ldr(payloadOf(address), scratch);
    return scratch;
}

Register
MacroAssemblerARMCompat::extractTag(const Address &address, Register scratch)
{
    ma_ldr(tagOf(address), scratch);
    return scratch;
}

Register
MacroAssemblerARMCompat::extractTag(const BaseIndex &address, Register scratch)
{
    ma_alu(address.base, lsl(address.index, address.scale), scratch, op_add, NoSetCond);
    return extractTag(Address(scratch, address.offset), scratch);
}

void
MacroAssemblerARMCompat::moveValue(const Value &val, Register type, Register data) {
    jsval_layout jv = JSVAL_TO_IMPL(val);
    ma_mov(Imm32(jv.s.tag), type);
    ma_mov(Imm32(jv.s.payload.i32), data);
}
void
MacroAssemblerARMCompat::moveValue(const Value &val, const ValueOperand &dest) {
    moveValue(val, dest.typeReg(), dest.payloadReg());
}

/////////////////////////////////////////////////////////////////
// X86/X64-common (ARM too now) interface.
/////////////////////////////////////////////////////////////////
void
MacroAssemblerARMCompat::storeValue(ValueOperand val, Operand dst) {
    ma_str(val.payloadReg(), ToPayload(dst));
    ma_str(val.typeReg(), ToType(dst));
}

void
MacroAssemblerARMCompat::storeValue(ValueOperand val, Register base, Register index, int32 shift)
{
    if (isValueDTRDCandidate(val)) {
        Register tmpIdx;
        if (shift == 0) {
            tmpIdx = index;
        } else if (shift < 0) {
            ma_asr(Imm32(-shift), index, ScratchRegister);
            tmpIdx = ScratchRegister;
        } else {
            ma_lsl(Imm32(shift), index, ScratchRegister);
            tmpIdx = ScratchRegister;
        }
        ma_strd(val.payloadReg(), val.typeReg(), EDtrAddr(base, EDtrOffReg(tmpIdx)));
    } else {
        // The ideal case is the base is dead so the sequence:
        // str val.payloadReg(), [base, index LSL shift]!
        // str val.typeReg(), [base, #4]
        // only clobbers dead registers
        // Sadly, this information is not available, so the scratch register is necessary.
        ma_alu(base, lsl(index, shift), ScratchRegister, op_add);

        // This case is handled by a simpler function.
        storeValue(val, Address(ScratchRegister, 0));
    }
}

void
MacroAssemblerARMCompat::loadValue(Register base, Register index, ValueOperand val)
{
    if (isValueDTRDCandidate(val)) {
        ma_lsl(Imm32(TimesEight), index, ScratchRegister);
        ma_ldrd(EDtrAddr(base, EDtrOffReg(ScratchRegister)), val.payloadReg(), val.typeReg());
    } else {
        // The ideal case is the base is dead so the sequence:
        // ldr val.payloadReg(), [base, index LSL shift]!
        // ldr val.typeReg(), [base, #4]
        // only clobbers dead registers
        // Sadly, this information is not available, so the scratch register is necessary.
        ma_alu(base, lsl(index, TimesEight), ScratchRegister, op_add);

        // This case is handled by a simpler function.
        loadValue(Address(ScratchRegister, 0), val);
    }
}

void
MacroAssemblerARMCompat::loadValue(Address src, ValueOperand val)
{
    Operand srcOp = Operand(src);
    Operand payload = ToPayload(srcOp);
    Operand type = ToType(srcOp);
    // TODO: copy this code into a generic function that acts on all sequences of memory accesses
    if (isValueDTRDCandidate(val)) {
        // If the value we want is in two consecutive registers starting with an even register,
        // they can be combined as a single ldrd.
        int offset = srcOp.disp();
        if (offset < 256 && offset > -256) {
            ma_ldrd(EDtrAddr(Register::FromCode(srcOp.base()), EDtrOffImm(srcOp.disp())), val.payloadReg(), val.typeReg());
            return;
        }
    }
    // if the value is lower than the type, then we may be able to use an ldm instruction

    if (val.payloadReg().code() < val.typeReg().code()) {
        if (srcOp.disp() <= 4 && srcOp.disp() >= -8 && (srcOp.disp() & 3) == 0) {
            // turns out each of the 4 value -8, -4, 0, 4 corresponds exactly with one of
            // LDM{DB, DA, IA, IB}
            DTMMode mode;
            switch(srcOp.disp()) {
              case -8:
                mode = DB;
                break;
              case -4:
                mode = DA;
                break;
              case 0:
                mode = IA;
                break;
              case 4:
                mode = IB;
                break;
              default:
                JS_NOT_REACHED("Bogus Offset for LoadValue as DTM");
            }
            startDataTransferM(IsLoad, Register::FromCode(srcOp.base()), mode);
            transferReg(val.payloadReg());
            transferReg(val.typeReg());
            finishDataTransfer();
            return;
        }
    }
    // Ensure that loading the payload does not erase the pointer to the
    // Value in memory.
    if (Register::FromCode(type.base()) != val.payloadReg()) {
        ma_ldr(payload, val.payloadReg());
        ma_ldr(type, val.typeReg());
    } else {
        ma_ldr(type, val.typeReg());
        ma_ldr(payload, val.payloadReg());
    }
}

void
MacroAssemblerARMCompat::tagValue(JSValueType type, Register payload, ValueOperand dest)
{
    JS_ASSERT(payload != dest.typeReg());
    ma_mov(ImmType(type), dest.typeReg());
    if (payload != dest.payloadReg())
        ma_mov(payload, dest.payloadReg());
}

void
MacroAssemblerARMCompat::pushValue(ValueOperand val) {
    ma_push(val.typeReg());
    ma_push(val.payloadReg());
}
void
MacroAssemblerARMCompat::popValue(ValueOperand val) {
    ma_pop(val.payloadReg());
    ma_pop(val.typeReg());
}
void
MacroAssemblerARMCompat::storePayload(const Value &val, Operand dest)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    if (val.isMarkable()) {
        ma_mov(ImmGCPtr((gc::Cell *)jv.s.payload.ptr), lr);
    } else {
        ma_mov(Imm32(jv.s.payload.i32), lr);
    }
    ma_str(lr, ToPayload(dest));
}
void
MacroAssemblerARMCompat::storePayload(Register src, Operand dest)
{
    if (dest.getTag() == Operand::MEM) {
        ma_str(src, ToPayload(dest));
        return;
    }
    JS_NOT_REACHED("why do we do all of these things?");

}

void
MacroAssemblerARMCompat::storePayload(const Value &val, Register base, Register index, int32 shift)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    if (val.isMarkable()) {
        ma_mov(ImmGCPtr((gc::Cell *)jv.s.payload.ptr), ScratchRegister);
    } else {
        ma_mov(Imm32(jv.s.payload.i32), ScratchRegister);
    }
    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);
    // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index << shift + imm]
    // cannot be encoded into a single instruction, and cannot be integrated into the as_dtr call.
    as_dtr(IsStore, 32, Offset, ScratchRegister, DTRAddr(base, DtrRegImmShift(index, LSL, shift)));
}
void
MacroAssemblerARMCompat::storePayload(Register src, Register base, Register index, int32 shift)
{
    JS_ASSERT((shift < 32) && (shift >= 0));
    // If NUNBOX32_PAYLOAD_OFFSET is not zero, the memory operand [base + index << shift + imm]
    // cannot be encoded into a single instruction, and cannot be integrated into the as_dtr call.
    JS_STATIC_ASSERT(NUNBOX32_PAYLOAD_OFFSET == 0);
    // Technically, shift > -32 can be handle by changing LSL to ASR, but should never come up,
    // and this is one less code path to get wrong.
    as_dtr(IsStore, 32, Offset, src, DTRAddr(base, DtrRegImmShift(index, LSL, shift)));
}

void
MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, Operand dest) {
    if (dest.getTag() == Operand::MEM) {
        ma_mov(tag, lr);
        ma_str(lr, ToType(dest));
        return;
    }

    JS_NOT_REACHED("why do we do all of these things?");

}

void
MacroAssemblerARMCompat::storeTypeTag(ImmTag tag, Register base, Register index, int32 shift) {
    JS_ASSERT(base != ScratchRegister);
    JS_ASSERT(index != ScratchRegister);
    // A value needs to be store a value int base + index << shift + 4.
    // Arm cannot handle this in a single operand, so a temp register is required.
    // However, the scratch register is presently in use to hold the immediate that
    // is being stored into said memory location. Work around this by modifying
    // the base so the valid [base + index << shift] format can be used, then
    // restore it.
    ma_add(base, Imm32(NUNBOX32_TYPE_OFFSET), base);
    ma_mov(tag, ScratchRegister);
    ma_str(ScratchRegister, DTRAddr(base, DtrRegImmShift(index, LSL, shift)));
    ma_sub(base, Imm32(NUNBOX32_TYPE_OFFSET), base);
}

void
MacroAssemblerARMCompat::linkExitFrame() {
    uint8 *dest = ((uint8*)GetIonContext()->cx->runtime) + offsetof(JSRuntime, ionTop);
    movePtr(ImmWord(dest), ScratchRegister);
    ma_str(StackPointer, Operand(ScratchRegister, 0));
}

// ARM says that all reads of pc will return 8 higher than the
// address of the currently executing instruction.  This means we are
// correctly storing the address of the instruction after the call
// in the register.
// Also ION is breaking the ARM EABI here (sort of). The ARM EABI
// says that a function call should move the pc into the link register,
// then branch to the function, and *sp is data that is owned by the caller,
// not the callee.  The ION ABI says *sp should be the address that
// we will return to when leaving this function
void
MacroAssemblerARM::ma_callIon(const Register r)
{
    // When the stack is 8 byte aligned,
    // we want to decrement sp by 8, and write pc+8 into the new sp.
    // when we return from this call, sp will be its present value minus 4.
    as_dtr(IsStore, 32, PreIndex, pc, DTRAddr(sp, DtrOffImm(-8)));
    as_blx(r);
}
void
MacroAssemblerARM::ma_callIonNoPush(const Register r)
{
    // Since we just write the return address into the stack, which is
    // popped on return, the net effect is removing 4 bytes from the stack
    as_dtr(IsStore, 32, Offset, pc, DTRAddr(sp, DtrOffImm(0)));
    as_blx(r);
}

void
MacroAssemblerARM::ma_callIonHalfPush(const Register r)
{
    // The stack is unaligned by 4 bytes.
    // We push the pc to the stack to align the stack before the call, when we
    // return the pc is poped and the stack is restored to its unaligned state.
    ma_push(pc);
    as_blx(r);
}

void
MacroAssemblerARM::ma_call(void *dest)
{
    ma_mov(Imm32((uint32)dest), CallReg);
    as_blx(CallReg);
}

void
MacroAssemblerARMCompat::breakpoint()
{
    as_bkpt();
}

void
MacroAssemblerARMCompat::setupABICall(uint32 args)
{
    JS_ASSERT(!inCall_);
    inCall_ = true;
    args_ = args;
    passedArgs_ = 0;
    usedSlots_ = 0;
    floatArgsInGPR[0] = VFPRegister();
    floatArgsInGPR[1] = VFPRegister();
}

void
MacroAssemblerARMCompat::setupAlignedABICall(uint32 args)
{
    setupABICall(args);

    dynamicAlignment_ = false;
}

void
MacroAssemblerARMCompat::setupUnalignedABICall(uint32 args, const Register &scratch)
{
    setupABICall(args);
    dynamicAlignment_ = true;

    ma_mov(sp, scratch);

    // Force sp to be aligned
    ma_and(Imm32(~(StackAlignment - 1)), sp, sp);
    ma_push(scratch);
}

void
MacroAssemblerARMCompat::passABIArg(const MoveOperand &from)
{
    MoveOperand to;
    uint32 increment = 1;
    bool useResolver = true;
    ++passedArgs_;
    Move::Kind kind = Move::GENERAL;
    if (from.isDouble()) {
        // Double arguments need to be rounded up to the nearest doubleword
        // boundary, even if it is in a register!
        usedSlots_ = (usedSlots_ + 1) & ~1;
        increment = 2;
        kind = Move::DOUBLE;
    }

    Register destReg;
    MoveOperand dest;
    if (GetArgReg(usedSlots_, &destReg)) {
        if (from.isDouble()) {
            floatArgsInGPR[destReg.code() >> 1] = VFPRegister(from.floatReg());
            useResolver = false;
        } else {
            dest = MoveOperand(destReg);
        }
    } else {
        uint32 disp = GetArgStackDisp(usedSlots_);
        dest = MoveOperand(sp, disp);
    }

    if (useResolver)
        enoughMemory_ = enoughMemory_ && moveResolver_.addMove(from, dest, kind);
    usedSlots_ += increment;
}

void
MacroAssemblerARMCompat::passABIArg(const Register &reg)
{
    passABIArg(MoveOperand(reg));
}

void
MacroAssemblerARMCompat::passABIArg(const FloatRegister &freg)
{
    passABIArg(MoveOperand(freg));
}

void MacroAssemblerARMCompat::checkStackAlignment()
{
#ifdef DEBUG
        Label good;
        ma_tst(sp, Imm32(StackAlignment - 1));
        ma_b(&good, Equal);
        breakpoint();
        bind(&good);
#endif
}

void
MacroAssemblerARMCompat::callWithABI(void *fun, Result result)
{
    JS_ASSERT(inCall_);
    uint32 stackAdjust = ((usedSlots_ > NumArgRegs) ? usedSlots_ - NumArgRegs : 0) * STACK_SLOT_SIZE;
    if (!dynamicAlignment_)
        stackAdjust +=
            ComputeByteAlignment(framePushed_ + stackAdjust, StackAlignment);
    else
        // STACK_SLOT_SIZE account for the saved stack pointer pushed by setupUnalignedABICall
        stackAdjust += ComputeByteAlignment(stackAdjust + STACK_SLOT_SIZE, StackAlignment);

    reserveStack(stackAdjust);
    // Position all arguments.
    {
        enoughMemory_ = enoughMemory_ && moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }
    for (int i = 0; i < 2; i++) {
        if (!floatArgsInGPR[i].isInvalid()) {
            ma_vxfer(floatArgsInGPR[i], Register::FromCode(i*2), Register::FromCode(i*2+1));
        }
    }
    checkStackAlignment();
    ma_call(fun);

    freeStack(stackAdjust);
    if (dynamicAlignment_) {
        // x86 supports pop esp.  on arm, that isn't well defined, so just
        // do it manually
        as_dtr(IsLoad, 32, Offset, sp, DTRAddr(sp, DtrOffImm(0)));
    }

    JS_ASSERT(inCall_);
    inCall_ = false;
}

void
MacroAssemblerARMCompat::handleException()
{
    // Reserve space for exception information.
    int size = (sizeof(ResumeFromException) + 7) & ~7;
    ma_sub(Imm32(size), sp);
    ma_mov(sp, r0);

    // Ask for an exception handler.
    setupAlignedABICall(1);
    passABIArg(r0);
    callWithABI(JS_FUNC_TO_DATA_PTR(void *, ion::HandleException));
    // Load the error value, load the new stack pointer, and return.
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    ma_ldr(Operand(sp, offsetof(ResumeFromException, stackPointer)), sp);

    // We're going to be returning by the ion calling convention, which returns
    // by ??? (for now, I think ldr pc, [sp]!)
    as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(4)));

}

Assembler::Condition
MacroAssemblerARMCompat::testStringTruthy(bool truthy, const ValueOperand &value)
{
    Register string = value.payloadReg();

    size_t mask = (0xFFFFFFFF << JSString::LENGTH_SHIFT);
    ma_dtr(IsLoad, string, Imm32(JSString::offsetOfLengthAndFlags()), ScratchRegister);
    // Bit clear into the scratch register. This is done because there is performs the operation
    // dest <- src1 & ~ src2. There is no instruction that does this without writing
    // the result somewhere, so the Scratch Register is sacrificed.
    ma_bic(Imm32(~mask), ScratchRegister, SetCond);
    return truthy ? Assembler::NonZero : Assembler::Zero;
}
