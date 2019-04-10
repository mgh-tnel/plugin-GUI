/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2018 Translational NeuroEngineering Laboratory

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef ATOMIC_SYNCHRONIZER_H_INCLUDED
#define ATOMIC_SYNCHRONIZER_H_INCLUDED

#include <atomic>
#include <vector>
#include <utility>
#include <cassert>
#include <cstdlib>

/*
 * AtomicSynchronizer: Facilitates the exchange of a resource between two threads
 * safely without using locks. One thread updates the resource and pushes updates
 * using an AtomicWriter; the other only reads the resource, and receives updates
 * when available using the corresponding AtomicReader. Directions for use:
 *
 * - The owner of an AtomicSynchronizer manages the actual objects being exchanged
 *   itself. It should allocate 3 instances of the resource type, probably in an
 *   array. The AtomicReader and AtomicWriter simply instruct their users on which
 *   instance is safe to access at any given time.
 *
 * - After creating an AtomicSynchronizer, one thread should obtain a pointer to its
 *   Reader by calling getReader, and the other can obtain a pointer to the Writer by
 *   getWriter. Each of the Reader and Writer should only be used in ONE thread and are
 *   not themselves thread-safe (but the simultaneous operation of one Reader and one
 *   Writer is safe).
 */

class AtomicSynchronizer {

public:
    class ScopedWriteIndex
    {
    public:
        explicit ScopedWriteIndex(AtomicSynchronizer& o)
            : owner(&o)
            , valid(o.checkoutWriter())
        {
            if (valid)
            {
                owner->updateWriterIndex();
            }
            else
            {
                // just to be sure - if not valid, shouldn't be able to access the synchronizer
                owner = nullptr;
            }
        }

        ScopedWriteIndex(const ScopedWriteIndex&) = delete;
        ScopedWriteIndex& operator=(const ScopedWriteIndex&) = delete;

        // push a write to the reader, then release write privileges
        ~ScopedWriteIndex()
        {
            if (valid)
            {
                owner->pushWrite();
                owner->returnWriter();
            }
        }

        // push a write to the reader without releasing writer privileges
        void pushUpdate()
        {
            if (valid)
            {
                owner->pushWrite();
            }
        }

        operator int() const
        {
            if (valid)
            {
                return owner->writerIndex;
            }
            return -1;
        }

        bool isValid() const
        {
            return valid;
        }

    private:
        AtomicSynchronizer* owner;
        const bool valid;
    };


    class ScopedReadIndex
    {
    public:
        explicit ScopedReadIndex(AtomicSynchronizer& o)
            : owner(&o)
            , valid(o.checkoutReader())
        {
            if (valid)
            {
                owner->updateReaderIndex();
            }
            else
            {
                // just to be sure - if not valid, shouldn't be able to access the synchronizer
                owner = nullptr;
            }
        }

        ScopedReadIndex(const ScopedReadIndex&) = delete;
        ScopedReadIndex& operator=(const ScopedReadIndex&) = delete;

        ~ScopedReadIndex()
        {
            if (valid)
            {
                owner->returnReader();
            }
        }

        // update the index, if a new version is available
        void pullUpdate()
        {
            if (valid)
            {
                owner->updateReaderIndex();
            }
        }

        operator int() const
        {
            if (valid)
            {
                return owner->readerIndex;
            }            
            return -1;
        }

        bool isValid() const
        {
            return valid;
        }

    private:
        AtomicSynchronizer* owner;
        const bool valid;
    };


    // Registers as both a reader and a writer, so no other reader or writer
    // can exist while it's held. Use to access all the underlying data without
    // conern for who has access to what, e.g. for updating settings, resizing, etc.
    class ScopedLockout
    {
    public:
        explicit ScopedLockout(AtomicSynchronizer& o)
            : owner         (&o)
            , haveReadLock  (o.checkoutReader())
            , haveWriteLock (o.checkoutWriter())
            , valid         (haveReadLock && haveWriteLock)
        {}

        ~ScopedLockout()
        {
            if (haveReadLock)
            {
                owner->returnReader();
            }

            if (haveWriteLock)
            {
                owner->returnWriter();
            }
        }

        bool isValid() const
        {
            return valid;
        }

    private:
        AtomicSynchronizer* owner;
        const bool haveReadLock;
        const bool haveWriteLock;
        const bool valid;
    };


    AtomicSynchronizer()
	: nReaders(0)
	, nWriters(0)
    {
        reset();
    }

    AtomicSynchronizer(const AtomicSynchronizer&) = delete;
    AtomicSynchronizer& operator=(const AtomicSynchronizer&) = delete;

    // Reset to state with no valid object
    // No readers or writers should be active when this is called!
    // If it does fail due to existing readers or writers, returns false
    bool reset()
    {
        ScopedLockout lock(*this);
        if (!lock.isValid())
        {
            return false;
        }

        readyToReadIndex = -1;
        readyToWriteIndex = 0;
        readyToWriteIndex2 = 1;
        writerIndex = 2;
        readerIndex = -1;

        return true;
    }

private:

    // Registers a writer and updates the writer index. If a writer already exists,
    // returns false, else returns true. returnWriter should be called to release.
    bool checkoutWriter()
    {
        // ensure there is not already a writer
        int currWriters = 0;
        if (!nWriters.compare_exchange_strong(currWriters, 1, std::memory_order_relaxed))
        {
            return false;
        }

        return true;
    }

    void returnWriter()
    {
        nWriters = 0;
    }

    // Registers a reader and updates the reader index. If a reader already exists,
    // returns false, else returns true. returnReader should be called to release.
    bool checkoutReader()
    {
        // ensure there is not already a reader
        int currReaders = 0;
        if (!nReaders.compare_exchange_strong(currReaders, 1, std::memory_order_relaxed))
        {
            return false;
        }

        return true;
    }

    void returnReader()
    {
        nReaders = 0;
    }

    // should only be called by a writer
    void updateWriterIndex()
    {
        if (writerIndex == -1)
        {
            // attempt to pull an index from readyToWriteIndex
            writerIndex = readyToWriteIndex.exchange(-1, std::memory_order_relaxed);

            if (writerIndex == -1)
            {
                writerIndex = readyToWriteIndex2.exchange(-1, std::memory_order_relaxed);

                // There are only 5 slots, so writerIndex, readyToWriteIndex, and
                // readyToWriteIndex2 cannot all be empty. There can't be a race condition
                // where one of these slots is now nonempty, because only the writer can
                // set any of them to -1 (and there's only one writer).
                assert(writerIndex != -1);
            }
        }
    }

    // should only be called by a writer
    void pushWrite()
    {
        if (writerIndex == -1)
        {
            // Shouldn't happen - it's an invariant that writerIndex != -1
            // except within this method before updateWriterIndex is called
            // and this method is not reentrant.
            assert(false);
        }
        else
        {
            writerIndex = readyToReadIndex.exchange(writerIndex, std::memory_order_relaxed);
        }
        updateWriterIndex();
    }

    // should only be called by a reader
    void updateReaderIndex()
    {
        // Check readyToReadIndex for newly pushed update
        // It can still be updated after checking, but it cannot be emptied because the 
        // writer cannot push -1 to readyToReadIndex.
        if (readyToReadIndex != -1)
        {
            if (readerIndex != -1)
            {
                // Great, there's a new update, first have to put current
                // readerIndex somewhere though.

                // Attempt to put index into readyToWriteIndex
                int expected = -1;
                if (!readyToWriteIndex.compare_exchange_strong(expected, readerIndex,
                    std::memory_order_relaxed))
                {
                    // readyToWriteIndex is already occupied
                    // readyToWriteIndex2 must be free at this point. newIndex, readerIndex, and
                    // readyToWriteIndex all contain something.
                    readyToWriteIndex2.exchange(readerIndex, std::memory_order_relaxed);
                }
            }
            readerIndex = readyToReadIndex.exchange(-1, std::memory_order_relaxed);
        }
    }

    // shared indices
    std::atomic<int> readyToReadIndex;  // assigned by the writer; can be read by the reader
    std::atomic<int> readyToWriteIndex; // assigned by the reader; can by modified by the writer
    std::atomic<int> readyToWriteIndex2; // another slot similar to readyToWriteIndex

    int writerIndex; // index the writer may currently be writing to
    int readerIndex; // index the reader may currently be reading from

    std::atomic<int> nWriters;
    std::atomic<int> nReaders;
};


// class to actually hold data controlled by an AtomicSynchronizer
template<typename T>
class AtomicallyShared
{
public:
    template<typename... Args>
    AtomicallyShared(Args&&... args)
    {
        for (int i = 0; i < 3; ++i)
        {
            data.emplace_back(std::forward<Args>(args)...);
        }
    }

    bool reset()
    {
        return sync.reset();
    }

    // Call a function on each underlying data member.
    // Requires that no readers or writers exist. Returns false if
    // this condition is unmet, true otherwise.
    template<typename UnaryFunction>
    bool apply(UnaryFunction f)
    {
        AtomicSynchronizer::ScopedLockout lock(sync);
        if (!lock.isValid())
        {
            return false;
        }

        for (T& obj : data)
        {
            f(obj);
        }

        return true;
    }

    class ScopedWritePtr
    {
    public:
        ScopedWritePtr(AtomicallyShared<T>& o)
            : owner (&o)
            , ind   (o.sync)
            , valid (ind.isValid())
        {}

        void pushUpdate()
        {
            ind.pushUpdate();
        }

        // provide access to data

        T& operator*()
        {
            if (!valid)
            {
                // abort! abort!
                assert(false);
                std::abort();
            }
            return owner->data[ind];
        }

        T* operator->()
        {
            return &(operator*());
        }

        bool isValid() const
        {
            return valid;
        }

    private:
        AtomicallyShared<T>* owner;
        AtomicSynchronizer::ScopedWriteIndex ind;
        const bool valid;
    };

    class ScopedReadPtr
    {
    public:
        ScopedReadPtr(AtomicallyShared<T>& o)
            : owner(&o)
            , ind(o.sync)
            // if the ind is valid, but is equal to -1, this pointer is still invalid (for now)
            , valid(ind != -1)
        {}

        void pullUpdate()
        {
            ind.pullUpdate();
            // in case ind is valid but was equal to -1:
            valid = ind != -1;
        }

        // provide access to data

        const T& operator*()
        {
            if (!valid)
            {
                // abort! abort!
                assert(false);
                std::abort();
            }
            return owner->data[ind];
        }

        const T* operator->()
        {
            return &(operator*());
        }

        bool isValid() const
        {
            return valid;
        }

    private:
        AtomicallyShared<T>* owner;
        AtomicSynchronizer::ScopedReadIndex ind;
        bool valid;
    };

private:
    std::vector<T> data;
    AtomicSynchronizer sync;
};

template<typename T>
using AtomicScopedWritePtr = typename AtomicallyShared<T>::ScopedWritePtr;

template<typename T>
using AtomicScopedReadPtr = typename AtomicallyShared<T>::ScopedReadPtr;

#endif // ATOMIC_SYNCHRONIZER_H_INCLUDED
