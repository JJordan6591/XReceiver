# Opus review brief

This is a handoff, not a request to restyle the media pipeline. Change behavior only when a test or a traced defect supports it.

## Commits

`master` is the release-candidate tip. Phase 5 and this wrap-up sit on `6d8fd22`. Read `git log --oneline 6d8fd22..HEAD` for the rename, docs, host runner, and directory move. Do not rewrite that history.

## What the program is

XReceiver is a C++/WinRT UWP app for Xbox Developer Mode. UxPlay on another LAN machine terminates AirPlay and forwards RTP. XReceiver listens for H.264 (UDP 5000, PT 96) and L16 stereo (UDP 5002, PT 96). It is not AirPlay, not Store software, and not native 4K.

## Threads

- UDP receive callbacks: parse, admit, jitter insert. Rejected packets must not refresh the idle timer.
- Video delivery and MediaStreamSource callbacks: `VideoReceiver` lock, then WinRT completion outside the lock. `VideoDeliveryCore` is the platform-free queue and keyframe gate.
- Audio quantum: ring read, resample, submit. No allocation, no string formatting, no heavy locks. A few atomics update fill and callback timing.
- UI timer (about 250 ms): status text, diagnostics text, overlay visibility, session health. `ReceiverSession` housekeeping is a 10 ms thread-pool timer for poll and connection state.

## Trust boundary

Every datagram is hostile until `ParseRtpPacket` and `AdmitRtpPacket` accept it. Caps live in the depacketizer, jitter buffer, SPS parser, and audio frame-size check. Health flags do not restart the session. UDP is not authenticated or encrypted. LAN only.

## Hot paths

RTP parse, jitter copy into a fixed pool, H.264 access-unit assembly, L16 convert, sinc resample. Do not add per-packet logging or unbounded buffers. Do not raise queue defaults to hide underruns.

## Compromises

- `recv→submit` is not display or speaker latency.
- Manual A/V offset only. Positive delays audio.
- Package publisher remains `CN=jack` so upgrades keep settings. Display name is XReceiver. `PublisherDisplayName` is `XReceiver contributors` and is not the package family.
- Manifest version stays `1.0.0.0`. The preview label is `v0.1.0-preview` and is not tagged.
- The checkout folder outside `XReceiver/` may still be named `UwpSmokeTestCpp`. That is the local workspace, not a tracked path.
- The optional H.264 PoC file was removed. Its old size did not match the documented 600-frame command.
- GitHub Actions host test is written and has not run on GitHub. No badge.
- Binary release is blocked. Do not ship `TemporaryKey.pfx`.

## Settings

Schema version 3. Version 0 unversioned installs keep the old diagnostics default when keys exist. Version 2 and 3 keep ports, offset, onboarding dismissal, and overlay mode. Invalid overlay values become Automatic. Keys are not named after the old project.

## Files to read first

- `XReceiver/RtpPacket.cpp`, `H264Depacketizer.cpp`, `H264Bitstream.cpp`
- `XReceiver/VideoReceiver.cpp`, `VideoDeliveryCore.cpp`, `AudioReceiver.cpp`, `AudioPresenter.cpp`
- `XReceiver/ReceiverSession.cpp`, `ReceiverSettings.cpp`, `UiChrome.h`
- `XReceiver/XReceiver.vcxproj` (Release excludes `SelfTest.cpp` and `PocSpikes.cpp`; `module.g.cpp` once)
- `Package.appxmanifest` identity versus display name

## Tests

`tools\run-host-selftest.cmd` is authoritative. It does not boot the Xbox, render XAML, or prove memory safety. UI tests cover overlay decisions and settings migration, not pixels.

## Still outside this tree

Opus review, a green GitHub Actions run after push, the Xbox checklist in `docs/XBOX_RC_CHECKLIST.md`, and release signing if a binary is published. Do not tag `v0.1.0-preview` before those gates.
