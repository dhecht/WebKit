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

#include <wtf/FastMalloc.h>
#include <wtf/Range.h>
#include <wtf/Vector.h>

namespace WTF {

// IntervalSet: A specialized B+ tree for storing non-overlapping intervals with efficient overlap queries.
// Uses WTF::Range<T> for interval representation and supports gap-based load balancing.

template<typename T, typename Value, size_t Order = 4>
    requires std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<Value>
class IntervalSet {
public:
    using Interval = Range<T>;
    
    static constexpr size_t cpuCacheLineSize = 64;
    static constexpr size_t nodesPerSlab = 8;
    
    class iterator;

    IntervalSet()
    : m_rootInterval(T{}, T{})
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
        NodePtr newChild = insertIntoSubtree(m_root, interval, value, 0);
        if (newChild) [[unlikely]] {
            ASSERT(newChild.size() + m_root.size() == Order + 1);
            // Need to add another level to the tree.
            InnerNode* newRoot = allocNode<InnerNode>();
            newRoot->interval(0) = m_root.coverage(m_height);
            newRoot->child(0) = m_root;
            newRoot->interval(1) = newChild.coverage(m_height);
            newRoot->child(1) = newChild;
            m_height++;
            m_root = NodePtr(newRoot, 2);
        }
        m_rootInterval = m_root.coverage(m_height);
    }

    // Remove an interval from the B+ tree
    void erase(const Interval& interval);

    // Find value by interval
    const Value* find(const Interval& query) const
    {
        if (!query.overlaps(m_rootInterval))
            return nullptr;

        ASSERT(m_root);
        NodePtr node = m_root;
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node.asInner();
            size_t pos = inner->firstIntervalEndAfter(node.size(), query.begin());
            if (pos == node.size())
                return nullptr; // query is entirely after this subtree
            if (query.end() <= inner->interval(pos).begin())
                return nullptr; // query is entirely before this subtree
            // Otherwise, there may exist an overlapping interval in this subtree 
            node = inner->child(pos);
        }
        LeafNode* leaf = node.asLeaf();
        size_t pos = leaf->firstIntervalEndAfter(node.size(), query.begin());
        ASSERT(pos < node.size()); // coverage check at parent level ensures this
        ASSERT(query.begin() < leaf->interval(pos).end());
        return leaf->interval(pos).begin() < query.end() ? &leaf->value(pos) : nullptr;
    }

    // Check if any stored interval overlaps with the query interval
    bool hasOverlap(const Interval& query) const
    {
        if (!query.overlaps(m_rootInterval))
            return false;

        ASSERT(m_root);
        NodePtr node = m_root;
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node.asInner();
            size_t pos = inner->firstIntervalEndAfter(node.size(), query.begin());
            if (pos == node.size())
                return false; // query starts after all intervals
            // query start lands either within the pos subtree or the gap immediately preceding that subtree
            ASSERT(query.begin() < inner->interval(pos).end());
            if (query.end() <= inner->interval(pos).begin())
                return false; // query is entirely in the gap before this subtree
            if (inner->interval(pos).end() <= query.end())
                return true; // query spans subtree end point so it must overlap the last interval
            if (query.begin() <= inner->interval(pos).begin())
                return true; // query spans subtree start point so it must overlap the first interval
            // Otherwise, subtree encompasses query so need to search subtree
            ASSERT(inner->interval(pos).begin() < query.begin() && query.end() < inner->interval(pos).end());
            node = inner->child(pos);
        }

        LeafNode* leaf = node.asLeaf();
        size_t pos = leaf->firstIntervalEndAfter(node.size(), query.end());
        ASSERT(query.begin() < leaf->interval(pos).end());
        return leaf->interval(pos).begin() < query.end();
    }

    iterator findFirstAfter(const Interval& interval);

private:
    class LeafNode;
    class InnerNode;
    
    class Node {
        // Common base class for all nodes - provides type identity for NodePtr
    };

    template<typename Payload, size_t N>
    class NodeImpl : public Node {
    public:
        using PayloadType = Payload;
        Interval& interval(unsigned i)
        {
            return intervals[i];
        }

        // XXX: maybe move this to NodePtr so it can directly update size?
        void insertAt(size_t& size, size_t index, const Interval& interval, const Payload& value)
        {
            ASSERT(size < N);
            ASSERT(index <= size);
            // Shift elements to the right
            // FIXME: use memmove?
            for (size_t i = size; i > index; --i) {
                intervals[i] = intervals[i - 1];
                payloads[i] = payloads[i - 1];
            }
            intervals[index] = interval;
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
                intervals[i] = intervals[i + 1];
                payloads[i] = payloads[i + 1];
            }
            size--;
        }

        // Find the least interval with end greater than the given point, and return the position,
        // or size if no such interval exists.
        size_t firstIntervalEndAfter(size_t size, T point) const
        {
            size_t i = 0;
            while (i < size && intervals[i].end() <= point)
                ++i;
            return i;
        }

        Interval intervals[N];
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

        const Interval coverage(unsigned distanceToLeaf) const
        {
            RELEASE_ASSERT(size());
            if (distanceToLeaf) {
                auto inner = asInner();
                return { inner->interval(0).begin(), inner->interval(size() - 1).end() };
             } else {
                auto leaf = asLeaf();
                return { leaf->interval(0).begin(), leaf->interval(size() - 1).end() };
            }
        }

        explicit operator bool() const { return m_bits; }

        template<typename NodeType> requires std::is_base_of_v<Node, NodeType>
        NodeType* as() const
        {
            return static_cast<NodeType*>(node());
        }

        LeafNode* asLeaf() const
        {
            return as<LeafNode>();
        }
        
        InnerNode* asInner() const
        {
            return as<InnerNode>();
        }

    private:
        // Low 4 bits contains the size, remaining bits contains the pointer.
        uintptr_t m_bits;
    };

    class LeafNode : public NodeImpl<Value, Order> {
    public:
        using NodeImpl<Value, Order>::payloads;
        Value& value(unsigned i)
        {
            return payloads[i];
        }
        
        LeafNode* next { nullptr };
        LeafNode* prev { nullptr };
        size_t size { 0 }; // FIXME: redudant with size in the NodePtr
    };

    class InnerNode : public NodeImpl<NodePtr, Order> {
    public:
        using NodeImpl<NodePtr, Order>::payloads;
        NodePtr& child(unsigned i)
        {
            return payloads[i];
        }
    };

public:
    class iterator {
    public:
        iterator() : m_leaf(nullptr), m_position(0) { }
        
        iterator(LeafNode* leaf, size_t position) : m_leaf(leaf), m_position(position) { }
        
        std::pair<Interval, Value> operator*() const
        {
            ASSERT(m_leaf && m_position < m_leaf->size);
            return { m_leaf->interval(m_position), m_leaf->value(m_position) };
        }
        
        iterator& operator++()
        {
            ASSERT(m_leaf);
            ++m_position;
            if (m_position >= m_leaf->size) {
                m_leaf = m_leaf->next;
                m_position = 0;
            }
            return *this;
        }
        
        bool operator==(const iterator& other) const
        {
            return m_leaf == other.m_leaf && m_position == other.m_position;
        }
        
        bool operator!=(const iterator& other) const
        {
            return !(*this == other);
        }
        
    private:
        LeafNode* m_leaf;
        size_t m_position;
    };

private:
    NodePtr insertIntoSubtree(NodePtr& subtree, const Interval& interval, const Value& value, unsigned depth)
    {
        if (depth == m_height) {
            size_t pos = subtree.asLeaf()->firstIntervalEndAfter(subtree.size(), interval.end());
            return insertInNodeSplitIfNeeded<LeafNode>(subtree, interval, value, pos);
        }

        InnerNode* inner = subtree.asInner();        
        size_t pos = inner->firstIntervalEndAfter(subtree.size(), interval.end());

        if (pos == subtree.size())
            pos = subtree.size() - 1;

        unsigned childDepth = depth + 1;
        NodePtr newChild = insertIntoSubtree(inner->child(pos), interval, value, childDepth);
        inner->interval(pos) = inner->child(pos).coverage(m_height - childDepth);

        if (newChild) [[unlikely]] {
            ASSERT(inner->child(pos).size() + newChild.size() == Order + 1);
            Interval newChildCoverage = newChild.coverage(m_height - childDepth);
            return insertInNodeSplitIfNeeded<InnerNode>(subtree, newChildCoverage, newChild, pos + 1);
        }
        return NodePtr(); // Inserted without needing to split
    }

    // Inserts interval and value into the node referred to by NodePtr, and updates NodePtr with
    // the new size. If the node needed to be split returns a NodePtr for the new node.
    template<typename NodeType>
    NodePtr insertInNodeSplitIfNeeded(NodePtr& nodePtr, const Interval& interval, const typename NodeType::PayloadType& value, size_t insertionPoint)
    {
        auto node = nodePtr.template as<NodeType>();
        size_t nodeSize = nodePtr.size();
        ASSERT(nodeSize <= Order);

        if (nodeSize == Order) [[unlikely]] {
            constexpr size_t splitPoint = Order / 2;
            // Node is full, need to split
            auto newNode = allocNode<NodeType>();

            for (size_t i = splitPoint; i < nodeSize; ++i) {
                newNode->intervals[i - splitPoint] = node->intervals[i];
                newNode->payloads[i - splitPoint] = node->payloads[i];
            }
            size_t newNodeSize = nodeSize - splitPoint;
            nodeSize = splitPoint;
            
            if (insertionPoint < splitPoint) {
                node->insertAt(nodeSize, insertionPoint, interval, value);
            } else {
                // Insert into new node (recalculate insertion point for new node)
                insertionPoint -= splitPoint;
                newNode->insertAt(newNodeSize, insertionPoint, interval, value);
            }
            
            // If this is a leaf node split, maintain the linked list and size
            if constexpr (std::is_same_v<NodeType, LeafNode>) {
                // Insert newNode after node in the linked list
                newNode->next = node->next;
                newNode->prev = node;
                
                if (node->next)
                    node->next->prev = newNode;
                node->next = newNode;
                
                node->size = nodeSize;
                newNode->size = newNodeSize;
            }
            
            // nodePtr node's size has changed so update the nodePtr to reflect the new size.
            nodePtr.setSize(nodeSize);
            return NodePtr(newNode, newNodeSize);
        }
        
        // Node has space, insert without splitting
        node->insertAt(nodeSize, insertionPoint, interval, value);
        // nodeSize was incremented by insertAt, so update the NodePtr
        nodePtr.setSize(nodeSize);
        
        // Update leaf size if this is a leaf node
        if constexpr (std::is_same_v<NodeType, LeafNode>) {
            node->size = nodeSize;
        }
        
        return NodePtr();
    }

    // FIXME: should be made more flexible.
    static constexpr size_t allocSize = std::max(sizeof(InnerNode), sizeof(LeafNode));

    template<typename NodeType>
    NodeType* allocNode()
    {
        static_assert(std::is_base_of_v<Node, NodeType>);        
        if (m_slabs.isEmpty() || m_slabOffset >= nodesPerSlab)
            allocateSlab();
        
        NodeType* node = reinterpret_cast<NodeType*>(m_slabs.last() + m_slabOffset * allocSize);
        ASSERT(!(reinterpret_cast<uintptr_t>(node) % cpuCacheLineSize)); // Ensure cache line alignment
        ++m_slabOffset;
        return node;
    }

    void allocateSlab()
    {
        size_t slabSize = allocSize * nodesPerSlab;
        char* slab = static_cast<char*>(fastAlignedMalloc(cpuCacheLineSize, slabSize));
        m_slabs.append(slab);
        m_slabOffset = 0;
    }

    void freeAllocations()
    {
        for (auto& slab : m_slabs)
            fastAlignedFree(slab);
        m_slabs.clear();
        m_slabOffset = 0;
    }

    LeafNode* findFirstLeaf() const
    {
        if (!m_root)
            return nullptr;
            
        NodePtr node = m_root;
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node.asInner();
            node = inner->child(0);
        }
        return node.asLeaf();
    }

public:
    iterator begin() const
    {
        LeafNode* firstLeaf = findFirstLeaf();
        if (!firstLeaf || firstLeaf->size() == 0)
            return end();
        return iterator(firstLeaf, 0);
    }
    
    iterator end() const
    {
        return iterator(nullptr, 0);
    }

private:
    // Root node pointer
    NodePtr m_root;
    Interval m_rootInterval;
    unsigned m_height { 0 };

    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
};

} // namespace WTF

using WTF::IntervalSet;

