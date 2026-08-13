# UnrealSplit

An Unreal Engine plugin for low-latency two-player co-op split-screen, local (couch) and remote (LAN / EOS).

The remote half of the screen is not a second simulated camera. UnrealSplit samples the sender's live view from `APlayerCameraManager` (`GetCameraLocation`, `GetCameraRotation`, `GetFOVAngle`) and plays that stream back on the receiver through `AUSPlayerViewProxy` and `AUSSpectatorPlayerController`.

## Camera constraint

The game must present a **conventional** view: `APlayerCameraManager` (the engine default is enough) plus a `UCameraComponent` on the pawn, typically on a spring arm.

Do **not** drive the player view with Experimental **Gameplay Cameras** (`AGameplayCamerasPlayerCameraManager`, `UGameplayCameraComponent`, camera directors / rigs). That stack evaluates a separate pose and often binds to Player 0 only. UnrealSplit will not see it as the stream, the shadow view will not track it, and a second local player will not get a correct half.

If a project ships both, the Gameplay Camera path must be off at runtime so the spring-arm camera is what the manager reports. `US.ViewState` is the check: both halves should show the same kind of follow camera.

This is a plugin limitation, not a game-specific quirk. Call it out when documenting or integrating UnrealSplit.
