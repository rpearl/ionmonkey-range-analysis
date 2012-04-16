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
 *   David Anderson <dvander@alliedmods.net>
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

#ifndef jsion_frames_h__
#define jsion_frames_h__

#include "jsfun.h"
#include "jstypes.h"
#include "jsutil.h"
#include "Registers.h"
#include "IonCode.h"
#include "IonFrameIterator.h"

struct JSFunction;
struct JSScript;

namespace js {
namespace ion {

typedef void * CalleeToken;

enum CalleeTokenTag
{
    CalleeToken_Function = 0x0, // untagged
    CalleeToken_Script = 0x1
};

static inline CalleeTokenTag
GetCalleeTokenTag(CalleeToken token)
{
    CalleeTokenTag tag = CalleeTokenTag(uintptr_t(token) & 0x3);
    JS_ASSERT(tag <= CalleeToken_Script);
    return tag;
}
static inline CalleeToken
CalleeToToken(JSFunction *fun)
{
    return CalleeToken(uintptr_t(fun) | uintptr_t(CalleeToken_Function));
}
static inline CalleeToken
CalleeToToken(JSScript *script)
{
    return CalleeToken(uintptr_t(script) | uintptr_t(CalleeToken_Script));
}
static inline bool
CalleeTokenIsFunction(CalleeToken token)
{
    return GetCalleeTokenTag(token) == CalleeToken_Function;
}
static inline JSFunction *
CalleeTokenToFunction(CalleeToken token)
{
    JS_ASSERT(CalleeTokenIsFunction(token));
    return (JSFunction *)token;
}
static inline JSScript *
CalleeTokenToScript(CalleeToken token)
{
    JS_ASSERT(GetCalleeTokenTag(token) == CalleeToken_Script);
    return (JSScript *)(uintptr_t(token) & ~uintptr_t(0x3));
}
JSScript *
MaybeScriptFromCalleeToken(CalleeToken token);

// In between every two frames lies a small header describing both frames. This
// header, minimally, contains a returnAddress word and a descriptor word. The
// descriptor describes the size and type of the previous frame, whereas the
// returnAddress describes the address the newer frame (the callee) will return
// to. The exact mechanism in which frames are laid out is architecture
// dependent.
//
// Two special frame types exist. Entry frames begin an ion activation, and
// therefore there is exactly one per activation of ion::Cannon. Exit frames
// are necessary to leave JIT code and enter C++, and thus, C++ code will
// always begin iterating from the topmost exit frame.

class LSafepoint;

// Two-tuple that lets you look up the safepoint entry given the
// displacement of a call instruction within the JIT code.
class SafepointIndex
{
    // The displacement is the distance from the first byte of the JIT'd code
    // to the return address (of the call that the safepoint was generated for).
    uint32 displacement_;

    union {
        LSafepoint *safepoint_;

        // Offset to the start of the encoded safepoint in the safepoint stream.
        uint32 safepointOffset_;
    };

    DebugOnly<bool> resolved;

  public:
    SafepointIndex(uint32 displacement, LSafepoint *safepoint)
      : displacement_(displacement),
        safepoint_(safepoint),
        resolved(false)
    { }

    void resolve();

    LSafepoint *safepoint() {
        JS_ASSERT(!resolved);
        return safepoint_;
    }
    uint32 displacement() const {
        return displacement_;
    }
    uint32 safepointOffset() const {
        return safepointOffset_;
    }
    void adjustDisplacement(uint32 offset) {
        JS_ASSERT(offset >= displacement_);
        displacement_ = offset;
    }
    inline SnapshotOffset snapshotOffset() const;
    inline bool hasSnapshotOffset() const;
};

class MacroAssembler;
// The OSI point is patched to a call instruction. Therefore, the
// returnPoint for an OSI call is the address immediately following that
// call instruction. The displacement of that point within the assembly
// buffer is the |returnPointDisplacement|.
class OsiIndex
{
    uint32 callPointDisplacement_;
    uint32 snapshotOffset_;

  public:
    OsiIndex(uint32 callPointDisplacement, uint32 snapshotOffset)
      : callPointDisplacement_(callPointDisplacement),
        snapshotOffset_(snapshotOffset)
    { }

    uint32 returnPointDisplacement() const;
    uint32 callPointDisplacement() const {
        return callPointDisplacement_;
    }
    uint32 snapshotOffset() const {
        return snapshotOffset_;
    }
    void fixUpOffset(MacroAssembler &masm);
};

// The layout of an Ion frame on the C stack is roughly:
//      argN     _
//      ...       \ - These are jsvals
//      arg0      /
//   -3 this    _/
//   -2 callee
//   -1 descriptor
//    0 returnAddress
//   .. locals ..

// The descriptor is organized into three sections:
// [ frame size | constructing bit | frame type ]
// < highest - - - - - - - - - - - - - - lowest >
static const uintptr_t FRAMESIZE_SHIFT = 3;
static const uintptr_t FRAMETYPE_BITS = 3;
static const uintptr_t FRAMETYPE_MASK = (1 << FRAMETYPE_BITS) - 1;

// Ion frames have a few important numbers associated with them:
//      Local depth:    The number of bytes required to spill local variables.
//      Argument depth: The number of bytes required to push arguments and make
//                      a function call.
//      Slack:          A frame may temporarily use extra stack to resolve cycles.
//
// The (local + argument) depth determines the "fixed frame size". The fixed
// frame size is the distance between the stack pointer and the frame header.
// Thus, fixed >= (local + argument).
//
// In order to compress guards, we create shared jump tables that recover the
// script from the stack and recover a snapshot pointer based on which jump was
// taken. Thus, we create a jump table for each fixed frame size.
//
// Jump tables are big. To control the amount of jump tables we generate, each
// platform chooses how to segregate stack size classes based on its
// architecture.
//
// On some architectures, these jump tables are not used at all, or frame
// size segregation is not needed. Thus, there is an option for a frame to not
// have any frame size class, and to be totally dynamic.
static const uint32 NO_FRAME_SIZE_CLASS_ID = uint32(-1);

class FrameSizeClass
{
    uint32 class_;

    explicit FrameSizeClass(uint32 class_) : class_(class_)
    { }
  
  public:
    FrameSizeClass()
    { }

    static FrameSizeClass None() {
        return FrameSizeClass(NO_FRAME_SIZE_CLASS_ID);
    }
    static FrameSizeClass FromClass(uint32 class_) {
        return FrameSizeClass(class_);
    }

    // These two functions are implemented in specific CodeGenerator-* files.
    static FrameSizeClass FromDepth(uint32 frameDepth);
    uint32 frameSize() const;

    uint32 classId() const {
        JS_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
        return class_;
    }

    bool operator ==(const FrameSizeClass &other) const {
        return class_ == other.class_;
    }
    bool operator !=(const FrameSizeClass &other) const {
        return class_ != other.class_;
    }
};

class IonJSFrameLayout;
class IonFrameIterator;

// Information needed to recover the content of the stack frame.
class FrameRecovery
{
    IonJSFrameLayout *fp_;
    uint8 *sp_;             // fp_ + frameSize

    MachineState machine_;
    uint32 snapshotOffset_;

    JSFunction *callee_;
    JSScript *script_;
    IonScript *ionScript_;

  private:
    FrameRecovery(uint8 *fp, uint8 *sp, const MachineState &machine);

    void setSnapshotOffset(uint32 snapshotOffset) {
        snapshotOffset_ = snapshotOffset;
    }
    void setBailoutId(BailoutId bailoutId);

    void unpackCalleeToken(CalleeToken token);

  public:
    static FrameRecovery FromBailoutId(uint8 *fp, uint8 *sp, const MachineState &machine,
                                       BailoutId bailoutId);
    static FrameRecovery FromSnapshot(uint8 *fp, uint8 *sp, const MachineState &machine,
                                      SnapshotOffset offset);

    // Override the ionScript gleaned from the JSScript.
    void setIonScript(IonScript *ionScript);

    MachineState &machine() {
        return machine_;
    }
    const MachineState &machine() const {
        return machine_;
    }
    JSFunction *callee() const {
        return callee_;
    }
    JSScript *script() const {
        return script_;
    }
    IonScript *ionScript() const;
    uint32 snapshotOffset() const {
        return snapshotOffset_;
    }
    uint32 frameSize() const {
        return ((uint8 *) fp_) - sp_;
    }
    IonJSFrameLayout *fp() {
        return fp_;
    }
};

// Data needed to recover from an exception.
struct ResumeFromException
{
    void *stackPointer;
};

void HandleException(ResumeFromException *rfe);

void MarkIonActivations(JSRuntime *rt, JSTracer *trc);

static inline uint32
MakeFrameDescriptor(uint32 frameSize, FrameType type)
{
    return (frameSize << FRAMESIZE_SHIFT) | type;
}

} // namespace ion
} // namespace js

#if defined(JS_CPU_X86) || defined (JS_CPU_X64)
# include "ion/shared/IonFrames-x86-shared.h"
#elif defined (JS_CPU_ARM)
# include "ion/arm/IonFrames-arm.h"
#else
# error "unsupported architecture"
#endif

namespace js {
namespace ion {

JSScript *
GetTopIonJSScript(JSContext *cx);

void
GetPcScript(JSContext *cx, JSScript **scriptRes, jsbytecode **pcRes);

// Given a slot index, returns the offset, in bytes, of that slot from an
// IonJSFrameLayout. Slot distances are uniform across architectures, however,
// the distance does depend on the size of the frame header.
static inline int32
OffsetOfFrameSlot(int32 slot)
{
    if (slot <= 0)
        return sizeof(IonJSFrameLayout) + -slot;
    return -(slot * STACK_SLOT_SIZE);
}

static inline uintptr_t
ReadFrameSlot(IonJSFrameLayout *fp, int32 slot)
{
    return *(uintptr_t *)((char *)fp + OffsetOfFrameSlot(slot));
}

static inline double
ReadFrameDoubleSlot(IonJSFrameLayout *fp, int32 slot)
{
    return *(double *)((char *)fp + OffsetOfFrameSlot(slot));
}

} /* namespace ion */
} /* namespace js */

#endif // jsion_frames_h__

