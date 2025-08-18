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
#ifdef USE_SLAB
        freeAllocations();
#else
        freeAllNodes();
#endif
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

        // Descend down the tree, recording the path taken.
        for (unsigned depth = 0; depth < m_height; depth++) {
            InnerNode* inner = node->asInner();        
            size_t index = inner->subtreeForInsert(node->size(), interval.end());
            path.append({ *node, index });

            node = &inner->child(index);
        }
        // Found the correct leaf for the insert, now determine the position within that leaf.
        size_t insertionIndex = node->asLeaf()->firstIntervalEndAfter(node->size(), interval.end());
        path.append( { *node, insertionIndex });
        ASSERT(path.size() == m_height + 1);

        auto [newNode, newNodeCoverage] = insertInNodeSplitIfNeeded<LeafNode>(path, m_height, interval, value);

        // Ascend back up the tree along the same path, inserting new inner nodes as needed.
        for (int depth = m_height - 1; depth >= 0; depth--) {    
            if (!newNode) [[likely]]
                return;
            PathEntry& entry = path[depth];
            InnerNode* inner = entry.node.asInner();

            ASSERT(inner->child(entry.index).size() + newNode.size() == (static_cast<unsigned>(depth + 1) == m_height ? LeafOrder : InnerOrder) + 1);
            ASSERT(newNodeCoverage);
            entry.index++; // Insert new parent immediately after the existing parent
            std::tie(newNode, newNodeCoverage) = insertInNodeSplitIfNeeded<InnerNode>(path, depth, newNodeCoverage, newNode);
        }

        // There's a new node at depth 0 so a new level is required.
        if (newNode) [[unlikely]] {
            ASSERT(m_root.size() + newNode.size() == (m_height ? InnerOrder : LeafOrder) + 1);
            // Need to add another level to the tree.
            InnerNode* newRoot = allocNode<InnerNode>();
            newRoot->interval(0) = m_rootInterval;
            newRoot->child(0) = m_root;
            newRoot->interval(1) = newNodeCoverage;
            newRoot->child(1) = newNode;
            m_height++;
            m_root = NodePtr(newRoot, 2);
            m_rootInterval = newRoot->coverage(2);
            dataLogLn("Added level height=", m_height);
        }
    }

    // remove the given interval from the IntervalSet. The interval must be present.
    void erase(const Interval& interval)
    {
        Path path;
        ASSERT(interval.overlaps(m_rootInterval));
        ASSERT(m_root);
        NodePtr* node = &m_root;

        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = node->asInner();
            size_t index = inner->firstIntervalEndAfter(node->size(), interval.begin());
            ASSERT(index < node->size());
            ASSERT(inner->interval(index).begin() < interval.end());
            path.append({ *node, index });
            node = &inner->child(index);
        }
        LeafNode* leaf = node->asLeaf();
        size_t eraseIndex = leaf->firstIntervalEndAfter(node->size(), interval.begin());
        ASSERT(leaf->interval(eraseIndex).begin() == interval.begin() && leaf->interval(eraseIndex).end() == interval.end());
        path.append({ *node, eraseIndex });

        bool removedNode = eraseFromNode<LeafNode>(path, m_height);

        // Ascend back up the tree along the same path, removing nodes that are now empty.
        for (int depth = m_height - 1; depth >= 0; depth--) {    
            if (!removedNode) [[likely]]
                return;
            removedNode = eraseFromNode<InnerNode>(path, depth);
        }

        // If removeNode was true at every depth, the tree is now empty.
        if (removedNode) [[unlikely]] {
            if (m_height)
                freeNode(m_root.asInner());
            else
                freeNode(m_root.asLeaf());
            m_root = NodePtr();
            m_rootInterval = Interval();
            m_height = 0;
            dataLogLn("Tree is empty");
        }
    }

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

    bool isFirstOrLastIndex(NodePtr node, unsigned index)
    {
        ASSERT(index < node.size());
        return index == 0 || index == node.size() - 1;
    }

    void updateCoverage(const Path& path, int depth, Interval coverage)
    {
        ASSERT(depth >= 0);
        depth--; // So that depth is at the parent of the node with 'coverage'.
        while (depth >= 0) {
            const PathEntry& entry = path[depth];
            InnerNode* inner = entry.node.asInner();
            inner->interval(entry.index) = coverage;

            // FIXME: and/or should we filter based on actual coverage value since we may hit a common ancestor
            // when modifying multiple node, and one could be first and the other could be last.
            if (!isFirstOrLastIndex(entry.node, entry.index)) {
                // Since first/last of this node was not modified, its coverage hasn't changed - no need to continue upward.
                verifyCoverageConsistency(path, depth, inner->coverage(entry.node.size()));
                return;
            }
            coverage = inner->coverage(entry.node.size());
            depth--;
        }
        m_rootInterval = coverage;
    }

    void verifyCoverageConsistency(const Path& path, int depth, Interval coverage)
    {
#ifdef ASSERT_ENABLED
        ASSERT(depth >= 0);
        depth--;
        while (depth >= 0) {
            const PathEntry& entry = path[depth];
            InnerNode* inner = entry.node.asInner();
            ASSERT(inner->interval(entry.index) == coverage);
            coverage = inner->coverage(entry.node.size());
            depth--;
        }
        if (m_rootInterval != coverage) {
            dataLogLn("FAIL: m_rootInterval=", m_rootInterval, " coverage=", coverage, " Tree=", *this);
        }
        ASSERT(m_rootInterval == coverage);
#endif
    }

    // Inserts interval and value into the node referred to by path at the given depth. Updates affected NodePtr 
    // sizes and coverages for the affected subtree. If the node needed to be split then returns the NodePtr and
    // coverage interval for the new node for the caller to insert into the parent.
    template<typename NodeType>
    std::pair<NodePtr, Interval> insertInNodeSplitIfNeeded(const Path& path, int depth, const Interval& interval, const typename NodeType::PayloadType& value)
    {
        NodePtr& nodePtr = path[depth].node;
        auto insertionIndex = path[depth].index;
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
            updateCoverage(path, depth, node->coverage(nodeSize));
            return { NodePtr(newNode, newNodeSize), newNode->coverage(newNodeSize) };
        }

        node->insertAt(nodeSize, insertionIndex, interval, value);
        nodePtr.setSize(nodeSize);
        if (isFirstOrLastIndex(nodePtr, insertionIndex))
            updateCoverage(path, depth, node->coverage(nodeSize));
        return { NodePtr(), Interval() };
    }

    template<typename NodeType>
    bool eraseFromNode(const Path& path, int depth)
    {
        NodePtr& nodePtr = path[depth].node;
        auto eraseIndex = path[depth].index;
        auto node = nodePtr.template as<NodeType>();
        size_t nodeSize = nodePtr.size();
        ASSERT(nodeSize <= NodeType::capacity);

        if (nodeSize == 1) [[unlikely]] {
            ASSERT(!eraseIndex);
            freeNode(node);
            nodePtr = NodePtr();
            return true;
        }
        node->removeAt(nodeSize, eraseIndex);
        if (isFirstOrLastIndex(nodePtr, eraseIndex))
            updateCoverage(path, depth, node->coverage(nodeSize));
        nodePtr.setSize(nodeSize);
        return false;
    }

#ifdef USE_SLAB
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

    template<typename NodeType>
    void freeNode(NodeType* node)
    {
        // In slab allocator, individual nodes cannot be freed
        // Memory is freed when the entire slab is freed
        UNUSED_PARAM(node);
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

#else
    template<typename NodeType>
    NodeType* allocNode()
    {
        return static_cast<NodeType*>(fastAlignedMalloc(cpuCacheLineSize, sizeof(NodeType)));
    }

    template<typename NodeType>
    void freeNode(NodeType* node)
    {
        fastAlignedFree(node);
    }

    // Free all nodes iteratively to avoid stack overflow
    void freeAllNodes()
    {
        if (!m_root)
            return;
    
        Vector<std::pair<NodePtr, unsigned>, 16> stack;
        stack.append({ m_root, m_height });
        
        while (!stack.isEmpty()) {
            auto [node, distanceToLeaf] = stack.takeLast();
            
            if (!distanceToLeaf) {
                freeNode(node.asLeaf());
                continue;
            }
            InnerNode* inner = node.asInner();
            for (size_t i = 0; i < node.size(); ++i)
                stack.append({ inner->child(i), distanceToLeaf - 1 });
            freeNode(inner);
        }
    }

#endif
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

    NodePtr m_root { };
    Interval m_rootInterval { T{}, T{} };
    unsigned m_height { 0 };

    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
};

} // namespace WTF

using WTF::IntervalSet;
