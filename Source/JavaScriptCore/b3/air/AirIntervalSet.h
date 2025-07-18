/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirTmp.h"

namespace JSC { namespace B3 { namespace Air {

template<typename Key, typename Value>
class BPlusTree {
public:
    class iterator;
    
    static constexpr size_t DefaultOrder = 16;

    BPlusTree()
    {
    }

    // Insert a key-value pair into the B+ tree
    void insert(const Key& key, const Value& value);

    // Remove a key from the B+ tree
    void erase(const Key& key);

    // Find value by key
    Value* find(const Key& key);
    const Value* find(const Key& key) const;

    iterator findFirstAfter(const Key& key);

    class iterator {
    public:
        iterator() = default;
        
        // TODO: Implement iterator for traversing intervals
        bool operator==(const iterator& other) const = default;
        iterator& operator++() { return *this; }
        // TODO: Add dereference operators
    };

private:
    // Forward declarations
    template<typename Payload, size_t N> class NodeBase;
    template<typename V, size_t N> class NodePtr;
    template<size_t N> class LeafNode;
    template<size_t N> class InnerNode;
    struct KeyValuePair;
    template<typename Payload, size_t N>
    class NodeBase {
    public:
        Key keys[N];
        Payload values[N];
        size_t size { 0 };
        
        bool isFull() const { return size == N; }
        bool isEmpty() const { return size == 0; }
        
        void insertAt(size_t index, const Key& key, const Payload& value)
        {
            ASSERT(index <= size);
            ASSERT(size < N);
            
            // Shift elements to the right
            for (size_t i = size; i > index; --i) {
                keys[i] = keys[i - 1];
                values[i] = values[i - 1];
            }
            
            keys[index] = key;
            values[index] = value;
            ++size;
        }
        
        void removeAt(size_t index)
        {
            ASSERT(index < size);
            
            // Shift elements to the left
            for (size_t i = index; i < size - 1; ++i) {
                keys[i] = keys[i + 1];
                values[i] = values[i + 1];
            }
            --size;
        }
        
        size_t findInsertionPoint(const Key& key) const
        {
            size_t i = 0;
            while (i < size && key >= keys[i])
                ++i;
            return i;
        }
    };

    template<typename V, size_t N>
    class NodePtr {
    public:
        using Node = NodeBase<V, N>;

        static constexpr uintptr_t size_mask = 0xf;

        NodePtr() : m_bits(0) { }
        
        NodePtr(NodeBase<V, N>* ptr, size_t size) :
            m_bits(reinterpret_cast<uintptr_t>(ptr) | size)
        {
            ASSERT(!(reinterpret_cast<uintptr_t>(ptr) & size_mask));
            ASSERT(size <= size_mask);
        }

        Node* get() const
        {
            return reinterpret_cast<Node*>(m_bits & ~size_mask);
        }
    
        size_t size() const
        {
            return m_bits & size_mask;
        }
        
        bool isNull() const { return !m_bits; }
        
        Node* operator->() const { return get(); }
        Node& operator*() const { return *get(); }

    private:
        // Low 4 bits contains the size, remaining bits contains the pointer.
        uintptr_t m_bits;
    };

    struct KeyValuePair {
        Point start;
        Tmp tmp;
    };

    template<size_t N>
    class LeafNode : public NodeBase<Value, N> {
    };

    template <size_t N>
    class InnerNode : public NodeBase<NodePtr<Value, N>, N> {
    public:
        // Find child index for a given end point
        size_t findChildIndex(Point end) const
        {
            size_t i = 0;
            while (i < this->size && end >= this->ends[i])
                ++i;
            return i;
        }
        
        NodePtr<Value, N>& childAt(size_t index) { return this->values[index]; }
        const NodePtr<Value, N>& childAt(size_t index) const { return this->values[index]; }
    };

    template<size_t N>
    void insertIntoLeaf(LeafNode<N>& leaf, Point start, Point end, Tmp tmp)
    {
        if (leaf.isFull()) {
            // XXX overflow - need to split leaf
            RELEASE_ASSERT_NOT_REACHED();
        }
        
        Value value { start, tmp };
        size_t insertionPoint = leaf.findInsertionPoint(end);
        leaf.insertAt(insertionPoint, end, value);
    }
    
    // Root node pointer - for now just use a simple pointer
    // In a full implementation, this would be a NodePtr
    void* m_root { nullptr };
    bool m_rootIsLeaf { true };
#if 0
    bool add(Tmp tmp)
    {
        if (tmp.isGP())
            return m_gp.add(tmp);
        return m_fp.add(tmp);
    }
    
    bool remove(Tmp tmp)
    {
        if (tmp.isGP())
            return m_gp.remove(tmp);
        return m_fp.remove(tmp);
    }
    
    bool contains(Tmp tmp)
    {
        if (tmp.isGP())
            return m_gp.contains(tmp);
        return m_fp.contains(tmp);
    }
    
    size_t size() const
    {
        return m_gp.size() + m_fp.size();
    }
    
    bool isEmpty() const
    {
        return !size();
    }

    class iterator {
    public:
        iterator()
        {
        }
        
        iterator(BitVector::iterator gpIter, BitVector::iterator fpIter)
            : m_gpIter(gpIter)
            , m_fpIter(fpIter)
        {
        }
        
        Tmp operator*()
        {
            if (!m_gpIter.isAtEnd())
                return Tmp::tmpForAbsoluteIndex(GP, *m_gpIter);
            return Tmp::tmpForAbsoluteIndex(FP, *m_fpIter);
        }
        
        iterator& operator++()
        {
            if (!m_gpIter.isAtEnd()) {
                ++m_gpIter;
                return *this;
            }
            ++m_fpIter;
            return *this;
        }
        
        friend bool operator==(const iterator&, const iterator&) = default;
        
    private:
        BitVector::iterator m_gpIter;
        BitVector::iterator m_fpIter;
    };
    
    iterator begin() const LIFETIME_BOUND { return iterator(m_gp.indices().begin(), m_fp.indices().begin()); }
    iterator end() const LIFETIME_BOUND { return iterator(m_gp.indices().end(), m_fp.indices().end()); }

private:
    IndexSet<Tmp::AbsolutelyIndexed<GP>> m_gp;
    IndexSet<Tmp::AbsolutelyIndexed<FP>> m_fp;
#endif
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

