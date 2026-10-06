// SPDX-License-Identifier: Apache-2.0
// 校时替身。/ Clock-sync fake.
#pragma once
enum class ClockSyncState{Failed,Complete};
struct TestClock{bool requestSync(){return true;}ClockSyncState syncState(){return ClockSyncState::Complete;}};
inline TestClock halClock;
