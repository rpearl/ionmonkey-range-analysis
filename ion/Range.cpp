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

RealRangeAnalysis::RealRangeAnalysis(MIRGraph &graph)
  : graph_(graph)
{ }

static bool
IsDominatedUse(MBasicBlock *block, MUse *use)
{
    MNode *n = use->node();
    bool isPhi = n->isDefinition() && n->toDefinition()->isPhi();

    if (isPhi)
        return block->dominates(n->block()->getPredecessor(use->index()));
    return block->dominates(n->block());
}

// XXX I *think* we just wanted MUseDefIterator (which skips bailout points
void
RealRangeAnalysis::replaceDominatedUsesWith(MDefinition *orig, MDefinition *dom,
                                          MBasicBlock *block)
{
    for (MUseIterator i(orig->usesBegin()); i != orig->usesEnd(); ) {
        if (i->node() != dom && IsDominatedUse(block, *i))
            i = i->node()->replaceOperand(i, dom);
        else
            i++;
    }
}

bool
RealRangeAnalysis::addBetaNobes()
{
    IonSpew(IonSpew_Range, "Adding beta nobes");

    for (MBasicBlockIterator i(graph_.begin()); i != graph_.end(); i++) {
        MBasicBlock *block = *i;
        IonSpew(IonSpew_Range, "Looking at block %d", block->id());

        BranchDirection branch_dir;
        MTest *test = block->immediateDominatorBranch(&branch_dir);
        if (!test || !test->getOperand(0)->isCompare()) continue;

        MCompare *compare = test->getOperand(0)->toCompare();

        MDefinition *left = compare->getOperand(0);
        MDefinition *right = compare->getOperand(1);
        int32 bound;
        MDefinition *val = NULL;

        JSOp jsop = compare->jsop();

        if (left->isConstant() && left->toConstant()->value().isInt32()) {
            bound = left->toConstant()->value().toInt32();
            val = right;
            jsop = analyze::NegateCompareOp(jsop);
        } else if (right->isConstant() && right->toConstant()->value().isInt32()) {
            bound = right->toConstant()->value().toInt32();
            val = left;
        } else {
            continue;
        }

        JS_ASSERT(val);

        if (branch_dir == FALSE_BRANCH)
            jsop = analyze::NegateCompareOp(jsop);
        int32 low = JSVAL_INT_MIN;
        int32 high = JSVAL_INT_MAX;
        switch (jsop) {
          case JSOP_LE:
            high = bound;
            break;
          case JSOP_LT:
            if (!SafeSub(bound, 1, &bound))
                break;
            high = bound;
            break;
          case JSOP_GE:
            low = bound;
            break;
          case JSOP_GT:
            if (!SafeAdd(bound, 1, &bound))
                break;
            low = bound;
            break;
          case JSOP_EQ:
            low = bound;
            high = bound;
          default:
            break; // well, for neq we could have
                   // [-\inf, bound-1] U [bound+1, \inf] but we only use contiguous ranges.
        }

        IonSpew(IonSpew_Range, "Adding beta node for %d", val->id());
        MBeta *beta = MBeta::New(val, low, high);
        block->insertBefore(*block->begin(), beta);
        replaceDominatedUsesWith(val, beta, block);
    }

    return true;
}

bool
RealRangeAnalysis::removeBetaNobes()
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
Range::printRange(FILE *fp)
{
    fprintf(fp, "[%d, %d]", lower_, upper_);
}

void
Range::intersectWith(Range *other)
{
    upper_ = std::min(upper_, other->upper_);
    lower_ = std::max(lower_, other->lower_);
    if (upper_ < lower_)
        makeRangeInfinite();
}

void
Range::unionWith(Range *other)
{
    upper_ = std::max(upper_, other->upper_);
    lower_ = std::min(lower_, other->lower_);
}

static inline int32
Saturate(int64_t res)
{
    if (res > JSVAL_INT_MAX)
        return JSVAL_INT_MAX;
    if (res < JSVAL_INT_MIN)
        return JSVAL_INT_MIN;
    return (int32)res;
}

static inline int32
SaturateAdd(int32 a, int32 b)
{
    return Saturate((int64_t)a + (int64_t)b);
}

static inline int32
SaturateSub(int32 a, int32 b)
{
    return Saturate((int64_t)a - (int64_t)b);
}

static inline int32
SaturateMul(int32 a, int32 b)
{
    return Saturate((int64_t)a * (int64_t)b);
}

void
Range::add(Range *other)
{
    upper_ = SaturateAdd(upper_, other->upper_);
    lower_ = SaturateAdd(lower_, other->lower_);
}

void
Range::sub(Range *other)
{
    upper_ = SaturateSub(upper_, other->upper_);
    lower_ = SaturateSub(lower_, other->lower_);
}

void
Range::mul(Range *other) {
    int32 a = SaturateMul(lower_, other->lower_);
    int32 b = SaturateMul(lower_, other->upper_);
    int32 c = SaturateMul(upper_, other->lower_);
    int32 d = SaturateMul(upper_, other->upper_);
    upper_ = std::max( std::max(a, b), std::max(c, d) );
    lower_ = std::min( std::min(a, b), std::min(c, d) );
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

void
Range::copy(Range *other)
{
    lower_ = other->lower_;
    upper_ = other->upper_;
}

bool
RealRangeAnalysis::analyze() {
    IonSpew(IonSpew_Range, "Doing range propagation");
    Vector <MDefinition *, 1, IonAllocPolicy> worklist;

    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MDefinitionIterator iter(*block); iter; iter++) {
            MDefinition *def = *iter;
            if (!def->isPhi() && !def->isBeta())
                continue;

            // unconditionally recompute the range here. There is probably a
            // cleaner way to do this.
            def->recomputeRange();
            for (MUseDefIterator use(def); use; use++) {
                if (!worklist.append(use.def()))
                    return false;
            }

        }
    }

    while (!worklist.empty()) {
        MDefinition *def = worklist.popCopy();
        IonSpew(IonSpew_Range, "recomputing range on %d", def->id());
        if (def->recomputeRange()) {
            IonSpew(IonSpew_Range, "Range changed; adding consumers");
            for (MUseDefIterator use(def); use; use++) {
                if (!worklist.append(use.def()))
                    return false;
            }
        }
    }
#ifdef DEBUG
    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MDefinitionIterator iter(*block); iter; iter++) {
            MDefinition *def = *iter;
            IonSpew(IonSpew_Range, "%d has range [%d, %d]", def->id(),
                    def->range()->lower(), def->range()->upper());
            JS_ASSERT(def->range()->lower() <= def->range()->upper());
        }
    }
#endif
    return true;
}
