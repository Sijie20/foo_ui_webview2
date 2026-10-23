#include "pch.h"
#include "webview/EnvironmentRecycleState.h"
#include <memory>

using webview_recycle::State;

TEST(EnvironmentRecycle, HostCallbacksAreNoOpDuringShutdownAndAfterHostDeletion) {
    auto gate = std::make_shared<webview_recycle::CallbackGate>();
    auto host = std::make_unique<int>(0);
    auto callback = [gate, ptr = host.get()] {
        if (!gate->CanRun()) return;
        ++*ptr;
    };
    callback();
    EXPECT_EQ(1, *host);
    gate->shuttingDown_.store(true);
    callback();
    EXPECT_EQ(1, *host);
    host.reset();
    callback(); // captured pointer is dangling; the gate prevents dereference
    EXPECT_FALSE(gate->CanRun());
}

TEST(EnvironmentRecycle, HostCloseIsIdempotent) {
    webview_recycle::CallbackGate gate;
    ASSERT_TRUE(gate.CanRun());
    EXPECT_TRUE(gate.Stop());
    EXPECT_FALSE(gate.Stop());
    EXPECT_FALSE(gate.CanRun());
}

TEST(EnvironmentRecycle, ShutdownMakesLateCreationCallbackNoOp) {
    State state;
    state.creating = true;
    const auto ticket = state.generation;
    ASSERT_TRUE(state.AcceptCallback(ticket));
    ASSERT_TRUE(state.BeginRelease(true));
    int invoked = 0;
    if (state.AcceptCallback(ticket)) ++invoked;
    state.EndRelease();
    if (state.AcceptCallback(ticket)) ++invoked;
    EXPECT_EQ(0, invoked);
    EXPECT_FALSE(state.ready);
    EXPECT_FALSE(state.creating);
}

TEST(EnvironmentRecycle, GetEnvironmentAfterReleaseReturnsDisconnectedAndClearsOutput) {
    State state;
    state.ready = true;
    auto owner = std::make_shared<int>(42);
    std::shared_ptr<int> result;
    ASSERT_EQ(S_OK, state.GetEnvironment(owner, result));
    ASSERT_TRUE(state.BeginRelease(true));
    owner.reset();
    state.EndRelease();
    EXPECT_EQ(CO_E_OBJNOTCONNECTED, state.GetEnvironment(owner, result));
    EXPECT_FALSE(result);
}

TEST(EnvironmentRecycle, GetEnvironmentDuringRecycleReturnsDisconnected) {
    State state;
    auto owner = std::make_shared<int>(42);
    auto result = owner;
    ASSERT_TRUE(state.BeginRelease(false));
    EXPECT_EQ(CO_E_OBJNOTCONNECTED, state.GetEnvironment(owner, result));
    EXPECT_FALSE(result);
}

TEST(EnvironmentRecycle, ReadySnapshotRetainsEnvironmentOutsideLock) {
    State state;
    state.ready = true;
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    std::shared_ptr<int> snapshot;
    ASSERT_EQ(S_OK, state.GetEnvironment(owner, snapshot));
    ASSERT_TRUE(state.BeginRelease(true));
    owner.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(42, *snapshot);
    snapshot.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(EnvironmentRecycle, RepeatedShutdownAndInvalidateAreSafe) {
    State state;
    state.ready = true;
    ASSERT_TRUE(state.Invalidate());
    const auto ticket = state.generation;
    EXPECT_FALSE(state.Invalidate());
    EXPECT_EQ(ticket, state.generation);
    ASSERT_TRUE(state.BeginRelease(true));
    EXPECT_FALSE(state.BeginRelease(true));
    EXPECT_FALSE(state.Invalidate());
    state.EndRelease();
    EXPECT_FALSE(state.BeginRelease(true));
    EXPECT_FALSE(state.Invalidate());
    EXPECT_EQ(CO_E_OBJNOTCONNECTED, state.AccessStatus());
}

TEST(EnvironmentRecycle, InvalidateCancelsInFlightEnvironmentCreation) {
    State state;
    state.creating = true;
    const auto oldTicket = state.generation;
    ASSERT_TRUE(state.Invalidate());
    EXPECT_FALSE(state.AcceptCallback(oldTicket));
    EXPECT_FALSE(state.creating);
    ASSERT_TRUE(state.BeginRelease(false));
    state.EndRelease();
    EXPECT_FALSE(state.AcceptCallback(oldTicket));
    EXPECT_TRUE(state.AcceptCallback(state.generation));
}

TEST(EnvironmentRecycle, FiveInvalidationsAllowRebuildWithoutAcceptingOldCallbacks) {
    State state;
    for (int round = 0; round < 5; ++round) {
        state.ready = true;
        const auto oldTicket = state.generation;
        ASSERT_TRUE(state.Invalidate());
        ASSERT_TRUE(state.BeginRelease(false));
        EXPECT_FALSE(state.AcceptCallback(oldTicket));
        state.EndRelease();
        EXPECT_EQ(S_OK, state.AccessStatus());
        EXPECT_FALSE(state.AcceptCallback(oldTicket));
        EXPECT_TRUE(state.AcceptCallback(state.generation));
        std::shared_ptr<int> empty, snapshot;
        EXPECT_EQ(E_PENDING, state.GetEnvironment(empty, snapshot));
    }
}

TEST(EnvironmentRecycle, NestedShutdownDuringRecyclePreventsRebuild) {
    State state;
    ASSERT_TRUE(state.BeginRelease(false));
    EXPECT_FALSE(state.BeginRelease(true));
    state.EndRelease();
    EXPECT_EQ(CO_E_OBJNOTCONNECTED, state.AccessStatus());
    EXPECT_FALSE(state.AcceptCallback(state.generation));
}

TEST(EnvironmentRecycle, ReleaseOrderAndReentrantGetEnvironment) {
    State state;
    state.ready = true;
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    ASSERT_TRUE(state.BeginRelease(false));
    std::vector<std::string> events;
    webview_recycle::ReleaseSequence(
        [&] { events.push_back("close"); },
        [&] {
            events.push_back("wait-exit-or-timeout");
            std::shared_ptr<int> result;
            EXPECT_EQ(CO_E_OBJNOTCONNECTED, state.GetEnvironment(owner, result));
            EXPECT_FALSE(weak.expired());
        },
        [&] { events.push_back("release"); owner.reset(); },
        [&] { events.push_back("pump-after-release"); EXPECT_TRUE(weak.expired()); });
    state.EndRelease();
    EXPECT_EQ((std::vector<std::string>{"close", "wait-exit-or-timeout", "release", "pump-after-release"}), events);
}

TEST(EnvironmentRecycle, ReentrantShutdownStopsRemainingCallbackBatch) {
    State state;
    const auto ticket = state.generation;
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        if (!state.AcceptCallback(ticket)) break;
        ++count;
        state.BeginRelease(true);
        state.EndRelease();
    }
    EXPECT_EQ(1, count);
}
