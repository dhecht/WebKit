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

#include <wtf/Vector.h>
#include <wtf/FastMalloc.h>

namespace JSC { namespace B3 { namespace Air {

// Consider making this specialized to Range<>, then searches can end early without traversing downward when no overlap.

template<typename Key, typename Value, size_t Order>
    requires std::is_trivially_destructible_v<Key> && std::is_trivially_destructible_v<Value>
class BPlusTree {
public:
    static constexpr size_t cpuCacheSize = 64;
    static constexpr size_t nodesPerSlab = 8;
    
    class iterator;

    BPlusTree()
    {
    }

    ~BPlusTree()
    {
        freeAllocations();
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
    class LeafNode;
    class InnerNode;
    
    class Node {
        // Common base class for all nodes - provides type identity for NodePtr
    };

    template<typename Payload, size_t N>
    class NodeImpl : public Node {
    public:

        void insertAt(size_t size, size_t index, const Key& key, const Payload& value)
        {
            ASSERT(index <= size);
            ASSERT(size < N);
            
            // Shift elements to the right
            for (size_t i = size; i > index; --i) {
                keys[i] = keys[i - 1];
                payloads[i] = payloads[i - 1];
            }
            keys[index] = key;
            payloads[index] = value;
        }
        
        void removeAt(size_t size, size_t index)
        {
            ASSERT(index < size);
            
            // Shift elements to the left
            for (size_t i = index; i < size - 1; ++i) {
                keys[i] = keys[i + 1];
                payloads[i] = payloads[i + 1];
            }
        }
        
        size_t findInsertionPoint(size_t size, const Key& key) const
        {
            size_t i = 0;
            while (i < size && keys[i] < key)
                ++i;
            return i;
        }

        Key keys[N];
        Payload payloads[N];
    };

    class NodePtr {
    public:
        static constexpr uintptr_t size_mask = 0xf;

        NodePtr() : m_bits(0) { }
        
        NodePtr(Node* ptr, size_t size) :
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
        
        explicit operator bool() const { return m_bits; }
        
        LeafNode* asLeaf() const
        {
            return static_cast<LeafNode*>(get());
        }
        
        InnerNode* asInner() const
        {
            return static_cast<InnerNode*>(get());
        }

    private:
        // Low 4 bits contains the size, remaining bits contains the pointer.
        uintptr_t m_bits;
    };

    class LeafNode : public NodeImpl<Value, Order> {
    };

    class InnerNode : public NodeImpl<NodePtr, Order> {
    };

    Node* allocNode()
    {
        static_assert(sizeof(InnerNode) == sizeof(LeafNode));
        
        if (m_slabs.isEmpty() || m_slabOffset >= nodesPerSlab)
            allocateSlab();
        
        Node* node = reinterpret_cast<Node*>(m_slabs.last() + m_slabOffset * sizeof(LeafNode));
        ++m_slabOffset;
        return node;
    }

    void allocateSlab()
    {
        size_t slabSize = sizeof(LeafNode) * nodesPerSlab;
        char* slab = static_cast<char*>(WTF::fastAlignedMalloc(cpuCacheSize, slabSize));
        m_slabs.append(slab);
        m_slabOffset = 0;
    }

    void freeAllocations()
    {
        for (auto& slab : m_slabs)
            WTF::fastAlignedFree(slab);
        m_slabs.clear();
        m_slabOffset = 0;
    }

    void insertIntoLeaf(LeafNode& leaf, size_t leafSize, const Key& key, const Value& value)
    {
        if (leafSize >= Order) {
            // XXX overflow - need to split leaf
            RELEASE_ASSERT_NOT_REACHED();
        }
        
        size_t insertionPoint = leaf.findInsertionPoint(leafSize, key);
        leaf.insertAt(leafSize, insertionPoint, key, value);
    }
    
    // Root node pointer - for now just use a simple pointer
    // In a full implementation, this would be a NodePtr
    void* m_root { nullptr };
    bool m_rootIsLeaf { true };
    size_t m_rootSize { 0 };

    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

