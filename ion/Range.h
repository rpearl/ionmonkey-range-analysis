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
 *   Ryan Pearl <rpearl@andrew.cmu.edu>
 *   Michael Sullivan <sully@msully.net>
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

#ifndef jsion_range_h__
#define jsion_range_h__

#include "MIR.h"
#include "CompileInfo.h"

namespace js {
namespace ion {

class MBasicBlock;
class MIRGraph;

class RealRangeAnalysis
{
  protected:
	bool blockDominates(MBasicBlock *b, MBasicBlock *b2);
	void replaceDominatedUsesWith(MDefinition *orig, MDefinition *dom,
	                              MBasicBlock *block);

  protected:
    MIRGraph &graph_;

  public:
    RealRangeAnalysis(MIRGraph &graph);
    bool addBetaNobes();
    bool analyze();
    bool removeBetaNobes();
};

class Range {
    private:
        /* TODO: we should do symbolic range evaluation, where we have
         * information of the form v1 < v2 for arbitrary defs v1 and v2, not
         * just constants
         */
        int32 lower_;
        int32 upper_;


    public:
        Range() :
            lower_(JSVAL_INT_MIN),
            upper_(JSVAL_INT_MAX)
        {}

        Range(int32 l, int32 h) :
            lower_(l),
            upper_(h)
        {}
        void intersectWith(Range *other);
        void unionWith(Range *other);
        void copy(Range *other);

        bool safeAdd(Range *other);
        bool safeSub(Range *other);
        bool safeMul(Range *other);

        /* TODO: we probably want a function to add by a constant */
        void shl(int32 c);
        void shr(int32 c);

        inline void makeRangeInfinite() {
            lower_ = JSVAL_INT_MIN;
            upper_ = JSVAL_INT_MAX;
        }

        inline int32 lower() const {
            return lower_;
        }

        inline int32 upper() const {
            return upper_;
        }

        void setLower(int32 x) {
            lower_ = x;
        }
        void setUpper(int32 x) {
            upper_ = x;
        }
};

} // namespace ion
} // namespace js

#endif // jsion_range_h__

