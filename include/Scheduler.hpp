#pragma once
#ifndef VELOX_CONTEXT_IMPL_HPP
#define VELOX_CONTEXT_IMPL_CPP
#include "Future.hpp"
#include "VeloxErrors.hpp"

/**
 * This file is mostly to serve as a "bus" for tying together our coroutines in AsyncTasks,
 * the SlotMap instances in Context, and all without creating a really gnarly and hard to
 * follow sourcetree of dependencies and control flow. It's still not great, but I'm at a loss
 * currently for how to do better
 */
namespace velox
{

// Slot is what we store in our actual slotmap object, created when webgpu fires the callback
// from the event processing loop. This is typed for each result type we have, and stores a result
// and a continuation handle that we selectively resume ourselves. We need to be able to resume
// ourselves to make sure we're in a state ready to resume and work, not just being bombarded with
// surprise coroutine resumptions
template<typename T>
struct Slot
{
    struct promise_type
    {
        T result_value;
        bool ready{ false };
        // map we will publish back into
        SlotMap<T, 512>* map;

        Slot<T> get_return_object() noexcept
        {
            return Slot<T>{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        // don't suspend initially, because we call the async function and let the awaitable
        // that constructs be what first sends us into suspension (after enqueuing our action)
        constexpr std::suspend_never initial_suspend() noexcept
        {
            // only initial step we perform: set result to nullopt, so try-get fails
            // until we actually fill it with a value on op complete
            result_value = std::nullopt;
            return {};
        }

        template<typename Awaitable>
        Awaitable&& await_transform(Awaitable&& await_inst) noexcept
        {
            static_assert("Didn't define appropriate await_transform for current type.");
        }

        // always suspend at the end so we can extract the result before destruction
        constexpr std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        template<typename U>
        void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            result_value = std::forward<U>(value);
        }

        void unhandled_exception()
        {
#ifndef __EMSCRIPTEN__
            std::terminate();
#else
            emscripten_force_exit(1);
#endif
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Slot(std::coroutine_handle<promise_type> _handle) noexcept
        : handle{ _handle }
    {
    }

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    Slot(Slot&& other) noexcept = default;
    Slot& operator=(Slot&& other) = default;

    ~Slot() = default;

    std::optional<T> TryGet()
    {
        if (!handle.done())
        {
            return std::nullopt;
        }
        else
        {
            return handle.promise().result_value;
        }
    }
};

using RenderPipelineSlot = Slot<Result<wgpu::RenderPipeline>>;
using ComputePipelineSlot = Slot<Result<wgpu::ComputePipeline>>;
using MapReadSlot = Slot<Result<const void*>>;
using MapWriteSlot = Slot<Result<void*>>;

struct Scheduler
{
    /* We cast to void* when passing into here because it can spare us an include (lol) */
    [[nodiscard]] SlotMapHandle RegisterPending(void* deferred_coroutine) noexcept;
    /* Return an error on MarkReady so failures can be propagated up and out, leaving this noexcept (and
     * giving us robustness) */
    [[nodiscard]] RhiError MarkReady(SlotMapHandle handle) noexcept;

private:
};

} // namespace velox

#endif // !VELOX_CONTEXT_IMPL_CPP
