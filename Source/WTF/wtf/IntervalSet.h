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
#include <wtf/MathExtras.h>
#include <wtf/Range.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WTF {

// IntervalSet: A specialized B+ tree for storing non-overlapping intervals with efficient overlap queries.
// Uses WTF::Range<T> for interval representation and supports gap-based load balancing.

template<typename T, typename Value, size_t cacheLinesPerNode = 1>
    requires std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<Value>
class IntervalSet {
public:
    using Interval = Range<T>;
    
    static constexpr size_t cpuCacheLineSize = 64;
    static constexpr size_t targetNodeSize = cacheLinesPerNode * cpuCacheLineSize;
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
    
    static constexpr size_t leafOrder = calculateLeafOrder();
    static constexpr size_t innerOrder = calculateInnerOrder();
    
    // Ensure cacheLinesPerNode parameter is large enough for valid B+ tree orders
    static_assert(leafOrder >= 2, "cacheLinesPerNode parameter too small: LeafNode order must be at least 2 for a valid B+ tree");
    static_assert(innerOrder >= 2, "cacheLinesPerNode parameter too small: InnerNode order must be at least 2 for a valid B+ tree");
    
    class iterator;

    IntervalSet() = default;

    ~IntervalSet()
    {
#ifdef USE_SLAB
        freeAllocations();
#else
        freeAllNodes();
#endif
        ASSERT(!assertOnlyNumNodes);
    }

    bool isEmpty() const { return !m_rootInterval; }

    // Insert an interval-value pair into the B+ tree
    void insert(const Interval& interval, const Value& value)
    {
        if (!m_root) [[unlikely]] {
            // Create initial root as a leaf
            LeafNode* leaf = allocNode<LeafNode>();
            m_root = NodeRef(leaf, 0);
            m_height = 0;
        }

        Path path;
        NodeRef* nodeRef = &m_root;

        // Descend down the tree, recording the path taken.
        for (unsigned depth = 0; depth < m_height; depth++) {
            InnerNode* inner = nodeRef->asInner();        
            size_t index = inner->subtreeForInsert(nodeRef->size(), interval.end());
            path.append({ nodeRef, index });

            nodeRef = &inner->child(index);
        }
        // Found the correct leaf for the insert, now determine the position within that leaf.
        size_t insertionIndex = nodeRef->asLeaf()->firstIntervalEndAfter(nodeRef->size(), interval.end());
        path.append( { nodeRef, insertionIndex });
        ASSERT(path.size() == m_height + 1);

        auto [newNode, newNodeCoverage] = insertInNodeSplitIfNeeded<LeafNode>(path, m_height, interval, value);

        // Ascend back up along the same path, splitting inner nodes as needed.
        for (int depth = m_height - 1; depth >= 0; depth--) {    
            if (!newNode) [[likely]]
                return;
            PathEntry& entry = path[depth];

            ASSERT(entry.nodeRef->asInner()->child(entry.index).size() + newNode.size() == (static_cast<unsigned>(depth + 1) == m_height ? leafOrder : innerOrder) + 1);
            ASSERT(newNodeCoverage);
            entry.index++; // Insert new parent immediately after the existing parent
            std::tie(newNode, newNodeCoverage) = insertInNodeSplitIfNeeded<InnerNode>(path, depth, newNodeCoverage, newNode);
        }

        // If there's a new node at depth 0 then a new level is required.
        if (newNode) [[unlikely]] {
            ASSERT(m_root.size() + newNode.size() == (m_height ? innerOrder : leafOrder) + 1);
            // Need to add another level to the tree.
            InnerNode* newRoot = allocNode<InnerNode>();
            newRoot->interval(0) = m_rootInterval;
            newRoot->child(0) = m_root;
            newRoot->interval(1) = newNodeCoverage;
            newRoot->child(1) = newNode;
            m_height++;
            m_root = NodeRef(newRoot, 2);
            m_rootInterval = newRoot->coverage(2);
        }
    }

    // remove the given interval from the IntervalSet. The interval must be present.
    void erase(const Interval& interval)
    {
        Path path;
        ASSERT(interval.overlaps(m_rootInterval));
        ASSERT(m_root);
        NodeRef* nodeRef = &m_root;

        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = nodeRef->asInner();
            size_t index = inner->firstIntervalEndAfter(nodeRef->size(), interval.begin());
            ASSERT(index < nodeRef->size());
            ASSERT(inner->interval(index).begin() < interval.end());
            path.append({ nodeRef, index });
            nodeRef = &inner->child(index);
        }
        LeafNode* leaf = nodeRef->asLeaf();
        size_t eraseIndex = leaf->firstIntervalEndAfter(nodeRef->size(), interval.begin());
        ASSERT(leaf->interval(eraseIndex).begin() == interval.begin() && leaf->interval(eraseIndex).end() == interval.end());
        path.append({ nodeRef, eraseIndex });

        bool removedNode = eraseFromNode<LeafNode>(path, m_height);

        // Ascend removing references to any child that was removed, which may in turn cause the parent to become empty.
        for (int depth = m_height - 1; depth >= 0; depth--) {    
            if (!removedNode) [[likely]]
                return;
            removedNode = eraseFromNode<InnerNode>(path, depth);
        }

        // If removedNode was true at every depth, the tree is now empty.
        if (removedNode) [[unlikely]] {
            ASSERT(!assertOnlyNumNodes);
            ASSERT(!m_root);
            m_rootInterval = Interval();
            m_height = 0;
        }
    }

    // returns the Interval and Value for the first interval, if any, that overlaps with the query interval
    std::optional<std::pair<Interval, Value>> find(const Interval& query) const
    {
        if (!query.overlaps(m_rootInterval))
            return std::nullopt;

        ASSERT(m_root);
        NodeRef nodeRef = m_root;
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = nodeRef.asInner();
            size_t pos = inner->firstIntervalEndAfter(nodeRef.size(), query.begin());
            if (pos == nodeRef.size())
                return std::nullopt; // query is entirely after this subtree
            if (query.end() <= inner->interval(pos).begin())
                return std::nullopt; // query is entirely before this subtree
            // Otherwise, there may exist an overlapping interval in this subtree
            nodeRef = inner->child(pos);
        }
        LeafNode* leaf = nodeRef.asLeaf();
        size_t index = leaf->firstIntervalEndAfter(nodeRef.size(), query.begin());
        ASSERT(index < nodeRef.size()); // coverage check at parent level ensures this
        ASSERT(query.begin() < leaf->interval(index).end());
        if (query.end() <= leaf->interval(index).begin())
            return std::nullopt;
        return std::make_pair(leaf->interval(index), leaf->value(index));
    }

    // Returns true iff any stored interval overlaps with the query interval
    bool hasOverlap(const Interval& query) const
    {
        if (!query.overlaps(m_rootInterval))
            return false;

        ASSERT(m_root);
        NodeRef nodeRef = m_root;
        for (unsigned depth = 0; depth < m_height; ++depth) {
            InnerNode* inner = nodeRef.asInner();
            size_t index = inner->firstIntervalEndAfter(nodeRef.size(), query.begin());
            if (index == nodeRef.size())
                return false; // query starts after all intervals
            // query start lands either within the pos subtree or the gap immediately preceding that subtree
            ASSERT(query.begin() < inner->interval(index).end());
            if (query.end() <= inner->interval(index).begin())
                return false; // query is entirely in the gap before this subtree
            if (inner->interval(index).end() <= query.end())
                return true; // query spans subtree end point so it must overlap the last interval
            if (query.begin() <= inner->interval(index).begin())
                return true; // query spans subtree start point so it must overlap the first interval
            // Otherwise, subtree encompasses query so need to search subtree
            ASSERT(inner->interval(index).begin() < query.begin() && query.end() < inner->interval(index).end());
            nodeRef = inner->child(index);
        }

        LeafNode* leaf = nodeRef.asLeaf();
        size_t index = leaf->firstIntervalEndAfter(nodeRef.size(), query.begin());
        ASSERT(query.begin() < leaf->interval(index).end());
        return leaf->interval(index).begin() < query.end();
    }

    // Pretty print the tree structure for debugging
    void dump(PrintStream& out) const
    {
        out.print("IntervalSet(height=", m_height, ", leafOrder=", leafOrder, ", innerOrder=", innerOrder, ")");
        if (!m_root) {
            out.print(" <empty>");
            return;
        }
        out.println(" coverage=", m_rootInterval);
        dumpSubtree(out, m_root, m_height, 0);
    }

private:
    struct LeafNode;
    struct InnerNode;
    
    struct Node {
        // Common base class for all nodes - provides type identity for NodePtr
    };

    template<typename Payload, size_t order>
    struct NodeImpl : public Node {
        using PayloadType = Payload;
        static constexpr size_t capacity = order;
        
        Interval& interval(unsigned i)
        {
            ASSERT(i < capacity);
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
            ASSERT(size < capacity);
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
            ASSERT(size <= capacity);
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
            ASSERT(size <= capacity);
            for (size_t i = 0; i < size; i++) {
                if (point < intervals[i].end())
                    return i;
            }
            return size;
        }

        Interval intervals[order];
        Payload payloads[order];
    };

    class NodeRef {
    public:
        static_assert(isPowerOfTwo(cpuCacheLineSize));

        static constexpr uintptr_t sizeMask = cpuCacheLineSize - 1;
        static_assert(leafOrder <= sizeMask && innerOrder <= sizeMask);

        NodeRef() : m_bits(0) { }
        
        NodeRef(Node* ptr, size_t size) :
            m_bits(reinterpret_cast<uintptr_t>(ptr) | size)
        {
            ASSERT(!(reinterpret_cast<uintptr_t>(ptr) & sizeMask));
            ASSERT(size <= sizeMask);
        }

        Node* node() const
        {
            return reinterpret_cast<Node*>(m_bits & ~sizeMask);
        }
    
        size_t size() const
        {
            return m_bits & sizeMask;
        }
        
        void setSize(size_t newSize)
        {
            ASSERT(newSize <= sizeMask);
            m_bits = (m_bits & ~sizeMask) | newSize;
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

    struct LeafNode : public NodeImpl<Value, leafOrder> {
        Value& value(unsigned i)
        {
            ASSERT(i < leafOrder);
            return this->payloads[i];
        }
    };

    struct InnerNode : public NodeImpl<NodeRef, innerOrder> {
        NodeRef& child(unsigned i)
        {
            ASSERT(i < innerOrder);
            return this->payloads[i];
        }

        size_t subtreeForInsert(size_t size, T endPoint) const
        {
            ASSERT(size <= innerOrder);
            // XXX: this only happens when the tree is empty or when creating a new level. Could we remove from this path?
            if (!size) [[unlikely]]
                return 0;
            for (size_t i = 0; i < size - 1; i++) {
                // XXX: or maybe keep adjacent intervals together
                if (endPoint <= this->intervals[i + 1].begin())
                    return i;
            }
            return size - 1;
        }
    };

private:
    struct PathEntry {
        NodeRef* nodeRef;
        size_t index;

        bool operator==(const PathEntry& other) const
        {
            return nodeRef->node() == other.nodeRef->node() && index == other.index;
        }
    };

    class Path : public Vector<PathEntry, 8>
    {
        friend class iterator;
        
        // Advances to the next index of the leaf node, if exists.
        // If the current leaf node is exhausted, advance to next
        // leaf node and set index to 0.
        void next()
        {
            ASSERT(this->size());
            int height = this->size() - 1;
            PathEntry& leafEntry = this->last();
            if (++leafEntry.index < leafEntry.nodeRef->size()) [[likely]]
                return;
            if (!height) {
                // Tree is a single leaf - reached end
                this->clear();
                return;
            }
            int depth = height - 1; // Deepest inner node level
            // Ascend up the tree until we find a node with indices to the right
            for (; depth >= 0; depth--) {
                PathEntry& innerEntry = this->at(depth);
                if (innerEntry.index < innerEntry.nodeRef->size() - 1)
                    break;
            }
            if (depth < 0) {
                // Exhausted all indices of the root node.
                this->clear();
                return;
            }
            // Descend down the left-most edges of the next subtree
            PathEntry& innerEntry = this->at(depth);
            innerEntry.index++;
            depth++;
            NodeRef* childRef = &innerEntry.nodeRef->asInner()->child(innerEntry.index);
            for (; depth < height; depth++) {
                this->at(depth).nodeRef = childRef;
                this->at(depth).index = 0;
                childRef = &childRef->asInner()->child(0);
            }
            this->at(depth).nodeRef = childRef;
            this->at(depth).index = 0;
            ASSERT(childRef->size());
        }
    };

public:
    class iterator {
    public:
        iterator() = default;
        
        iterator(Path&& path)
        : m_path(WTFMove(path))
        { }
        
        const Interval& interval() const
        {
            auto [leaf, index] = leafAndIndex();
            return leaf->interval(index);
        }
        
        const Value& value() const
        {
            auto [leaf, index] = leafAndIndex();
            return leaf->value(index);
        }

        const std::pair<Interval, Value> operator*() const
        {
            return { interval(), value() };
        }

        iterator& operator++()
        {
            m_path.next();
            return *this;
        }

        bool operator==(const iterator& other) const
        {
            return m_path == other.m_path;
        }
        
        bool operator!=(const iterator& other) const
        {
            return !(*this == other);
        }
        
    private:
        const std::pair<LeafNode*, unsigned> leafAndIndex() const
        {
            const PathEntry& entry = m_path.last();
            return { entry.nodeRef->asLeaf(), entry.index };
        }

        Path m_path;
    };

    // returns an iterator with the path to the left-most leaf node and index 0
    iterator begin() const
    {
        if (!m_root)
            return end();
        Path path;
        NodeRef* nodeRef = const_cast<NodeRef*>(&m_root);
        // Generate path to the left-most leaf node.
        for (unsigned depth = 0; depth < m_height; depth++) {
            ASSERT(nodeRef->size());
            path.append({ nodeRef, 0 });
            nodeRef = &nodeRef->asInner()->child(0);
        }
        // Leaf node
        ASSERT(nodeRef->size());
        path.append({ nodeRef, 0 });
        ASSERT(path.size() == m_height + 1);
        return iterator(WTFMove(path));
    }

    iterator end() const
    {
        return iterator();
    }

private:
    bool isFirstOrLastIndex(NodeRef nodeRef, unsigned index)
    {
        ASSERT(index < nodeRef.size());
        return index == 0 || index == nodeRef.size() - 1;
    }

    void updateCoverage(const Path& path, int depth, Interval coverage)
    {
        ASSERT(depth >= 0);
        depth--; // So that depth is at the parent of the node with 'coverage'.
        while (depth >= 0) {
            const PathEntry& entry = path[depth];
            InnerNode* inner = entry.nodeRef->asInner();
            inner->interval(entry.index) = coverage;

            // FIXME: and/or should we filter based on actual coverage value since we may hit a common ancestor
            // when modifying multiple node, and one could be first and the other could be last.
            if (!isFirstOrLastIndex(*entry.nodeRef, entry.index)) {
                // Since first/last of this node was not modified, its coverage hasn't changed - no need to continue upward.
                verifyCoverageConsistency(path, depth, inner->coverage(entry.nodeRef->size()));
                return;
            }
            coverage = inner->coverage(entry.nodeRef->size());
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
            InnerNode* inner = entry.nodeRef->asInner();
            ASSERT(inner->interval(entry.index) == coverage);
            coverage = inner->coverage(entry.nodeRef->size());
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
    std::pair<NodeRef, Interval> insertInNodeSplitIfNeeded(const Path& path, int depth, const Interval& interval, const typename NodeType::PayloadType& value)
    {
        NodeRef* nodeRef = path[depth].nodeRef;
        auto insertionIndex = path[depth].index;
        auto node = nodeRef->template as<NodeType>();
        size_t nodeSize = nodeRef->size();
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
            nodeRef->setSize(nodeSize);
            updateCoverage(path, depth, node->coverage(nodeSize));
            return { NodeRef(newNode, newNodeSize), newNode->coverage(newNodeSize) };
        }

        node->insertAt(nodeSize, insertionIndex, interval, value);
        nodeRef->setSize(nodeSize);
        if (isFirstOrLastIndex(*nodeRef, insertionIndex))
            updateCoverage(path, depth, node->coverage(nodeSize));
        return { NodeRef(), Interval() };
    }

    template<typename NodeType>
    bool eraseFromNode(const Path& path, int depth)
    {
        NodeRef* nodeRef = path[depth].nodeRef;
        auto eraseIndex = path[depth].index;
        auto node = nodeRef->template as<NodeType>();
        size_t nodeSize = nodeRef->size();
        ASSERT(nodeSize <= NodeType::capacity);

        if (nodeSize == 1) [[unlikely]] {
            ASSERT(!eraseIndex);
            freeNode(node);
            *nodeRef = NodeRef();
            return true;
        }
        node->removeAt(nodeSize, eraseIndex);
        if (isFirstOrLastIndex(*nodeRef, eraseIndex))
            updateCoverage(path, depth, node->coverage(nodeSize));
        nodeRef->setSize(nodeSize);
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
        ASSERT(++assertOnlyNumNodes);
        return static_cast<NodeType*>(fastAlignedMalloc(cpuCacheLineSize, sizeof(NodeType)));
    }

    template<typename NodeType>
    void freeNode(NodeType* node)
    {
        ASSERT(assertOnlyNumNodes--);
        fastAlignedFree(node);
    }

    void freeAllNodes()
    {
        if (!m_root)
            return;
    
        Vector<std::pair<NodeRef, unsigned>, 16> stack;
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
    void dumpSubtree(PrintStream& out, NodeRef nodeRef, unsigned distanceToLeaf, unsigned indent) const
    {
        auto printIndent = [&] {
            for (unsigned i = 0; i < indent; ++i)
                out.print("  ");
        };

        if (distanceToLeaf) {
            InnerNode* inner = nodeRef.asInner();
            printIndent();
            out.println("Inner(size=", nodeRef.size(), ", coverage=", inner->coverage(nodeRef.size()), "):");
            
            for (size_t i = 0; i < nodeRef.size(); ++i) {
                printIndent();
                out.println("  [", i, "] ", inner->interval(i));
                dumpSubtree(out, inner->child(i), distanceToLeaf - 1, indent + 2);
            }
        } else {
            CommaPrinter comma;
            LeafNode* leaf = nodeRef.asLeaf();
            printIndent();
            out.print("Leaf(size=", nodeRef.size(), "): ");
            for (size_t i = 0; i < nodeRef.size(); ++i)
                out.print(comma, leaf->interval(i), "=", leaf->value(i));
            out.println();
        }
    }

    NodeRef m_root { };
    Interval m_rootInterval { T{}, T{} };
    unsigned m_height { 0 };

#ifdef USE_SLAB
    // Slab allocator state
    Vector<char*, 8> m_slabs;
    size_t m_slabOffset { 0 };
#else
#if ASSERT_ENABLED
    unsigned assertOnlyNumNodes { 0 };
#endif
#endif
};

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

using WTF::IntervalSet;
