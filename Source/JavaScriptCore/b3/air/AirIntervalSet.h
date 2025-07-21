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
#include <wtf/Range.h>

namespace JSC { namespace B3 { namespace Air {

// IntervalSet: A specialized B+ tree for storing non-overlapping intervals with efficient overlap queries.
// Uses WTF::Range<T> for interval representation and supports gap-based load balancing.

template<typename T, typename Value, size_t Order>
    requires std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<Value>
class IntervalSet {
public:
    using Interval = WTF::Range<T>;
    
    static constexpr size_t cpuCacheLineSize = 64;
    static constexpr size_t nodesPerSlab = 8;
    
    class iterator;

    IntervalSet()
    {
    }

    ~IntervalSet()
    {
        freeAllocations();
    }

    // Insert an interval-value pair into the B+ tree
    void insert(const Interval& interval, const Value& value)
    {
        if (!m_root) {
            // Create initial root as a leaf
            LeafNode* leaf = allocNode<LeafNode>();
            m_root = NodePtr(leaf, 0);
            m_height = 0;
        }
        NodePtr newChild = insertImpl(m_root, m_rootInterval, interval, value, 0);
        if (newChild) {
            ASSERT(m_root.size() == Order); // Otherwise, root should have been updated.
            // Need to add another level to the tree.
            InnerNode* newRoot = allocNode<InnerNode>();
            newRoot->key(0) = m_rootInterval;
            newRoot->child(0) = m_root;
            newRoot->key(1) = newChild.coverage();
            newRoot->child(1) = newChild;
            m_root = NodePtr(newRoot, 2);
            m_height++;
        }
    }

    // Remove an interval from the B+ tree
    void erase(const Interval& interval);

    // Find value by interval
    Value* find(const Interval& interval) {
        if (!m_root)
            return nullptr;
        
        return findImpl(m_root, interval);
    }

    const Value* find(const Interval& interval) const
    {
        if (!m_root)
            return nullptr;
        
        return findImpl(m_root, interval);
    }

    // Check if any stored interval overlaps with the query interval
    bool hasOverlap(const Interval& query) const
    {
        return hasOverlapImpl(query);
    }

    iterator findFirstAfter(const Interval& interval);

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
        Interval& key(unsigned i)
        {
            return keys[i];
        }

        // XXX: maybe move this to NodePtr so it can directly update size?
        void insertAt(size_t& size, size_t index, const Interval& interval, const Payload& value)
        {
            ASSERT(size < N);
            ASSERT(index <= size);
            // Shift elements to the right
            // FIXME: use memmove?
            for (size_t i = size; i > index; --i) {
                keys[i] = keys[i - 1];
                payloads[i] = payloads[i - 1];
            }
            keys[index] = interval;
            payloads[index] = value;
            size++;
        }
        
        void removeAt(size_t& size, size_t index)
        {
            ASSERT(size <= N);
            ASSERT(index < size);
            // Shift elements to the left
            // FIXME: use memmove?
            for (size_t i = index; i < size - 1; ++i) {
                keys[i] = keys[i + 1];
                payloads[i] = payloads[i + 1];
            }
            size--;
        }

        // Find the first position with an interval whose end is greater than the given start.
        // This is used for interval-based routing in the specialized B+ tree.
        size_t lowerBound(size_t size, T start) const
        {
            size_t i = 0;
            while (i < size && keys[i].end() <= start)
                ++i;
            return i;
        }

        Interval keys[N];
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

        Node* node() const
        {
            return reinterpret_cast<Node*>(m_bits & ~size_mask);
        }
    
        size_t size() const
        {
            return m_bits & size_mask;
        }
        
        void setSize(size_t newSize)
        {
            ASSERT(newSize <= size_mask);
            m_bits = (m_bits & ~size_mask) | newSize;
        }

        const Interval coverage() const
        {
            RELEASE_ASSERT(size());
            return { node()->key(0).start(), node()->key(size() - 1).end() };
        }

        explicit operator bool() const { return m_bits; }

        template<typename NodeType> requires std::is_base_of_v<Node, NodeType>
        NodeType* as() const
        {
            return static_cast<NodeType*>(node());
        }

        LeafNode* asLeaf() const
        {
            return static_cast<LeafNode*>(node());
        }
        
        InnerNode* asInner() const
        {
            return static_cast<InnerNode*>(node());
        }

    private:
        // Low 4 bits contains the size, remaining bits contains the pointer.
        uintptr_t m_bits;
    };

    class LeafNode : public NodeImpl<Value, Order> {
        using NodeImpl<Value, Order>::payloads;
    public:
        Value& value(unsigned i)
        {
            return payloads[i];
        }
    };

    class InnerNode : public NodeImpl<NodePtr, Order> {
        using NodeImpl<NodePtr, Order>::payloads;
    public:
        NodePtr& child(unsigned i)
        {
            return payloads[i];
        }
    };

    NodePtr insertImpl(NodePtr& subtree, Interval& subtreeInterval, const Interval& interval, const Value& value, unsigned depth)
    {
        size_t pos = subtree.node()->lowerBound(subtree.size(), interval.end());
        if (depth == m_height) {
            // subtree is really a leaf. Insert and, if the node was split, return the new leaf node.
            NodePtr newLeaf = insertInNodeSplitIfNeeded<LeafNode>(subtree, interval, value, pos);
            subtreeInterval = subtree.coverage();
            return newLeaf;
        }
        size_t oldSize = subtree.size();
        // Not a leaf so insert into this subtree.
        InnerNode* inner = subtree.asInner();
        
        if (pos == subtree.size())
            pos = subtree.size() - 1;

        NodePtr newChild = insertImpl(inner->child(pos), inner->key(pos), interval, value, depth + 1);
        if (newChild) {
            ASSERT_UNUSED(oldSize, oldSize == Order); // Otherwise, should have been inserted.
            ASSERT_UNUSED(oldSize, inner->child(pos).size() + newChild.size() == oldSize + 1);
            // Insert the new child into the subtree. The key for inner nodes is the coverage interval of the children.
            const Interval& newChildInterval = newChild.coverage();
            NodePtr newInner = insertInNodeSplitIfNeeded<InnerNode>(subtree, newChildInterval, newChild, pos + 1);
            subtreeInterval = subtree.coverage();
            return newInner;
        }
        ASSERT_UNUSED(oldSize, inner->child(pos).size() == oldSize + 1);
        // Update subtreeInterval in case the maximum interval of this subtree changed
        subtreeInterval = subtree.coverage();
        // Nothing more to do, new interval/value was inserted and the tree fully updated.
        return nullptr;
    }

    Value* findImpl(NodePtr node, const Interval& interval) const
    {
        // Traverse down to the leaf
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node.asInner();
            size_t pos = inner->lowerBound(node.size(), interval.begin());
            if (pos == node.size())
                return nullptr;
            node = inner->child(pos);
        }
        LeafNode* leaf = node.asLeaf();
        for (size_t i = 0; i < node.size(); ++i) {
            if (leaf->key(i) == interval)
                return &leaf->value(i);
        }
        return nullptr; // Interval not found
    }

    // Check if any stored interval overlaps with the query interval
    bool hasOverlapImpl(const Interval& query) const
    {
        if (!m_root)
            return false;
            
        NodePtr node = m_root;
        
        // Traverse down to leaf, using coverage intervals for pruning
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node.asInner();
            
            // Find first child whose coverage might contain overlapping intervals
            size_t pos = inner->lowerBound(node.size(), query.begin());
            if (pos == node.size())
                return false; // Query starts after all coverage intervals
                
            // Check if this child's coverage actually overlaps the query
            if (!inner->key(pos).overlaps(query))
                return false; // No overlap possible
                
            node = inner->child(pos);
        }
        
        // At leaf level - since intervals are sorted and don't overlap,
        // we just need to check if any interval overlaps the query
        LeafNode* leaf = node.asLeaf();
        for (size_t i = 0; i < node.size(); ++i) {
            if (leaf->key(i).overlaps(query))
                return true;
            // Since intervals are sorted by start and don't overlap,
            // if this interval starts after query ends, no more can overlap
            if (leaf->key(i).begin() >= query.end())
                break;
        }
        
        return false;
    }

    // Inserts interval and value into the node referred to by NodePtr, and updates NodePtr with
    // the new size. If the node needed to be split returns a NodePtr for the new node.
    template<typename NodeType>
    NodePtr insertInNodeSplitIfNeeded(NodePtr& nodePtr, const Interval& interval, const Value& value, size_t insertionPoint)
    {
        auto node = nodePtr.template as<NodeType>();
        size_t nodeSize = nodePtr.size();

        if (nodeSize == Order) {
            constexpr size_t splitPoint = Order / 2;            
            // Leaf is full, need to split
            auto newNode = allocNode<NodeType>();

            for (size_t i = splitPoint; i < nodeSize; ++i) {
                newNode->keys[i - splitPoint] = node->keys[i];
                newNode->payloads[i - splitPoint] = node->payloads[i];
            }
            size_t newNodeSize = nodeSize - splitPoint;
            nodeSize = splitPoint;
            
            if (insertionPoint < splitPoint) {
                node->insertAt(splitPoint, insertionPoint, interval, value);
            } else {
                // Insert into new leaf (recalculate insertion point for new leaf)
                insertionPoint -= splitPoint;
                newNode->insertAt(newNodeSize, insertionPoint, interval, value);
            }
            // nodePtr node's size has changed so update the nodePtr to reflect the new size.
            nodePtr.setSize(nodeSize);
            return NodePtr(newNode, newNodeSize);
        }
        
        // Node has space, simple insertion
        node->insertAt(nodeSize, insertionPoint, interval, value);
        nodePtr.setSize(nodeSize);
        return nullptr;
    }

    template<typename NodeType>
    NodeType* allocNode()
    {
        static_assert(std::is_base_of_v<Node, NodeType>);
        // FIXME: should be made more flexible.
        static_assert(sizeof(InnerNode) == sizeof(LeafNode));
        static_assert(sizeof(NodeType) == sizeof(LeafNode));
        
        if (m_slabs.isEmpty() || m_slabOffset >= nodesPerSlab)
            allocateSlab();
        
        NodeType* node = reinterpret_cast<NodeType*>(m_slabs.last() + m_slabOffset * sizeof(LeafNode));
        ASSERT(!(reinterpret_cast<uintptr_t>(node) % cpuCacheLineSize)); // Ensure cache line alignment
        ++m_slabOffset;
        return node;
    }

    void allocateSlab()
    {
        size_t slabSize = sizeof(LeafNode) * nodesPerSlab;
        char* slab = static_cast<char*>(WTF::fastAlignedMalloc(cpuCacheLineSize, slabSize));
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

    // Root node pointer
    NodePtr m_root;
    Interval m_rootInterval;
    unsigned m_height { 0 };

    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

