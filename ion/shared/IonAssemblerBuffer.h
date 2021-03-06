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
 *   Marty Rosenberg <mrosenberg@mozilla.com>
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

#ifndef __ion_assembler_buffer_h
#define __ion_assembler_buffer_h
// needed for the definition of Label :(
#include "ion/shared/Assembler-shared.h"

namespace js {
namespace ion {

// This should theoretically reside inside of AssemblerBuffer, but that won't be nice
// AssemblerBuffer is templated, BufferOffset would be indirectly.
// A BufferOffset is the offset into a buffer, expressed in bytes of instructions.

class BufferOffset
{
    int offset;
  public:
    friend BufferOffset nextOffset();
    explicit BufferOffset(int offset_) : offset(offset_) {}
    // Return the offset as a raw integer.
    int getOffset() const { return offset; }

    // A BOffImm is a Branch Offset Immediate. It is an architecture-specific
    // structure that holds the immediate for a pc relative branch.
    // diffB takes the label for the destination of the branch, and encodes
    // the immediate for the branch.  This will need to be fixed up later, since
    // A pool may be inserted between the branch and its destination
    template <class BOffImm>
    BOffImm diffB(BufferOffset other) const {
        return BOffImm(offset - other.offset);
    }

    template <class BOffImm>
    BOffImm diffB(Label *other) const {
        JS_ASSERT(other->bound());
        return BOffImm(offset - other->offset());
    }

    explicit BufferOffset(Label *l) : offset(l->offset()) {
    }

    BufferOffset() : offset(INT_MIN) {}
    bool assigned() const { return offset != INT_MIN; };
};

template<int SliceSize>
struct BufferSlice : public InlineForwardListNode<BufferSlice<SliceSize> > {
  protected:
    // How much data has been added to the current node.
    uint32 nodeSize;
  public:
    BufferSlice *getNext() { return static_cast<BufferSlice *>(this->next); }
    void setNext(BufferSlice<SliceSize> *next_) {
        JS_ASSERT(this->next == NULL);
        this->next = next_;
    }
    uint8 instructions [SliceSize];
    unsigned int size() {
        return nodeSize;
    }
    BufferSlice() : InlineForwardListNode<BufferSlice<SliceSize> >(NULL), nodeSize(0) {}
    void putBlob(uint32 instSize, uint8* inst) {
        if (inst != NULL)
            memcpy(&instructions[size()], inst, instSize);
        nodeSize += instSize;
    }
};

template<int SliceSize, class Inst>
struct AssemblerBuffer {
  public:
    AssemblerBuffer() : head(NULL), tail(NULL), m_oom(false), bufferSize(0) {}
  protected:
    typedef BufferSlice<SliceSize> Slice;
    Slice *head;
    Slice *tail;
  public:
    bool m_oom;
    // How much data has been added to the buffer thusfar.
    uint32 bufferSize;
    uint32 lastInstSize;
    bool isAligned(int alignment) const {
        // make sure the requested alignment is a power of two.
        JS_ASSERT((alignment & (alignment-1)) == 0);
        return !(size() & (alignment - 1));
    }
    virtual Slice *newSlice() {
        Slice *tmp = static_cast<Slice*>(malloc(sizeof(Slice)));
        if (!tmp) {
            m_oom = true;
            return NULL;
        }
        new (tmp) Slice;
        return tmp;
    }
    bool ensureSpace(int size) {
        if (tail != NULL && tail->size()+size <= SliceSize)
            return true;
        Slice *tmp = newSlice();
        if (tmp == NULL)
            return false;
        if (tail != NULL) {
            bufferSize += tail->size();
            tail->setNext(tmp);
        }
        tail = tmp;
        if (head == NULL)
            head = tmp;
        return true;
    }

    void putByte(uint8 value) {
        putBlob(sizeof(value), (uint8*)&value);
    }

    void putShort(uint16 value) {
        putBlob(sizeof(value), (uint8*)&value);
    }

    void putInt(uint32 value) {
        putBlob(sizeof(value), (uint8*)&value);
    }
    void putBlob(uint32 instSize, uint8 *inst) {
        if (!ensureSpace(instSize))
            return;
        tail->putBlob(instSize, inst);
    }
    unsigned int size() const {
        int executableSize;
        if (tail != NULL)
            executableSize = bufferSize + tail->size();
        else
            executableSize = bufferSize;
        return executableSize;
    }
    unsigned int uncheckedSize() const {
        return size();
    }
    bool oom() const {
        return m_oom;
    }
    void fail_oom() {
        m_oom = true;
    }
    Inst *getInst(BufferOffset off) {
        unsigned int local_off = off.getOffset();
        Slice *cur = NULL;
        if (local_off > bufferSize) {
            local_off -= bufferSize;
            cur = tail;
        } else {
            for (cur = head; cur != NULL; cur = cur->getNext()) {
                if (local_off < cur->size())
                    break;
                local_off -= cur->size();
            }
            JS_ASSERT(cur != NULL);
        }
        // the offset within this node should not be larger than the node itself.
        JS_ASSERT(local_off < cur->size());
        return (Inst*)&cur->instructions[local_off];
    }
    BufferOffset nextOffset() const {
        if (tail != NULL)
            return BufferOffset(bufferSize + tail->size());
        else
            return BufferOffset(bufferSize);
    }
    BufferOffset prevOffset() const {
        JS_NOT_REACHED("Don't current record lastInstSize");
        return BufferOffset(bufferSize + tail->nodeSize - lastInstSize);
    }

    // Break the instruction stream so we can go back and edit it at this point
    void perforate() {
        Slice *tmp = newSlice();
        if (!tmp)
            m_oom = true;
        bufferSize += tail->size();
        tail->setNext(tmp);
        tail = tmp;
    }

};

} // ion
} // js

#endif // __ion_assembler_buffer_h
