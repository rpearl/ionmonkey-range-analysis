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
 *   David Anderson <danderson@mozilla.com>
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

#include "LIR.h"
#include "Lowering.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "IonSpewer.h"
#include "jsanalyze.h"
#include "jsbool.h"
#include "jsnum.h"
#include "jsobjinlines.h"
#include "shared/Lowering-shared-inl.h"

using namespace js;
using namespace ion;

bool
LIRGenerator::visitParameter(MParameter *param)
{
    ptrdiff_t offset;
    if (param->index() == MParameter::THIS_SLOT)
        offset = THIS_FRAME_SLOT;
    else
        offset = 1 + param->index();

    LParameter *ins = new LParameter;
    if (!defineBox(ins, param, LDefinition::PRESET))
        return false;

    offset *= sizeof(Value);
#if defined(JS_NUNBOX32)
# if defined(IS_BIG_ENDIAN)
    ins->getDef(0)->setOutput(LArgument(offset));
    ins->getDef(1)->setOutput(LArgument(offset + 4));
# else
    ins->getDef(0)->setOutput(LArgument(offset + 4));
    ins->getDef(1)->setOutput(LArgument(offset));
# endif
#elif defined(JS_PUNBOX64)
    ins->getDef(0)->setOutput(LArgument(offset));
#endif

    return true;
}

bool
LIRGenerator::visitCallee(MCallee *callee)
{
    LCallee *ins = new LCallee;
    if (!define(ins, callee, LDefinition::PRESET))
        return false;

    ins->getDef(0)->setOutput(LArgument(-sizeof(IonJSFrameLayout)
                                        + IonJSFrameLayout::offsetOfCalleeToken()));

    return true;
}

bool
LIRGenerator::visitGoto(MGoto *ins)
{
    return add(new LGoto(ins->target()));
}

bool
LIRGenerator::visitCheckOverRecursed(MCheckOverRecursed *ins)
{
    LCheckOverRecursed *lir = new LCheckOverRecursed(temp());

    if (!add(lir))
        return false;
    if (!assignSafepoint(lir, ins))
        return false;

    return true;
}

bool
LIRGenerator::visitDefVar(MDefVar *ins)
{
    LAllocation scopeChain = useRegister(ins->scopeChain());
    LDefVar *lir = new LDefVar(scopeChain, temp());

    lir->setMir(ins);

    if (!add(lir))
        return false;
    if (!assignSafepoint(lir, ins))
        return false;

    return true;
}

bool
LIRGenerator::visitNewArray(MNewArray *ins)
{
    LNewArray *lir = new LNewArray();
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitNewObject(MNewObject *ins)
{
    LNewObject *lir = new LNewObject();
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitInitProp(MInitProp *ins)
{
    LInitProp *lir = new LInitProp(useRegister(ins->getObject()));
    if (!useBox(lir, LInitProp::ValueIndex, ins->getValue()))
        return false;

    return add(lir) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitPrepareCall(MPrepareCall *ins)
{
    allocateArguments(ins->argc());
    return true;
}

bool
LIRGenerator::visitPassArg(MPassArg *arg)
{
    MDefinition *opd = arg->getArgument();
    JS_ASSERT(opd->type() == MIRType_Value);

    uint32 argslot = getArgumentSlot(arg->getArgnum());

    LStackArg *stack = new LStackArg(argslot);
    if (!useBox(stack, 0, opd))
        return false;

    // Pass through the virtual register of the operand.
    // This causes snapshots to correctly copy the operand on the stack.
    //
    // This keeps the backing store around longer than strictly required.
    // We could do better by informing snapshots about the argument vector.
    arg->setVirtualRegister(opd->virtualRegister());

    return add(stack);
}

bool
LIRGenerator::visitCreateThis(MCreateThis *ins)
{
    LCreateThis *lir = new LCreateThis(useRegister(ins->getCallee()),
                                       useRegister(ins->getPrototype()));
    
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCall(MCall *call)
{
    JS_ASSERT(CallTempReg0 != CallTempReg1);
    JS_ASSERT(CallTempReg0 != ArgumentsRectifierReg);
    JS_ASSERT(CallTempReg1 != ArgumentsRectifierReg);
    JS_ASSERT(call->getFunction()->type() == MIRType_Object);

    // Height of the current argument vector.
    uint32 argslot = getArgumentSlotForCall();
    JSFunction *target = call->getSingleTarget();

    if (target && target->isNative()) {
        LCallNative *lir = new LCallNative(argslot, tempFixed(CallTempReg0),
                tempFixed(CallTempReg1), tempFixed(CallTempReg2), tempFixed(CallTempReg3));

        if (!defineReturn(lir, call))
            return false;
        if (!assignSafepoint(lir, call))
            return false;
    } else if (!target && call->isConstructing()) {
        LCallConstructor *lir = new LCallConstructor(useFixed(call->getFunction(), CallTempReg0),
                                                       argslot);
        if (!defineVMReturn(lir, call))
            return false;
        if (!assignSafepoint(lir, call))
            return false;
    } else {
        LCallGeneric *lir = new LCallGeneric(useFixed(call->getFunction(), CallTempReg0),
            argslot, tempFixed(ArgumentsRectifierReg), tempFixed(CallTempReg2));

        // Bailout is only needed in the case of possible non-JSFunction callee.
        if (!target && !assignSnapshot(lir))
            return false;

        if (!defineReturn(lir, call))
            return false;
        if (!assignSafepoint(lir, call))
            return false;
    }

    freeArguments(call->argc());
    return true;
}

static JSOp
ReorderComparison(JSOp op, MDefinition **lhsp, MDefinition **rhsp)
{
    MDefinition *lhs = *lhsp;
    MDefinition *rhs = *rhsp;

    if (lhs->isConstant()) {
        *rhsp = lhs;
        *lhsp = rhs;
        return js::analyze::ReverseCompareOp(op);
    }
    return op;
}

bool
LIRGenerator::visitTest(MTest *test)
{
    MDefinition *opd = test->getOperand(0);
    MBasicBlock *ifTrue = test->ifTrue();
    MBasicBlock *ifFalse = test->ifFalse();

    if (opd->type() == MIRType_Value) {
        LTestVAndBranch *lir = new LTestVAndBranch(ifTrue, ifFalse, tempFloat());
        if (!useBox(lir, LTestVAndBranch::Input, opd))
            return false;
        return add(lir);
    }

    // These must be explicitly sniffed out since they are constants and have
    // no payload.
    if (opd->type() == MIRType_Undefined || opd->type() == MIRType_Null)
        return add(new LGoto(ifFalse));

    // Objects are easy, too.
    if (opd->type() == MIRType_Object)
        return add(new LGoto(ifTrue));

    // Check if the operand for this test is a compare operation. If it is, we want
    // to emit an LCompare*AndBranch rather than an LTest*AndBranch, to fuse the
    // compare and jump instructions.
    if (opd->isCompare()) {
        MCompare *comp = opd->toCompare();
        MDefinition *left = comp->getOperand(0);
        MDefinition *right = comp->getOperand(1);

        if (comp->specialization() == MIRType_Int32 || comp->specialization() == MIRType_Object) {
            JSOp op = ReorderComparison(comp->jsop(), &left, &right);
            LAllocation rhs = comp->specialization() == MIRType_Object
                              ? useRegister(right)
                              : useAnyOrConstant(right);
            return add(new LCompareAndBranch(opd->toCompare(), op, useRegister(left), rhs,
                                             ifTrue, ifFalse));
        }
        if (comp->specialization() == MIRType_Double) {
            return add(new LCompareDAndBranch(comp->jsop(), useRegister(left),
                                              useRegister(right), ifTrue, ifFalse));
        }

        // The second operand has known null/undefined type, so just test the
        // first operand.
        if (IsNullOrUndefined(comp->specialization())) {
            LIsNullOrUndefinedAndBranch *lir = new LIsNullOrUndefinedAndBranch(ifTrue, ifFalse);
            if (!useBox(lir, LIsNullOrUndefinedAndBranch::Value, comp->getOperand(0)))
                return false;
            return add(lir, comp);
        }
    }

    if (opd->type() == MIRType_Double)
        return add(new LTestDAndBranch(useRegister(opd), ifTrue, ifFalse));

    JS_ASSERT(opd->type() == MIRType_Int32 || opd->type() == MIRType_Boolean);
    return add(new LTestIAndBranch(useRegister(opd), ifTrue, ifFalse));
}

static inline bool
CanEmitCompareAtUses(MInstruction *ins)
{
    if (!ins->canEmitAtUses())
        return false;

    bool foundTest = false;
    for (MUseIterator iter(ins->usesBegin()); iter != ins->usesEnd(); iter++) {
        MNode *node = iter->node();
        if (!node->isDefinition())
            return false;
        if (!node->toDefinition()->isTest())
            return false;
        if (foundTest)
            return false;
        foundTest = true;
    }
    return true;
}

bool
LIRGenerator::visitCompare(MCompare *comp)
{
    MDefinition *left = comp->getOperand(0);
    MDefinition *right = comp->getOperand(1);

    if (comp->specialization() != MIRType_None) {
        // Sniff out if the output of this compare is used only for a branching.
        // If it is, then we willl emit an LCompare*AndBranch instruction in place
        // of this compare and any test that uses this compare. Thus, we can
        // ignore this Compare.
        if (CanEmitCompareAtUses(comp))
            return emitAtUses(comp);

        if (comp->specialization() == MIRType_Int32 || comp->specialization() == MIRType_Object) {
            JSOp op = ReorderComparison(comp->jsop(), &left, &right);
            LAllocation rhs = comp->specialization() == MIRType_Object
                              ? useRegister(right)
                              : useAnyOrConstant(right);
            return define(new LCompare(op, useRegister(left), rhs), comp);
        }

        if (comp->specialization() == MIRType_Double)
            return define(new LCompareD(comp->jsop(), useRegister(left), useRegister(right)), comp);

        JS_ASSERT(IsNullOrUndefined(comp->specialization()));

        LIsNullOrUndefined *lir = new LIsNullOrUndefined();
        if (!useBox(lir, LIsNullOrUndefined::Value, comp->getOperand(0)))
            return false;
        return define(lir, comp);
    }

    LCompareV *lir = new LCompareV(comp->jsop());
    if (!useBox(lir, LCompareV::LhsInput, left))
        return false;
    if (!useBox(lir, LCompareV::RhsInput, right))
        return false;
    return defineVMReturn(lir, comp) && assignSafepoint(lir, comp);
}

static void
ReorderCommutative(MDefinition **lhsp, MDefinition **rhsp)
{
    MDefinition *lhs = *lhsp;
    MDefinition *rhs = *rhsp;

    // Put the constant in the left-hand side, if there is one.
    if (lhs->isConstant()) {
        *rhsp = lhs;
        *lhsp = rhs;
    }
}

bool
LIRGenerator::lowerBitOp(JSOp op, MInstruction *ins)
{
    MDefinition *lhs = ins->getOperand(0);
    MDefinition *rhs = ins->getOperand(1);

    if (lhs->type() == MIRType_Int32 && rhs->type() == MIRType_Int32) {
        ReorderCommutative(&lhs, &rhs);
        return lowerForALU(new LBitOpI(op), ins, lhs, rhs);
    }

    LBitOpV *lir = new LBitOpV(op);
    if (!useBox(lir, LBitOpV::LhsInput, lhs))
        return false;
    if (!useBox(lir, LBitOpV::RhsInput, rhs))
        return false;

    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitTypeOf(MTypeOf *ins)
{
    MDefinition *opd = ins->input();
    JS_ASSERT(opd->type() == MIRType_Value);

    LTypeOfV *lir = new LTypeOfV();
    if (!useBox(lir, LTypeOfV::Input, opd))
        return false;
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitToId(MToId *ins)
{
    LToIdV *lir = new LToIdV();
    if (!useBox(lir, LToIdV::Object, ins->lhs()))
        return false;
    if (!useBox(lir, LToIdV::Index, ins->rhs()))
        return false;
    if (!defineVMReturn(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitBitNot(MBitNot *ins)
{
    MDefinition *input = ins->getOperand(0);

    if (input->type() == MIRType_Int32)
        return lowerForALU(new LBitNotI(), ins, input);

    LBitNotV *lir = new LBitNotV;
    if (!useBox(lir, LBitNotV::Input, input))
        return false;
    if (!defineVMReturn(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitBitAnd(MBitAnd *ins)
{
    return lowerBitOp(JSOP_BITAND, ins);
}

bool
LIRGenerator::visitBitOr(MBitOr *ins)
{
    return lowerBitOp(JSOP_BITOR, ins);
}

bool
LIRGenerator::visitBitXor(MBitXor *ins)
{
    return lowerBitOp(JSOP_BITXOR, ins);
}

bool
LIRGenerator::lowerShiftOp(JSOp op, MInstruction *ins)
{
    MDefinition *lhs = ins->getOperand(0);
    MDefinition *rhs = ins->getOperand(1);

    if (lhs->type() == MIRType_Int32 && rhs->type() == MIRType_Int32) {
        LShiftOp *lir = new LShiftOp(op);
        if (op == JSOP_URSH) {
            MUrsh *ursh = ins->toUrsh();
            if (ursh->fallible() && !assignSnapshot(lir))
                return false;
        }
        return lowerForShift(lir, ins, lhs, rhs);
    }

    if (op == JSOP_URSH) {
        LBinaryV *lir = new LBinaryV(op);
        if (!useBox(lir, LBinaryV::LhsInput, lhs))
            return false;
        if (!useBox(lir, LBinaryV::RhsInput, rhs))
            return false;
        return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
    } else {
        LBitOpV *lir = new LBitOpV(op);
        if (!useBox(lir, LBitOpV::LhsInput, lhs))
            return false;
        if (!useBox(lir, LBitOpV::RhsInput, rhs))
            return false;
        return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
    }
}

bool
LIRGenerator::visitLsh(MLsh *ins)
{
    return lowerShiftOp(JSOP_LSH, ins);
}

bool
LIRGenerator::visitRsh(MRsh *ins)
{
    return lowerShiftOp(JSOP_RSH, ins);
}

bool
LIRGenerator::visitUrsh(MUrsh *ins)
{
    return lowerShiftOp(JSOP_URSH, ins);
}

bool
LIRGenerator::visitRound(MRound *ins)
{
    JS_ASSERT(ins->num()->type() == MIRType_Double);
    LRound *lir = new LRound(useRegister(ins->num()));
    if (!assignSnapshot(lir))
        return false;
    return define(lir, ins);
}

bool
LIRGenerator::visitAbs(MAbs *ins)
{
    MDefinition *num = ins->num();

    if (num->type() == MIRType_Int32) {
        LAbsI *lir = new LAbsI(useRegisterAtStart(num));
        // needed to handle abs(INT32_MIN)
        if (!assignSnapshot(lir))
            return false;
        return defineReuseInput(lir, ins, 0);
    } else {
        JS_ASSERT(num->type() == MIRType_Double);
        LAbsD *lir = new LAbsD(useRegister(num));
        return define(lir, ins);
    }
}

bool
LIRGenerator::visitSqrt(MSqrt *ins)
{
    MDefinition *num = ins->num();
    JS_ASSERT(num->type() == MIRType_Double);
    LSqrtD *lir = new LSqrtD(useRegister(num));
    return define(lir, ins);
}

bool
LIRGenerator::visitAdd(MAdd *ins)
{
    MDefinition *lhs = ins->getOperand(0);
    MDefinition *rhs = ins->getOperand(1);

    JS_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        JS_ASSERT(lhs->type() == MIRType_Int32);
        ReorderCommutative(&lhs, &rhs);
        LAddI *lir = new LAddI;
        if (!ins->isTruncated() && !assignSnapshot(lir))
            return false;

        return lowerForALU(lir, ins, lhs, rhs);
    }

    if (ins->specialization() == MIRType_Double) {
        JS_ASSERT(lhs->type() == MIRType_Double);
        return lowerForFPU(new LMathD(JSOP_ADD), ins, lhs, rhs);
    }

    return lowerBinaryV(JSOP_ADD, ins);
}

bool
LIRGenerator::visitSub(MSub *ins)
{
    MDefinition *lhs = ins->lhs();
    MDefinition *rhs = ins->rhs();

    JS_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        JS_ASSERT(lhs->type() == MIRType_Int32);
        LSubI *lir = new LSubI;
        if (!ins->isTruncated() && !assignSnapshot(lir))
            return false;

        return lowerForALU(lir, ins, lhs, rhs);
    }
    if (ins->specialization() == MIRType_Double) {
        JS_ASSERT(lhs->type() == MIRType_Double);
        return lowerForFPU(new LMathD(JSOP_SUB), ins, lhs, rhs);
    }

    return lowerBinaryV(JSOP_SUB, ins);
}

bool
LIRGenerator::visitMul(MMul *ins)
{
    MDefinition *lhs = ins->lhs();
    MDefinition *rhs = ins->rhs();
    JS_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        JS_ASSERT(lhs->type() == MIRType_Int32);
        ReorderCommutative(&lhs, &rhs);
        return lowerMulI(ins, lhs, rhs);
    }
    if (ins->specialization() == MIRType_Double) {
        JS_ASSERT(lhs->type() == MIRType_Double);
        return lowerForFPU(new LMathD(JSOP_MUL), ins, lhs, rhs);
    }

    return lowerBinaryV(JSOP_MUL, ins);
}

bool
LIRGenerator::visitDiv(MDiv *ins)
{
    MDefinition *lhs = ins->lhs();
    MDefinition *rhs = ins->rhs();
    JS_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        JS_ASSERT(lhs->type() == MIRType_Int32);
        return lowerDivI(ins);
    }
    if (ins->specialization() == MIRType_Double) {
        JS_ASSERT(lhs->type() == MIRType_Double);
        return lowerForFPU(new LMathD(JSOP_DIV), ins, lhs, rhs);
    }

    return lowerBinaryV(JSOP_DIV, ins);
}

bool
LIRGenerator::visitMod(MMod *ins)
{
    if (ins->type() == MIRType_Int32 &&
        ins->specialization() == MIRType_Int32)
    {
        JS_ASSERT(ins->lhs()->type() == MIRType_Int32);
        return lowerModI(ins);
    }
    // TODO: Implement for ins->specialization() == MIRType_Double

    return lowerBinaryV(JSOP_MOD, ins);
}

bool
LIRGenerator::lowerBinaryV(JSOp op, MBinaryInstruction *ins)
{
    MDefinition *lhs = ins->getOperand(0);
    MDefinition *rhs = ins->getOperand(1);

    JS_ASSERT(lhs->type() == MIRType_Value);
    JS_ASSERT(rhs->type() == MIRType_Value);

    LBinaryV *lir = new LBinaryV(op);
    if (!useBox(lir, LBinaryV::LhsInput, lhs))
        return false;
    if (!useBox(lir, LBinaryV::RhsInput, rhs))
        return false;
    if (!defineVMReturn(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitConcat(MConcat *ins)
{
    MDefinition *lhs = ins->getOperand(0);
    MDefinition *rhs = ins->getOperand(1);

    JS_ASSERT(lhs->type() == MIRType_String);
    JS_ASSERT(rhs->type() == MIRType_String);

    LConcat *lir = new LConcat(useRegister(lhs), useRegister(rhs));
    if (!defineVMReturn(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCharCodeAt(MCharCodeAt *ins)
{
    MDefinition *str = ins->getOperand(0);
    MDefinition *idx = ins->getOperand(1);

    JS_ASSERT(str->type() == MIRType_String);
    JS_ASSERT(idx->type() == MIRType_Int32);

    LCharCodeAt *lir = new LCharCodeAt(useRegister(str), useRegister(idx));
    if (!define(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitFromCharCode(MFromCharCode *ins)
{
    MDefinition *code = ins->getOperand(0);

    JS_ASSERT(code->type() == MIRType_Int32);

    LFromCharCode *lir = new LFromCharCode(useRegister(code));
    if (!define(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitStart(MStart *start)
{
    // Create a snapshot that captures the initial state of the function.
    LStart *lir = new LStart;
    if (!assignSnapshot(lir))
        return false;

    if (start->startType() == MStart::StartType_Default)
        lirGraph_.setEntrySnapshot(lir->snapshot());
    return add(lir);
}

bool
LIRGenerator::visitOsrEntry(MOsrEntry *entry)
{
    LOsrEntry *lir = new LOsrEntry;
    return defineFixed(lir, entry, LAllocation(AnyRegister(OsrFrameReg)));
}

bool
LIRGenerator::visitOsrValue(MOsrValue *value)
{
    LOsrValue *lir = new LOsrValue(useRegister(value->entry()));
    return defineBox(lir, value);
}

bool
LIRGenerator::visitOsrScopeChain(MOsrScopeChain *object)
{
    LOsrScopeChain *lir = new LOsrScopeChain(useRegister(object->entry()));
    return define(lir, object);
}

bool
LIRGenerator::visitToDouble(MToDouble *convert)
{
    MDefinition *opd = convert->input();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToDouble *lir = new LValueToDouble();
        if (!useBox(lir, LValueToDouble::Input, opd))
            return false;
        return assignSnapshot(lir) && define(lir, convert);
      }

      case MIRType_Null:
        return lowerConstantDouble(0, convert);

      case MIRType_Undefined:
        return lowerConstantDouble(js_NaN, convert);

      case MIRType_Int32:
      case MIRType_Boolean:
      {
        LInt32ToDouble *lir = new LInt32ToDouble(useRegister(opd));
        return define(lir, convert);
      }

      case MIRType_Double:
        return redefine(convert, opd);

      default:
        // Objects might be effectful.
        // Strings are complicated - we don't handle them yet.
        JS_NOT_REACHED("unexpected type");
    }
    return false;
}

bool
LIRGenerator::visitToInt32(MToInt32 *convert)
{
    MDefinition *opd = convert->input();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToInt32 *lir = new LValueToInt32(tempFloat(), LValueToInt32::NORMAL);
        if (!useBox(lir, LValueToInt32::Input, opd))
            return false;
        return assignSnapshot(lir) && define(lir, convert);
      }

      case MIRType_Null:
        return define(new LInteger(0), convert);

      case MIRType_Int32:
      case MIRType_Boolean:
        return redefine(convert, opd);

      case MIRType_Double:
      {
        LDoubleToInt32 *lir = new LDoubleToInt32(useRegister(opd));
        return assignSnapshot(lir) && define(lir, convert);
      }

      default:
        // Undefined coerces to NaN, not int32.
        // Objects might be effectful.
        // Strings are complicated - we don't handle them yet.
        JS_NOT_REACHED("unexpected type");
    }

    return false;
}

bool
LIRGenerator::visitTruncateToInt32(MTruncateToInt32 *truncate)
{
    MDefinition *opd = truncate->input();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToInt32 *lir = new LValueToInt32(tempFloat(), LValueToInt32::TRUNCATE);
        if (!useBox(lir, LValueToInt32::Input, opd))
            return false;
        return assignSnapshot(lir) && define(lir, truncate);
      }

      case MIRType_Null:
      case MIRType_Undefined:
        return define(new LInteger(0), truncate);

      case MIRType_Int32:
      case MIRType_Boolean:
        return redefine(truncate, opd);

      case MIRType_Double:
      {
        LTruncateDToInt32 *lir = new LTruncateDToInt32(useRegister(opd), tempFloat());
        return assignSnapshot(lir) && define(lir, truncate);
      }

      default:
        // Objects might be effectful.
        // Strings are complicated - we don't handle them yet.
        JS_NOT_REACHED("unexpected type");
    }

    return false;
}

bool
LIRGenerator::visitToString(MToString *ins)
{
    MDefinition *opd = ins->input();

    switch (opd->type()) {
      case MIRType_Double:
      case MIRType_Null:
      case MIRType_Undefined:
      case MIRType_Boolean:
        JS_NOT_REACHED("NYI: Lower MToString");
        break;

      case MIRType_Int32: {
        LIntToString *lir = new LIntToString(useRegister(opd));

        if (!defineVMReturn(lir, ins))
            return false;
        return assignSafepoint(lir, ins);
      }

      default:
        // Objects might be effectful. (see ToPrimitive)
        JS_NOT_REACHED("unexpected type");
        break;
    }
    return false;
}

bool
LIRGenerator::visitRegExp(MRegExp *ins)
{
    LRegExp *lir = new LRegExp();
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitLambda(MLambda *ins)
{
    LLambda *lir = new LLambda(useRegister(ins->scopeChain()));
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitImplicitThis(MImplicitThis *ins)
{
    JS_ASSERT(ins->callee()->type() == MIRType_Object);

    LImplicitThis *lir = new LImplicitThis(useRegister(ins->callee()));
    return assignSnapshot(lir) && defineBox(lir, ins);
}

bool
LIRGenerator::visitSlots(MSlots *ins)
{
    return define(new LSlots(useRegister(ins->object())), ins);
}

bool
LIRGenerator::visitElements(MElements *ins)
{
    return define(new LElements(useRegister(ins->object())), ins);
}

bool
LIRGenerator::visitLoadSlot(MLoadSlot *ins)
{
    switch (ins->type()) {
      case MIRType_Value:
        return defineBox(new LLoadSlotV(useRegister(ins->slots())), ins);

      case MIRType_Undefined:
      case MIRType_Null:
        JS_NOT_REACHED("typed load must have a payload");
        return false;

      default:
        return define(new LLoadSlotT(useRegister(ins->slots())), ins);
    }

    return true;
}

bool
LIRGenerator::visitFunctionEnvironment(MFunctionEnvironment *ins)
{
    return define(new LFunctionEnvironment(useRegister(ins->function())), ins);
}

bool
LIRGenerator::visitStoreSlot(MStoreSlot *ins)
{
    LInstruction *lir;

    switch (ins->value()->type()) {
      case MIRType_Value:
        lir = new LStoreSlotV(useRegister(ins->slots()));
        if (!useBox(lir, LStoreSlotV::Value, ins->value()))
            return false;
        return add(lir, ins);

      case MIRType_Double:
        return add(new LStoreSlotT(useRegister(ins->slots()), useRegister(ins->value())), ins);

      default:
        return add(new LStoreSlotT(useRegister(ins->slots()), useRegisterOrConstant(ins->value())),
                   ins);
    }

    return true;
}

bool
LIRGenerator::visitTypeBarrier(MTypeBarrier *ins)
{
    // Requesting a non-GC pointer is safe here since we never re-enter C++
    // from inside a type barrier test.
    LTypeBarrier *barrier = new LTypeBarrier(temp());
    if (!useBox(barrier, LTypeBarrier::Input, ins->input()))
        return false;
    if (!assignSnapshot(barrier, ins->bailoutKind()))
        return false;
    return defineAs(barrier, ins, ins->input()) && add(barrier);
}

bool
LIRGenerator::visitMonitorTypes(MMonitorTypes *ins)
{
    // Requesting a non-GC pointer is safe here since we never re-enter C++
    // from inside a type check.
    LMonitorTypes *lir = new LMonitorTypes(temp());
    if (!useBox(lir, LMonitorTypes::Input, ins->input()))
        return false;
    return assignSnapshot(lir, Bailout_Monitor) && add(lir, ins);
}

bool
LIRGenerator::visitArrayLength(MArrayLength *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    return define(new LArrayLength(useRegister(ins->elements())), ins);
}

bool
LIRGenerator::visitTypedArrayLength(MTypedArrayLength *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);
    return define(new LTypedArrayLength(useRegisterAtStart(ins->object())), ins);
}

bool
LIRGenerator::visitTypedArrayElements(MTypedArrayElements *ins)
{
    JS_ASSERT(ins->type() == MIRType_Elements);
    return define(new LTypedArrayElements(useRegisterAtStart(ins->object())), ins);
}

bool
LIRGenerator::visitInitializedLength(MInitializedLength *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    return define(new LInitializedLength(useRegister(ins->elements())), ins);
}

bool
LIRGenerator::visitSetInitializedLength(MSetInitializedLength *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    JS_ASSERT(ins->index()->isConstant());
    return add(new LSetInitializedLength(useRegister(ins->elements()),
                                         useRegisterOrConstant(ins->index())), ins);
}

bool
LIRGenerator::visitNot(MNot *ins)
{
    MDefinition *op = ins->operand();

    // String is converted to length of string in the IonBuilder phase
    JS_ASSERT(op->type() != MIRType_String);

    // - boolean: x xor 1
    // - int32: LCompare(x, 0)
    // - double: LCompare(x, 0)
    // - null or undefined: true
    // - object: false
    switch (op->type()) {
      case MIRType_Boolean: {
        MConstant *cons = MConstant::New(Int32Value(1));
        ins->block()->insertBefore(ins, cons);
        return lowerForALU(new LBitOpI(JSOP_BITXOR), ins, op, cons);
      }
      case MIRType_Int32: {
        return define(new LNotI(useRegister(op)), ins);
      }
      case MIRType_Double:
        return define(new LNotD(useRegister(op)), ins);
      case MIRType_Undefined:
      case MIRType_Null:
        return define(new LInteger(1), ins);
      case MIRType_Object:
        return define(new LInteger(0), ins);
      case MIRType_Value: {
          LNotV *lir = new LNotV(tempFloat());
        if (!useBox(lir, LNotV::Input, op))
            return false;
        return define(lir, ins);
      }

      default:
        JS_NOT_REACHED("Unexpected MIRType.");
        return false;
    }
}

bool
LIRGenerator::visitBoundsCheck(MBoundsCheck *ins)
{
    LInstruction *check;
    if (ins->minimum() || ins->maximum()) {
        check = new LBoundsCheckRange(useRegisterOrConstant(ins->index()),
                                      useRegister(ins->length()),
                                      temp());
    } else {
        check = new LBoundsCheck(useRegisterOrConstant(ins->index()),
                                 useRegister(ins->length()));
    }
    return assignSnapshot(check) && add(check, ins);
}

bool
LIRGenerator::visitBoundsCheckLower(MBoundsCheckLower *ins)
{
    LInstruction *check = new LBoundsCheckLower(useRegister(ins->index()));
    return assignSnapshot(check) && add(check, ins);
}

bool
LIRGenerator::visitLoadElement(MLoadElement *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    switch (ins->type()) {
      case MIRType_Value:
      {
        LLoadElementV *lir = new LLoadElementV(useRegister(ins->elements()),
                                               useRegisterOrConstant(ins->index()));
        if (ins->fallible() && !assignSnapshot(lir))
            return false;
        return defineBox(lir, ins);
      }
      case MIRType_Undefined:
      case MIRType_Null:
        JS_NOT_REACHED("typed load must have a payload");
        return false;

      default:
        JS_ASSERT(!ins->fallible());
        return define(new LLoadElementT(useRegister(ins->elements()),
                                        useRegisterOrConstant(ins->index())), ins);
    }
}

bool
LIRGenerator::visitLoadElementHole(MLoadElementHole *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);
    JS_ASSERT(ins->initLength()->type() == MIRType_Int32);
    JS_ASSERT(ins->type() == MIRType_Value);

    LLoadElementHole *lir = new LLoadElementHole(useRegister(ins->elements()),
                                                 useRegisterOrConstant(ins->index()),
                                                 useRegister(ins->initLength()));
    return defineBox(lir, ins);
}

bool
LIRGenerator::visitStoreElement(MStoreElement *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    switch (ins->value()->type()) {
      case MIRType_Value:
      {
        LInstruction *lir = new LStoreElementV(elements, index);
        if (!useBox(lir, LStoreElementV::Value, ins->value()))
            return false;
        return add(lir, ins);
      }

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        return add(new LStoreElementT(elements, index, value), ins);
      }
    }
}

bool
LIRGenerator::visitStoreElementHole(MStoreElementHole *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse object = useRegister(ins->object());
    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    LInstruction *lir;
    switch (ins->value()->type()) {
      case MIRType_Value:
        lir = new LStoreElementHoleV(object, elements, index);
        if (!useBox(lir, LStoreElementHoleV::Value, ins->value()))
            return false;
        break;

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        lir = new LStoreElementHoleT(object, elements, index, value);
        break;
      }
    }

    return add(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitArrayPopShift(MArrayPopShift *ins)
{
    LUse object = useRegister(ins->object());

    switch (ins->type()) {
      case MIRType_Value:
      {
        LArrayPopShiftV *lir = new LArrayPopShiftV(object, temp(), temp());
        return defineBox(lir, ins) && assignSafepoint(lir, ins);
      }
      case MIRType_Undefined:
      case MIRType_Null:
        JS_NOT_REACHED("typed load must have a payload");
        return false;

      default:
      {
        LArrayPopShiftT *lir = new LArrayPopShiftT(object, temp(), temp());
        return define(lir, ins) && assignSafepoint(lir, ins);
      }
    }
}

bool
LIRGenerator::visitArrayPush(MArrayPush *ins)
{
    JS_ASSERT(ins->type() == MIRType_Int32);

    LUse object = useRegister(ins->object());

    switch (ins->value()->type()) {
      case MIRType_Value:
      {
        LArrayPushV *lir = new LArrayPushV(object, temp());
        if (!useBox(lir, LArrayPushV::Value, ins->value()))
            return false;
        return define(lir, ins) && assignSafepoint(lir, ins);
      }

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        LArrayPushT *lir = new LArrayPushT(object, value, temp());
        return define(lir, ins) && assignSafepoint(lir, ins);
      }
    }
}

bool
LIRGenerator::visitLoadTypedArrayElement(MLoadTypedArrayElement *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    JS_ASSERT(IsNumberType(ins->type()));

    // We need a temp register for Uint32Array with known double result.
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->arrayType() == TypedArray::TYPE_UINT32 && ins->type() == MIRType_Double)
        tempDef = temp();

    LLoadTypedArrayElement *lir = new LLoadTypedArrayElement(elements, index, tempDef);
    if (ins->fallible() && !assignSnapshot(lir))
        return false;
    return define(lir, ins);
}

bool
LIRGenerator::visitClampToUint8(MClampToUint8 *ins)
{
    MDefinition *in = ins->input();

    switch (in->type()) {
      case MIRType_Boolean:
        return redefine(ins, in);

      case MIRType_Int32:
        return define(new LClampIToUint8(useRegisterAtStart(in)), ins);

      case MIRType_Double:
        return define(new LClampDToUint8(useRegisterAtStart(in), tempCopy(in, 0)), ins);

      case MIRType_Value:
      {
        LClampVToUint8 *lir = new LClampVToUint8(tempFloat());
        if (!useBox(lir, LClampVToUint8::Input, in))
            return false;
        return assignSnapshot(lir) && define(lir, ins);
      }

      default:
        JS_NOT_REACHED("Unexpected type");
        return false;
    }
}

bool
LIRGenerator::visitLoadTypedArrayElementHole(MLoadTypedArrayElementHole *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    JS_ASSERT(ins->type() == MIRType_Value);

    const LUse object = useRegister(ins->object());
    const LAllocation index = useRegisterOrConstant(ins->index());

    LLoadTypedArrayElementHole *lir = new LLoadTypedArrayElementHole(object, index);
    if (ins->fallible() && !assignSnapshot(lir))
        return false;
    return defineBox(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitLoadFixedSlot(MLoadFixedSlot *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);

    if (ins->type() == MIRType_Value) {
        LLoadFixedSlotV *lir = new LLoadFixedSlotV(useRegister(ins->object()));
        return defineBox(lir, ins);
    }

    LLoadFixedSlotT *lir = new LLoadFixedSlotT(useRegister(ins->object()));
    return define(lir, ins);
}

bool
LIRGenerator::visitStoreFixedSlot(MStoreFixedSlot *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);

    if (ins->value()->type() == MIRType_Value) {
        LStoreFixedSlotV *lir = new LStoreFixedSlotV(useRegister(ins->object()));

        if (!useBox(lir, LStoreFixedSlotV::Value, ins->value()))
            return false;
        return add(lir, ins);
    }

    LStoreFixedSlotT *lir = new LStoreFixedSlotT(useRegister(ins->object()),
                                                 useRegisterOrConstant(ins->value()));

    return add(lir, ins);
}

bool
LIRGenerator::visitGetPropertyCache(MGetPropertyCache *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);
    if (ins->type() == MIRType_Value) {
        LGetPropertyCacheV *lir = new LGetPropertyCacheV(useRegister(ins->object()));
        if (!defineBox(lir, ins))
            return false;
        return assignSafepoint(lir, ins);
    }

    LGetPropertyCacheT *lir = new LGetPropertyCacheT(useRegister(ins->object()));
    if (!define(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitGetElementCache(MGetElementCache *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);
    JS_ASSERT(ins->index()->type() == MIRType_Value);
    JS_ASSERT(ins->type() == MIRType_Value);

    LGetElementCacheV *lir = new LGetElementCacheV(useRegister(ins->object()));
    if (!useBox(lir, LGetElementCacheV::Index, ins->index()))
        return false;
    if (!defineBox(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitBindNameCache(MBindNameCache *ins)
{
    JS_ASSERT(ins->scopeChain()->type() == MIRType_Object);
    JS_ASSERT(ins->type() == MIRType_Object);

    LBindNameCache *lir = new LBindNameCache(useRegister(ins->scopeChain()));
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitGuardClass(MGuardClass *ins)
{
    LDefinition t = temp();
    LGuardClass *guard = new LGuardClass(useRegister(ins->obj()), t);
    return assignSnapshot(guard) && add(guard, ins);
}

bool
LIRGenerator::visitCallGetProperty(MCallGetProperty *ins)
{
    LCallGetProperty *lir = new LCallGetProperty();
    if (!useBox(lir, LCallGetProperty::Value, ins->value()))
        return false;
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCallGetName(MCallGetName *ins)
{
    LCallGetName *lir = new LCallGetName();
    lir->setOperand(0, useRegister(ins->getOperand(0)));
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCallGetNameTypeOf(MCallGetNameTypeOf *ins)
{
    LCallGetNameTypeOf *lir = new LCallGetNameTypeOf();
    lir->setOperand(0, useRegister(ins->getOperand(0)));
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCallGetElement(MCallGetElement *ins)
{
    JS_ASSERT(ins->lhs()->type() == MIRType_Value);
    JS_ASSERT(ins->rhs()->type() == MIRType_Value);

    LCallGetElement *lir = new LCallGetElement();
    if (!useBox(lir, LCallGetElement::LhsInput, ins->lhs()))
        return false;
    if (!useBox(lir, LCallGetElement::RhsInput, ins->rhs()))
        return false;
    if (!defineVMReturn(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCallSetProperty(MCallSetProperty *ins)
{
    LInstruction *lir = new LCallSetProperty(useRegister(ins->obj()));
    if (!useBox(lir, LCallSetProperty::Value, ins->value()))
        return false;
    if (!add(lir, ins))
        return false;
    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitDeleteProperty(MDeleteProperty *ins)
{
    LCallDeleteProperty *lir = new LCallDeleteProperty();
    if(!useBox(lir, LCallDeleteProperty::Value, ins->value()))
        return false;
    return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitSetPropertyCache(MSetPropertyCache *ins)
{
    LUse obj = useRegisterAtStart(ins->obj());
    LDefinition slots = tempCopy(ins->obj(), 0);

    LInstruction *lir;
    if (ins->value()->type() == MIRType_Value) {
        lir = new LSetPropertyCacheV(obj, slots);
        if (!useBox(lir, LSetPropertyCacheV::Value, ins->value()))
            return false;
    } else {
        LAllocation value = useRegisterOrConstant(ins->value());
        lir = new LSetPropertyCacheT(obj, slots, value, ins->value()->type());
    }

    if (!add(lir, ins))
        return false;

    return assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitCallSetElement(MCallSetElement *ins)
{
    JS_ASSERT(ins->object()->type() == MIRType_Object);
    JS_ASSERT(ins->index()->type() == MIRType_Value);
    JS_ASSERT(ins->value()->type() == MIRType_Value);

    LCallSetElement *lir = new LCallSetElement();
    lir->setOperand(0, useRegister(ins->object()));
    if (!useBox(lir, LCallSetElement::Index, ins->index()))
        return false;
    if (!useBox(lir, LCallSetElement::Value, ins->value()))
        return false;
    return add(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitIteratorStart(MIteratorStart *ins)
{
    // Call a stub if this is not a simple for-in loop.
    if (ins->flags() != JSITER_ENUMERATE) {
        LCallIteratorStart *lir = new LCallIteratorStart(useRegister(ins->object()));
        return defineVMReturn(lir, ins) && assignSafepoint(lir, ins);
    }

    LIteratorStart *lir = new LIteratorStart(useRegister(ins->object()), temp(), temp(), temp());
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitIteratorNext(MIteratorNext *ins)
{
    LIteratorNext *lir = new LIteratorNext(useRegister(ins->iterator()), temp());
    return defineBox(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitIteratorMore(MIteratorMore *ins)
{
    LIteratorMore *lir = new LIteratorMore(useRegister(ins->iterator()), temp());
    return define(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitIteratorEnd(MIteratorEnd *ins)
{
    LIteratorEnd *lir = new LIteratorEnd(useRegister(ins->iterator()), temp(), temp());
    return add(lir, ins) && assignSafepoint(lir, ins);
}

bool
LIRGenerator::visitStringLength(MStringLength *ins)
{
    JS_ASSERT(ins->string()->type() == MIRType_String);
    return define(new LStringLength(useRegister(ins->string())), ins);
}

bool
LIRGenerator::visitThrow(MThrow *ins)
{
    MDefinition *value = ins->getOperand(0);
    JS_ASSERT(value->type() == MIRType_Value);

    LThrow *lir = new LThrow;
    if (!useBox(lir, LThrow::Value, value))
        return false;
    return add(lir, ins) && assignSafepoint(lir, ins);
}

static void
SpewResumePoint(MBasicBlock *block, MInstruction *ins, MResumePoint *resumePoint)
{
    fprintf(IonSpewFile, "Current resume point %p details:\n", (void *)resumePoint);
    fprintf(IonSpewFile, "    frame count: %u\n", resumePoint->frameCount());

    if (ins) {
        fprintf(IonSpewFile, "    taken after: ");
        ins->printName(IonSpewFile);
    } else {
        fprintf(IonSpewFile, "    taken at block %d entry", block->id());
    }
    fprintf(IonSpewFile, "\n");

    fprintf(IonSpewFile, "    pc: %p (script: %p, offset: %d)\n",
            (void *)resumePoint->pc(),
            (void *)resumePoint->block()->info().script(),
            resumePoint->pc() - resumePoint->block()->info().script()->code);

    for (size_t i = 0; i < resumePoint->numOperands(); i++) {
        MDefinition *in = resumePoint->getOperand(i);
        fprintf(IonSpewFile, "    slot%u: ", (unsigned)i);
        in->printName(IonSpewFile);
        fprintf(IonSpewFile, "\n");
    }
}

bool
LIRGenerator::visitInstruction(MInstruction *ins)
{
    if (!gen->ensureBallast())
        return false;
    if (!ins->accept(this))
        return false;

    if (ins->resumePoint())
        updateResumeState(ins);

    if (gen->errored())
        return false;
#ifdef DEBUG
    ins->setInWorklistUnchecked();
#endif

    // If no safepoint was created, there's no need for an OSI point.
    if (LOsiPoint *osiPoint = popOsiPoint()) {
        if (!add(osiPoint))
            return false;
    }

    return true;
}

bool
LIRGenerator::definePhis()
{
    size_t lirIndex = 0;
    MBasicBlock *block = current->mir();
    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
        if (phi->type() == MIRType_Value) {
            if (!defineUntypedPhi(*phi, lirIndex))
                return false;
            lirIndex += BOX_PIECES;
        } else {
            if (!defineTypedPhi(*phi, lirIndex))
                return false;
            lirIndex += 1;
        }
    }
    return true;
}

void
LIRGenerator::updateResumeState(MInstruction *ins)
{
    lastResumePoint_ = ins->resumePoint();
    if (IonSpewEnabled(IonSpew_Snapshots))
        SpewResumePoint(NULL, ins, lastResumePoint_);
}

void
LIRGenerator::updateResumeState(MBasicBlock *block)
{
    lastResumePoint_ = block->entryResumePoint();
    if (IonSpewEnabled(IonSpew_Snapshots))
        SpewResumePoint(block, NULL, lastResumePoint_);
}

void
LIRGenerator::allocateArguments(uint32 argc)
{
    argslots_ += argc;
    if (argslots_ > maxargslots_)
        maxargslots_ = argslots_;
}

uint32
LIRGenerator::getArgumentSlot(uint32 argnum)
{
    // First slot has index 1.
    JS_ASSERT(argnum < argslots_);
    return argslots_ - argnum ;
}

void
LIRGenerator::freeArguments(uint32 argc)
{
    JS_ASSERT(argc <= argslots_);
    argslots_ -= argc;
}

bool
LIRGenerator::visitBlock(MBasicBlock *block)
{
    current = block->lir();
    updateResumeState(block);

    if (!definePhis())
        return false;

    if (!add(new LLabel()))
        return false;

    for (MInstructionIterator iter = block->begin(); *iter != block->lastIns(); iter++) {
        if (!visitInstruction(*iter))
            return false;
    }

    if (block->successorWithPhis()) {
        // If we have a successor with phis, lower the phi input now that we
        // are approaching the join point.
        MBasicBlock *successor = block->successorWithPhis();
        uint32 position = block->positionInPhiSuccessor();
        size_t lirIndex = 0;
        for (MPhiIterator phi(successor->phisBegin()); phi != successor->phisEnd(); phi++) {
            MDefinition *opd = phi->getOperand(position);
            if (!ensureDefined(opd))
                return false;

            JS_ASSERT(opd->type() == phi->type());

            if (phi->type() == MIRType_Value) {
                lowerUntypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += BOX_PIECES;
            } else {
                lowerTypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += 1;
            }
        }
    }

    // Now emit the last instruction, which is some form of branch.
    if (!visitInstruction(block->lastIns()))
        return false;

    return true;
}

bool
LIRGenerator::precreatePhi(LBlock *block, MPhi *phi)
{
    LPhi *lir = LPhi::New(gen, phi);
    if (!lir)
        return false;
    if (!block->addPhi(lir))
        return false;
    return true;
}

bool
LIRGenerator::generate()
{
    // Create all blocks and prep all phis beforehand.
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        current = LBlock::New(*block);
        if (!current)
            return false;
        if (!lirGraph_.addBlock(current))
            return false;
        block->assignLir(current);

        // For each MIR phi, add LIR phis as appropriate. We'll fill in their
        // operands on each incoming edge, and set their definitions at the
        // start of their defining block.
        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
            int numPhis = (phi->type() == MIRType_Value) ? BOX_PIECES : 1;
            for (int i = 0; i < numPhis; i++) {
                if (!precreatePhi(block->lir(), *phi))
                    return false;
            }
        }
    }

    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (!visitBlock(*block))
            return false;
    }

    if (graph.osrBlock())
        lirGraph_.setOsrBlock(graph.osrBlock()->lir());

    lirGraph_.setArgumentSlotCount(maxargslots_);

    return true;
}

