#pragma once
#include <array>
#include <utility>

namespace display_recovery {
inline bool ShouldDeferNotification(bool fullscreen, bool updating, bool dpiNotification) {
    return fullscreen && updating && !dpiNotification;
}

// Borderless window recovery only. Never change display mode, refresh rate or HDR.
template<class Exit, class Clamp>
bool RecoverDisplayChange(bool fullscreen, bool updating, Exit&& exit, Clamp&& clamp) {
    if (!fullscreen || updating) return false;
    std::forward<Exit>(exit)();
    std::forward<Clamp>(clamp)();
    return true;
}

// F12 can be reserved by Windows/debuggers. Try deterministic alternatives.
inline constexpr std::array<unsigned, 3> kEmergencyKeys{0x7B, 0x7A, 0x79}; // F12/F11/F10
template<class Register, class Report>
unsigned RegisterEmergencyKey(Register&& attempt, Report&& report) {
    for (unsigned key : kEmergencyKeys) {
        const bool success = attempt(key);
        report(key, success);
        if (success) return key;
    }
    return 0;
}

class UpdatingGuard {
public:
    explicit UpdatingGuard(bool& flag) : flag_(flag), previous_(flag) { flag_ = true; }
    ~UpdatingGuard() { flag_ = previous_; }
private:
    bool& flag_;
    bool previous_;
};
}
