#pragma once

// Run a callable on ANOTHER thread through a C-style "call this function with
// this pointer and wait for it" primitive, and give its result to the caller.
// In the product the primitive is juce::MessageManager::callFunctionOnMessageThread
// (JuceHost::callSync in dotcharter_audio.cpp); the RT gate hands it a plain
// worker thread.
//
// THE CALLABLE'S EXCEPTION BELONGS TO THE CALLER. The bodies run this way are
// hosted-plugin calls (prepareToPlay, getStateInformation, a VST3 load), and a
// third-party plugin can throw out of any of them. The trampoline catches
// everything, carries it back as an exception_ptr and rethrows it on the calling
// thread once `post` returns, where the export's guard() turns it into a status.
// Nothing unwinds out of the trampoline into the other thread's dispatch loop.

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace dcsync {

/// The primitive's function shape: juce::MessageManager::MessageCallbackFunction.
using Trampoline = void* (*)(void*);

/// Runs `f` through `post(trampoline, user)`, which must call
/// `trampoline(user)` exactly once on its thread before returning. Returns what
/// `f` returned (a default-constructed result when `post` never ran it, which is
/// what the message thread's "could not post" case has always produced) and
/// rethrows on the calling thread whatever `f` threw.
template <class F, class Post>
auto callThroughTrampoline(F&& f, Post&& post) -> std::invoke_result_t<F>
{
    using R  = std::invoke_result_t<F>;
    using Fn = std::remove_reference_t<F>;
    struct Call
    {
        Fn*                                                fn = nullptr;
        std::conditional_t<std::is_void_v<R>, char, R>     result {};
        std::exception_ptr                                 failure;
    };

    Call call;
    call.fn = std::addressof(f);
    Trampoline trampoline = [](void* user) -> void*
    {
        auto* c = static_cast<Call*>(user);
        try
        {
            if constexpr (std::is_void_v<R>) (*c->fn)();
            else                             c->result = (*c->fn)();
        }
        catch (...)
        {
            c->failure = std::current_exception();
        }
        return nullptr;
    };
    post(trampoline, static_cast<void*>(&call));

    if (call.failure) std::rethrow_exception(call.failure);
    if constexpr (! std::is_void_v<R>) return std::move(call.result);
}

} // namespace dcsync
