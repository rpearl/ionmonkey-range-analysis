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
#ifndef jsion_assembler_shared_h__
#define jsion_assembler_shared_h__

#include <limits.h>
#include "ion/IonAllocPolicy.h"
#include "ion/Registers.h"
#include "ion/RegisterSets.h"

namespace js {
namespace ion {

enum Scale {
    TimesOne,
    TimesTwo,
    TimesFour,
    TimesEight
};

static inline Scale
ScaleFromShift(int shift)
{
    switch (shift) {
      case 1:
        return TimesOne;
      case 2:
        return TimesTwo;
      case 4:
        return TimesFour;
      case 8:
        return TimesEight;
    }

    JS_NOT_REACHED("Invalid scale");
    return TimesOne;
}

// Used for 32-bit immediates which do not require relocation.
struct Imm32
{
    int32_t value;

    explicit Imm32(int32_t value) : value(value)
    { }

    static inline Imm32 ShiftOf(enum Scale s) {
        switch (s) {
          case TimesOne:
            return Imm32(0);
          case TimesTwo:
            return Imm32(1);
          case TimesFour:
            return Imm32(2);
          case TimesEight:
            return Imm32(3);
        };
        JS_NOT_REACHED("Invalid scale");
        return Imm32(-1);
    }

    static inline Imm32 FactorOf(enum Scale s) {
        return Imm32(1 << ShiftOf(s).value);
    }
};

// Pointer-sized immediate.
struct ImmWord
{
    uintptr_t value;

    explicit ImmWord(uintptr_t value) : value(value)
    { }
    explicit ImmWord(const void *ptr) : value(reinterpret_cast<uintptr_t>(ptr))
    { }

    void *asPointer() {
        return reinterpret_cast<void *>(value);
    }
};

// Used for immediates which require relocation.
struct ImmGCPtr
{
    uintptr_t value;

    explicit ImmGCPtr(const gc::Cell *ptr) : value(reinterpret_cast<uintptr_t>(ptr))
    { }
};

// Specifies a hardcoded, absolute address.
struct AbsoluteAddress {
    void *addr;

    explicit AbsoluteAddress(void *addr)
      : addr(addr)
    { }
};

// Specifies an address computed in the form of a register base and a constant,
// 32-bit offset.
struct Address
{
    Register base;
    int32 offset;

    Address(Register base, int32 offset) : base(base), offset(offset)
    { }

    Address() { PodZero(this); }
};

// Specifies an address computed in the form of a register base and a constant,
// 32-bit offset.
struct BaseIndex
{
    Register base;
    Register index;
    Scale scale;
    int32 offset;

    BaseIndex(Register base, Register index, Scale scale, int32 offset = 0)
      : base(base), index(index), scale(scale), offset(offset)
    { }

    BaseIndex() { PodZero(this); }
};

class Relocation {
  public:
    enum Kind {
        // The target is immovable, so patching is only needed if the source
        // buffer is relocated and the reference is relative.
        HARDCODED,

        // The target is the start of an IonCode buffer, which must be traced
        // during garbage collection. Relocations and patching may be needed.
        IONCODE
    };
};

struct LabelBase
{
  protected:
    // offset_ >= 0 means that the label is either bound or has incoming
    // uses and needs to be bound.
    int32 offset_ : 31;
    bool bound_   : 1;

    // Disallow assignment.
    void operator =(const LabelBase &label);

  public:
    static const int32 INVALID_OFFSET = -1;

    LabelBase() : offset_(INVALID_OFFSET), bound_(false)
    { }
    LabelBase(const LabelBase &label)
      : offset_(label.offset_),
        bound_(label.bound_)
    { }

    // If the label is bound, all incoming edges have been patched and any
    // future incoming edges will be immediately patched.
    bool bound() const {
        return bound_;
    }
    int32 offset() const {
        JS_ASSERT(bound() || used());
        return offset_;
    }
    // Returns whether the label is not bound, but has incoming uses.
    bool used() const {
        return !bound() && offset_ > INVALID_OFFSET;
    }
    // Binds the label, fixing its final position in the code stream.
    void bind(int32 offset) {
        JS_ASSERT(!bound());
        offset_ = offset;
        bound_ = true;
        JS_ASSERT(offset_ == offset);
    }
    // Marks the label as neither bound nor used.
    void reset() {
        offset_ = INVALID_OFFSET;
        bound_ = false;
    }
    // Sets the label's latest used position, returning the old use position in
    // the process.
    int32 use(int32 offset) {
        JS_ASSERT(!bound());

        int32 old = offset_;
        offset_ = offset;
        JS_ASSERT(offset_ == offset);

        return old;
    }
};

// A label represents a position in an assembly buffer that may or may not have
// already been generated. Labels can either be "bound" or "unbound", the
// former meaning that its position is known and the latter that its position
// is not yet known.
//
// A jump to an unbound label adds that jump to the label's incoming queue. A
// jump to a bound label automatically computes the jump distance. The process
// of binding a label automatically corrects all incoming jumps.
class Label : public LabelBase
{
  public:
    Label()
    { }
    Label(const Label &label) : LabelBase(label)
    { }
    ~Label()
    {
        JS_ASSERT(!used());
    }
};

// An absolute label is like a Label, except it represents an absolute
// reference rather than a relative one. Thus, it cannot be patched until after
// linking.
struct AbsoluteLabel : public LabelBase
{
  public:
    AbsoluteLabel()
    { }
    AbsoluteLabel(const AbsoluteLabel &label) : LabelBase(label)
    { }
    int32 prev() const {
        JS_ASSERT(!bound());
        if (!used())
            return INVALID_OFFSET;
        return offset();
    }
    void setPrev(int32 offset) {
        use(offset);
    }
    void bind() {
        bound_ = true;

        // These labels cannot be used after being bound.
        offset_ = -1;
    }
};

// A code label contains an absolute reference to a point in the code
// Thus, it cannot be patched until after linking
class CodeLabel : public TempObject
{
    // The destination position, where the absolute reference should get patched into
    AbsoluteLabel dest_;
    
    // The source label (relative) in the code to where the 
    // the destination should get patched to.
    Label src_;

  public:
    CodeLabel()
    { }
    AbsoluteLabel *dest() {
        return &dest_;
    }
    Label *src() {
        return &src_;
    }
};

// Deferred data is a chunk of data that cannot be computed until an assembly
// buffer has been fully allocated, but should be attached to the final code
// stream. At the time deferred data is emitted, the code buffer has been
// completely allocated.
class DeferredData : public TempObject
{
    // Label, which before linking is unbound.
    AbsoluteLabel label_;

    // Offset from the start of the data section.
    int32 offset_;

  public:
    DeferredData() : offset_(-1)
    { }
    int32 offset() const {
        JS_ASSERT(offset_ > -1);
        return offset_;
    }
    void setOffset(int32 offset) {
        offset_ = offset;
    }
    AbsoluteLabel *label() {
        return &label_;
    }

    // Must copy pending data into the buffer.
    virtual void copy(IonCode *code, uint8 *buffer) const = 0;
};

// Location of a jump or label in a generated IonCode block, relative to the
// start of the block.

class CodeOffsetJump
{
    size_t offset_;

#ifdef JS_CPU_X64
    size_t jumpTableIndex_;
#endif

  public:

#ifdef JS_CPU_X64
    CodeOffsetJump(size_t offset, size_t jumpTableIndex)
        : offset_(offset), jumpTableIndex_(jumpTableIndex)
    {}
    size_t jumpTableIndex() const {
        return jumpTableIndex_;
    }
#else
    CodeOffsetJump(size_t offset) : offset_(offset) {}
#endif

    CodeOffsetJump() {
        PodZero(this);
    }

    size_t offset() const {
        return offset_;
    }
};

class CodeOffsetLabel
{
    size_t offset_;

  public:
    CodeOffsetLabel(size_t offset) : offset_(offset) {}
    CodeOffsetLabel() : offset_(0) {}

    size_t offset() const {
        return offset_;
    }
    void fixup(MacroAssembler *masm);

};

// Absolute location of a jump or a label in some generated IonCode block.
// Can also encode a CodeOffset{Jump,Label}, such that the offset is initially
// set and the absolute location later filled in after the final IonCode is
// allocated.

class CodeLocationJump
{
    uint8 *raw_;

#ifdef JS_CPU_X64
    uint8 *jumpTableEntry_;
#endif

#ifdef DEBUG
    bool absolute;
    void markAbsolute(bool value) {
        absolute = value;
    }
#else
    void markAbsolute(bool value) {}
#endif

  public:
    CodeLocationJump() {}
    CodeLocationJump(IonCode *code, CodeOffsetJump base) {
        *this = base;
        repoint(code);
    }

    void operator = (CodeOffsetJump base) {
        raw_ = (uint8 *) base.offset();
#ifdef JS_CPU_X64
        jumpTableEntry_ = (uint8 *) base.jumpTableIndex();
#endif
        markAbsolute(false);
    }

    void repoint(IonCode *code, MacroAssembler* masm = NULL);

    uint8 *raw() const {
        JS_ASSERT(absolute);
        return raw_;
    }
    uint8 *offset() const {
        JS_ASSERT(!absolute);
        return raw_;
    }

#ifdef JS_CPU_X64
    uint8 *jumpTableEntry() {
        JS_ASSERT(absolute);
        return jumpTableEntry_;
    }
#endif
};

class CodeLocationLabel
{
    uint8 *raw_;
#ifdef DEBUG
    bool absolute;
    void markAbsolute(bool value) {
        absolute = value;
    }
#else
    void markAbsolute(bool value) {}
#endif

  public:
    CodeLocationLabel() {}
    CodeLocationLabel(IonCode *code, CodeOffsetLabel base) {
        *this = base;
        repoint(code);
    }
    CodeLocationLabel(IonCode *code) {
        raw_ = code->raw();
        markAbsolute(true);
    }
    CodeLocationLabel(uint8 *raw) {
        raw_ = raw;
        markAbsolute(true);
    }

    void operator = (CodeOffsetLabel base) {
        raw_ = (uint8 *) base.offset();
        markAbsolute(false);
    }
    ptrdiff_t operator - (const CodeLocationLabel &other) {
        return raw_ - other.raw_;
    }

    void repoint(IonCode *code, MacroAssembler *masm = NULL);

    uint8 *raw() {
        JS_ASSERT(absolute);
        return raw_;
    }
    uint8 *offset() {
        JS_ASSERT(!absolute);
        return raw_;
    }
};

} // namespace ion
} // namespace js

#endif // jsion_assembler_shared_h__

