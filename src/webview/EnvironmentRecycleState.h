#pragma once
#include <Windows.h>
#include <cstdint>
#include <atomic>

namespace webview_recycle {

// Captured by callbacks rather than dereferencing a possibly destroyed host.
struct CallbackGate {
    std::atomic<bool> alive{true};
    std::atomic<bool> shuttingDown_{false};
    bool CanRun() const {
        return !shuttingDown_.load(std::memory_order_acquire) &&
            alive.load(std::memory_order_acquire);
    }
    bool Stop() {
        if (shuttingDown_.exchange(true, std::memory_order_acq_rel)) return false;
        alive.store(false, std::memory_order_release);
        return true;
    }
};

// All operations run under WebViewEnvironment::mutex_. Kept independent of the
// runtime so cancellation/reentrancy can be tested without starting a browser.
struct State {
    bool ready = false;
    bool creating = false;
    bool invalidated = false;
    bool shuttingDown_ = false;
    bool terminal = false;
    std::uint64_t generation = 0;

    HRESULT AccessStatus() const {
        return shuttingDown_ || terminal ? CO_E_OBJNOTCONNECTED : S_OK;
    }

    template <typename Pointer>
    HRESULT GetEnvironment(const Pointer& owned, Pointer& snapshot) const {
        snapshot.reset();
        const HRESULT hr = AccessStatus();
        if (FAILED(hr)) return hr;
        if (!ready || !owned) return E_PENDING;
        snapshot = owned; // retain while the caller is outside the lock
        return S_OK;
    }

    bool AcceptCallback(std::uint64_t ticket) const {
        return SUCCEEDED(AccessStatus()) && !invalidated && ticket == generation;
    }

    bool Invalidate() {
        if (terminal || shuttingDown_ || invalidated || (!ready && !creating)) return false;
        invalidated = true;
        ready = creating = false;
        ++generation;
        return true;
    }

    bool BeginRelease(bool shutdown) {
        if (shutdown) terminal = true; // nested Shutdown wins over a recycle
        if (shuttingDown_) return false;
        shuttingDown_ = true;
        ready = creating = false;
        ++generation;
        return true;
    }

    void EndRelease() {
        invalidated = false;
        shuttingDown_ = terminal;
    }
};

// The same sequence is used by shutdown and invalidate/rebuild. Hooks allow
// deterministic tests of order and nested shutdown without COM or rendering.
template <typename Close, typename Wait, typename Release, typename Drain>
void ReleaseSequence(Close close, Wait wait, Release release, Drain drain) {
    close();
    wait();
    release();
    drain();
}
}
