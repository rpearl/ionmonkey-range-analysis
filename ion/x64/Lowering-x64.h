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

#ifndef jsion_ion_lowering_x64_h__
#define jsion_ion_lowering_x64_h__

#include "ion/shared/Lowering-x86-shared.h"

namespace js {
namespace ion {

class LIRGeneratorX64 : public LIRGeneratorX86Shared
{
  public:
    LIRGeneratorX64(MIRGenerator *gen, MIRGraph &graph, LIRGraph &lirGraph)
      : LIRGeneratorX86Shared(gen, graph, lirGraph)
    { }

  protected:
    void lowerUntypedPhiInput(MPhi *phi, uint32 inputPosition, LBlock *block, size_t lirIndex);
    bool defineUntypedPhi(MPhi *phi, size_t lirIndex);

    // Adds a use at operand |n| of a value-typed insturction.
    bool useBox(LInstruction *lir, size_t n, MDefinition *mir,
                LUse::Policy policy = LUse::REGISTER, bool useAtStart = false);

    bool lowerForShift(LInstructionHelper<1, 2, 0> *ins, MDefinition *mir, MDefinition *lhs,
                       MDefinition *rhs);

    bool lowerForALU(LInstructionHelper<1, 1, 0> *ins, MDefinition *mir, MDefinition *input);
    bool lowerForALU(LInstructionHelper<1, 2, 0> *ins, MDefinition *mir, MDefinition *lhs,
                     MDefinition *rhs);
    bool lowerForFPU(LMathD *ins, MDefinition *mir, MDefinition *lhs, MDefinition *rhs);

    bool lowerConstantDouble(double d, MInstruction *ins);
    bool lowerDivI(MDiv *div);
    bool visitGuardShape(MGuardShape *ins);

  public:
    bool visitConstant(MConstant *ins);
    bool visitBox(MBox *box);
    bool visitUnbox(MUnbox *unbox);
    bool visitReturn(MReturn *ret);
    bool visitStoreTypedArrayElement(MStoreTypedArrayElement *ins);
};

typedef LIRGeneratorX64 LIRGeneratorSpecific;

} // namespace ion
} // namespace js

#endif // jsion_ion_lowering_x64_h__

