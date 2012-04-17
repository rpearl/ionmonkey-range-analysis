/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=4 sw=4 et tw=79: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdio.h>
#include <algorithm>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "Range.h"
#include "IonSpewer.h"

using namespace js;
using namespace js::ion;

BetaNodeBuilder::BetaNodeBuilder(MIRGraph &graph)
  : graph_(graph)
{ }

// figures out whether one node dominates another
// XXX: should be a more efficient way; is this code somewhere else?
bool
BetaNodeBuilder::blockDominates(MBasicBlock *b, MBasicBlock *b2)
{
    while (1) {
        if (b == b2) return true;
        if (b2->immediateDominator() == b2) return false;
        b2 = b2->immediateDominator();
    }
}

void
BetaNodeBuilder::replaceDominatedUsesWith(MDefinition *orig, MDefinition *dom,
                                          MBasicBlock *block)
{
    for (MUseIterator i(orig->usesBegin()); i != orig->usesEnd(); ) {
        MNode *n = i->node();
        bool isPhi = n->isDefinition() && n->toDefinition()->isPhi();
        if (n != dom &&
            ((!isPhi && blockDominates(block, n->block())) ||
             (isPhi &&
              blockDominates(block, n->block()->getPredecessor(i->index()))))) {
            i = n->replaceOperand(i, dom);
        } else {
            i++;
        }
    }
}

bool
BetaNodeBuilder::addBetaNobes()
{
    IonSpew(IonSpew_Range, "Adding beta nobes");

    for (MBasicBlockIterator i(graph_.begin()); i != graph_.end(); i++) {
        MBasicBlock *block = *i;

        BranchDirection branch_dir;
        MTest *test = block->immediateDominatorBranch(&branch_dir);
        if (!test || !test->getOperand(0)->isCompare()) continue;

        MCompare *compare = test->getOperand(0)->toCompare();

        // Add a beta node for the non-constant operands
        for (uint32 i = 0; i < compare->numOperands(); i++) {
            MDefinition *op = compare->getOperand(i);
            if (op->isConstant()) continue;

            IonSpew(IonSpew_Range, "Adding beta node for %d", op->id());

            MBeta *beta = MBeta::New(op, compare, branch_dir == TRUE_BRANCH);
            block->insertBefore(*block->begin(), beta);
            replaceDominatedUsesWith(op, beta, block);
        }

    }

    return true;
}

bool
BetaNodeBuilder::removeBetaNobes()
{
    IonSpew(IonSpew_Range, "Removing beta nobes");

    for (MBasicBlockIterator i(graph_.begin()); i != graph_.end(); i++) {
        MBasicBlock *block = *i;
        for (MDefinitionIterator iter(*i); iter; ) {
            MDefinition *def = *iter;
            if (def->isBeta()) {
                MDefinition *op = def->getOperand(0);
                IonSpew(IonSpew_Range, "Removing beta node %d for %d",
                        def->id(), op->id());
                def->replaceAllUsesWith(op);
                iter = block->discardDefAt(iter);
            } else {
                iter++;
            }
        }
    }
    return true;
}

void
Range::intersectWith(Range *other)
{
    upper_ = std::min(upper_, other->upper_);
    lower_ = std::max(lower_, other->lower_);
}

void
Range::unionWith(Range *other)
{
    upper_ = std::max(upper_, other->upper_);
    lower_ = std::min(lower_, other->lower_);
}

bool
Range::safeAdd(Range *other)
{
    int32 newUpper, newLower;
    bool overflow;
    overflow  = SafeAdd(upper_, other->upper_, &newUpper);
    overflow |= SafeAdd(lower_, other->lower_, &newLower);
    if (!overflow) {
        upper_ = newUpper;
        lower_ = newLower;
    } else {
        makeRangeInfinite();
    }
    return overflow; //Not sure if needed, but for now...
}

// TODO: Macro-ify?
bool
Range::safeSub(Range *other)
{
    int32 newUpper, newLower;
    bool overflow;
    overflow  = SafeSub(upper_, other->upper_, &newUpper);
    overflow |= SafeSub(lower_, other->lower_, &newLower);
    if (!overflow) {
        upper_ = newUpper;
        lower_ = newLower;
    } else {
        makeRangeInfinite();
    }
    return overflow;
}

bool
Range::safeMul(Range *other) {
    int32 newUpper, newLower;
    bool overflow;
    overflow  = SafeMul(upper_, other->upper_, &newUpper);
    overflow |= SafeMul(lower_, other->lower_, &newLower);
    if (!overflow) {
        upper_ = newUpper;
        lower_ = newLower;
    } else {
        makeRangeInfinite();
    }
    return overflow;
}

void
Range::shl(int32 c)
{
    int32 shift = c & 0x1f;
    int32 newUpper = upper_ << shift;
    int32 newLower = lower_ << shift;
    if (newUpper >> c != upper_ || newLower >> c != lower_) {
        makeRangeInfinite();
    } else {
        upper_ = newUpper;
        lower_ = newLower;
    }
}

void
Range::shr(int32 c)
{
    int32 shift = c & 0x1f;
    upper_ >>= shift;
    lower_ >>= shift;
}
