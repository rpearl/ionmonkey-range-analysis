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

#include "IonAnalysis.h"
#include "IonBuilder.h"
#include "MIRGraph.h"
#include "Ion.h"
#include "IonAnalysis.h"
#include "IonSpewer.h"
#include "frontend/BytecodeEmitter.h"
#include "jsscriptinlines.h"

#ifdef JS_THREADSAFE
# include "prthread.h"
#endif

using namespace js;
using namespace js::ion;

IonBuilder::IonBuilder(JSContext *cx, JSObject *scopeChain, TempAllocator &temp, MIRGraph &graph,
                       TypeOracle *oracle, CompileInfo &info, size_t inliningDepth, uint32 loopDepth)
  : MIRGenerator(cx, temp, graph, info),
    script(info.script()),
    initialScopeChain_(scopeChain),
    loopDepth_(loopDepth),
    callerResumePoint_(NULL),
    callerBuilder_(NULL),
    oracle(oracle),
    inliningDepth(inliningDepth)
{
    pc = info.startPC();
}

bool
IonBuilder::abort(const char *message, ...)
{
    va_list ap;
    va_start(ap, message);
    abortFmt(message, ap);
    va_end(ap);
    IonSpew(IonSpew_Abort, "aborted @ %s:%d", script->filename, PCToLineNumber(script, pc));
    return false;
}

static inline int32
GetJumpOffset(jsbytecode *pc)
{
    JS_ASSERT(js_CodeSpec[JSOp(*pc)].type() == JOF_JUMP);
    return GET_JUMP_OFFSET(pc);
}

IonBuilder::CFGState
IonBuilder::CFGState::If(jsbytecode *join, MBasicBlock *ifFalse)
{
    CFGState state;
    state.state = IF_TRUE;
    state.stopAt = join;
    state.branch.ifFalse = ifFalse;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::IfElse(jsbytecode *trueEnd, jsbytecode *falseEnd, MBasicBlock *ifFalse)
{
    CFGState state;
    // If the end of the false path is the same as the start of the
    // false path, then the "else" block is empty and we can devolve
    // this to the IF_TRUE case. We handle this here because there is
    // still an extra GOTO on the true path and we want stopAt to point
    // there, whereas the IF_TRUE case does not have the GOTO.
    state.state = (falseEnd == ifFalse->pc())
                  ? IF_TRUE_EMPTY_ELSE
                  : IF_ELSE_TRUE;
    state.stopAt = trueEnd;
    state.branch.falseEnd = falseEnd;
    state.branch.ifFalse = ifFalse;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::AndOr(jsbytecode *join, MBasicBlock *joinStart)
{
    CFGState state;
    state.state = AND_OR;
    state.stopAt = join;
    state.branch.ifFalse = joinStart;
    return state;
}

JSFunction *
IonBuilder::getSingleCallTarget(uint32 argc, jsbytecode *pc)
{
    types::TypeSet *calleeTypes = oracle->getCallTarget(script, argc, pc);
    JSObject *obj = calleeTypes->getSingleton(cx, false);
    if (!obj || !obj->isFunction())
        return NULL;
    return obj->toFunction();
}

bool
IonBuilder::canInlineTarget(JSFunction *target)
{
    if (!target->isInterpreted()) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to non-interpreted");
        return false;
    }

    if (target->getParent() != script->global()) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to scope mismatch");
        return false;
    }

    JSScript *inlineScript = target->script();

    if (!inlineScript->canIonCompile()) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to disable Ion compilation");
        return false;
    }

    // Allow inlining of recursive calls, but only one level deep.
    IonBuilder *builder = callerBuilder_;
    while (builder) {
        if (builder->script == inlineScript) {
            IonSpew(IonSpew_Inlining, "Not inlining recursive call");
            return false;
        }
        builder = builder->callerBuilder_;
    }

    bool canInline = oracle->canEnterInlinedFunction(target);

    if (!canInline) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to oracle veto");
        return false;
    }

    IonSpew(IonSpew_Inlining, "Inlining good to go!");
    return true;
}

void
IonBuilder::popCfgStack()
{
    if (cfgStack_.back().isLoop())
        loops_.popBack();
    cfgStack_.popBack();
}

bool
IonBuilder::pushLoop(CFGState::State initial, jsbytecode *stopAt, MBasicBlock *entry,
                     jsbytecode *bodyStart, jsbytecode *bodyEnd, jsbytecode *exitpc,
                     jsbytecode *continuepc)
{
    if (!continuepc)
        continuepc = entry->pc();

    ControlFlowInfo loop(cfgStack_.length(), continuepc);
    if (!loops_.append(loop))
        return false;

    CFGState state;
    state.state = initial;
    state.stopAt = stopAt;
    state.loop.bodyStart = bodyStart;
    state.loop.bodyEnd = bodyEnd;
    state.loop.exitpc = exitpc;
    state.loop.entry = entry;
    state.loop.successor = NULL;
    state.loop.breaks = NULL;
    state.loop.continues = NULL;
    return cfgStack_.append(state);
}

bool
IonBuilder::build()
{
    current = newBlock(pc);
    if (!current)
        return false;

    IonSpew(IonSpew_MIR, "Analyzing script %s:%d (%p)",
            script->filename, script->lineno, (void *) script);

    if (!initParameters())
        return false;

    // Initialize local variables.
    for (uint32 i = 0; i < info().nlocals(); i++) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        current->initSlot(info().localSlot(i), undef);
    }

    // Guard against over-recursion.
    MCheckOverRecursed *check = new MCheckOverRecursed;
    current->add(check);
    check->setResumePoint(current->entryResumePoint());

    // Recompile to inline calls if this function is hot.
    insertRecompileCheck();

    current->makeStart(MStart::New(MStart::StartType_Default));

    // Parameters have been checked to correspond to the typeset, now we unbox
    // what we can in an infallible manner.
    rewriteParameters();

    // Prevent |this| from being DCE'd: necessary for constructors.
    if (info().fun())
        current->getSlot(info().thisSlot())->setGuard();

    // The type analysis phase attempts to insert unbox operations near
    // definitions of values. It also attempts to replace uses in resume points
    // with the narrower, unboxed variants. However, we must prevent this
    // replacement from happening on values in the entry snapshot. Otherwise we
    // could get this:
    //
    //       v0 = MParameter(0)
    //       v1 = MParameter(1)
    //       --   ResumePoint(v2, v3)
    //       v2 = Unbox(v0, INT32)
    //       v3 = Unbox(v1, INT32)
    //
    // So we attach the initial resume point to each parameter, which the type
    // analysis explicitly checks (this is the same mechanism used for
    // effectful operations).
    for (uint32 i = 0; i < CountArgSlots(info().fun()); i++) {
        MInstruction *ins = current->getEntrySlot(i)->toInstruction();
        if (ins->type() == MIRType_Value)
            ins->setResumePoint(current->entryResumePoint());
    }

    if (!traverseBytecode())
        return false;

    JS_ASSERT(loopDepth_ == 0);
    return true;
}

bool
IonBuilder::buildInline(IonBuilder *callerBuilder, MResumePoint *callerResumePoint,
                        MDefinition *thisDefn, MDefinitionVector &argv)
{
    current = newBlock(pc);
    if (!current)
        return false;

    IonSpew(IonSpew_MIR, "Inlining script %s:%d (%p)",
            script->filename, script->lineno, (void *) script);

    callerBuilder_ = callerBuilder;
    callerResumePoint_ = callerResumePoint;
    MBasicBlock *predecessor = callerResumePoint->block();
    predecessor->end(MGoto::New(current));
    if (!current->addPredecessorWithoutPhis(predecessor))
        return false;
    JS_ASSERT(predecessor->numSuccessors() == 1);
    JS_ASSERT(current->numPredecessors() == 1);
    current->setCallerResumePoint(callerResumePoint);

    // Fill in any missing arguments with undefined.
    const size_t nargs = info().nargs();
    if (argv.length() - 1 < nargs) {
        for (size_t i = 0, missing = nargs - (argv.length() - 1); i < missing; ++i) {
            MConstant *undef = MConstant::New(UndefinedValue());
            current->add(undef);
            if (!argv.append(undef))
                return false;
        }
    }

    // The oracle ensures that the inlined script does not use the scope chain, so we
    // just initialize its slot to |undefined|.
    JS_ASSERT(!script->analysis()->usesScopeChain());
    MInstruction *scope = MConstant::New(UndefinedValue());
    current->add(scope);
    current->initSlot(info().scopeChainSlot(), scope);

    current->initSlot(info().thisSlot(), thisDefn);

    IonSpew(IonSpew_Inlining, "Initializing %u arg slots", nargs);

    // Initialize argument references.
    MDefinitionVector::Range args = argv.all();
    args.popFront();
    JS_ASSERT(args.remain() >= nargs);
    for (size_t i = 0; i < nargs; ++i) {
        MDefinition *arg = args.popCopyFront();
        current->initSlot(info().argSlot(i), arg);
    }

    IonSpew(IonSpew_Inlining, "Initializing %u local slots", info().nlocals());

    // Initialize local variables.
    for (uint32 i = 0; i < info().nlocals(); i++) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        current->initSlot(info().localSlot(i), undef);
    }

    IonSpew(IonSpew_Inlining, "Inline entry block MResumePoint %p, %u operands",
            (void *) current->entryResumePoint(), current->entryResumePoint()->numOperands());

    // Note: +2 for the scope chain and |this|.
    JS_ASSERT(current->entryResumePoint()->numOperands() == nargs + info().nlocals() + 2);

    return traverseBytecode();
}

// Apply Type Inference information to parameters early on, unboxing them if
// they have a definitive type. The actual guards will be emitted by the code
// generator, explicitly, as part of the function prologue.
void
IonBuilder::rewriteParameters()
{
    JS_ASSERT(info().scopeChainSlot() == 0);
    static const uint32 START_SLOT = 1;

    for (uint32 i = START_SLOT; i < CountArgSlots(info().fun()); i++) {
        MParameter *param = current->getSlot(i)->toParameter();
        types::TypeSet *types = param->typeSet();
        if (!types)
            continue;

        JSValueType definiteType = types->getKnownTypeTag(cx);
        if (definiteType == JSVAL_TYPE_UNKNOWN)
            continue;

        MInstruction *actual = NULL;
        switch (definiteType) {
          case JSVAL_TYPE_UNDEFINED:
            actual = MConstant::New(UndefinedValue());
            break;

          case JSVAL_TYPE_NULL:
            actual = MConstant::New(NullValue());
            break;

          default:
            actual = MUnbox::New(param, MIRTypeFromValueType(definiteType), MUnbox::Infallible);
            break;
        }

        // Careful! We leave the original MParameter in the entry resume point. The
        // arguments still need to be checked unless proven otherwise at the call
        // site, and these checks can bailout. We can end up:
        //   v0 = Parameter(0)
        //   v1 = Unbox(v0, INT32)
        //   --   ResumePoint(v0)
        //
        // As usual, it would be invalid for v1 to be captured in the initial
        // resume point, rather than v0.
        current->add(actual);
        current->rewriteSlot(i, actual);
    }
}

bool
IonBuilder::initParameters()
{
    // The scope chain is only tracked in scripts that have NAME opcodes which
    // will try to access the scope. For other scripts, the scope instructions
    // will be held live by resume points and code will still be generated for
    // them, so just use a constant undefined value.
    MInstruction *scope;
    if (script->analysis()->usesScopeChain()) {
        if (info().fun()) {
            MCallee *callee = MCallee::New();
            current->add(callee);

            scope = MFunctionEnvironment::New(callee);
        } else {
            if (!script->compileAndGo)
                return abort("non-CNG global scripts are not supported");

            scope = MConstant::New(ObjectValue(*initialScopeChain_));
        }
    } else {
        scope = MConstant::New(UndefinedValue());
    }

    current->add(scope);
    current->initSlot(info().scopeChainSlot(), scope);

    if (!info().fun())
        return true;

    MParameter *param = MParameter::New(MParameter::THIS_SLOT,
                                        oracle->thisTypeSet(script));
    current->add(param);
    current->initSlot(info().thisSlot(), param);

    for (uint32 i = 0; i < info().nargs(); i++) {
        param = MParameter::New(i, oracle->parameterTypeSet(script, i));
        current->add(param);
        current->initSlot(info().argSlot(i), param);
    }

    return true;
}

// We try to build a control-flow graph in the order that it would be built as
// if traversing the AST. This leads to a nice ordering and lets us build SSA
// in one pass, since the bytecode is structured.
//
// We traverse the bytecode iteratively, maintaining a current basic block.
// Each basic block has a mapping of local slots to instructions, as well as a
// stack depth. As we encounter instructions we mutate this mapping in the
// current block.
//
// Things get interesting when we encounter a control structure. This can be
// either an IFEQ, downward GOTO, or a decompiler hint stashed away in source
// notes. Once we encounter such an opcode, we recover the structure of the
// control flow (its branches and bounds), and push it on a stack.
//
// As we continue traversing the bytecode, we look for points that would
// terminate the topmost control flow path pushed on the stack. These are:
//  (1) The bounds of the current structure (end of a loop or join/edge of a
//      branch).
//  (2) A "return", "break", or "continue" statement.
//
// For (1), we expect that there is a current block in the progress of being
// built, and we complete the necessary edges in the CFG. For (2), we expect
// that there is no active block.
//
// For normal diamond join points, we construct Phi nodes as we add
// predecessors. For loops, care must be taken to propagate Phi nodes back
// through uses in the loop body.
bool
IonBuilder::traverseBytecode()
{
    for (;;) {
        JS_ASSERT(pc < info().limitPC());

        for (;;) {
            if (!temp().ensureBallast())
                return false;

            // Check if we've hit an expected join point or edge in the bytecode.
            // Leaving one control structure could place us at the edge of another,
            // thus |while| instead of |if| so we don't skip any opcodes.
            if (!cfgStack_.empty() && cfgStack_.back().stopAt == pc) {
                ControlStatus status = processCfgStack();
                if (status == ControlStatus_Error)
                    return false;
                if (!current)
                    return true;
                continue;
            }

            // Some opcodes need to be handled early because they affect control
            // flow, terminating the current basic block and/or instructing the
            // traversal algorithm to continue from a new pc.
            //
            //   (1) If the opcode does not affect control flow, then the opcode
            //       is inspected and transformed to IR. This is the process_opcode
            //       label.
            //   (2) A loop could be detected via a forward GOTO. In this case,
            //       we don't want to process the GOTO, but the following
            //       instruction.
            //   (3) A RETURN, STOP, BREAK, or CONTINUE may require processing the
            //       CFG stack to terminate open branches.
            //
            // Similar to above, snooping control flow could land us at another
            // control flow point, so we iterate until it's time to inspect a real
            // opcode.
            ControlStatus status;
            if ((status = snoopControlFlow(JSOp(*pc))) == ControlStatus_None)
                break;
            if (status == ControlStatus_Error)
                return false;
            if (!current)
                return true;
        }

        // Nothing in inspectOpcode() is allowed to advance the pc.
        JSOp op = JSOp(*pc);
        if (!inspectOpcode(op))
            return false;

        pc += js_CodeSpec[op].length;
#ifdef TRACK_SNAPSHOTS
        current->updateTrackedPc(pc);
#endif
    }

    return true;
}

IonBuilder::ControlStatus
IonBuilder::snoopControlFlow(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return maybeLoop(op, info().getNote(cx, pc));

      case JSOP_POP:
        return maybeLoop(op, info().getNote(cx, pc));

      case JSOP_RETURN:
      case JSOP_STOP:
        return processReturn(op);

      case JSOP_THROW:
        return processThrow();

      case JSOP_GOTO:
      {
        jssrcnote *sn = info().getNote(cx, pc);
        switch (sn ? SN_TYPE(sn) : SRC_NULL) {
          case SRC_BREAK:
          case SRC_BREAK2LABEL:
            return processBreak(op, sn);

          case SRC_CONTINUE:
          case SRC_CONT2LABEL:
            return processContinue(op, sn);

          case SRC_SWITCHBREAK:
            return processSwitchBreak(op, sn);

          case SRC_WHILE:
          case SRC_FOR_IN:
            // while (cond) { }
            return whileOrForInLoop(op, sn);

          default:
            // Hard assert for now - make an error later.
            JS_NOT_REACHED("unknown goto case");
            break;
        }
        break;
      }

      case JSOP_TABLESWITCH:
        return tableSwitch(op, info().getNote(cx, pc));

      case JSOP_IFNE:
        // We should never reach an IFNE, it's a stopAt point, which will
        // trigger closing the loop.
        JS_NOT_REACHED("we should never reach an ifne!");
        return ControlStatus_Error;

      default:
        break;
    }
    return ControlStatus_None;
}

bool
IonBuilder::inspectOpcode(JSOp op)
{
    // Don't compile fat opcodes, run the decomposed version instead.
    if (js_CodeSpec[op].format & JOF_DECOMPOSE)
        return true;

    switch (op) {
      case JSOP_LOOPENTRY:
        return true;

      case JSOP_NOP:
        return true;

      case JSOP_LABEL:
        return true;

      case JSOP_UNDEFINED:
        return pushConstant(UndefinedValue());

      case JSOP_IFEQ:
        return jsop_ifeq(JSOP_IFEQ);

      case JSOP_BITNOT:
        return jsop_bitnot();

      case JSOP_BITAND:
      case JSOP_BITOR:
      case JSOP_BITXOR:
      case JSOP_LSH:
      case JSOP_RSH:
      case JSOP_URSH:
        return jsop_bitop(op);

      case JSOP_ADD:
      case JSOP_SUB:
      case JSOP_MUL:
      case JSOP_DIV:
      case JSOP_MOD:
      	return jsop_binary(op);

      case JSOP_POS:
        return jsop_pos();

      case JSOP_NEG:
        return jsop_neg();

      case JSOP_AND:
      case JSOP_OR:
        return jsop_andor(op);

      case JSOP_DEFVAR:
      case JSOP_DEFCONST:
        return jsop_defvar(GET_UINT32_INDEX(pc));

      case JSOP_LOCALINC:
      case JSOP_INCLOCAL:
      case JSOP_LOCALDEC:
      case JSOP_DECLOCAL:
        return jsop_localinc(op);

      case JSOP_EQ:
      case JSOP_NE:
      case JSOP_STRICTEQ:
      case JSOP_STRICTNE:
      case JSOP_LT:
      case JSOP_LE:
      case JSOP_GT:
      case JSOP_GE:
        return jsop_compare(op);

      case JSOP_ARGINC:
      case JSOP_INCARG:
      case JSOP_ARGDEC:
      case JSOP_DECARG:
        return jsop_arginc(op);

      case JSOP_DOUBLE:
        return pushConstant(info().getConst(pc));

      case JSOP_STRING:
        return pushConstant(StringValue(info().getAtom(pc)));

      case JSOP_ZERO:
        return pushConstant(Int32Value(0));

      case JSOP_ONE:
        return pushConstant(Int32Value(1));

      case JSOP_NULL:
        return pushConstant(NullValue());

      case JSOP_VOID:
        current->pop();
        return pushConstant(UndefinedValue());

      case JSOP_HOLE:
        return pushConstant(MagicValue(JS_ARRAY_HOLE));

      case JSOP_FALSE:
        return pushConstant(BooleanValue(false));

      case JSOP_TRUE:
        return pushConstant(BooleanValue(true));

      case JSOP_NOTEARG:
        return jsop_notearg();

      case JSOP_GETARG:
      case JSOP_CALLARG:
        current->pushArg(GET_SLOTNO(pc));
        return true;

      case JSOP_SETARG:
        current->setArg(GET_SLOTNO(pc));
        return true;

      case JSOP_GETLOCAL:
      case JSOP_CALLLOCAL:
        current->pushLocal(GET_SLOTNO(pc));
        return true;

      case JSOP_SETLOCAL:
        current->setLocal(GET_SLOTNO(pc));
        return true;

      case JSOP_POP:
        current->pop();
        return true;

      case JSOP_NEWINIT:
        if (GET_UINT8(pc) == JSProto_Array)
            return jsop_newarray(0);
        return jsop_newobject(NULL);

      case JSOP_NEWARRAY:
        return jsop_newarray(GET_UINT24(pc));

      case JSOP_NEWOBJECT:
        return jsop_newobject(info().getObject(pc));

      case JSOP_INITELEM:
        return jsop_initelem();

      case JSOP_INITPROP:
        return jsop_initprop(info().getAtom(pc));

      case JSOP_ENDINIT:
        return true;

      case JSOP_FUNCALL:
        return jsop_funcall(GET_ARGC(pc));

      case JSOP_CALL:
      case JSOP_NEW:
      case JSOP_FUNAPPLY:
        return jsop_call(GET_ARGC(pc), (JSOp)*pc == JSOP_NEW);

      case JSOP_INT8:
        return pushConstant(Int32Value(GET_INT8(pc)));

      case JSOP_UINT16:
        return pushConstant(Int32Value(GET_UINT16(pc)));

      case JSOP_GETGNAME:
      case JSOP_CALLGNAME:
        return jsop_getgname(info().getAtom(pc));

      case JSOP_BINDGNAME:
        return pushConstant(ObjectValue(*script->global()));

      case JSOP_SETGNAME:
        return jsop_setgname(info().getAtom(pc));

      case JSOP_NAME:
      case JSOP_CALLNAME:
        return jsop_getname(info().getAtom(pc));

      case JSOP_BINDNAME:
        return jsop_bindname(info().getName(pc));

      case JSOP_DUP:
        current->pushSlot(current->stackDepth() - 1);
        return true;

      case JSOP_DUP2:
        return jsop_dup2();

      case JSOP_SWAP:
        current->swapAt(-1);
        return true;

      case JSOP_PICK:
        current->pick(-GET_INT8(pc));
        return true;

      case JSOP_UINT24:
        return pushConstant(Int32Value(GET_UINT24(pc)));

      case JSOP_INT32:
        return pushConstant(Int32Value(GET_INT32(pc)));

      case JSOP_LOOPHEAD:
        // JSOP_LOOPHEAD is handled when processing the loop header.
        JS_NOT_REACHED("JSOP_LOOPHEAD outside loop");
        return true;

      case JSOP_GETELEM:
      case JSOP_CALLELEM:
        return jsop_getelem();

      case JSOP_SETELEM:
        return jsop_setelem();

      case JSOP_LENGTH:
        return jsop_length();

      case JSOP_NOT:
        return jsop_not();

      case JSOP_THIS:
        return jsop_this();

      case JSOP_GETPROP:
      case JSOP_CALLPROP:
        return jsop_getprop(info().getAtom(pc));

      case JSOP_SETPROP:
      case JSOP_SETNAME:
        return jsop_setprop(info().getAtom(pc));

      case JSOP_DELPROP:
        return jsop_delprop(info().getAtom(pc));

      case JSOP_REGEXP:
        return jsop_regexp(info().getRegExp(pc));

      case JSOP_OBJECT:
        return jsop_object(info().getObject(pc));

      case JSOP_TYPEOF:
      case JSOP_TYPEOFEXPR:
        return jsop_typeof();

      case JSOP_TOID:
        return jsop_toid();

      case JSOP_LAMBDA:
        return jsop_lambda(info().getFunction(pc));

      case JSOP_ITER:
        return jsop_iter(GET_INT8(pc));

      case JSOP_ITERNEXT:
        return jsop_iternext(GET_INT8(pc));

      case JSOP_MOREITER:
        return jsop_itermore();

      case JSOP_ENDITER:
        return jsop_iterend();

      default:
#ifdef DEBUG
        return abort("Unsupported opcode: %s (line %d)", js_CodeName[op], info().lineno(cx, pc));
#else
        return abort("Unsupported opcode: %d (line %d)", op, info().lineno(cx, pc));
#endif
    }
}

// Given that the current control flow structure has ended forcefully,
// via a return, break, or continue (rather than joining), propagate the
// termination up. For example, a return nested 5 loops deep may terminate
// every outer loop at once, if there are no intervening conditionals:
//
// for (...) {
//   for (...) {
//     return x;
//   }
// }
//
// If |current| is NULL when this function returns, then there is no more
// control flow to be processed.
IonBuilder::ControlStatus
IonBuilder::processControlEnd()
{
    JS_ASSERT(!current);

    if (cfgStack_.empty()) {
        // If there is no more control flow to process, then this is the
        // last return in the function.
        return ControlStatus_Ended;
    }

    return processCfgStack();
}

// Processes the top of the CFG stack. This is used from two places:
// (1) processControlEnd(), whereby a break, continue, or return may interrupt
//     an in-progress CFG structure before reaching its actual termination
//     point in the bytecode.
// (2) traverseBytecode(), whereby we reach the last instruction in a CFG
//     structure.
IonBuilder::ControlStatus
IonBuilder::processCfgStack()
{
    ControlStatus status = processCfgEntry(cfgStack_.back());

    // If this terminated a CFG structure, act like processControlEnd() and
    // keep propagating upward.
    while (status == ControlStatus_Ended) {
        popCfgStack();
        if (cfgStack_.empty())
            return status;
        status = processCfgEntry(cfgStack_.back());
    }

    // If some join took place, the current structure is finished.
    if (status == ControlStatus_Joined)
        popCfgStack();

    return status;
}

IonBuilder::ControlStatus
IonBuilder::processCfgEntry(CFGState &state)
{
    switch (state.state) {
      case CFGState::IF_TRUE:
      case CFGState::IF_TRUE_EMPTY_ELSE:
        return processIfEnd(state);

      case CFGState::IF_ELSE_TRUE:
        return processIfElseTrueEnd(state);

      case CFGState::IF_ELSE_FALSE:
        return processIfElseFalseEnd(state);

      case CFGState::DO_WHILE_LOOP_BODY:
        return processDoWhileBodyEnd(state);

      case CFGState::DO_WHILE_LOOP_COND:
        return processDoWhileCondEnd(state);

      case CFGState::WHILE_LOOP_COND:
        return processWhileCondEnd(state);

      case CFGState::WHILE_LOOP_BODY:
        return processWhileBodyEnd(state);

      case CFGState::FOR_LOOP_COND:
        return processForCondEnd(state);

      case CFGState::FOR_LOOP_BODY:
        return processForBodyEnd(state);

      case CFGState::FOR_LOOP_UPDATE:
        return processForUpdateEnd(state);

      case CFGState::TABLE_SWITCH:
        return processNextTableSwitchCase(state);

      case CFGState::AND_OR:
        return processAndOrEnd(state);

      default:
        JS_NOT_REACHED("unknown cfgstate");
    }
    return ControlStatus_Error;
}

IonBuilder::ControlStatus
IonBuilder::processIfEnd(CFGState &state)
{
    if (current) {
        // Here, the false block is the join point. Create an edge from the
        // current block to the false block. Note that a RETURN opcode
        // could have already ended the block.
        current->end(MGoto::New(state.branch.ifFalse));

        if (!state.branch.ifFalse->addPredecessor(current))
            return ControlStatus_Error;
    }

    current = state.branch.ifFalse;
    graph_.moveBlockToEnd(current);
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseTrueEnd(CFGState &state)
{
    // We've reached the end of the true branch of an if-else. Don't
    // create an edge yet, just transition to parsing the false branch.
    state.state = CFGState::IF_ELSE_FALSE;
    state.branch.ifTrue = current;
    state.stopAt = state.branch.falseEnd;
    pc = state.branch.ifFalse->pc();
    current = state.branch.ifFalse;
    graph_.moveBlockToEnd(current);
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseFalseEnd(CFGState &state)
{
    // Update the state to have the latest block from the false path.
    state.branch.ifFalse = current;

    // To create the join node, we need an incoming edge that has not been
    // terminated yet.
    MBasicBlock *pred = state.branch.ifTrue
                        ? state.branch.ifTrue
                        : state.branch.ifFalse;
    MBasicBlock *other = (pred == state.branch.ifTrue) ? state.branch.ifFalse : state.branch.ifTrue;

    if (!pred)
        return ControlStatus_Ended;

    // Create a new block to represent the join.
    MBasicBlock *join = newBlock(pred, state.branch.falseEnd);
    if (!join)
        return ControlStatus_Error;

    // Create edges from the true and false blocks as needed.
    pred->end(MGoto::New(join));

    if (other) {
        other->end(MGoto::New(join));
        if (!join->addPredecessor(other))
            return ControlStatus_Error;
    }

    // Ignore unreachable remainder of false block if existent.
    current = join;
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBrokenLoop(CFGState &state)
{
    JS_ASSERT(!current);

    JS_ASSERT(loopDepth_);
    loopDepth_--;

    // A broken loop is not a real loop (it has no header or backedge), so
    // reset the loop depth.
    for (MBasicBlockIterator i(graph_.begin(state.loop.entry)); i != graph_.end(); i++) {
        if (i->loopDepth() > loopDepth_)
            i->setLoopDepth(i->loopDepth() - 1);
    }

    // If the loop started with a condition (while/for) then even if the
    // structure never actually loops, the condition itself can still fail and
    // thus we must resume at the successor, if one exists.
    current = state.loop.successor;
    if (current) {
        JS_ASSERT(current->loopDepth() == loopDepth_);
        graph_.moveBlockToEnd(current);
    }

    // Join the breaks together and continue parsing.
    if (state.loop.breaks) {
        MBasicBlock *block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (current) {
            current->end(MGoto::New(block));
            if (!block->addPredecessor(current))
                return ControlStatus_Error;
        }

        current = block;
    }

    // If the loop is not gated on a condition, and has only returns, we'll
    // reach this case. For example:
    // do { ... return; } while ();
    if (!current)
        return ControlStatus_Ended;

    // Otherwise, the loop is gated on a condition and/or has breaks so keep
    // parsing at the successor.
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::finishLoop(CFGState &state, MBasicBlock *successor)
{
    JS_ASSERT(current);

    JS_ASSERT(loopDepth_);
    loopDepth_--;
    JS_ASSERT_IF(successor, successor->loopDepth() == loopDepth_);

    // Compute phis in the loop header and propagate them throughout the loop,
    // including the successor.
    if (!state.loop.entry->setBackedge(current))
        return ControlStatus_Error;
    if (successor) {
        graph_.moveBlockToEnd(successor);
        successor->inheritPhis(state.loop.entry);
    }

    if (state.loop.breaks) {
        // Propagate phis placed in the header to individual break exit points.
        DeferredEdge *edge = state.loop.breaks;
        while (edge) {
            edge->block->inheritPhis(state.loop.entry);
            edge = edge->next;
        }

        // Create a catch block to join all break exits.
        MBasicBlock *block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (successor) {
            // Finally, create an unconditional edge from the successor to the
            // catch block.
            successor->end(MGoto::New(block));
            if (!block->addPredecessor(successor))
                return ControlStatus_Error;
        }
        successor = block;
    }

    current = successor;

    // An infinite loop (for (;;) { }) will not have a successor.
    if (!current)
        return ControlStatus_Ended;

    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // No current means control flow cannot reach the condition, so this will
    // never loop.
    if (!current)
        return processBrokenLoop(state);

    MBasicBlock *header = newBlock(current, state.loop.updatepc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    state.state = CFGState::DO_WHILE_LOOP_COND;
    state.stopAt = state.loop.updateEnd;
    pc = state.loop.updatepc;
    current = header;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // We're guaranteed a |current|, it's impossible to break or return from
    // inside the conditional expression.
    JS_ASSERT(current);

    // Pop the last value, and create the successor block.
    MDefinition *vins = current->pop();
    MBasicBlock *successor = newBlock(current, GetNextPc(pc), loopDepth_ - 1);
    if (!successor)
        return ControlStatus_Error;

    // Create the test instruction and end the current block.
    MTest *test = MTest::New(vins, state.loop.entry, successor);
    current->end(test);
    return finishLoop(state, successor);
}

IonBuilder::ControlStatus
IonBuilder::processWhileCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Balance the stack past the IFNE.
    MDefinition *ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock *body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest *test = MTest::New(ins, body, state.loop.successor);
    current->end(test);

    state.state = CFGState::WHILE_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    current = body;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processWhileBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

IonBuilder::ControlStatus
IonBuilder::processForCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Balance the stack past the IFNE.
    MDefinition *ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock *body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest *test = MTest::New(ins, body, state.loop.successor);
    current->end(test);

    state.state = CFGState::FOR_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    current = body;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // If there is no updatepc, just go right to processing what would be the
    // end of the update clause. Otherwise, |current| might be NULL; if this is
    // the case, the udpate is unreachable anyway.
    if (!state.loop.updatepc || !current)
        return processForUpdateEnd(state);

    pc = state.loop.updatepc;

    state.state = CFGState::FOR_LOOP_UPDATE;
    state.stopAt = state.loop.updateEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForUpdateEnd(CFGState &state)
{
    // If there is no current, we couldn't reach the loop edge and there was no
    // update clause.
    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

bool
IonBuilder::processDeferredContinues(CFGState &state)
{
    // If there are any continues for this loop, and there is an update block,
    // then we need to create a new basic block to house the update.
    if (state.loop.continues) {
        DeferredEdge *edge = state.loop.continues;

        MBasicBlock *update = newBlock(edge->block, loops_.back().continuepc);
        if (!update)
            return false;

        if (current) {
            current->end(MGoto::New(update));
            if (!update->addPredecessor(current))
                return ControlStatus_Error;
        }

        // No need to use addPredecessor for first edge,
        // because it is already predecessor.
        edge->block->end(MGoto::New(update));
        edge = edge->next;

        // Remaining edges
        while (edge) {
            edge->block->end(MGoto::New(update));
            if (!update->addPredecessor(edge->block))
                return ControlStatus_Error;
            edge = edge->next;
        }
        state.loop.continues = NULL;

        current = update;
    }

    return true;
}

MBasicBlock *
IonBuilder::createBreakCatchBlock(DeferredEdge *edge, jsbytecode *pc)
{
    // Create block, using the first break statement as predecessor
    MBasicBlock *successor = newBlock(edge->block, pc);
    if (!successor)
        return NULL;

    // No need to use addPredecessor for first edge,
    // because it is already predecessor.
    edge->block->end(MGoto::New(successor));
    edge = edge->next;

    // Finish up remaining breaks.
    while (edge) {
        edge->block->end(MGoto::New(successor));
        if (!successor->addPredecessor(edge->block))
            return NULL;
        edge = edge->next;
    }

    return successor;
}

IonBuilder::ControlStatus
IonBuilder::processNextTableSwitchCase(CFGState &state)
{
    JS_ASSERT(state.state == CFGState::TABLE_SWITCH);

    state.tableswitch.currentBlock++;

    // Test if there are still unprocessed successors (cases/default)
    if (state.tableswitch.currentBlock >= state.tableswitch.ins->numBlocks())
        return processTableSwitchEnd(state);

    // Get the next successor
    MBasicBlock *successor = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock);

    // Add current block as predecessor if available.
    // This means the previous case didn't have a break statement.
    // So flow will continue in this block.
    if (current) {
        current->end(MGoto::New(successor));
        successor->addPredecessor(current);

        // Insert successor after the current block, to maintain RPO.
        graph_.moveBlockToEnd(successor);
    }

    // If this is the last successor the block should stop at the end of the tableswitch
    // Else it should stop at the start of the next successor
    if (state.tableswitch.currentBlock+1 < state.tableswitch.ins->numBlocks())
        state.stopAt = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock+1)->pc();
    else
        state.stopAt = state.tableswitch.exitpc;

    current = successor;
    pc = current->pc();
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processTableSwitchEnd(CFGState &state)
{
    // No break statements and no current
    // This means that control flow is cut-off from this point
    // (e.g. all cases have return statements).
    if (!state.tableswitch.breaks && !current)
        return ControlStatus_Ended;

    // Create successor block.
    // If there are breaks, create block with breaks as predecessor
    // Else create a block with current as predecessor
    MBasicBlock *successor = NULL;
    if (state.tableswitch.breaks)
        successor = createBreakCatchBlock(state.tableswitch.breaks, state.tableswitch.exitpc);
    else
        successor = newBlock(current, state.tableswitch.exitpc);

    if (!successor)
        return ControlStatus_Ended;

    // If there is current, the current block flows into this one.
    // So current is also a predecessor to this block
    if (current) {
        current->end(MGoto::New(successor));
        if (state.tableswitch.breaks)
            successor->addPredecessor(current);
    }

    pc = state.tableswitch.exitpc;
    current = successor;
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processAndOrEnd(CFGState &state)
{
    // We just processed the RHS of an && or || expression.
    // Now jump to the join point (the false block).
    current->end(MGoto::New(state.branch.ifFalse));

    if (!state.branch.ifFalse->addPredecessor(current))
        return ControlStatus_Error;

    current = state.branch.ifFalse;
    graph_.moveBlockToEnd(current);
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBreak(JSOp op, jssrcnote *sn)
{
    JS_ASSERT(op == JSOP_GOTO);

    // Find the target loop.
    CFGState *found = NULL;
    jsbytecode *target = pc + GetJumpOffset(pc);
    for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
        CFGState &cfg = cfgStack_[loops_[i].cfgEntry];
        if (cfg.loop.exitpc == target) {
            found = &cfg;
            break;
        }
    }

    if (!found) {
        // Sometimes, we can't determine the structure of a labeled break. For
        // example:
        //
        // 0:    label: {
        // 1:        for (;;) {
        // 2:            break label;
        // 3:        }
        // 4:        stuff;
        // 5:    }
        //
        // In this case, the successor of the block is 4, but the target of the
        // single-level break is actually 5. To recognize this case we'd need
        // to know about the label structure at 0,5 ahead of time - and lacking
        // those source notes for now, we just abort instead.
        abort("could not find the target of a break");
        return ControlStatus_Error;
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    CFGState &state = *found;

    state.loop.breaks = new DeferredEdge(current, state.loop.breaks);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

static inline jsbytecode *
EffectiveContinue(jsbytecode *pc)
{
    if (JSOp(*pc) == JSOP_GOTO)
        return pc + GetJumpOffset(pc);
    return pc;
}

IonBuilder::ControlStatus
IonBuilder::processContinue(JSOp op, jssrcnote *sn)
{
    JS_ASSERT(op == JSOP_GOTO);

    // Find the target loop.
    CFGState *found = NULL;
    jsbytecode *target = pc + GetJumpOffset(pc);
    for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
        if (loops_[i].continuepc == target ||
            EffectiveContinue(loops_[i].continuepc) == target)
        {
            found = &cfgStack_[loops_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    JS_ASSERT(found);
    CFGState &state = *found;

    state.loop.continues = new DeferredEdge(current, state.loop.continues);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processSwitchBreak(JSOp op, jssrcnote *sn)
{
    JS_ASSERT(op == JSOP_GOTO);

    // Find the target switch.
    CFGState *found = NULL;
    jsbytecode *target = pc + GetJumpOffset(pc);
    for (size_t i = switches_.length() - 1; i < switches_.length(); i--) {
        if (switches_[i].continuepc == target) {
            found = &cfgStack_[switches_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    JS_ASSERT(found);
    CFGState &state = *found;

    state.tableswitch.breaks = new DeferredEdge(current, state.tableswitch.breaks);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::maybeLoop(JSOp op, jssrcnote *sn)
{
    // This function looks at the opcode and source note and tries to
    // determine the structure of the loop. For some opcodes, like
    // POP/NOP which are not explicitly control flow, this source note is
    // optional. For opcodes with control flow, like GOTO, an unrecognized
    // or not-present source note is a compilation failure.
    switch (op) {
      case JSOP_POP:
        // for (init; ; update?) ...
        if (sn && SN_TYPE(sn) == SRC_FOR) {
            current->pop();
            return forLoop(op, sn);
        }
        break;

      case JSOP_NOP:
        if (sn) {
            // do { } while (cond)
            if (SN_TYPE(sn) == SRC_WHILE)
                return doWhileLoop(op, sn);
            // Build a mapping such that given a basic block, whose successor
            // has a phi

            // for (; ; update?)
            if (SN_TYPE(sn) == SRC_FOR)
                return forLoop(op, sn);
        }
        break;

      default:
        JS_NOT_REACHED("unexpected opcode");
        return ControlStatus_Error;
    }

    return ControlStatus_None;
}

void
IonBuilder::assertValidLoopHeadOp(jsbytecode *pc)
{
#ifdef DEBUG
    JS_ASSERT(JSOp(*pc) == JSOP_LOOPHEAD);

    // Make sure this is the next opcode after the loop header,
    // unless the for loop is unconditional.
    CFGState &state = cfgStack_.back();
    JS_ASSERT_IF((JSOp)*(state.loop.entry->pc()) == JSOP_GOTO,
        GetNextPc(state.loop.entry->pc()) == pc);

    // do-while loops have a source note.
    jssrcnote *sn = info().getNote(cx, pc);
    if (sn) {
        jsbytecode *ifne = pc + js_GetSrcNoteOffset(sn, 0);

        jsbytecode *expected_ifne;
        switch (state.state) {
          case CFGState::DO_WHILE_LOOP_BODY:
            expected_ifne = state.loop.updateEnd;
            break;

          default:
            JS_NOT_REACHED("JSOP_LOOPHEAD unexpected source note");
            return;
        }

        // Make sure this loop goes to the same ifne as the loop header's
        // source notes or GOTO.
        JS_ASSERT(ifne == expected_ifne);
    } else {
        JS_ASSERT(state.state != CFGState::DO_WHILE_LOOP_BODY);
    }
#endif
}

IonBuilder::ControlStatus
IonBuilder::doWhileLoop(JSOp op, jssrcnote *sn)
{
    // do { } while() loops have the following structure:
    //    NOP         ; SRC_WHILE (offset to COND)
    //    LOOPHEAD    ; SRC_WHILE (offset to IFNE)
    //    LOOPENTRY
    //    ...         ; body
    //    ...
    //    COND        ; start of condition
    //    ...
    //    IFNE ->     ; goes to LOOPHEAD
    int condition_offset = js_GetSrcNoteOffset(sn, 0);
    jsbytecode *conditionpc = pc + condition_offset;

    jssrcnote *sn2 = info().getNote(cx, pc+1);
    int offset = js_GetSrcNoteOffset(sn2, 0);
    jsbytecode *ifne = pc + offset + 1;
    JS_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    jsbytecode *loopHead = GetNextPc(pc);
    JS_ASSERT(JSOp(*loopHead) == JSOP_LOOPHEAD);
    JS_ASSERT(loopHead == ifne + GetJumpOffset(ifne));

    jsbytecode *loopEntry = GetNextPc(loopHead);
    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopHead, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    jsbytecode *bodyStart = GetNextPc(GetNextPc(pc));
    jsbytecode *bodyEnd = conditionpc;
    jsbytecode *exitpc = GetNextPc(ifne);
    if (!pushLoop(CFGState::DO_WHILE_LOOP_BODY, conditionpc, header, bodyStart, bodyEnd, exitpc, conditionpc))
        return ControlStatus_Error;

    CFGState &state = cfgStack_.back();
    state.loop.updatepc = conditionpc;
    state.loop.updateEnd = ifne;

    current = header;
    if (!jsop_loophead(GetNextPc(pc)))
        return ControlStatus_Error;

    pc = bodyStart;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::whileOrForInLoop(JSOp op, jssrcnote *sn)
{
    // while (cond) { } loops have the following structure:
    //    GOTO cond   ; SRC_WHILE (offset to IFNE)
    //    LOOPHEAD
    //    ...
    //  cond:
    //    LOOPENTRY
    //    ...
    //    IFNE        ; goes to LOOPHEAD
    // for (x in y) { } loops are similar; the cond will be a MOREITER.
    size_t which = (SN_TYPE(sn) == SRC_FOR_IN) ? 1 : 0;
    int ifneOffset = js_GetSrcNoteOffset(sn, which);
    jsbytecode *ifne = pc + ifneOffset;
    JS_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    jsbytecode *loopHead = GetNextPc(pc);
    JS_ASSERT(JSOp(*loopHead) == JSOP_LOOPHEAD);
    JS_ASSERT(loopHead == ifne + GetJumpOffset(ifne));

    jsbytecode *loopEntry = pc + GetJumpOffset(pc);
    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopHead, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    // Skip past the JSOP_LOOPHEAD for the body start.
    jsbytecode *bodyStart = GetNextPc(GetNextPc(pc));
    jsbytecode *bodyEnd = pc + GetJumpOffset(pc);
    jsbytecode *exitpc = GetNextPc(ifne);
    if (!pushLoop(CFGState::WHILE_LOOP_COND, ifne, header, bodyStart, bodyEnd, exitpc))
        return ControlStatus_Error;

    // Parse the condition first.
    current = header;
    if (!jsop_loophead(GetNextPc(pc)))
        return ControlStatus_Error;

    pc = bodyEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::forLoop(JSOp op, jssrcnote *sn)
{
    // Skip the NOP or POP.
    JS_ASSERT(op == JSOP_POP || op == JSOP_NOP);
    pc = GetNextPc(pc);

    jsbytecode *condpc = pc + js_GetSrcNoteOffset(sn, 0);
    jsbytecode *updatepc = pc + js_GetSrcNoteOffset(sn, 1);
    jsbytecode *ifne = pc + js_GetSrcNoteOffset(sn, 2);
    jsbytecode *exitpc = GetNextPc(ifne);

    // for loops have the following structures:
    //
    //   NOP or POP
    //   [GOTO cond | NOP]
    //   LOOPHEAD
    // body:
    //    ; [body]
    // [increment:]
    //    ; [increment]
    // [cond:]
    //   LOOPENTRY
    //   GOTO body
    //
    // If there is a condition (condpc != ifne), this acts similar to a while
    // loop otherwise, it acts like a do-while loop.
    jsbytecode *bodyStart = pc;
    jsbytecode *bodyEnd = updatepc;
    jsbytecode *loopEntry = condpc;
    if (condpc != ifne) {
        JS_ASSERT(JSOp(*bodyStart) == JSOP_GOTO);
        JS_ASSERT(bodyStart + GetJumpOffset(bodyStart) == condpc);
        bodyStart = GetNextPc(bodyStart);
    } else {
        // No loop condition, such as for(j = 0; ; j++)
        if (op != JSOP_NOP) {
            // If the loop starts with POP, we have to skip a NOP.
            JS_ASSERT(JSOp(*bodyStart) == JSOP_NOP);
            bodyStart = GetNextPc(bodyStart);
        }
        loopEntry = GetNextPc(bodyStart);
    }
    jsbytecode *loopHead = bodyStart;
    JS_ASSERT(JSOp(*bodyStart) == JSOP_LOOPHEAD);
    JS_ASSERT(ifne + GetJumpOffset(ifne) == bodyStart);
    bodyStart = GetNextPc(bodyStart);

    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopHead, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    // If there is no condition, we immediately parse the body. Otherwise, we
    // parse the condition.
    jsbytecode *stopAt;
    CFGState::State initial;
    if (condpc != ifne) {
        pc = condpc;
        stopAt = ifne;
        initial = CFGState::FOR_LOOP_COND;
    } else {
        pc = bodyStart;
        stopAt = bodyEnd;
        initial = CFGState::FOR_LOOP_BODY;
    }

    if (!pushLoop(initial, stopAt, header, bodyStart, bodyEnd, exitpc, updatepc))
        return ControlStatus_Error;

    CFGState &state = cfgStack_.back();
    state.loop.condpc = (condpc != ifne) ? condpc : NULL;
    state.loop.updatepc = (updatepc != condpc) ? updatepc : NULL;
    if (state.loop.updatepc)
        state.loop.updateEnd = condpc;

    current = header;
    if (!jsop_loophead(loopHead))
        return ControlStatus_Error;

    return ControlStatus_Jumped;
}

int
IonBuilder::CmpSuccessors(const void *a, const void *b)
{
    const MBasicBlock *a0 = * (MBasicBlock * const *)a;
    const MBasicBlock *b0 = * (MBasicBlock * const *)b;
    if (a0->pc() == b0->pc())
        return 0;

    return (a0->pc() > b0->pc()) ? 1 : -1;
}

IonBuilder::ControlStatus
IonBuilder::tableSwitch(JSOp op, jssrcnote *sn)
{
    // TableSwitch op contains the following data
    // (length between data is JUMP_OFFSET_LEN)
    //
    // 0: Offset of default case
    // 1: Lowest number in tableswitch
    // 2: Highest number in tableswitch
    // 3: Offset of case low
    // 4: Offset of case low+1
    // .: ...
    // .: Offset of case high

    JS_ASSERT(op == JSOP_TABLESWITCH);

    // Pop input.
    MDefinition *ins = current->pop();

    // Get the default and exit pc
    jsbytecode *exitpc = pc + js_GetSrcNoteOffset(sn, 0);
    jsbytecode *defaultpc = pc + GET_JUMP_OFFSET(pc);

    JS_ASSERT(defaultpc > pc && defaultpc <= exitpc);

    // Get the low and high from the tableswitch
    jsbytecode *pc2 = pc;
    pc2 += JUMP_OFFSET_LEN;
    int low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int high = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;

    // Create MIR instruction
    MTableSwitch *tableswitch = MTableSwitch::New(ins, low, high);

    // Create default case
    MBasicBlock *defaultcase = newBlock(current, defaultpc);
    if (!defaultcase)
        return ControlStatus_Error;
    tableswitch->addDefault(defaultcase);
    tableswitch->addBlock(defaultcase);

    // Create cases
    jsbytecode *casepc = NULL;
    for (int i = 0; i < high-low+1; i++) {
        casepc = pc + GET_JUMP_OFFSET(pc2);

        JS_ASSERT(casepc >= pc && casepc <= exitpc);

        MBasicBlock *caseblock = newBlock(current, casepc);
        if (!caseblock)
            return ControlStatus_Error;

        // If the casepc equals the current pc, it is not a written case,
        // but a filled gap. That way we can use a tableswitch instead of
        // lookupswitch, even if not all numbers are consecutive.
        // In that case this block goes to the default case
        if (casepc == pc) {
            caseblock->end(MGoto::New(defaultcase));
            defaultcase->addPredecessor(caseblock);
        }

        tableswitch->addCase(caseblock);
        
        // If this is an actual case (not filled gap),
        // add this block to the list that still needs to get processed 
        if (casepc != pc)
            tableswitch->addBlock(caseblock);

        pc2 += JUMP_OFFSET_LEN;
    }

    // Move defaultcase to the end, to maintain RPO.
    graph_.moveBlockToEnd(defaultcase);

    JS_ASSERT(tableswitch->numCases() == (uint32)(high - low + 1));
    JS_ASSERT(tableswitch->numSuccessors() > 0);

    // Sort the list of blocks that still needs to get processed by pc
    qsort(tableswitch->blocks(), tableswitch->numBlocks(),
          sizeof(MBasicBlock*), CmpSuccessors);

    // Create info
    ControlFlowInfo switchinfo(cfgStack_.length(), exitpc);
    if (!switches_.append(switchinfo))
        return ControlStatus_Error;

    // Use a state to retrieve some information
    CFGState state;
    state.state = CFGState::TABLE_SWITCH;
    state.tableswitch.exitpc = exitpc;
    state.tableswitch.breaks = NULL;
    state.tableswitch.ins = tableswitch;
    state.tableswitch.currentBlock = 0;

    // Save the MIR instruction as last instruction of this block.
    current->end(tableswitch);

    // If there is only one successor the block should stop at the end of the switch
    // Else it should stop at the start of the next successor
    if (tableswitch->numBlocks() == 1)
        state.stopAt = exitpc;
    else
        state.stopAt = tableswitch->getBlock(1)->pc();
    current = tableswitch->getBlock(0);

    if (!cfgStack_.append(state))
        return ControlStatus_Error;

    pc = current->pc();
    return ControlStatus_Jumped;
}

bool
IonBuilder::jsop_andor(JSOp op)
{
    jsbytecode *rhsStart = pc + js_CodeSpec[op].length;
    jsbytecode *joinStart = pc + GetJumpOffset(pc);
    JS_ASSERT(joinStart > pc);

    // We have to leave the LHS on the stack.
    MDefinition *lhs = current->peek(-1);

    MBasicBlock *evalRhs = newBlock(current, rhsStart);
    MBasicBlock *join = newBlock(current, joinStart);
    if (!evalRhs || !join)
        return false;

    if (op == JSOP_AND) {
        current->end(MTest::New(lhs, evalRhs, join));
    } else {
        JS_ASSERT(op == JSOP_OR);
        current->end(MTest::New(lhs, join, evalRhs));
    }

    if (!cfgStack_.append(CFGState::AndOr(joinStart, join)))
        return false;

    current = evalRhs;
    return true;
}

bool
IonBuilder::jsop_dup2()
{
    uint32 lhsSlot = current->stackDepth() - 2;
    uint32 rhsSlot = current->stackDepth() - 1;
    current->pushSlot(lhsSlot);
    current->pushSlot(rhsSlot);
    return true;
}

bool
IonBuilder::jsop_loophead(jsbytecode *pc)
{
    assertValidLoopHeadOp(pc);
    insertRecompileCheck();
    return true;
}

bool
IonBuilder::jsop_ifeq(JSOp op)
{
    // IFEQ always has a forward offset.
    jsbytecode *trueStart = pc + js_CodeSpec[op].length;
    jsbytecode *falseStart = pc + GetJumpOffset(pc);
    JS_ASSERT(falseStart > pc);

    // We only handle cases that emit source notes.
    jssrcnote *sn = info().getNote(cx, pc);
    if (!sn)
        return abort("expected sourcenote");

    MDefinition *ins = current->pop();

    // Create true and false branches.
    MBasicBlock *ifTrue = newBlock(current, trueStart);
    MBasicBlock *ifFalse = newBlock(current, falseStart);
    if (!ifTrue || !ifFalse)
        return false;

    current->end(MTest::New(ins, ifTrue, ifFalse));

    // The bytecode for if/ternary gets emitted either like this:
    //
    //    IFEQ X  ; src note (IF_ELSE, COND) points to the GOTO
    //    ...
    //    GOTO Z
    // X: ...     ; else/else if
    //    ...
    // Z:         ; join
    //
    // Or like this:
    //
    //    IFEQ X  ; src note (IF) has no offset
    //    ...
    // Z: ...     ; join
    //
    // We want to parse the bytecode as if we were parsing the AST, so for the
    // IF_ELSE/COND cases, we use the source note and follow the GOTO. For the
    // IF case, the IFEQ offset is the join point.
    switch (SN_TYPE(sn)) {
      case SRC_IF:
        if (!cfgStack_.append(CFGState::If(falseStart, ifFalse)))
            return false;
        break;

      case SRC_IF_ELSE:
      case SRC_COND:
      {
        // Infer the join point from the JSOP_GOTO[X] sitting here, then
        // assert as we much we can that this is the right GOTO.
        jsbytecode *trueEnd = pc + js_GetSrcNoteOffset(sn, 0);
        JS_ASSERT(trueEnd > pc);
        JS_ASSERT(trueEnd < falseStart);
        JS_ASSERT(JSOp(*trueEnd) == JSOP_GOTO);
        JS_ASSERT(!info().getNote(cx, trueEnd));

        jsbytecode *falseEnd = trueEnd + GetJumpOffset(trueEnd);
        JS_ASSERT(falseEnd > trueEnd);
        JS_ASSERT(falseEnd >= falseStart);

        if (!cfgStack_.append(CFGState::IfElse(trueEnd, falseEnd, ifFalse)))
            return false;
        break;
      }

      default:
        JS_NOT_REACHED("unexpected source note type");
        break;
    }

    // Switch to parsing the true branch. Note that no PC update is needed,
    // it's the next instruction.
    current = ifTrue;

    return true;
}

IonBuilder::ControlStatus
IonBuilder::processReturn(JSOp op)
{
    MDefinition *def;
    switch (op) {
      case JSOP_RETURN:
        def = current->pop();
        break;

      case JSOP_STOP:
      {
        MInstruction *ins = MConstant::New(UndefinedValue());
        current->add(ins);
        def = ins;
        break;
      }

      default:
        def = NULL;
        JS_NOT_REACHED("unknown return op");
        break;
    }

    MReturn *ret = MReturn::New(def);
    current->end(ret);

    if (!graph().addExit(current))
        return ControlStatus_Error;

    // Make sure no one tries to use this block now.
    current = NULL;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processThrow()
{
    MDefinition *def = current->pop();

    MThrow *ins = MThrow::New(def);
    current->end(ins);

    if (!graph().addExit(current))
        return ControlStatus_Error;

    // Make sure no one tries to use this block now.
    current = NULL;
    return processControlEnd();
}

bool
IonBuilder::pushConstant(const Value &v)
{
    MConstant *ins = MConstant::New(v);
    current->add(ins);
    current->push(ins);
    return true;
}

bool
IonBuilder::jsop_bitnot()
{
    MDefinition *input = current->pop();
    MBitNot *ins = MBitNot::New(input);

    current->add(ins);
    ins->infer(oracle->unaryOp(script, pc));

    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}
bool
IonBuilder::jsop_bitop(JSOp op)
{
    // Pop inputs.
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    MBinaryBitwiseInstruction *ins;
    switch (op) {
      case JSOP_BITAND:
        ins = MBitAnd::New(left, right);
        break;

      case JSOP_BITOR:
        ins = MBitOr::New(left, right);
        break;

      case JSOP_BITXOR:
        ins = MBitXor::New(left, right);
        break;

      case JSOP_LSH:
        ins = MLsh::New(left, right);
        break;

      case JSOP_RSH:
        ins = MRsh::New(left, right);
        break;

      case JSOP_URSH:
        ins = MUrsh::New(left, right);
        break;

      default:
        JS_NOT_REACHED("unexpected bitop");
        return false;
    }

    current->add(ins);
    ins->infer(oracle->binaryOp(script, pc));

    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;

    return true;
}

bool
IonBuilder::jsop_binary(JSOp op, MDefinition *left, MDefinition *right)
{
    TypeOracle::Binary b = oracle->binaryOp(script, pc);

    if (op == JSOP_ADD && b.rval == MIRType_String &&
        (b.lhs == MIRType_String || b.lhs == MIRType_Int32) &&
        (b.rhs == MIRType_String || b.rhs == MIRType_Int32))
    {
        MConcat *ins = MConcat::New(left, right);
        current->add(ins);
        current->push(ins);
        return true;
    }

    MBinaryArithInstruction *ins;
    switch (op) {
      case JSOP_ADD:
        ins = MAdd::New(left, right);
        break;

      case JSOP_SUB:
        ins = MSub::New(left, right);
        break;

      case JSOP_MUL:
        ins = MMul::New(left, right);
        break;

      case JSOP_DIV:
        ins = MDiv::New(left, right);
        break;

      case JSOP_MOD:
        ins = MMod::New(left, right);
        break;

      default:
        JS_NOT_REACHED("unexpected binary opcode");
        return false;
    }

    TypeOracle::BinaryTypes types = oracle->binaryTypes(script, pc);
    current->add(ins);
    ins->infer(cx, types);
    current->push(ins);

    if (ins->isEffectful())
        return resumeAfter(ins);
    return true;
}

bool
IonBuilder::jsop_binary(JSOp op)
{
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    return jsop_binary(op, left, right);
}

bool
IonBuilder::jsop_pos()
{
    TypeOracle::Unary types = oracle->unaryOp(script, pc);
    MDefinition *value = current->pop();
    if (types.rval == MIRType_Int32) {
        // Already an int32, no semantic difference!
        current->push(value);
        return true;
    }

    MToDouble *ins = MToDouble::New(value);
    current->add(ins);
    current->push(ins);

    // If the expected type is >= string, we compile this as a more expensive
    // variant and consider it effectful.
    if (types.ival >= MIRType_String) {
       ins->unspecialize();
       if (!resumeAfter(ins))
           return false;
    }

    return true;
}

bool
IonBuilder::jsop_neg()
{
    // Since JSOP_NEG does not use a slot, we cannot push the MConstant.
    // The MConstant is therefore passed to JSOP_MUL without slot traffic.
    MConstant *negator = MConstant::New(Int32Value(-1));
    current->add(negator);

    MDefinition *right = current->pop();

    if (!jsop_binary(JSOP_MUL, negator, right))
        return false;
    return true;
}

bool
IonBuilder::jsop_notearg()
{
    // JSOP_NOTEARG notes that the value in current->pop() has just
    // been pushed onto the stack for use in calling a function.
    MDefinition *def = current->pop();
    MPassArg *arg = MPassArg::New(def);

    current->add(arg);
    current->push(arg);
    return true;
}

bool
IonBuilder::jsop_call_inline(JSFunction *callee, uint32 argc, IonBuilder &inlineBuilder)
{
#ifdef DEBUG
    uint32 origStackDepth = current->stackDepth();
#endif

    // |top| jumps into the callee subgraph -- save it for later use.
    MBasicBlock *top = current;

    // Add the function constant before the resume point whihc map call
    // arguments.
    MConstant *constFun = MConstant::New(ObjectValue(*callee));
    current->add(constFun);

    // This resume point collects outer variables only; it is never used to
    // directly build a snapshot.
    MResumePoint *inlineResumePoint = MResumePoint::New(top, pc, callerResumePoint_,
                                                        MResumePoint::Outer);
    if (!inlineResumePoint)
        return false;

    // Gather up the arguments and |this| to the inline function.
    // Note that we leave the callee on the simulated stack for the
    // duration of the call.
    MDefinitionVector argv;

    // Arguments are popped right-to-left so we have to fill |args| backwards.
    if (!discardCallArgs(argc, argv, top))
        return false;

    // Replace the potential object load by the corresponding constant version
    // which is inlined here.
    inlineResumePoint->replaceOperand(inlineResumePoint->numOperands() - (argc + 2), constFun);
    current->pop();
    current->push(constFun);

    MDefinition *thisDefn = argv[0];

    // Build the graph.
    if (!inlineBuilder.buildInline(this, inlineResumePoint, thisDefn, argv))
        return false;

    MIRGraphExits &exits = *inlineBuilder.graph().exitAccumulator();

    // Create a |bottom| block for all the callee subgraph exits to jump to.
    JS_ASSERT(*pc == JSOP_CALL);
    jsbytecode *postCall = GetNextPc(pc);
    MBasicBlock *bottom = newBlock(NULL, postCall);
    bottom->setCallerResumePoint(callerResumePoint_);

    // Link graph exits to |bottom| via MGotos, replacing MReturns.
    Vector<MDefinition *, 8, IonAllocPolicy> retvalDefns;
    for (MBasicBlock **it = exits.begin(), **end = exits.end(); it != end; ++it) {
        MBasicBlock *exitBlock = *it;
        JS_ASSERT(exitBlock->lastIns()->op() == MDefinition::Op_Return);

        if (!retvalDefns.append(exitBlock->lastIns()->toReturn()->getOperand(0)))
            return false;

        exitBlock->discardLastIns();
        MGoto *replacement = MGoto::New(bottom);
        exitBlock->end(replacement);
        if (!bottom->addPredecessorWithoutPhis(exitBlock))
            return false;
    }
    JS_ASSERT(!retvalDefns.empty());

    if (!bottom->inheritNonPredecessor(top))
        return false;

    // Pop the callee and push the return value.
    (void) bottom->pop();

    MDefinition *retvalDefn;
    if (retvalDefns.length() > 1) {
        // Need to create a phi to merge the returns together.
        MPhi *phi = MPhi::New(bottom->stackDepth());
        bottom->addPhi(phi);

        for (MDefinition **it = retvalDefns.begin(), **end = retvalDefns.end(); it != end; ++it) {
            if (!phi->addInput(*it))
                return false;
        }
        retvalDefn = phi;
    } else {
        retvalDefn = retvalDefns.back();
    }

    bottom->push(retvalDefn);

    // Replace the callee with the return value in the resumepoint.
    uint32 retvalSlot = bottom->stackDepth() - 1;
    bottom->entryResumePoint()->replaceOperand(retvalSlot, retvalDefn);

    // Check the depth change:
    //  -argc for popped args
    //  -2 for callee/this
    //  +1 for retval
    JS_ASSERT(bottom->stackDepth() == origStackDepth - argc - 1);

    current = bottom;
    return true;
}

class AutoAccumulateExits
{
    MIRGraph &graph_;
    MIRGraphExits *prev_;

  public:
    AutoAccumulateExits(MIRGraph &graph, MIRGraphExits &exits) : graph_(graph) {
        prev_ = graph_.exitAccumulator();
        graph_.setExitAccumulator(&exits);
    }
    ~AutoAccumulateExits() {
        graph_.setExitAccumulator(prev_);
    }
};

bool
IonBuilder::makeInliningDecision(JSFunction *target)
{
    if (inliningDepth >= 2)
        return false;

    if (script->getUseCount() < js_IonOptions.usesBeforeInlining) {
        IonSpew(IonSpew_Inlining, "Not inlining, caller is not hot");
        return false;
    }

    if (!oracle->canInlineCall(script, pc)) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to uninlineable call site");
        return false;
    }

    if (!canInlineTarget(target)) {
        IonSpew(IonSpew_Inlining, "Decided not to inline");
        return false;
    }

    return true;
}

bool
IonBuilder::inlineScriptedCall(JSFunction *target, uint32 argc)
{
    IonSpew(IonSpew_Inlining, "Recursively building");

    // Compilation information is allocated for the duration of the current tempLifoAlloc
    // lifetime.
    CompileInfo *info = cx->tempLifoAlloc().new_<CompileInfo>(target->script(),
                                                              target, (jsbytecode *)NULL);
    if (!info)
        return false;

    MIRGraphExits exits;
    AutoAccumulateExits aae(graph(), exits);

    if (cx->typeInferenceEnabled()) {
        TypeInferenceOracle oracle;
        if (!oracle.init(cx, target->script()))
            return false;
        IonBuilder inlineBuilder(cx, NULL, temp(), graph(), &oracle,
                                 *info, inliningDepth + 1, loopDepth_);
        return jsop_call_inline(target, argc, inlineBuilder);
    }

    DummyOracle oracle;
    IonBuilder inlineBuilder(cx, NULL, temp(), graph(), &oracle,
                             *info, inliningDepth + 1, loopDepth_);
    return jsop_call_inline(target, argc, inlineBuilder);
}

MDefinition *
IonBuilder::createThisNative()
{
    // Native constructors build the new Object themselves.
    MConstant *magic = MConstant::New(MagicValue(JS_IS_CONSTRUCTING));
    current->add(magic);
    return magic;
}

MDefinition *
IonBuilder::createThisScripted(MDefinition *callee)
{
    // Get callee.prototype.
    // This instruction MUST be idempotent: since it does not correspond to an
    // explicit operation in the bytecode, we cannot use resumeAfter(). But
    // calling GetProperty can trigger a GC, and thus invalidation.
    MCallGetProperty *getProto =
        MCallGetProperty::New(callee, cx->runtime->atomState.classPrototypeAtom);

    // Getters may not override |prototype| fetching, so this is repeatable.
    getProto->markUneffectful();
    current->add(getProto);

    MCreateThis *createThis = MCreateThis::New(callee, getProto, NULL);
    current->add(createThis);

    return createThis;
}

JSObject *
IonBuilder::getSingletonPrototype(JSFunction *target)
{
    if (!target->hasSingletonType())
        return NULL;
    if (target->getType(cx)->unknownProperties())
        return NULL;

    jsid protoid = ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom);
    types::TypeSet *protoTypes = target->getType(cx)->getProperty(cx, protoid, false);

    return protoTypes->getSingleton(cx, true); // freeze the singleton if existent.
}

MDefinition *
IonBuilder::createThisScriptedSingleton(JSFunction *target, JSObject *proto, MDefinition *callee)
{
    // Generate an inline path to create a new |this| object with
    // the given singleton prototype.
    types::TypeObject *type = proto->getNewType(cx, target);
    if (!type)
        return NULL;
    if (!types::TypeScript::ThisTypes(target->script())->hasType(types::Type::ObjectType(type)))
        return NULL;

    JSObject *templateObject = js_CreateThisForFunctionWithProto(cx, target, proto);
    if (!templateObject)
        return NULL;

    // Trigger recompilation if the templateObject changes.
    if (templateObject->type()->newScript)
        types::TypeSet::WatchObjectStateChange(cx, templateObject->type());

    MConstant *protoDef = MConstant::New(ObjectValue(*proto));
    current->add(protoDef);

    MCreateThis *createThis = MCreateThis::New(callee, protoDef, templateObject);
    current->add(createThis);

    return createThis;
}

MDefinition *
IonBuilder::createThis(JSFunction *target, MDefinition *callee)
{
    if (target->isNative()) {
        if (!target->isNativeConstructor())
            return NULL;
        return createThisNative();
    }

    MDefinition *createThis = NULL;
    JSObject *proto = getSingletonPrototype(target);

    // Try baking in the prototype.
    if (proto)
        createThis = createThisScriptedSingleton(target, proto, callee);

    // If the prototype could not be hardcoded, emit a GETPROP.
    if (!createThis)
        createThis = createThisScripted(callee);

    return createThis;
}

bool
IonBuilder::jsop_funcall(uint32 argc)
{
    // Stack for JSOP_FUNCALL:
    // 1:      MPassArg(arg0)
    // ...
    // argc:   MPassArg(argN)
    // argc+1: MPassArg(JSFunction *), the 'f' in |f.call()|, in |this| position.
    // argc+2: The native 'call' function.

    // If |Function.prototype.call| may be overridden, don't optimize callsite.
    JSFunction *native = getSingleCallTarget(argc, pc);
    if (!native || !native->isNative() || native->native() != &js_fun_call)
        return makeCall(native, argc, false);

    // Extract call target.
    types::TypeSet *funTypes = oracle->getCallArg(script, argc, 0, pc);
    JSObject *funobj   = (funTypes) ? funTypes->getSingleton(cx, false) : NULL;
    JSFunction *target = (funobj && funobj->isFunction()) ? funobj->toFunction() : NULL;

    // Unwrap the (JSFunction *) parameter.
    int funcDepth = -((int)argc + 1);
    MPassArg *passFunc = current->peek(funcDepth)->toPassArg();
    current->rewriteAtDepth(funcDepth, passFunc->getArgument());

    // Remove the MPassArg(JSFunction *).
    passFunc->replaceAllUsesWith(passFunc->getArgument());
    passFunc->block()->discard(passFunc);

    // Shimmy the slots down to remove the native 'call' function.
    current->shimmySlots(funcDepth - 1);

    // If no |this| argument was provided, explicitly pass Undefined.
    // Pushing is safe here, since one stack slot has been removed.
    if (argc == 0) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        MPassArg *pass = MPassArg::New(undef);
        current->add(pass);
        current->push(pass);
    } else {
        // |this| becomes implicit in the call.
        argc -= 1; 
    }

    // Call without inlining.
    return makeCall(target, argc, false);
}

bool
IonBuilder::jsop_call(uint32 argc, bool constructing)
{
    // Acquire known call target if existent.
    JSFunction *target = getSingleCallTarget(argc, pc);

    // Attempt to inline native and scripted functions.
    if (inliningEnabled() && target) {
        if (target->isNative() && inlineNativeCall(target, argc, constructing))
            return true;
        if (!constructing && makeInliningDecision(target))
            return inlineScriptedCall(target, argc);
    }

    return makeCall(target, argc, constructing);
}

bool
IonBuilder::makeCall(JSFunction *target, uint32 argc, bool constructing)
{
    // This function may be called with mutated stack.
    // Querying TI for popped types is invalid.

    uint32 targetArgs = argc;

    // Collect number of missing arguments provided that the target is
    // scripted. Native functions are passed an explicit 'argc' parameter.
    if (target && !target->isNative())
        targetArgs = Max<uint32>(target->nargs, argc);

    MCall *call = MCall::New(targetArgs + 1, constructing); // +1 for implicit this.
    if (!call)
        return false;

    // Explicitly pad any missing arguments with |undefined|.
    // This permits skipping the argumentsRectifier.
    for (int i = targetArgs; i > (int)argc; i--) {
        JS_ASSERT_IF(target, !target->isNative());
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        MPassArg *pass = MPassArg::New(undef);
        current->add(pass);
        call->addArg(i, pass);
    }

    // Add explicit arguments.
    // Bytecode order: Function, This, Arg0, Arg1, ..., ArgN, Call.
    for (int32 i = argc; i > 0; i--)
        call->addArg(i, current->pop()->toPassArg());

    // Place an MPrepareCall before the first passed argument, before we
    // potentially perform rearrangement.
    MPrepareCall *start = new MPrepareCall;
    MPassArg *firstArg = current->peek(-1)->toPassArg();
    firstArg->block()->insertBefore(firstArg, start);
    call->initPrepareCall(start);

    MPassArg *thisArg = current->pop()->toPassArg();

    // If the target is known, inline the constructor on the caller-side.
    if (constructing && target) {
        MDefinition *callee = current->peek(-1);
        MDefinition *create = createThis(target, callee);
        if (!create)
            return abort("Failure inlining constructor for call.");

        MPassArg *newThis = MPassArg::New(create);

        thisArg->block()->discard(thisArg);
        current->add(newThis);
        thisArg = newThis;
    }

    // Pass |this| and function.
    call->addArg(0, thisArg);
    call->initFunction(current->pop());

    if (target)
        call->setSingleTarget(target);

    current->add(call);
    current->push(call);
    if (!resumeAfter(call))
        return false;

    types::TypeSet *barrier;
    types::TypeSet *types = oracle->returnTypeSet(script, pc, &barrier);
    return pushTypeBarrier(call, types, barrier);
}

bool
IonBuilder::jsop_incslot(JSOp op, uint32 slot)
{
    int32 amt = (js_CodeSpec[op].format & JOF_INC) ? 1 : -1;
    bool post = !!(js_CodeSpec[op].format & JOF_POST);
    TypeOracle::Binary b = oracle->binaryOp(script, pc);

    // Grab the value at the local slot, and convert it to a number. Currently,
    // we use ToInt32 or ToNumber which are fallible but idempotent. This whole
    // operation must be idempotent because we cannot resume in the middle of
    // an INC op.
    current->pushSlot(slot);
    MDefinition *value = current->pop();
    MInstruction *lhs;
    if (b.lhs == MIRType_Int32) {
        lhs = MToInt32::New(value);
    } else if (b.lhs == MIRType_Double) {
        lhs = MToDouble::New(value);
    } else {
        // Don't compile effectful incslot ops.
        return abort("INCSLOT non-int/double lhs");
    }
    current->add(lhs);

    // If this is a post operation, save the original value.
    if (post)
        current->push(lhs);

    MConstant *rhs = MConstant::New(Int32Value(amt));
    current->add(rhs);

    TypeOracle::BinaryTypes types = oracle->binaryTypes(script, pc);
    MAdd *result = MAdd::New(lhs, rhs);
    current->add(result);
    result->infer(cx, types);
    current->push(result);
    current->setSlot(slot);

    if (post)
        current->pop();
    return true;
}

bool
IonBuilder::jsop_localinc(JSOp op)
{
    return jsop_incslot(op, info().localSlot(GET_SLOTNO(pc)));
}

bool
IonBuilder::jsop_arginc(JSOp op)
{
    return jsop_incslot(op, info().argSlot(GET_SLOTNO(pc)));
}

bool
IonBuilder::jsop_compare(JSOp op)
{
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    MCompare *ins = MCompare::New(left, right, op);
    current->add(ins);
    current->push(ins);

    ins->infer(cx, oracle->binaryTypes(script, pc));

    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}

bool
IonBuilder::jsop_newarray(uint32 count)
{
    JS_ASSERT(script->hasGlobal());

    types::TypeObject *type = NULL;
    if (!types::UseNewTypeForInitializer(cx, script, pc)) {
        type = types::TypeScript::InitObject(cx, script, pc, JSProto_Array);
        if (!type)
            return false;
    }

    MNewArray *ins = new MNewArray(count, type, MNewArray::NewArray_Allocating);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_newobject(JSObject *baseObj)
{
    // Don't bake in the TypeObject for non-CNG scripts.
    JS_ASSERT(script->hasGlobal());

    types::TypeObject *type = NULL;
    if (!types::UseNewTypeForInitializer(cx, script, pc)) {
        type = types::TypeScript::InitObject(cx, script, pc, JSProto_Object);
        if (!type)
            return false;
    }

    MNewObject *ins = MNewObject::New(baseObj, type);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_initelem()
{
    if (oracle->propertyWriteCanSpecialize(script, pc)) {
        if (oracle->elementWriteIsDenseArray(script, pc))
            return jsop_initelem_dense();
    }

    return abort("NYI: JSOP_INITELEM supports for non dense objects/arrays.");
}

bool
IonBuilder::jsop_initelem_dense()
{
    MDefinition *value = current->pop();
    MDefinition *id = current->pop();
    MDefinition *obj = current->peek(-1);

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    // Store the value.
    MStoreElement *store = MStoreElement::New(elements, id, value);
    current->add(store);

    // Update the length.
    MSetInitializedLength *initLength = MSetInitializedLength::New(elements, id);
    current->add(initLength);

    if (!resumeAfter(initLength))
        return false;

   return true;
}

bool
IonBuilder::jsop_initprop(JSAtom *atom)
{
    MDefinition *value = current->pop();
    MDefinition *obj = current->peek(-1);

    JSObject *baseObj = obj->toNewObject()->baseObj();

    if (!oracle->propertyWriteCanSpecialize(script, pc)) {
        // This should only happen for a few names like __proto__.
        return abort("INITPROP Monitored initprop");
    }

    // JSOP_NEWINIT becomes an MNewObject without a known base.
    if (!baseObj) {
        MInitProp *init = MInitProp::New(obj, (PropertyName *)atom, value);
        current->add(init);
        return resumeAfter(init);
    }

    JSObject *holder;
    JSProperty *prop = NULL;
    DebugOnly<bool> res = LookupPropertyWithFlags(cx, baseObj, ATOM_TO_JSID(atom),
                                                  JSRESOLVE_QUALIFIED, &holder, &prop);
    JS_ASSERT(res && prop && holder == baseObj);

    Shape *shape = (Shape *)prop;

    if (baseObj->isFixedSlot(shape->slot())) {
        MStoreFixedSlot *store = MStoreFixedSlot::New(obj, value, shape->slot());
        current->add(store);
        return resumeAfter(store);
    }

    MSlots *slots = MSlots::New(obj);
    current->add(slots);

    MStoreSlot *store = MStoreSlot::New(slots, baseObj->dynamicSlotIndex(shape->slot()), value);
    current->add(store);
    return resumeAfter(store);
}

MBasicBlock *
IonBuilder::addBlock(MBasicBlock *block, uint32 loopDepth)
{
    if (!block)
        return NULL;
    graph().addBlock(block);
    block->setLoopDepth(loopDepth);
    return block;
}

MBasicBlock *
IonBuilder::newBlock(MBasicBlock *predecessor, jsbytecode *pc)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    return addBlock(block, loopDepth_);
}

MBasicBlock *
IonBuilder::newBlockAfter(MBasicBlock *at, MBasicBlock *predecessor, jsbytecode *pc)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    graph_.insertBlockAfter(at, block);
    return block;
}

MBasicBlock *
IonBuilder::newBlock(MBasicBlock *predecessor, jsbytecode *pc, uint32 loopDepth)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    return addBlock(block, loopDepth);
}

MBasicBlock *
IonBuilder::newOsrPreheader(MBasicBlock *predecessor, jsbytecode *loopHead, jsbytecode *loopEntry)
{
    JS_ASSERT((JSOp)*loopHead == JSOP_LOOPHEAD);
    JS_ASSERT((JSOp)*loopEntry == JSOP_LOOPENTRY);
    JS_ASSERT(loopEntry == info().osrPc());

    // Create two blocks: one for the OSR entry with no predecessors, one for
    // the preheader, which has the OSR entry block as a predecessor. The
    // OSR block is always the second block (with id 1).
    MBasicBlock *osrBlock  = newBlockAfter(*graph_.begin(), loopEntry);
    MBasicBlock *preheader = newBlock(predecessor, loopEntry);

    MOsrEntry *entry = MOsrEntry::New();
    osrBlock->add(entry);

    // Initialize |scopeChain|.
    {
        uint32 slot = info().scopeChainSlot();

        MOsrScopeChain *scopev = MOsrScopeChain::New(entry);
        osrBlock->add(scopev);
        osrBlock->initSlot(slot, scopev);
    }

    if (info().fun()) {
        // Initialize |this| parameter.
        uint32 slot = info().thisSlot();
        ptrdiff_t offset = StackFrame::offsetOfThis(info().fun());

        MOsrValue *thisv = MOsrValue::New(entry, offset);
        osrBlock->add(thisv);
        osrBlock->initSlot(slot, thisv);

        // Initialize arguments.
        for (uint32 i = 0; i < info().nargs(); i++) {
            uint32 slot = info().argSlot(i);
            ptrdiff_t offset = StackFrame::offsetOfFormalArg(info().fun(), i);

            MOsrValue *osrv = MOsrValue::New(entry, offset);
            osrBlock->add(osrv);
            osrBlock->initSlot(slot, osrv);
        }
    }

    // Initialize locals.
    for (uint32 i = 0; i < info().nlocals(); i++) {
        uint32 slot = info().localSlot(i);
        ptrdiff_t offset = StackFrame::offsetOfFixed(i);

        MOsrValue *osrv = MOsrValue::New(entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Initialize stack.
    uint32 numSlots = preheader->stackDepth() - CountArgSlots(info().fun()) - info().nlocals();
    for (uint32 i = 0; i < numSlots; i++) {
        uint32 slot = info().stackSlot(i);
        ptrdiff_t offset = StackFrame::offsetOfFixed(info().nlocals() + i);

        MOsrValue *osrv = MOsrValue::New(entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Create an MStart to hold the first valid MResumePoint.
    MStart *start = MStart::New(MStart::StartType_Osr);
    osrBlock->add(start);
    graph().setOsrStart(start);

    // MOsrValue instructions are infallible, so the first MResumePoint must
    // occur after they execute, at the point of the MStart.
    if (!resumeAt(start, loopEntry))
        return NULL;

    // Link the same MResumePoint from the MStart to each MOsrValue.
    // This causes logic in ShouldSpecializeInput() to not replace Uses with
    // Unboxes in the MResumePiont, so that the MStart always sees Values.
    osrBlock->linkOsrValues(start);

    // Clone types of the other predecessor of the pre-header to the osr block,
    // such as pre-header phi's won't discard specialized type of the
    // predecessor.
    JS_ASSERT(predecessor->stackDepth() == osrBlock->stackDepth());
    JS_ASSERT(info().scopeChainSlot() == 0);
    JS_ASSERT(osrBlock->getSlot(info().scopeChainSlot())->type() == MIRType_Object);

    Vector<MIRType> slotTypes(cx);
    if (!slotTypes.growByUninitialized(osrBlock->stackDepth()))
        return NULL;

    // Fill slotTypes with the types of the predecessor block.
    for (uint32 i = 0; i < osrBlock->stackDepth(); i++)
        slotTypes[i] = predecessor->getSlot(i)->type();

    // Update slotTypes for slots that may have a different type at this join point.
    oracle->getNewTypesAtJoinPoint(script, loopHead, slotTypes);

    for (uint32 i = 1; i < osrBlock->stackDepth(); i++) {
        MIRType type = slotTypes[i];
        // Unbox the MOsrValue if it is known to be unboxable.
        if (type != MIRType_Value && type != MIRType_Undefined && type != MIRType_Null) {
            MDefinition *def = osrBlock->getSlot(i);
            JS_ASSERT(def->type() == MIRType_Value);
            MInstruction *actual = MUnbox::New(def, slotTypes[i], MUnbox::Fallible);
            osrBlock->add(actual);
            osrBlock->rewriteSlot(i, actual);
        }
    }

    // Finish the osrBlock.
    osrBlock->end(MGoto::New(preheader));
    preheader->addPredecessor(osrBlock);
    graph().setOsrBlock(osrBlock);

    // Wrap |this| with a guaranteed use, to prevent instruction elimination.
    // Prevent |this| from being DCE'd: necessary for constructors.
    if (info().fun())
        preheader->getSlot(info().thisSlot())->setGuard();

    return preheader;
}

MBasicBlock *
IonBuilder::newPendingLoopHeader(MBasicBlock *predecessor, jsbytecode *pc)
{
    loopDepth_++;
    MBasicBlock *block = MBasicBlock::NewPendingLoopHeader(graph(), info(), predecessor, pc);
    return addBlock(block, loopDepth_);
}

// A resume point is a mapping of stack slots to MDefinitions. It is used to
// capture the environment such that if a guard fails, and IonMonkey needs
// to exit back to the interpreter, the interpreter state can be
// reconstructed.
//
// We capture stack state at critical points:
//   * (1) At the beginning of every basic block.
//   * (2) After every effectful operation.
//
// As long as these two properties are maintained, instructions can
// be moved, hoisted, or, eliminated without problems, and ops without side
// effects do not need to worry about capturing state at precisely the
// right point in time.
//
// Effectful instructions, of course, need to capture state after completion,
// where the interpreter will not attempt to repeat the operation. For this,
// ResumeAfter must be used. The state is attached directly to the effectful
// instruction to ensure that no intermediate instructions could be injected
// in between by a future analysis pass.
//
// During LIR construction, if an instruction can bail back to the interpreter,
// we create an LSnapshot, which uses the last known resume point to request
// register/stack assignments for every live value.
bool
IonBuilder::resume(MInstruction *ins, jsbytecode *pc, MResumePoint::Mode mode)
{
    JS_ASSERT(ins->isEffectful());

    MResumePoint *resumePoint = MResumePoint::New(ins->block(), pc, callerResumePoint_, mode);
    if (!resumePoint)
        return false;
    ins->setResumePoint(resumePoint);
    return true;
}

bool
IonBuilder::resumeAt(MInstruction *ins, jsbytecode *pc)
{
    return resume(ins, pc, MResumePoint::ResumeAt);
}

bool
IonBuilder::resumeAfter(MInstruction *ins)
{
    return resume(ins, pc, MResumePoint::ResumeAfter);
}

void
IonBuilder::insertRecompileCheck()
{
    if (!inliningEnabled())
        return;

    // Don't recompile if we are already inlining.
    if (script->getUseCount() >= js_IonOptions.usesBeforeInlining)
        return;

    // Don't recompile if the oracle cannot provide inlining information
    // or if the script has no calls.
    if (!oracle->canInlineCalls())
        return;

    MRecompileCheck *check = MRecompileCheck::New();
    current->add(check);
}

static inline bool
TestSingletonProperty(JSContext *cx, JSObject *obj, jsid id, bool *isKnownConstant)
{
    // We would like to completely no-op property/global accesses which can
    // produce only a particular JSObject. When indicating the access result is
    // definitely an object, type inference does not account for the
    // possibility that the property is entirely missing from the input object
    // and its prototypes (if this happens, a semantic trigger would be hit and
    // the pushed types updated, even if there is no type barrier).
    //
    // If the access definitely goes through obj, either directly or on the
    // prototype chain, then if obj has a defined property now, and the
    // property has a default or method shape, then the property is not missing
    // and the only way it can become missing in the future is if it is deleted.
    // Deletion causes type properties to be explicitly marked with undefined.

    *isKnownConstant = false;

    JSObject *pobj = obj;
    while (pobj) {
        if (!pobj->isNative())
            return true;
        if (pobj->getClass()->ops.lookupProperty)
            return true;
        pobj = pobj->getProto();
    }

    JSObject *holder;
    JSProperty *prop = NULL;
    if (!obj->lookupGeneric(cx, id, &holder, &prop))
        return false;
    if (!prop)
        return true;

    Shape *shape = (Shape *)prop;
    if (shape->hasDefaultGetter()) {
        if (!shape->hasSlot())
            return true;
        if (holder->getSlot(shape->slot()).isUndefined())
            return true;
    }

    *isKnownConstant = true;
    return true;
}

static inline bool
TestSingletonPropertyTypes(JSContext *cx, types::TypeSet *types,
                           GlobalObject *globalObj, jsid id,
                           bool *isKnownConstant, bool *testObject)
{
    // As for TestSingletonProperty, but the input is any value in a type set
    // rather than a specific object. If testObject is set then the constant
    // result can only be used after ensuring the input is an object.

    *isKnownConstant = false;
    *testObject = false;

    if (!types || types->unknownObject())
        return true;

    JSObject *singleton = types->getSingleton(cx);
    if (singleton)
        return TestSingletonProperty(cx, singleton, id, isKnownConstant);

    if (!globalObj)
        return true;

    JSProtoKey key;
    JSValueType type = types->getKnownTypeTag(cx);
    switch (type) {
      case JSVAL_TYPE_STRING:
        key = JSProto_String;
        break;

      case JSVAL_TYPE_INT32:
      case JSVAL_TYPE_DOUBLE:
        key = JSProto_Number;
        break;

      case JSVAL_TYPE_BOOLEAN:
        key = JSProto_Boolean;
        break;

      case JSVAL_TYPE_OBJECT:
      case JSVAL_TYPE_UNKNOWN:
        // For property accesses which may be on many objects, we just need to
        // find a prototype common to all the objects; if that prototype
        // has the property, the access will not be on a missing property.
        if (types->getObjectCount() == 1) {
            types::TypeObject *object = types->getTypeObject(0);
            if (!object)
                return true;
            if (object && object->proto) {
                if (!TestSingletonProperty(cx, object->proto, id, isKnownConstant))
                    return false;
                if (*isKnownConstant) {
                    types->addFreeze(cx);

                    // If this is not a known object, a test will be needed.
                    *testObject = (type != JSVAL_TYPE_OBJECT);
                }
                return true;
            }
        }
        return true;

      default:
        return true;
    }

    JSObject *proto;
    if (!js_GetClassPrototype(cx, globalObj, key, &proto, NULL))
        return false;

    return TestSingletonProperty(cx, proto, id, isKnownConstant);
}

// Given an actual and observed type set, annotates the IR as much as possible:
// (1) If no type information is provided, the value on the top of the stack is
//     left in place.
// (2) If a single type definitely exists, and no type barrier is in place,
//     then an infallible unbox instruction replaces the value on the top of
//     the stack.
// (3) If a type barrier is in place, but has an unknown type set, leave the
//     value at the top of the stack.
// (4) If a type barrier is in place, and has a single type, an unbox
//     instruction replaces the top of the stack.
// (5) Lastly, a type barrier instruction replaces the top of the stack.
bool
IonBuilder::pushTypeBarrier(MInstruction *ins, types::TypeSet *actual, types::TypeSet *observed)
{
    // If the instruction has no side effects, we'll resume the entire operation.
    // The actual type barrier will occur in the interpreter. If the
    // instruction is effectful, even if it has a singleton type, there
    // must be a resume point capturing the original def, and resuming
    // to that point will explicitly monitor the new type.

    if (!actual) {
        JS_ASSERT(!observed);
        return true;
    }

    if (!observed) {
        JSValueType type = actual->getKnownTypeTag(cx);
        MInstruction *replace = NULL;
        switch (type) {
          case JSVAL_TYPE_UNDEFINED:
            replace = MConstant::New(UndefinedValue());
            break;
          case JSVAL_TYPE_NULL:
            replace = MConstant::New(NullValue());
            break;
          case JSVAL_TYPE_UNKNOWN:
            break;
          default: {
            MIRType replaceType = MIRTypeFromValueType(type);
            if (ins->type() == MIRType_Value)
                replace = MUnbox::New(ins, replaceType, MUnbox::Infallible);
            else
                JS_ASSERT(ins->type() == replaceType);
            break;
          }
        }
        if (replace) {
            current->pop();
            current->add(replace);
            current->push(replace);
        }
        return true;
    }

    if (observed->unknown())
        return true;

    current->pop();
    observed->addFreeze(cx);

    MInstruction *barrier;
    JSValueType type = observed->getKnownTypeTag(cx);

    // An unbox instruction isn't enough to capture JSVAL_TYPE_OBJECT.
    if (type == JSVAL_TYPE_OBJECT && !observed->hasType(types::Type::AnyObjectType()))
        type = JSVAL_TYPE_UNKNOWN;

    switch (type) {
      case JSVAL_TYPE_UNKNOWN:
      case JSVAL_TYPE_UNDEFINED:
      case JSVAL_TYPE_NULL:
        barrier = MTypeBarrier::New(ins, observed);
        current->add(barrier);

        if (type == JSVAL_TYPE_UNDEFINED)
            return pushConstant(UndefinedValue());
        if (type == JSVAL_TYPE_NULL)
            return pushConstant(NullValue());
        break;
      default:
        MUnbox::Mode mode = ins->isEffectful() ? MUnbox::TypeBarrier : MUnbox::TypeGuard;
        barrier = MUnbox::New(ins, MIRTypeFromValueType(type), mode);
        current->add(barrier);
    }
    current->push(barrier);
    return true;
}

// Test the type of values returned by a VM call. This is an optimized version
// of calling TypeScript::Monitor inside such stubs.
void
IonBuilder::monitorResult(MInstruction *ins, types::TypeSet *types)
{
    if (!types || types->unknown())
        return;

    MInstruction *monitor = MMonitorTypes::New(ins, types);
    current->add(monitor);
}

bool
IonBuilder::jsop_getgname(JSAtom *atom)
{
    // Optimize undefined, NaN, and Infinity.
    if (atom == cx->runtime->atomState.typeAtoms[JSTYPE_VOID])
        return pushConstant(UndefinedValue());
    if (atom == cx->runtime->atomState.NaNAtom)
        return pushConstant(cx->runtime->NaNValue);
    if (atom == cx->runtime->atomState.InfinityAtom)
        return pushConstant(cx->runtime->positiveInfinityValue);

    jsid id = ATOM_TO_JSID(atom);
    JSObject *globalObj = script->global();
    JS_ASSERT(globalObj->isNative());

    // For the fastest path, the property must be found, and it must be found
    // as a normal data property on exactly the global object.
    const js::Shape *shape = globalObj->nativeLookup(cx, id);
    if (!shape || !shape->hasDefaultGetter() || !shape->hasSlot())
        return jsop_getname(atom);

    types::TypeSet *propertyTypes = oracle->globalPropertyTypeSet(script, pc, id);
    if (propertyTypes && propertyTypes->isOwnProperty(cx, globalObj->getType(cx), true)) {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        return jsop_getname(atom);
    }

    // If the property is permanent, a shape guard isn't necessary.
    JSValueType knownType = JSVAL_TYPE_UNKNOWN;

    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);
    if (types) {
        JSObject *singleton = types->getSingleton(cx);

        knownType = types->getKnownTypeTag(cx);
        if (!barrier) {
            if (singleton) {
                // Try to inline a known constant value.
                bool isKnownConstant;
                if (!TestSingletonProperty(cx, globalObj, id, &isKnownConstant))
                    return false;
                if (isKnownConstant)
                    return pushConstant(ObjectValue(*singleton));
            }
            if (knownType == JSVAL_TYPE_UNDEFINED)
                return pushConstant(UndefinedValue());
            if (knownType == JSVAL_TYPE_NULL)
                return pushConstant(NullValue());
        }
    }

    MInstruction *global = MConstant::New(ObjectValue(*globalObj));
    current->add(global);

    // If we have a property typeset, the isOwnProperty call will trigger recompilation if
    // the property is deleted or reconfigured.
    if (!propertyTypes && shape->configurable()) {
        MGuardShape *guard = MGuardShape::New(global, globalObj->lastProperty());
        current->add(guard);
    }

    JS_ASSERT(shape->slot() >= globalObj->numFixedSlots());

    MSlots *slots = MSlots::New(global);
    current->add(slots);
    MLoadSlot *load = MLoadSlot::New(slots, shape->slot() - globalObj->numFixedSlots());
    current->add(load);

    // Slot loads can be typed, if they have a single, known, definitive type.
    if (knownType != JSVAL_TYPE_UNKNOWN && !barrier)
        load->setResultType(MIRTypeFromValueType(knownType));

    current->push(load);
    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::jsop_setgname(JSAtom *atom)
{
    jsid id = ATOM_TO_JSID(atom);

    bool canSpecialize;
    types::TypeSet *propertyTypes = oracle->globalPropertyWrite(script, pc, id, &canSpecialize);

    // This should only happen for a few names like __proto__.
    if (!canSpecialize)
        return jsop_setprop(atom);

    JSObject *globalObj = script->global();
    JS_ASSERT(globalObj->isNative());

    // For the fastest path, the property must be found, and it must be found
    // as a normal data property on exactly the global object.
    const js::Shape *shape = globalObj->nativeLookup(cx, id);
    if (!shape || !shape->hasDefaultSetter() || !shape->writable() || !shape->hasSlot())
        return jsop_setprop(atom);

    if (propertyTypes && propertyTypes->isOwnProperty(cx, globalObj->getType(cx), true)) {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        return jsop_setprop(atom);
    }

    MInstruction *global = MConstant::New(ObjectValue(*globalObj));
    current->add(global);

    // If we have a property type set, the isOwnProperty call will trigger recompilation
    // if the property is deleted or reconfigured. Without TI, we always need a shape guard
    // to guard against the property being reconfigured as non-writable.
    if (!propertyTypes) {
        MGuardShape *guard = MGuardShape::New(global, globalObj->lastProperty());
        current->add(guard);
    }

    JS_ASSERT(shape->slot() >= globalObj->numFixedSlots());

    MSlots *slots = MSlots::New(global);
    current->add(slots);

    MDefinition *value = current->pop();
    MStoreSlot *store = MStoreSlot::New(slots, shape->slot() - globalObj->numFixedSlots(), value);
    current->add(store);

    // Determine whether write barrier is required.
    if (cx->compartment->needsBarrier() && (!propertyTypes || propertyTypes->needsBarrier(cx)))
        store->setNeedsBarrier(true);

    if (!resumeAfter(store))
        return false;

    // Pop the global object pushed by bindgname.
    DebugOnly<MDefinition *> pushedGlobal = current->pop();
    JS_ASSERT(&pushedGlobal->toConstant()->value().toObject() == globalObj);

    // If the property has a known type, we may be able to optimize typed stores by not
    // storing the type tag. This only works if the property does not have its initial
    // |undefined| value; if |undefined| is assigned at a later point, it will be added
    // to the type set.
    if (propertyTypes && !globalObj->getSlot(shape->slot()).isUndefined()) {
        JSValueType knownType = propertyTypes->getKnownTypeTag(cx);
        if (knownType != JSVAL_TYPE_UNKNOWN)
            store->setSlotType(MIRTypeFromValueType(knownType));
    }

    current->push(value);
    return true;
}

bool
IonBuilder::jsop_getname(JSAtom *atom)
{
    MDefinition *object;
    if (js_CodeSpec[*pc].format & JOF_GNAME) {
        MInstruction *global = MConstant::New(ObjectValue(*script->global()));
        current->add(global);
        object = global;
    } else {
        current->pushSlot(info().scopeChainSlot());
        object = current->pop();
    }

    MCallGetNameInstruction *ins;

    JSOp op2 = JSOp(*GetNextPc(pc));
    if (op2 == JSOP_TYPEOF)
        ins = MCallGetNameTypeOf::New(object, atom);
    else
        ins = MCallGetName::New(object, atom);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);

    monitorResult(ins, types);
    return pushTypeBarrier(ins, types, barrier);
}

bool
IonBuilder::jsop_bindname(PropertyName *name)
{
    JS_ASSERT(script->analysis()->usesScopeChain());

    MDefinition *scopeChain = current->getSlot(info().scopeChainSlot());
    MBindNameCache *ins = MBindNameCache::New(scopeChain, name, script, pc);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_getelem()
{
    if (oracle->elementReadIsDenseArray(script, pc))
        return jsop_getelem_dense();

    int arrayType = TypedArray::TYPE_MAX;
    if (oracle->elementReadIsTypedArray(script, pc, &arrayType))
        return jsop_getelem_typed(arrayType);

    MDefinition *rhs = current->pop();
    MDefinition *lhs = current->pop();

    MInstruction *ins;

    // TI does not account for GETELEM with string indexes, so we have to monitor
    // the result of MGetElementCache if it's expected to access string properties.
    // If the result of MGetElementCache is not monitored, we won't generate any
    // getprop stubs.
    bool mustMonitorResult = false;
    bool cacheable = false;

    oracle->elementReadGeneric(script, pc, &cacheable, &mustMonitorResult);

    if (cacheable)
        ins = MGetElementCache::New(lhs, rhs, mustMonitorResult);
    else
        ins = MCallGetElement::New(lhs, rhs);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);

    if (mustMonitorResult)
        monitorResult(ins, types);
    return pushTypeBarrier(ins, types, barrier);
}

bool
IonBuilder::jsop_getelem_dense()
{
    if (oracle->arrayPrototypeHasIndexedProperty())
        return abort("GETELEM Array proto has indexed properties");

    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);
    bool needsHoleCheck = !oracle->elementReadIsPacked(script, pc);
    bool maybeUndefined = types->hasType(types::Type::UndefinedType());

    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    JSValueType knownType = JSVAL_TYPE_UNKNOWN;
    if (!needsHoleCheck && !barrier) {
        knownType = types->getKnownTypeTag(cx);

        // Null and undefined have no payload so they can't be specialized.
        // Since folding null/undefined while building SSA is not safe (see the
        // comment in IsPhiObservable), we just add an untyped load instruction
        // and rely on pushTypeBarrier and DCE to replace it with a null/undefined
        // constant.
        if (knownType == JSVAL_TYPE_UNDEFINED || knownType == JSVAL_TYPE_NULL)
            knownType = JSVAL_TYPE_UNKNOWN;
    }

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    MInitializedLength *initLength = MInitializedLength::New(elements);
    current->add(initLength);

    MInstruction *load;

    if (!maybeUndefined) {
        // This load should not return undefined, so likely we're reading
        // in-bounds elements, and the array is packed or its holes are not
        // read. This is the best case: we can separate the bounds check for
        // hoisting.
        MBoundsCheck *check = MBoundsCheck::New(id, initLength);
        current->add(check);

        load = MLoadElement::New(elements, id, needsHoleCheck);
        current->add(load);
    } else {
        // This load may return undefined, so assume that we *can* read holes,
        // or that we can read out-of-bounds accesses. In this case, the bounds
        // check is part of the opcode.
        load = MLoadElementHole::New(elements, id, initLength, needsHoleCheck);
        current->add(load);

        // If maybeUndefined was true, the typeset must have undefined, and
        // then either additional types or a barrier. This means we should
        // never have a typed version of LoadElementHole.
        JS_ASSERT(knownType == JSVAL_TYPE_UNKNOWN);
    }

    if (knownType != JSVAL_TYPE_UNKNOWN)
        load->setResultType(MIRTypeFromValueType(knownType));

    current->push(load);
    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::jsop_getelem_typed(int arrayType)
{
    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);

    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    bool maybeUndefined = types->hasType(types::Type::UndefinedType());

    // Reading from an Uint32Array will result in a double for values
    // that don't fit in an int32. We have to bailout if this happens
    // and the instruction is not known to return a double.
    bool allowDouble = types->hasType(types::Type::DoubleType());

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    if (!maybeUndefined) {
        // Assume the index is in range, so that we can hoist the length,
        // elements vector and bounds check.

        // If we are reading in-bounds elements, we can use knowledge about
        // the array type to determine the result type. This may be more
        // precise than the known pushed type.
        MIRType knownType;
        switch (arrayType) {
          case TypedArray::TYPE_INT8:
          case TypedArray::TYPE_UINT8:
          case TypedArray::TYPE_UINT8_CLAMPED:
          case TypedArray::TYPE_INT16:
          case TypedArray::TYPE_UINT16:
          case TypedArray::TYPE_INT32:
            knownType = MIRType_Int32;
            break;
          case TypedArray::TYPE_UINT32:
            knownType = allowDouble ? MIRType_Double : MIRType_Int32;
            break;
          case TypedArray::TYPE_FLOAT32:
          case TypedArray::TYPE_FLOAT64:
            knownType = MIRType_Double;
            break;
          default:
            JS_NOT_REACHED("Unknown typed array type");
            return false;
        }

        // Get the length.
        MTypedArrayLength *length = MTypedArrayLength::New(obj);
        current->add(length);

        // Bounds check.
        MBoundsCheck *check = MBoundsCheck::New(id, length);
        current->add(check);

        // Get the elements vector.
        MTypedArrayElements *elements = MTypedArrayElements::New(obj);
        current->add(elements);

        // Load the element.
        MLoadTypedArrayElement *load = MLoadTypedArrayElement::New(elements, id, arrayType);
        current->add(load);
        current->push(load);

        load->setResultType(knownType);

        // Note: we can ignore the type barrier here, we know the type must
        // be valid and unbarriered.
        JS_ASSERT_IF(knownType == MIRType_Int32, types->hasType(types::Type::Int32Type()));
        JS_ASSERT_IF(knownType == MIRType_Double, types->hasType(types::Type::DoubleType()));
        return true;
    } else {
        // Assume we will read out-of-bound values. In this case the
        // bounds check will be part of the instruction, and the instruction
        // will always return a Value.
        MLoadTypedArrayElementHole *load = MLoadTypedArrayElementHole::New(obj, id, arrayType, allowDouble);
        current->add(load);
        current->push(load);

        return resumeAfter(load) && pushTypeBarrier(load, types, barrier);
    }
}

bool
IonBuilder::jsop_setelem()
{
    if (oracle->propertyWriteCanSpecialize(script, pc)) {
        if (oracle->elementWriteIsDenseArray(script, pc))
            return jsop_setelem_dense();

        int arrayType = TypedArray::TYPE_MAX;
        if (oracle->elementWriteIsTypedArray(script, pc, &arrayType))
            return jsop_setelem_typed(arrayType);
    }

    MDefinition *value = current->pop();
    MDefinition *index = current->pop();
    MDefinition *object = current->pop();

    MInstruction *ins = MCallSetElement::New(object, index, value);
    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_setelem_dense()
{
    if (oracle->arrayPrototypeHasIndexedProperty())
        return abort("SETELEM Array proto has indexed properties");

    MIRType elementType = oracle->elementWrite(script, pc);
    bool packed = oracle->elementWriteIsPacked(script, pc);

    MDefinition *value = current->pop();
    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    // Use MStoreElementHole if this SETELEM has written to out-of-bounds
    // indexes in the past. Otherwise, use MStoreElement so that we can hoist
    // the initialized length and bounds check.
    MStoreElementCommon *store;
    if (oracle->setElementHasWrittenHoles(script, pc)) {
        MStoreElementHole *ins = MStoreElementHole::New(obj, elements, id, value);
        store = ins;

        current->add(ins);
        current->push(value);

        if (!resumeAfter(ins))
            return false;
    } else {
        MInitializedLength *initLength = MInitializedLength::New(elements);
        current->add(initLength);

        MBoundsCheck *check = MBoundsCheck::New(id, initLength);
        current->add(check);

        MStoreElement *ins = MStoreElement::New(elements, id, value);
        store = ins;

        current->add(ins);
        current->push(value);

        if (!resumeAfter(ins))
            return false;
    }

    // Determine whether a write barrier is required.
    if (cx->compartment->needsBarrier() && oracle->elementWriteNeedsBarrier(script, pc))
        store->setNeedsBarrier(true);

    if (elementType != MIRType_None && packed)
        store->setElementType(elementType);

    return true;
}

bool
IonBuilder::jsop_setelem_typed(int arrayType)
{
    MDefinition *value = current->pop();
    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the length.
    MTypedArrayLength *length = MTypedArrayLength::New(obj);
    current->add(length);

    // Bounds check.
    MBoundsCheck *check = MBoundsCheck::New(id, length);
    current->add(check);

    // Get the elements vector.
    MTypedArrayElements *elements = MTypedArrayElements::New(obj);
    current->add(elements);

    // Clamp value to [0, 255] for Uint8ClampedArray.
    MDefinition *unclampedValue = value;
    if (arrayType == TypedArray::TYPE_UINT8_CLAMPED) {
        value = MClampToUint8::New(value);
        current->add(value->toInstruction());
    }

    // Store the value.
    MStoreTypedArrayElement *store = MStoreTypedArrayElement::New(elements, id, value, arrayType);
    current->add(store);

    current->push(unclampedValue);
    return resumeAfter(store);
}

bool
IonBuilder::jsop_length()
{
    if (jsop_length_fastPath())
        return true;
    return jsop_getprop(info().getAtom(pc));
}

bool
IonBuilder::jsop_length_fastPath()
{
    TypeOracle::UnaryTypes sig = oracle->unaryTypes(script, pc);
    if (!sig.inTypes || !sig.outTypes)
        return false;

    if (sig.outTypes->getKnownTypeTag(cx) != JSVAL_TYPE_INT32)
        return false;

    switch (sig.inTypes->getKnownTypeTag(cx)) {
      case JSVAL_TYPE_STRING: {
        MDefinition *obj = current->pop();
        MStringLength *ins = MStringLength::New(obj);
        current->add(ins);
        current->push(ins);
        return true;
      }

      case JSVAL_TYPE_OBJECT: {
        if (!sig.inTypes->hasObjectFlags(cx, types::OBJECT_FLAG_NON_DENSE_ARRAY)) {
            MDefinition *obj = current->pop();
            MElements *elements = MElements::New(obj);
            current->add(elements);

            // Read length.
            MArrayLength *length = new MArrayLength(elements);
            current->add(length);
            current->push(length);
            return true;
        }

        if (!sig.inTypes->hasObjectFlags(cx, types::OBJECT_FLAG_NON_TYPED_ARRAY)) {
            MDefinition *obj = current->pop();
            MTypedArrayLength *length = MTypedArrayLength::New(obj);
            current->add(length);
            current->push(length);
            return true;
        }

        return false;
      }

      default:
        break;
    }

    return false;
}

inline types::TypeSet *
GetDefiniteSlot(JSContext *cx, types::TypeSet *types, JSAtom *atom)
{
    if (!types || types->unknownObject() || types->getObjectCount() != 1)
        return NULL;

    types::TypeObject *type = types->getTypeObject(0);
    if (!type || type->unknownProperties())
        return NULL;

    jsid id = ATOM_TO_JSID(atom);
    if (id != types::MakeTypeId(cx, id))
        return NULL;

    types::TypeSet *propertyTypes = type->getProperty(cx, id, false);
    if (!propertyTypes ||
        !propertyTypes->isDefiniteProperty() ||
        propertyTypes->isOwnProperty(cx, type, true))
    {
        return NULL;
    }

    types->addFreeze(cx);
    return propertyTypes;
}

bool
IonBuilder::jsop_not()
{
    MDefinition *value = current->pop();

    MNot *ins = new MNot(value);
    current->add(ins);
    current->push(ins);
    return true;
}

bool
IonBuilder::jsop_getprop(JSAtom *atom)
{
    MDefinition *obj = current->pop();
    MInstruction *ins;

    types::TypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::TypeSet *types = oracle->propertyRead(script, pc);

    TypeOracle::Unary unary = oracle->unaryOp(script, pc);
    TypeOracle::UnaryTypes unaryTypes = oracle->unaryTypes(script, pc);

    JSObject *singleton = types ? types->getSingleton(cx) : NULL;
    if (singleton && !barrier) {
        bool isKnownConstant, testObject;
        if (!TestSingletonPropertyTypes(cx, unaryTypes.inTypes,
                                        script->global(), ATOM_TO_JSID(atom),
                                        &isKnownConstant, &testObject))
        {
            return false;
        }

        if (isKnownConstant) {
            if (testObject) {
                MGuardObject *guard = MGuardObject::New(obj);
                current->add(guard);
            }
            return pushConstant(ObjectValue(*singleton));
        }
    }

    if (types::TypeSet *propTypes = GetDefiniteSlot(cx, unaryTypes.inTypes, atom)) {
        MLoadFixedSlot *fixed = MLoadFixedSlot::New(obj, propTypes->definiteSlot());
        if (!barrier)
            fixed->setResultType(unary.rval);

        current->add(fixed);
        current->push(fixed);

        return pushTypeBarrier(fixed, types, barrier);
    }

    if (unary.ival == MIRType_Object) {
        MGetPropertyCache *load = MGetPropertyCache::New(obj, atom);
        if (!barrier) {
            // Use the default type (Value) when the output type is undefined or null.
            // Because specializing to those types isn't possible.
            if (unary.rval != MIRType_Undefined && unary.rval != MIRType_Null)
                load->setResultType(unary.rval);
        }
        ins = load;
    } else {
        ins = MCallGetProperty::New(obj, atom);
    }

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    if (ins->isCallGetProperty())
        monitorResult(ins, types);
    return pushTypeBarrier(ins, types, barrier);
}

bool
IonBuilder::jsop_setprop(JSAtom *atom)
{

    MDefinition *value = current->pop();
    MDefinition *obj = current->pop();

    bool monitored = !oracle->propertyWriteCanSpecialize(script, pc);

    TypeOracle::BinaryTypes binaryTypes = oracle->binaryTypes(script, pc);

    if (!monitored) {
        if (types::TypeSet *propTypes = GetDefiniteSlot(cx, binaryTypes.lhsTypes, atom)) {
            MStoreFixedSlot *fixed = MStoreFixedSlot::New(obj, value, propTypes->definiteSlot());
            current->add(fixed);
            current->push(value);
            if (cx->compartment->needsBarrier() && propTypes->needsBarrier(cx))
                fixed->setNeedsBarrier();
            return resumeAfter(fixed);
        }
    }

    oracle->binaryOp(script, pc);

    MSetPropertyInstruction *ins;
    if (monitored) {
        ins = MCallSetProperty::New(obj, value, atom, script->strictModeCode);
    } else {
        ins = MSetPropertyCache::New(obj, value, atom, script->strictModeCode);

        jsid id = ATOM_TO_JSID(atom);
        if (cx->compartment->needsBarrier() &&
            (!binaryTypes.lhsTypes || binaryTypes.lhsTypes->propertyNeedsBarrier(cx, id))) {
            ins->setNeedsBarrier();
        }
    }

    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_delprop(JSAtom *atom)
{
    MDefinition *obj = current->pop();

    MInstruction *ins = MDeleteProperty::New(obj, atom);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_regexp(RegExpObject *reobj)
{
    MRegExp *ins = MRegExp::New(reobj, MRegExp::MustClone);
    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_object(JSObject *obj)
{
    MConstant *ins = MConstant::New(ObjectValue(*obj));
    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_lambda(JSFunction *fun)
{
    JS_ASSERT(script->analysis()->usesScopeChain());
    MDefinition *scopeChain = current->getSlot(info().scopeChainSlot());
    MLambda *ins = MLambda::New(scopeChain, fun);
    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_deflocalfun(uint32 local, JSFunction *fun)
{
    JS_ASSERT(script->analysis()->usesScopeChain());
    MDefinition *scopeChain = current->getSlot(info().scopeChainSlot());

    MLambda *ins = MLambda::New(scopeChain, fun);
    current->add(ins);
    current->push(ins);

    current->setLocal(local);
    current->pop();

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_defvar(uint32 index)
{
    JS_ASSERT(JSOp(*pc) == JSOP_DEFVAR || JSOp(*pc) == JSOP_DEFCONST);

    PropertyName *name = script->getName(index);

    // Bake in attrs.
    unsigned attrs = JSPROP_ENUMERATE;
    // isEvalFrame() requires  attrs |= JSPROP_PERMANENT.
    if (JSOp(*pc) == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    // Pass the ScopeChain.
    JS_ASSERT(script->analysis()->usesScopeChain());
    MDefinition *scopeChain = current->getSlot(info().scopeChainSlot());
    JS_ASSERT(scopeChain->type() == MIRType_Object);

    // Bake the name pointer into the MDefVar.
    MDefVar *defvar = MDefVar::New(name, attrs, scopeChain);
    current->add(defvar);

    return resumeAfter(defvar);
}

bool
IonBuilder::jsop_this()
{
    if (!info().fun())
        return abort("JSOP_THIS outside of a JSFunction.");

    if (script->strictModeCode) {
        current->pushSlot(info().thisSlot());
        return true;
    }

    types::TypeSet *types = oracle->thisTypeSet(script);
    if (types && types->getKnownTypeTag(cx) == JSVAL_TYPE_OBJECT) {
        // This is safe, because if the entry type of |this| is an object, it
        // will necessarily be an object throughout the entire function. OSR
        // can introduce a phi, but this phi will be specialized.
        current->pushSlot(info().thisSlot());
        return true;
    }

    return abort("JSOP_THIS hard case not yet handled");
}

bool
IonBuilder::jsop_typeof()
{
    TypeOracle::Unary unary = oracle->unaryOp(script, pc);

    MDefinition *input = current->pop();
    MTypeOf *ins = MTypeOf::New(input, unary.ival);

    current->add(ins);
    current->push(ins);

    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}

bool
IonBuilder::jsop_toid()
{
    // No-op if the index is an integer.
    TypeOracle::Unary unary = oracle->unaryOp(script, pc);
    if (unary.ival == MIRType_Int32)
        return true;

    MDefinition *index = current->pop();
    MToId *ins = MToId::New(current->peek(-1), index);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iter(uint8 flags)
{
    MDefinition *obj = current->pop();
    MInstruction *ins = MIteratorStart::New(obj, flags);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iternext(uint8 depth)
{
    MDefinition *iter = current->peek(-depth);
    MInstruction *ins = MIteratorNext::New(iter);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_itermore()
{
    MDefinition *iter = current->peek(-1);
    MInstruction *ins = MIteratorMore::New(iter);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iterend()
{
    MDefinition *iter = current->pop();
    MInstruction *ins = MIteratorEnd::New(iter);

    current->add(ins);

    return resumeAfter(ins);
}
