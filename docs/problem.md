# Quest 3 Friend Invite Panel Problem Analysis

## Problem Description

An external developer reports that their **Friend Invite Panel opens and then immediately closes**. The user cannot select friends or send invites.

## Investigation Summary

Based on analyzing the developer's code screenshots and logs, we identified the following:

### What Works ✅
- `GroupPresence_Set()` succeeds
- `GroupPresence_SetDeeplinkMessageOverride()` (failsafe) succeeds
- Photon room creation succeeds
- All presence options appear correct

### The Suspicious Log
```
[2026.02.17-23.15.22:759] Successfully set group presence
[2026.02.17-23.15.22:759] TriggerDeeplinkMessageFailsafe: Trying to override the deeplink message...
[2026.02.17-23.15.22:786] TriggerDeeplinkMessageFailsafe: Successfully set deeplink message override!
[2026.02.17-23.15.23:454] Case APP CMD LOST FOCUS  ← 668ms later
```

### Key Insight

**`APP CMD LOST FOCUS` is EXPECTED when the invite panel opens!**

The invite panel is a system dialog that takes focus from the app. The question is: **What happens in their code when focus is lost?**

## Developer's Code Flow Analysis

Based on the screenshots:

```
1. OpenFriendsPanel() called
2. RequestHostNewRoom() → CreateOrJoinCustomRoom(DestinationApiName, RandomSessionId)
3. OnCreatePhotonRoomReturned() callback:
   ├── AddRegionToDeeplinkMessage() → HostDeeplinkMessageOverride
   ├── Create FOvrGroupPresenceOptions with all fields
   ├── GroupPresence_Set() with callback:
   │   └── Callback (success OR failure):
   │       └── TriggerDeeplinkMessageFailsafe()  ← Redundant second presence call!
   │           └── GroupPresence_SetDeeplinkMessageOverride() with callback:
   │               └── Callback (success OR failure):
   │                   └── OnPhotonRoomCreatedDelegate.Broadcast()  ← Always fires!
4. OnPhotonRoomCreatedDelegate triggers:
   └── OpenFriendsPanel() → LaunchInvitePanel()
```

### Problems Identified

1. **Double Presence Call**: They call `GroupPresence_Set()` with deeplink message in options, THEN call `GroupPresence_SetDeeplinkMessageOverride()` as a "failsafe". This is redundant and may cause issues.

2. **Broadcast Regardless of Success**: Both callbacks have TODO comments acknowledging the bug:
   - "TODO JD: This should only be called if we actually are successful!"
   - "TODO JD: This should probably only be called on success!!"

3. **No Focus Loss Handling**: There's no tracking of whether the invite panel is open. If their app has `OnApplicationPause` or focus handlers, those could interfere.

4. **Possible Focus Handler Interference**: The `MenuManager->SetPauseMode()` call in their code suggests they have pause handling that might react to focus loss.

## Hypothesis

The invite panel opens (causing `APP CMD LOST FOCUS`), but something in their app reacts to the focus loss by:
- Disconnecting from Photon
- Changing UI state
- Showing a pause menu
- Clearing presence
- Or some combination that causes the panel to close

## Test App Created

Created `/Users/abrady/github/Meta-Spatial-SDK-Samples/PresenceInviteSample` with:

- **PresenceManager.kt**: Wraps Group Presence APIs with proper state tracking
- **PhotonManager.kt**: Wraps Photon room operations
- **PresenceInviteSampleActivity.kt**: Main activity with:
  - `hostRoom()` - Correct flow
  - `reproduceDeveloperBuggyFlow()` - Reproduces their flow
  - `isInvitePanelOpen` tracking for focus handling
- **Panels.kt**: UI for testing

## Next Steps

1. **Ask the developer**:
   - Do you have `OnApplicationPause` or `OnApplicationFocus` handlers?
   - Does Photon disconnect when the app goes to background?
   - What does `SetPauseMode()` do?

2. **Have them test**:
   - Add logging to their focus/pause handlers
   - Add `bIsInvitePanelOpen` flag and skip focus handling while true

## Recommended Fix

```cpp
// In their class
bool bIsInvitePanelOpen = false;

// Before launching panel
void OpenFriendsPanel() {
    bIsInvitePanelOpen = true;

    auto InvitePanelDelegate = ...[this](bool bSuccess, FInvitePanelResultInfo* ResultInfo) {
        bIsInvitePanelOpen = false;
        // Handle result
    });

    LaunchInvitePanel(...);
}

// In focus/pause handlers
void OnApplicationPause(bool bPaused) {
    if (bIsInvitePanelOpen) {
        // Skip - this is expected when invite panel is open
        UE_LOG(LogPhoton, Display, TEXT("Ignoring pause - invite panel is open"));
        return;
    }
    // Normal pause handling
}
```

## Files Created

- `/Users/abrady/github/Meta-Spatial-SDK-Samples/PresenceInviteSample/` - Complete test app
- This document - Problem analysis

## References

- [Group Presence Overview](https://developer.oculus.com/documentation/unity/ps-group-presence-overview/)
- [Meta Platform SDK](https://developer.oculus.com/documentation/native/ps-platform-intro/)
- [Photon Realtime SDK](https://www.photonengine.com/sdks#realtime-android)
