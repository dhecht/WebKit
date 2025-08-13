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

#include <wtf/CommaPrinter.h>
#include <wtf/DataLog.h>
#include <wtf/FastMalloc.h>
#include <wtf/Range.h>
#include <wtf/Vector.h>

namespace WTF {

// IntervalSet: A specialized B+ tree for storing non-overlapping intervals with efficient overlap queries.
// Uses WTF::Range<T> for interval representation and supports gap-based load balancing.

template<typename T, typename Value, size_t CacheLinesPerNode = 1>
    requires std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<Value>
class IntervalSet {
public:
    using Interval = Range<T>;
    
    static constexpr size_t cpuCacheLineSize = 64;
    static constexpr size_t targetNodeSize = CacheLinesPerNode * cpuCacheLineSize;
    static constexpr size_t nodesPerSlab = 8;
    
    // Calculate optimal order for each node type based on target cache line usage
    static constexpr size_t calculateLeafOrder() {
        constexpr size_t sizePerOrder = sizeof(Interval) + sizeof(Value);
        return targetNodeSize / sizePerOrder;
    }
    
    static constexpr size_t calculateInnerOrder() {
        constexpr size_t sizePerOrder = sizeof(Interval) + sizeof(uintptr_t);
        return targetNodeSize / sizePerOrder;
    }
    
    static constexpr size_t LeafOrder = calculateLeafOrder();
    static constexpr size_t InnerOrder = calculateInnerOrder();
    
    // Ensure CacheLinesPerNode parameter is large enough for valid B+ tree orders
    static_assert(LeafOrder >= 2, "CacheLinesPerNode parameter too small: LeafNode order must be at least 2 for a valid B+ tree");
    static_assert(InnerOrder >= 2, "CacheLinesPerNode parameter too small: InnerNode order must be at least 2 for a valid B+ tree");
    
    class iterator;

    IntervalSet() { dataLogLn("leafOrder=", LeafOrder, " innerOrder=", InnerOrder);}

    ~IntervalSet()
    {
        freeAllocations();
    }

    // Insert an interval-value pair into the B+ tree
    void insert(const Interval& interval, const Value& value)
    {
        if (!m_root) [[unlikely]] {
            // Create initial root as a leaf
            LeafNode* leaf = allocNode<LeafNode>();
            m_root = NodePtr(leaf, 0);
            m_height = 0;
        }

        Path path;
        NodePtr* node = &m_root;

        for (unsigned depth = 0; depth < m_height; depth++) {
            InnerNode* inner = node->asInner();        
            size_t index = inner->subtreeForInsert(node->size(), interval.end());
            path.append({ *node, index });

            node = &inner->child(index);
        }
        path.append( { *node, 0 }); // leaf has no child so index is irrelevant
        ASSERT(path.size() == m_height + 1);

        size_t index = node->asLeaf()->firstIntervalEndAfter(node->size(), interval.end());
        auto [newNode, newNodeCoverage] = insertInNodeSplitIfNeeded<LeafNode>(path, m_height, interval, value, index);
        
        Interval coverage = node->asLeaf()->coverage(node->size());

        for (int depth = m_height - 1; depth >= 0; depth--) {    
            PathEntry& entry = path[depth];
            InnerNode* inner = entry.node.asInner();

            if (inner->interval(entry.index) != coverage) [[unlikely]]
                inner->interval(entry.index) = coverage;
    
            if (newNode) [[unlikely]] {
                ASSERT(inner->child(entry.index).size() + newNode.size() == (static_cast<unsigned>(depth + 1) == m_height ? LeafOrder : InnerOrder) + 1);
                ASSERT(newNodeCoverage);
                std::tie(newNode, newNodeCoverage) = insertInNodeSplitIfNeeded<InnerNode>(path, depth, newNodeCoverage, newNode, entry.index + 1);
            }
            coverage = inner->coverage(entry.node.size());
            // FIXME: if neither coverage nor newChild changed, we can stop
        }

        // Root was split so need to add a new level to the tree.
        if (newNode) [[unlikely]] {
            ASSERT(newNode.size() + m_root.size() == (m_height ? InnerOrder : LeafOrder) + 1);
            // Need to add another level to the tree.
            InnerNode* newRoot = allocNode<InnerNode>();
            if (m_height) {
                newRoot->interval(0) = m_root.asInner()->coverage(m_root.size());
                newRoot->interval(1) = newNode.asInner()->coverage(newNode.size());
            } else {
                newRoot->interval(0) = m_root.asLeaf()->coverage(m_root.size());
                newRoot->interval(1) = newNode.asLeaf()->coverage(newNode.size());
            }
            newRoot->child(0) = m_root;
            newRoot->child(1) = newNode;
            m_height++;
            m_root = NodePtr(newRoot, 2);
            coverage = newRoot->coverage(2);
            dataLogLn("Added level height=", m_height);
        }
        // FIXME: only update when necessary?
        m_rootInterval = coverage;
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
        size_t pos = leaf->firstIntervalEndAfter(node.size(), query.begin());
        ASSERT(query.begin() < leaf->interval(pos).end());
        return leaf->interval(pos).begin() < query.end();
    }

    iterator findFirstAfter(const Interval& interval);

    // Pretty print the tree structure for debugging
    void dump(PrintStream& out) const
    {
        out.print("IntervalSet(height=", m_height, ", leafOrder=", LeafOrder, ", innerOrder=", InnerOrder, ")");
        if (!m_root) {
            out.print(" <empty>");
            return;
        }
        out.print(" coverage=", m_rootInterval);
        out.print("\n");
        dumpSubtree(out, m_root, m_height, 0);
    }

private:
    struct LeafNode;
    struct InnerNode;
    
    struct Node {
        // Common base class for all nodes - provides type identity for NodePtr
    };

    template<typename Payload, size_t N>
    struct NodeImpl : public Node {
        using PayloadType = Payload;
        static constexpr size_t capacity = N;
        
        Interval& interval(unsigned i)
        {
            ASSERT(i < N);
            return intervals[i];
        }

        const Interval coverage(size_t size) const
        {
            RELEASE_ASSERT(size);
            return { intervals[0].begin(), intervals[size - 1].end() };
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
            ASSERT(size <= N);
            for (size_t i = 0; i < size; i++) {
                if (point < intervals[i].end())
                    return i;
            }
            return size;
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

    struct LeafNode : public NodeImpl<Value, LeafOrder> {
        Value& value(unsigned i)
        {
            ASSERT(i < LeafOrder);
            return this->payloads[i];
        }
    };

    struct InnerNode : public NodeImpl<NodePtr, InnerOrder> {
        NodePtr& child(unsigned i)
        {
            ASSERT(i < InnerOrder);
            return this->payloads[i];
        }

        size_t subtreeForInsert(size_t size, T point) const
        {
            ASSERT(size <= InnerOrder);
            // XXX: this only happens when the tree is empty or when creating a new level. Could we remove from this path?
            if (!size) [[unlikely]]
                return 0;
            for (size_t i = 0; i < size - 1; i++) {
                // XXX: or maybe keep adjacent intervals together
                if (point <= this->intervals[i + 1].begin())
                    return i;
            }
            return size - 1;
        }
    };
    struct PathEntry {
        NodePtr& node;
        size_t index;
    };

    using Path = Vector<PathEntry, 8>;

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
    // Inserts interval and value into the node referred to by path at the given depth, and updates NodePtr with
    // the new size. If the node needed to be split returns a NodePtr for the new node.
    template<typename NodeType>
    std::pair<NodePtr, Interval> insertInNodeSplitIfNeeded(const Path& path, int depth, const Interval& interval, const typename NodeType::PayloadType& value, size_t insertionIndex)
    {
        NodePtr& nodePtr = path[depth].node;
        auto node = nodePtr.template as<NodeType>();
        size_t nodeSize = nodePtr.size();
        ASSERT(nodeSize <= NodeType::capacity);

        if (nodeSize == NodeType::capacity) [[unlikely]] {
            constexpr size_t splitPoint = NodeType::capacity / 2;
            // Node is full, need to split
            auto newNode = allocNode<NodeType>();

            for (size_t i = splitPoint; i < nodeSize; ++i) {
                newNode->intervals[i - splitPoint] = node->intervals[i];
                newNode->payloads[i - splitPoint] = node->payloads[i];
            }
            size_t newNodeSize = nodeSize - splitPoint;
            nodeSize = splitPoint;
            
            if (insertionIndex < splitPoint) {
                node->insertAt(nodeSize, insertionIndex, interval, value);
            } else {
                insertionIndex -= splitPoint;
                newNode->insertAt(newNodeSize, insertionIndex, interval, value);
            }            
            nodePtr.setSize(nodeSize);
            return { NodePtr(newNode, newNodeSize), newNode->coverage(newNodeSize) };
        }

        node->insertAt(nodeSize, insertionIndex, interval, value);
        nodePtr.setSize(nodeSize);
        return { NodePtr(), Interval() };
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

    void dumpSubtree(PrintStream& out, NodePtr node, unsigned distanceToLeaf, unsigned indent) const
    {
        auto printIndent = [&] {
            for (unsigned i = 0; i < indent; ++i)
                out.print("  ");
        };

        if (distanceToLeaf) {
            InnerNode* inner = node.asInner();
            printIndent();
            out.println("Inner(size=", node.size(), ", coverage=", inner->coverage(node.size()), "):");
            
            for (size_t i = 0; i < node.size(); ++i) {
                printIndent();
                out.println("  [", i, "] ", inner->interval(i));
                dumpSubtree(out, inner->child(i), distanceToLeaf - 1, indent + 2);
            }
        } else {
            CommaPrinter comma;
            LeafNode* leaf = node.asLeaf();
            printIndent();
            out.print("Leaf(size=", node.size(), "): ");
            for (size_t i = 0; i < node.size(); ++i)
                out.print(comma, leaf->interval(i), "=", leaf->value(i));
            out.println();
        }
    }

public:
    iterator begin() const
    {
        LeafNode* firstLeaf = findFirstLeaf();
        if (!firstLeaf || !firstLeaf->size)
            return end();
        return iterator(firstLeaf, 0);
    }
    
    iterator end() const
    {
        return iterator(nullptr, 0);
    }

private:
    // Root node pointer
    NodePtr m_root { };
    Interval m_rootInterval { T{}, T{} };
    unsigned m_height { 0 };

    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
};

} // namespace WTF

using WTF::IntervalSet;

