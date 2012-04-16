/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=4 sw=4 et tw=79: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdio.h>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "Range.h"

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
        // XXX: phi nodes...
        if ((!isPhi && blockDominates(block, n->block())) ||
            (isPhi &&
             blockDominates(block, n->block()->getPredecessor(i->index())))) {
            i = n->replaceOperand(i, dom);
        } else {
            i++;
        }
    }
}

bool
BetaNodeBuilder::addBetaNobes()
{

    for (MBasicBlockIterator i(graph_.begin()); i != graph_.end(); i++) {
        MBasicBlock *block = *i;

        if (block->numPredecessors() != 1) continue;
        if (!block->getPredecessor(0)->lastIns()->isTest()) continue;

        MTest *test = block->getPredecessor(0)->lastIns()->toTest();
        if (!test->getOperand(0)->isCompare()) continue;
        MCompare *compare = test->getOperand(0)->toCompare();

        // Add a beta node for the non-constant operands
        for (uint32 i = 0; i < compare->numOperands(); i++) {
            MDefinition *op = compare->getOperand(i);
            if (op->isConstant()) continue;

            MBeta *beta = MBeta::New(op, compare);
            block->insertBefore(*block->begin(), beta);
            replaceDominatedUsesWith(op, beta, block);
        }

    }

    return true;
}
