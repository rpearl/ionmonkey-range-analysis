/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=4 sw=4 et tw=79: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdio.h>

#include "Ion.h"
#include "MIR.h"
#include "MIRGraph.h"

using namespace js;
using namespace js::ion;

// figures out whether one node dominates another
// XXX: should be a more efficient way; is this code somewhere else?
bool blockDominates(MBasicBlock *b, MBasicBlock *b2)
{
    while (1) {
        if (b == b2) return true;
        if (b2->immediateDominator() == b2) return false;
        b2 = b2->immediateDominator();
    }
}

void replaceDominatedUsesWith(MDefinition *orig, MDefinition *dom,
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
