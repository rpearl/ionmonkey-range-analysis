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

#ifndef jsion_bytecode_analyzer_h__
#define jsion_bytecode_analyzer_h__

// This file declares the data structures for building a MIRGraph from a
// JSScript.

#include "MIR.h"
#include "MIRGraph.h"

namespace js {
namespace ion {

class IonBuilder : public MIRGenerator
{
    enum ControlStatus {
        ControlStatus_Error,
        ControlStatus_Ended,        // There is no continuation/join point.
        ControlStatus_Joined,       // Created a join node.
        ControlStatus_Jumped,       // Parsing another branch at the same level.
        ControlStatus_None          // No control flow.
    };

    struct DeferredEdge : public TempObject
    {
        MBasicBlock *block;
        DeferredEdge *next;

        DeferredEdge(MBasicBlock *block, DeferredEdge *next)
          : block(block), next(next)
        { }
    };

    struct ControlFlowInfo {
        // Entry in the cfgStack.
        uint32 cfgEntry;

        // Label that continues go to.
        jsbytecode *continuepc;

        ControlFlowInfo(uint32 cfgEntry, jsbytecode *continuepc)
          : cfgEntry(cfgEntry),
            continuepc(continuepc)
        { }
    };

    // To avoid recursion, the bytecode analyzer uses a stack where each entry
    // is a small state machine. As we encounter branches or jumps in the
    // bytecode, we push information about the edges on the stack so that the
    // CFG can be built in a tree-like fashion.
    struct CFGState {
        enum State {
            IF_TRUE,            // if() { }, no else.
            IF_TRUE_EMPTY_ELSE, // if() { }, empty else
            IF_ELSE_TRUE,       // if() { X } else { }
            IF_ELSE_FALSE,      // if() { } else { X }
            DO_WHILE_LOOP_BODY, // do { x } while ()
            DO_WHILE_LOOP_COND, // do { } while (x)
            WHILE_LOOP_COND,    // while (x) { }
            WHILE_LOOP_BODY,    // while () { x }
            FOR_LOOP_COND,      // for (; x;) { }
            FOR_LOOP_BODY,      // for (; ;) { x }
            FOR_LOOP_UPDATE,    // for (; ; x) { }
            TABLE_SWITCH,       // switch() { x }
            AND_OR              // && x, || x
        };

        State state;            // Current state of this control structure.
        jsbytecode *stopAt;     // Bytecode at which to stop the processing loop.
        
        // For if structures, this contains branch information.
        union {
            struct {
                MBasicBlock *ifFalse;
                jsbytecode *falseEnd;
                MBasicBlock *ifTrue;    // Set when the end of the true path is reached.
            } branch;
            struct {
                // Common entry point.
                MBasicBlock *entry;

                // Position of where the loop body starts and ends.
                jsbytecode *bodyStart;
                jsbytecode *bodyEnd;

                // pc immediately after the loop exits.
                jsbytecode *exitpc;

                // Common exit point. Created lazily, so it may be NULL.
                MBasicBlock *successor;

                // Deferred break and continue targets.
                DeferredEdge *breaks;
                DeferredEdge *continues;

                // For-loops only.
                jsbytecode *condpc;
                jsbytecode *updatepc;
                jsbytecode *updateEnd;
            } loop;
            struct {
                // pc immediately after the switch.
                jsbytecode *exitpc;

                // Deferred break and continue targets.
                DeferredEdge *breaks;

                // MIR instruction
                MTableSwitch *ins;

                // The number of current successor that get mapped into a block. 
                uint32 currentBlock;

            } tableswitch;
        };

        inline bool isLoop() const {
            switch (state) {
              case DO_WHILE_LOOP_COND:
              case DO_WHILE_LOOP_BODY:
              case WHILE_LOOP_COND:
              case WHILE_LOOP_BODY:
              case FOR_LOOP_COND:
              case FOR_LOOP_BODY:
              case FOR_LOOP_UPDATE:
                return true;
              default:
                return false;
            }
        }

        static CFGState If(jsbytecode *join, MBasicBlock *ifFalse);
        static CFGState IfElse(jsbytecode *trueEnd, jsbytecode *falseEnd, MBasicBlock *ifFalse);
        static CFGState AndOr(jsbytecode *join, MBasicBlock *joinStart);
    };

    static int CmpSuccessors(const void *a, const void *b);

  public:
    IonBuilder(JSContext *cx, JSObject *scopeChain, TempAllocator &temp, MIRGraph &graph,
               TypeOracle *oracle, CompileInfo &info, size_t inliningDepth = 0, uint32 loopDepth = 0);

    bool build();
    bool buildInline(IonBuilder *callerBuilder, MResumePoint *callerResumePoint, MDefinition *thisDefn,
                     MDefinitionVector &args);

  private:
    bool traverseBytecode();
    ControlStatus snoopControlFlow(JSOp op);
    bool inspectOpcode(JSOp op);
    uint32 readIndex(jsbytecode *pc);
    JSAtom *readAtom(jsbytecode *pc);
    bool abort(const char *message, ...);

    static bool inliningEnabled() {
        return js_IonOptions.inlining;
    }

    JSFunction *getSingleCallTarget(uint32 argc, jsbytecode *pc);
    bool canInlineTarget(JSFunction *target);

    void popCfgStack();
    bool processDeferredContinues(CFGState &state);
    ControlStatus processControlEnd();
    ControlStatus processCfgStack();
    ControlStatus processCfgEntry(CFGState &state);
    ControlStatus processIfEnd(CFGState &state);
    ControlStatus processIfElseTrueEnd(CFGState &state);
    ControlStatus processIfElseFalseEnd(CFGState &state);
    ControlStatus processDoWhileBodyEnd(CFGState &state);
    ControlStatus processDoWhileCondEnd(CFGState &state);
    ControlStatus processWhileCondEnd(CFGState &state);
    ControlStatus processWhileBodyEnd(CFGState &state);
    ControlStatus processForCondEnd(CFGState &state);
    ControlStatus processForBodyEnd(CFGState &state);
    ControlStatus processForUpdateEnd(CFGState &state);
    ControlStatus processNextTableSwitchCase(CFGState &state);
    ControlStatus processTableSwitchEnd(CFGState &state);
    ControlStatus processAndOrEnd(CFGState &state);
    ControlStatus processSwitchBreak(JSOp op, jssrcnote *sn);
    ControlStatus processReturn(JSOp op);
    ControlStatus processThrow();
    ControlStatus processContinue(JSOp op, jssrcnote *sn);
    ControlStatus processBreak(JSOp op, jssrcnote *sn);
    ControlStatus maybeLoop(JSOp op, jssrcnote *sn);
    bool pushLoop(CFGState::State state, jsbytecode *stopAt, MBasicBlock *entry,
                  jsbytecode *bodyStart, jsbytecode *bodyEnd, jsbytecode *exitpc,
                  jsbytecode *continuepc = NULL);

    MBasicBlock *addBlock(MBasicBlock *block, uint32 loopDepth);
    MBasicBlock *newBlock(MBasicBlock *predecessor, jsbytecode *pc);
    MBasicBlock *newBlock(MBasicBlock *predecessor, jsbytecode *pc, uint32 loopDepth);
    MBasicBlock *newBlockAfter(MBasicBlock *at, MBasicBlock *predecessor, jsbytecode *pc);
    MBasicBlock *newOsrPreheader(MBasicBlock *header, jsbytecode *loopHead, jsbytecode *loopEntry);
    MBasicBlock *newPendingLoopHeader(MBasicBlock *predecessor, jsbytecode *pc);
    MBasicBlock *newBlock(jsbytecode *pc) {
        return newBlock(NULL, pc);
    }
    MBasicBlock *newBlockAfter(MBasicBlock *at, jsbytecode *pc) {
        return newBlockAfter(at, NULL, pc);
    }

    // Given a list of pending breaks, creates a new block and inserts a Goto
    // linking each break to the new block.
    MBasicBlock *createBreakCatchBlock(DeferredEdge *edge, jsbytecode *pc);

    // Finishes loops that do not actually loop, containing only breaks or
    // returns.
    ControlStatus processBrokenLoop(CFGState &state);

    // Computes loop phis, places them in all successors of a loop, then
    // handles any pending breaks.
    ControlStatus finishLoop(CFGState &state, MBasicBlock *successor);

    void assertValidLoopHeadOp(jsbytecode *pc);

    ControlStatus forLoop(JSOp op, jssrcnote *sn);
    ControlStatus whileOrForInLoop(JSOp op, jssrcnote *sn);
    ControlStatus doWhileLoop(JSOp op, jssrcnote *sn);
    ControlStatus tableSwitch(JSOp op, jssrcnote *sn);

    // Please see the Big Honkin' Comment about how resume points work in
    // IonBuilder.cpp, near the definition for this function.
    bool resume(MInstruction *ins, jsbytecode *pc, MResumePoint::Mode mode);
    bool resumeAt(MInstruction *ins, jsbytecode *pc);
    bool resumeAfter(MInstruction *ins);

    void insertRecompileCheck();

    bool initParameters();
    void rewriteParameters();
    bool pushConstant(const Value &v);
    bool pushTypeBarrier(MInstruction *ins, types::TypeSet *actual, types::TypeSet *observed);
    void monitorResult(MInstruction *ins, types::TypeSet *types);

    JSObject *getSingletonPrototype(JSFunction *target);

    MDefinition *createThisNative();
    MDefinition *createThisScripted(MDefinition *callee);
    MDefinition *createThisScriptedSingleton(JSFunction *target, JSObject *proto, MDefinition *callee);
    MDefinition *createThis(JSFunction *target, MDefinition *callee);

    bool makeCall(JSFunction *target, uint32 argc, bool constructing);

    bool jsop_add(MDefinition *left, MDefinition *right);
    bool jsop_bitnot();
    bool jsop_bitop(JSOp op);
    bool jsop_binary(JSOp op);
    bool jsop_binary(JSOp op, MDefinition *left, MDefinition *right);
    bool jsop_pos();
    bool jsop_neg();
    bool jsop_defvar(uint32 index);
    bool jsop_notearg();
    bool jsop_funcall(uint32 argc);
    bool jsop_call(uint32 argc, bool constructing);
    bool jsop_ifeq(JSOp op);
    bool jsop_andor(JSOp op);
    bool jsop_dup2();
    bool jsop_loophead(jsbytecode *pc);
    bool jsop_incslot(JSOp op, uint32 slot);
    bool jsop_localinc(JSOp op);
    bool jsop_arginc(JSOp op);
    bool jsop_compare(JSOp op);
    bool jsop_getgname(JSAtom *atom);
    bool jsop_setgname(JSAtom *atom);
    bool jsop_getname(JSAtom *atom);
    bool jsop_bindname(PropertyName *name);
    bool jsop_getelem();
    bool jsop_getelem_dense();
    bool jsop_getelem_typed(int arrayType);
    bool jsop_setelem();
    bool jsop_setelem_dense();
    bool jsop_setelem_typed(int arrayType);
    bool jsop_length();
    bool jsop_length_fastPath();
    bool jsop_not();
    bool jsop_getprop(JSAtom *atom);
    bool jsop_setprop(JSAtom *atom);
    bool jsop_delprop(JSAtom *atom);
    bool jsop_newinit(bool isArray);
    bool jsop_newarray(uint32 count);
    bool jsop_newobject(JSObject *baseObj);
    bool jsop_initelem();
    bool jsop_initelem_dense();
    bool jsop_initprop(JSAtom *atom);
    bool jsop_regexp(RegExpObject *reobj);
    bool jsop_object(JSObject *obj);
    bool jsop_lambda(JSFunction *fun);
    bool jsop_deflocalfun(uint32 local, JSFunction *fun);
    bool jsop_this();
    bool jsop_typeof();
    bool jsop_toid();
    bool jsop_iter(uint8 flags);
    bool jsop_iternext(uint8 depth);
    bool jsop_itermore();
    bool jsop_iterend();

    // Replace generic calls to native function by instructions which can be
    // specialized and which can enable GVN & LICM on these native calls.
    bool discardCallArgs(uint32 argc, MDefinitionVector &argv, MBasicBlock *bb);
    bool discardCall(uint32 argc, MDefinitionVector &argv, MBasicBlock *bb);
    bool inlineNativeCall(JSFunction *target, uint32 argc, bool constructing);

    /* Inlining. */

    enum InliningStatus
    {
        InliningStatus_Error,
        InliningStatus_NotInlined,
        InliningStatus_Inlined
    };

    bool jsop_call_inline(JSFunction *callee, uint32 argc, IonBuilder &inlineBuilder);
    bool inlineScriptedCall(JSFunction *target, uint32 argc);
    bool makeInliningDecision(JSFunction *target);

  public:
    // A builder is inextricably tied to a particular script.
    JSScript * const script;

  private:
    jsbytecode *pc;
    JSObject *initialScopeChain_;
    MBasicBlock *current;
    uint32 loopDepth_;

    /* Information used for inline-call builders. */
    MResumePoint *callerResumePoint_;
    jsbytecode *callerPC() {
        return callerResumePoint_ ? callerResumePoint_->pc() : NULL;
    }
    IonBuilder *callerBuilder_;

    Vector<CFGState, 8, IonAllocPolicy> cfgStack_;
    Vector<ControlFlowInfo, 4, IonAllocPolicy> loops_;
    Vector<ControlFlowInfo, 0, IonAllocPolicy> switches_;
    TypeOracle *oracle;
    size_t inliningDepth;
};

} // namespace ion
} // namespace js

#endif // jsion_bytecode_analyzer_h__

