#pragma once
#ifndef VELOX_RHI_CORO_TASK_HPP
#define VELOX_RHI_CORO_TASK_HPP
#include <coroutine>
#include <exception>
#include <expected>
#include <semaphore>
#include <stop_token>
#include <type_traits>

// used to make sure continuations are not inlined, which can cause errors
// with LTO according to the findings of the team using Task<> objects
// in facebook's folly
#ifdef _MSC_VER
#define FINAL_AWAITER_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define FINAL_AWAITER_NOINLINE [[gnu::noinline]]
#else
#error "Unsupported compiler"
#endif

// used to need an unreachable def, now c++23 we have std::unreachable()!

namespace velox
{
template<typename T>
class Task
{
public:
    struct promise_type
    {
        T result_value;
        Task<T> get_return_object() noexcept
        {
            return Task<T>{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        // execute eagerly until first await, don't suspend at the start of the coroutine
        std::suspend_never initial_suspend() noexcept
        {
            return {};
        }

        // suspend at the end so we can get result before destroying the coroutine
        std::suspend_always final_suspend() noexcept
        {
#ifndef __EMSCRIPTEN__
            // on native builds, we use a binary semaphore signal to unblock callers waiting on
            // Task<T>::Wait()
            resultSemaphore.release();
#endif
            return {};
        }

        template<typename U>
        void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        {
            result_value = std::forward<U>(value);
        }

        void unhandled_exception() noexcept
        {
            // need to shore this up bc of webassembly quirks eventually
            std::terminate();
        }

#ifndef __EMSCRIPTEN__
        std::binary_semaphore resultSemaphore{ 0 };
#endif
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) noexcept
        : handle(h)
    {
    }
    
    Task(Task&) = delete;
    Task& operator=(Task&) = delete;

    Task(Task&& other) noexcept
        : handle(other.handle)
    {
        other.handle = nullptr;
    }

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle)
            {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    ~Task() noexcept
    {
        if (handle)
        {
            handle.destroy();
        }
    }

    void Wait() noexcept
    {
#ifndef __EMSCRIPTEN__
        handle.promise().resultSemaphore.acquire();
#endif
    }

    T GetResult() noexcept
    {
        return handle.promise().result_value;
    }
};
} // namespace velox

#endif //! VELOX_RHI_CORO_TASK_HPP
