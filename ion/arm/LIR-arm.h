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

#ifndef jsion_lir_arm_h__
#define jsion_lir_arm_h__

#include "ion/TypeOracle.h"

namespace js {
namespace ion {

class LBox : public LInstructionHelper<2, 1, 0>
{
    MIRType type_;

  public:
    LIR_HEADER(Box);

    LBox(const LAllocation &in_payload, MIRType type)
      : type_(type)
    {
        setOperand(0, in_payload);
    }

    MIRType type() const {
        return type_;
    }
};

class LBoxDouble : public LInstructionHelper<2, 1, 1>
{
  public:
    LIR_HEADER(BoxDouble);

    LBoxDouble(const LAllocation &in, const LDefinition &temp) {
        setOperand(0, in);
        setTemp(0, temp);
    }
};

class LUnbox : public LInstructionHelper<1, 2, 0>
{
  public:
    LIR_HEADER(Unbox);

    MUnbox *mir() const {
        return mir_->toUnbox();
    }
    const LAllocation *type() {
        return getOperand(1);
    }

};

class LUnboxDouble : public LInstructionHelper<1, 2, 0>
{
  public:
    LIR_HEADER(UnboxDouble);

    static const size_t Input = 0;

    const LDefinition *output() {
        return getDef(0);
    }
    MUnbox *mir() const {
        return mir_->toUnbox();
    }
};

// Constant double.
class LDouble : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(Double);

    LDouble(const LConstantIndex &cindex) {
        setOperand(0, cindex);
    }
};

// LDivI is presently implemented as a proper C function,
// so it trashes r0, r1, r2 and r3.  The call also trashes lr, and has the
// ability to trash ip. The function also takes two arguments (dividend in r0,
// divisor in r1). The LInstruction gets encoded such that the divisor and
// dividend are passed in their apropriate registers, and are marked as copy
// so we can modify them (and the function will).
// The other thre registers that can be trashed are marked as such. For the time
// being, the link register is not marked as trashed because we never allocate
// to the link register.
class LDivI : public LBinaryMath<2>
{
  public:
    LIR_HEADER(DivI);

    LDivI(const LAllocation &lhs, const LAllocation &rhs,
          const LDefinition &temp1, const LDefinition &temp2) {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp1);
        setTemp(1, temp2);
    }

    MDiv *mir() const {
        return mir_->toDiv();
    }
};

class LModI : public LBinaryMath<2>
{
  public:
    LIR_HEADER(ModI);

    LModI(const LAllocation &lhs, const LAllocation &rhs,
          const LDefinition &temp1, const LDefinition &temp2)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp1);
        setTemp(1, temp2);
    }
};

class LModPowTwoI : public LInstructionHelper<1,1,0>
{
    const int32 shift_;

  public:
    LIR_HEADER(ModPowTwoI);
    int32 shift()
    {
        return shift_;
    }

    LModPowTwoI(const LAllocation &lhs, int32 shift) 
      : shift_(shift)
    {
        setOperand(0, lhs);
    }
};

class LModMaskI : public LInstructionHelper<1,1,1>
{
    const int32 shift_;

  public:
    LIR_HEADER(ModMaskI);
    int32 shift()
    {
        return shift_;
    }

    LModMaskI(const LAllocation &lhs, const LDefinition &temp1, int32 shift)
      : shift_(shift)
    {
        setOperand(0, lhs);
        setTemp(0, temp1);
    }
};
// Takes a tableswitch with an integer to decide
class LTableSwitch : public LInstructionHelper<0, 1, 1>
{
  public:
    LIR_HEADER(TableSwitch);

    LTableSwitch(const LAllocation &in, const LDefinition &inputCopy, MTableSwitch *ins)
    {
        setOperand(0, in);
        setTemp(0, inputCopy);
        setMir(ins);
    }

    MTableSwitch *mir() const {
        return mir_->toTableSwitch();
    }

    const LAllocation *index() {
        return getOperand(0);
    }
    const LAllocation *tempInt() {
        return getTemp(0)->output();
    }
};

// Guard against an object's shape.
class LGuardShape : public LInstructionHelper<0, 1, 1>
{
  public:
    LIR_HEADER(GuardShape);

    LGuardShape(const LAllocation &in, const LDefinition &temp) {
        setOperand(0, in);
        setTemp(0, temp);
    }
    const MGuardShape *mir() const {
        return mir_->toGuardShape();
    }
    const LAllocation *input() {
        return getOperand(0);
    }
    const LAllocation *tempInt() {
        return getTemp(0)->output();
    }
};

class LRecompileCheck : public LInstructionHelper<0, 0, 1>
{
  public:
    LIR_HEADER(RecompileCheck);

    LRecompileCheck(const LDefinition &temp) {
        setTemp(0, temp);
    }
    const LAllocation *tempInt() {
        return getTemp(0)->output();
    }
};

class LMulI : public LBinaryMath<0>
{
  public:
    LIR_HEADER(MulI);

    MMul *mir() {
        return mir_->toMul();
    }
};

} // namespace ion
} // namespace js

#endif // jsion_lir_arm_h__

