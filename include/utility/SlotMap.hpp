#pragma once
#ifndef VELOX_UTILITY_SLOTMAP_HPP
#define VELOX_UTILITY_SLOTMAP_HPP
// SlotMap<T, N>
//
// A fixed-capacity, contiguous, allocation-free container backed by an intrusive free list. Sometimes called
// a "slot map" or "generational index array" -- insertion returns a SlotMapHandle (index + generation) that
// stays valid until that specific slot is erased, even as other slots are inserted and removed around it.
// Effectively the same storage family as most containers used in ECS frameworks, but without the dense-sparse
// split and less focus on iteration performance (there may be gaps in the storage, for example).
//
// Handles are generation-checked: erasing a slot bumps its generation, so a stale SlotMapHandle obtained
// before an Erase() will never alias a slot that has since been reused for something else. Occupancy is
// folded into the same counter (odd generation == occupied, even == free) rather than kept in a parallel bool
// array, so per-slot bookkeeping costs exactly one uint32_t.
//
// Iteration order follows physical slot order, not insertion order, and visits only occupied slots. This is a
// forward_iterator; do not insert or erase into the SlotMap while an iteration over it is in progress.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

// enforcing this contract to make sure only simple POD types are used in the slotmap.
// it would be fine if not, but this is the contract I want to enforce on myself just in case
template<typename T>
concept SlotmapElementType =
    std::is_trivially_destructible_v<T> && std::is_move_constructible_v<T> &&
    std::is_nothrow_move_constructible_v<T> && std::is_move_assignable_v<T> &&
    std::is_nothrow_move_assignable_v<T> && std::is_trivial_v<T> && std::is_standard_layout_v<T>;

template<SlotmapElementType T, std::size_t N>
class SlotMap;

/**This is defined independent of the SlotMap as it is *not* dependent on the template specialization of
 * the slotmap, which is an asset to us since we can return this from functions that are already doing
 * type erasure shenanigans with coroutine_handle<>
 */
class SlotMapHandle
{
public:
    using IndexType = std::uint32_t;
    using GenerationType = std::uint32_t;
    static constexpr IndexType kInvalidIndex = std::numeric_limits<IndexType>::max();
    static constexpr GenerationType kInvalidGeneration = std::numeric_limits<GenerationType>::max();

    constexpr SlotMapHandle() noexcept = default;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return index != kInvalidIndex;
    }

    [[nodiscard]] friend constexpr bool operator==(const SlotMapHandle& lhs,
                                                   const SlotMapHandle& rhs) noexcept = default;

private:
    template<SlotmapElementType T, std::size_t N>
    friend class SlotMap;

    constexpr SlotMapHandle(IndexType index_in, GenerationType generation_in) noexcept
        : index(index_in),
          generation(generation_in)
    {
    }

    std::uint32_t index{ kInvalidIndex };
    std::uint32_t generation{ 0 };
};

template<SlotmapElementType T, std::size_t N>
class SlotMap
{
public:
    static_assert(N > 0, "SlotMap capacity must be greater than zero");

    using ValueType = T;
    using HandleType = typename SlotMapHandle;
    using IndexType = typename SlotMapHandle::IndexType;
    using GenerationType = typename SlotMapHandle::GenerationType;

    constexpr SlotMap() noexcept
    {
        initFreeList();
    }

    ~SlotMap() noexcept
    {
        destroyAll();
    }

    SlotMap(const SlotMap&) = delete;
    SlotMap& operator=(const SlotMap&) = delete;

    SlotMap(SlotMap&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        moveFrom(other);
    }

    SlotMap& operator=(SlotMap&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (this != &other)
        {
            destroyAll();
            moveFrom(other);
        }

        return *this;
    }

    template<typename... Args>
    [[nodiscard]] SlotMapHandle Emplace(Args&&... args)
    {
        if (Full())
        {
            return SlotMapHandle();
        }

        const IndexType index = freeListHead;
        Slot& slot = slots[index];
        freeListHead = slot.nextFree;
        // placement new, since memory is already allocated
        ::new (static_cast<void*>(&slot.value)) T(std::forward<Args>(args)...);

        generations[index] += 1;
        ++size;

        return SlotMapHandle(index, generations[index]);
    }

    [[nodiscard]] SlotMapHandle Insert(const T& value)
    {
        return Emplace(value);
    }

    [[nodiscard]] SlotMapHandle Insert(T&& value)
    {
        return Emplace(std::move(value));
    }

    bool Erase(SlotMapHandle handle) noexcept(std::is_nothrow_destructible_v<T>)
    {
        if (!Contains(handle))
        {
            return false;
        }

        const IndexType index = handle.index;
        Slot& slot = slots[index];
        slot.value.~T();

        generations[index] += 1;

        slot.nextFree = freeListHead;
        freeListHead = index;

        --size;
        return true;
    }

    void Clear() noexcept(std::is_nothrow_destructible_v<T>)
    {
        destroyAll();
        initFreeList();
    }

    [[nodiscard]] bool Contains(SlotMapHandle handle) const noexcept
    {
        if (!handle.IsValid())
        {
            return false;
        }

        if (handle.index >= N)
        {
            return false;
        }

        return handle.generation == generations[handle.index];
    }

    [[nodiscard]] T* TryGet(SlotMapHandle handle) noexcept
    {
        if (!Contains(handle))
        {
            return nullptr;
        }

        return &slots[handle.index].value;
    }

    [[nodiscard]] const T* TryGet(SlotMapHandle handle) const noexcept
    {
        if (!Contains(handle))
        {
            return nullptr;
        }

        return &slots[handle.index].value;
    }

    [[nodiscard]] T& operator[](SlotMapHandle handle) noexcept
    {
        return slots[handle.index].value;
    }

    [[nodiscard]] const T& operator[](SlotMapHandle handle) const noexcept
    {
        return slots[handle.index].value;
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return size;
    }

    [[nodiscard]] static constexpr std::size_t Capacity() noexcept
    {
        return N;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return size == 0;
    }

    [[nodiscard]] bool Full() const noexcept
    {
        return size == N;
    }

    struct Entry
    {
        SlotMapHandle handle;
        T& value;
    };

    struct ConstEntry
    {
        SlotMapHandle handle;
        const T& value;
    };

    class Iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Entry;

        Iterator(SlotMap* owner_in, IndexType index_in) noexcept
            : owner(owner_in),
              index(index_in)
        {
            skipToOccupied();
        }

        [[nodiscard]] Entry operator*() const noexcept
        {
            return Entry{ owner->makeHandle(index), owner->slots[index].value };
        }

        Iterator& operator++() noexcept
        {
            ++index;
            skipToOccupied();
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator result = *this;
            ++(*this);
            return result;
        }

        [[nodiscard]] friend bool operator==(const Iterator& lhs, const Iterator& rhs) noexcept
        {
            return lhs.owner == rhs.owner && lhs.index == rhs.index;
        }

    private:
        void skipToOccupied() noexcept
        {
            while (index < N && !owner->isOccupied(index))
            {
                ++index;
            }
        }

        SlotMap* owner;
        IndexType index;
    };

    class ConstIterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ConstEntry;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = ConstEntry;

        ConstIterator(const SlotMap* owner_in, IndexType index_in) noexcept
            : owner(owner_in),
              index(index_in)
        {
            skipToOccupied();
        }

        [[nodiscard]] ConstEntry operator*() const noexcept
        {
            return ConstEntry{ owner->makeHandle(index), owner->slots[index].value };
        }

        ConstIterator& operator++() noexcept
        {
            ++index;
            skipToOccupied();
            return *this;
        }

        ConstIterator operator++(int) noexcept
        {
            ConstIterator result = *this;
            ++(*this);
            return result;
        }

        [[nodiscard]] friend bool operator==(const ConstIterator& lhs, const ConstIterator& rhs) noexcept
        {
            return lhs.owner == rhs.owner && lhs.index == rhs.index;
        }

    private:
        void skipToOccupied() noexcept
        {
            while (index < N && !owner->isOccupied(index))
            {
                ++index;
            }
        }

        const SlotMap* owner;
        IndexType index;
    };

    [[nodiscard]] Iterator begin() noexcept
    {
        return Iterator(this, 0);
    }

    [[nodiscard]] Iterator end() noexcept
    {
        return Iterator(this, static_cast<IndexType>(N));
    }

    [[nodiscard]] ConstIterator begin() const noexcept
    {
        return ConstIterator(this, 0);
    }

    [[nodiscard]] ConstIterator end() const noexcept
    {
        return ConstIterator(this, static_cast<IndexType>(N));
    }

    [[nodiscard]] ConstIterator cbegin() const noexcept
    {
        return begin();
    }

    [[nodiscard]] ConstIterator cend() const noexcept
    {
        return end();
    }

private:
    union Slot
    {
        Slot() noexcept
        {
        }

        ~Slot() noexcept
        {
        }

        T value;
        IndexType nextFree;
    };

    [[nodiscard]] bool isOccupied(IndexType index_in) const noexcept
    {
        return (generations[index_in] & 1u) != 0u;
    }

    [[nodiscard]] SlotMapHandle makeHandle(IndexType index_in) const noexcept
    {
        return SlotMapHandle(index_in, generations[index_in]);
    }

    void initFreeList() noexcept
    {
        for (IndexType i = 0; i < N; ++i)
        {
            slots[i].nextFree = i + 1;
            generations[i] = 0;
        }

        freeListHead = 0;
        size = 0;
    }

    void destroyAll() noexcept(std::is_nothrow_destructible_v<T>)
    {
        for (IndexType i = 0; i < N; ++i)
        {
            if (isOccupied(i))
            {
                slots[i].value.~T();
            }
        }
    }

    void moveFrom(SlotMap& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        for (IndexType i = 0; i < N; ++i)
        {
            generations[i] = other.generations[i];

            if (other.isOccupied(i))
            {
                ::new (static_cast<void*>(&slots[i].value)) T(std::move(other.slots[i].value));
                other.slots[i].value.~T();
            }
            else
            {
                slots[i].nextFree = other.slots[i].nextFree;
            }
        }

        freeListHead = other.freeListHead;
        size = other.size;

        other.initFreeList();
    }

    std::array<Slot, N> slots{};
    std::array<GenerationType, N> generations{};
    IndexType freeListHead{ 0 };
    std::size_t size{ 0 };
};

#endif // VELOX_UTILITY_SLOTMAP_HPP
