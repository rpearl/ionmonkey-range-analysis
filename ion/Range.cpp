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

static inline void
SpewRange(MDefinition *def)
{
#ifdef DEBUG
    if (IonSpewEnabled(IonSpew_Range)) {
        IonSpewHeader(IonSpew_Range);
        fprintf(IonSpewFile, "%d has range ", def->id());
        def->range()->printRange(IonSpewFile);
        fprintf(IonSpewFile, "\n");
    }
#endif
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
    fprintf(fp, "[");
    if (lower_infinite_) { fprintf(fp, "-inf"); } else { fprintf(fp, "%d", lower_); }
    fprintf(fp, ", ");
    if (upper_infinite_) { fprintf(fp, "inf"); } else { fprintf(fp, "%d", upper_); }
    fprintf(fp, "]");
}

void
Range::intersectWith(Range *other)
{
    setUpper(std::min(upper_, other->upper_));
    setLower(std::max(lower_, other->lower_));
    // FIXME: This is completely not true: upper_ being less than
    // lower_ means that the range is *empty*, not infinite!. How
    // should we deal with this?
    if (upper_ < lower_)
        makeRangeInfinite();
}

void
Range::unionWith(Range *other)
{
    setUpper(std::max(upper_, other->upper_));
    setLower(std::min(lower_, other->lower_));
}

void
Range::add(Range *other)
{
    setUpper((int64_t)upper_ + (int64_t)other->upper_);
    setLower((int64_t)lower_ + (int64_t)other->lower_);
}

void
Range::sub(Range *other)
{
    setUpper((int64_t)upper_ - (int64_t)other->lower_);
    setLower((int64_t)lower_ - (int64_t)other->upper_);
}

void
Range::mul(Range *other) {
    int64_t a = (int64_t)lower_ * (int64_t)other->lower_;
    int64_t b = (int64_t)lower_ * (int64_t)other->upper_;
    int64_t c = (int64_t)upper_ * (int64_t)other->lower_;
    int64_t d = (int64_t)upper_ * (int64_t)other->upper_;
    setUpper(std::max( std::max(a, b), std::max(c, d) ));
    setLower(std::min( std::min(a, b), std::min(c, d) ));
}

void
Range::shl(int32 c)
{
    int32 shift = c & 0x1f;
    setLower((int64_t)lower_ << shift);
    setUpper((int64_t)upper_ << shift);
}

void
Range::shr(int32 c)
{
    int32 shift = c & 0x1f;
    setUpper(upper_ >> shift);
    setLower(lower_ >> shift);
}

void
Range::copy(Range *other)
{
    lower_ = other->lower_;
    lower_infinite_ = other->lower_infinite_;
    upper_ = other->upper_;
    upper_infinite_ = other->upper_infinite_;
}

bool
RealRangeAnalysis::analyze() {
    IonSpew(IonSpew_Range, "Doing range propagation");
    Vector <MDefinition *, 1, IonAllocPolicy> worklist;

    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MDefinitionIterator iter(*block); iter; iter++) {
            MDefinition *def = *iter;
            // unconditionally recompute the range here. There is probably a
            // cleaner way to do this.
            def->recomputeRange();

            if (!def->isPhi() && !def->isBeta())
                continue;

            for (MUseDefIterator use(def); use; use++) {
                if (!worklist.append(use.def()))
                    return false;
            }

        }
    }

    while (!worklist.empty()) {
        MDefinition *def = worklist.popCopy();
        IonSpew(IonSpew_Range, "recomputing range on %d", def->id());
        SpewRange(def);
        if (def->recomputeRange()) {
            JS_ASSERT(def->range()->lower() <= def->range()->upper());
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
            SpewRange(def);
            JS_ASSERT(def->range()->lower() <= def->range()->upper());
        }
    }
#endif
    return true;
}
