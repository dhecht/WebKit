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
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 { namespace Air {

class IntervalSet {
public:
    class iterator;

    IntervalSet()
    {
    }

    // XXX: use end as key, start, tmp as value?
    void insert(Point start, Point end, Tmp tmp);

    // XXX: should it only take end?
    void erase(Point start, Point end, Tmp tmp);

    iterator findFirstEndingAfter(Point point);

    class iterator {

    };

private:
    template<typename Value, size_t N>
    class NodeBase {
    
    private:
        Point ends[N];
        Value values[N];
    };

    template<typename V, size_t N>
    class NodePtr {

        using Node = NodeBase<V, N>;

        static constexpr uintptr_t size_mask = 0xf;

        NodePtr(NodeBase<V, N>* ptr, size_t size) :
            m_bits(static_cast<uintptr_t>(ptr) | size)
        {
            ASSERT(!(static_cast<uintptr_t>(ptr) & size_mask));
            ASSERT(size < size_mask);
        }

        Node* get()
        {
            return reinterpret_cast<Node*>(m_bits & ~size_mask);
        }
    
        size_t size()
        {
            return m_bits & size_mask;
        }

    private:
        // Low 4 bits contains the size, remaining bits contains the pointer.
        uintptr_t m_bits;
    };

    struct Value {
        Point start;
        Tmp tmp;
    };

    template<size_t N>
    class LeafNode : public NodeBase<Value, N> {
    };

    template <size_t N>
    class InnerNode : public NodeBase<NodePtr<>, N> {
        using Base = NodeBase<N>;
    
    private:
        Base* children[N];
    };

    template<size_t N>
    void insertIntoLeaf(LeafNode<N>& leaf, size_t size, Point start, Point end, Tmp tmp)
    {
        if (size == N) {
            // XXX overflow
            RELEASE_ASSERT_NOT_REACHED();
        }
        unsigned i = 0;
        for (; i < size; i++) {
            if (end < leaf.ends[i])
                break;
        }
        leaf.ends[i] = end;
        leaf.starts[i] = start;
        leaf.tmps[i] = tmp;
    }
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

