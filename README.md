# XReceiver

XReceiver is an open-source preview of an Xbox LAN media receiver. It receives H.264 video and L16 audio RTP forwarded by a separate [UxPlay](https://github.com/FDH2/UxPlay) server.

It is not an Apple product, not a Microsoft or Xbox product, and not affiliated with or endorsed by Apple, Microsoft, Xbox, or UxPlay. It does not implement AirPlay on the Xbox. UxPlay, running on another computer, is the AirPlay receiver. XReceiver only listens for the RTP that UxPlay sends.

This is Developer Mode software. It is not distributed through the Microsoft Store. It is not production-certified. It is not a native 4K receiver. It is not intended for a hostile or untrusted network.

## What it does

- Listens for H.264 video RTP (default UDP 5000, payload type 96, 90 kHz)
- Listens for L16 stereo audio RTP (default UDP 5002, payload type 96, 44.1 kHz)
- Plays video through a real-time MediaStreamSource and audio through AudioGraph
- Recovers on a new keyframe after loss, and on resolution or sender changes
- Offers Automatic, Always visible, and Video only overlays
- Keeps a manual A/V offset. Positive values delay audio further

## What it does not do

- AirPlay, FairPlay, or pairing
- HEVC / H.265, or native 4K decode
- Automatic A/V calibration
- Accounts, telemetry, or cloud streaming
- Authentication of the UDP sender

A 1080p picture on a 4K television can be scaled by the Xbox or the display. That is not native 4K. UxPlay's 4K mirror uses HEVC, which this preview does not accept.

## Architecture

```text
Apple device  --AirPlay-->  UxPlay on a LAN computer  --RTP-->  XReceiver on Xbox
```

UxPlay is installed and configured on its own. XReceiver is the UWP app you build in Visual Studio and deploy to Xbox Developer Mode.

## Requirements

- Xbox in Developer Mode
- Visual Studio 2022 with the C++ Universal Windows Platform workload and MSVC v143
- Windows SDK 10.0.26100.0
- UxPlay 1.73 or later on another machine on the same LAN, built with RTP forwarding (`-vrtp` and `-artp`)

## Build

1. Clone the repository.
2. Open `XReceiver.sln`.
3. If NuGet has not restored `Microsoft.Windows.CppWinRT` 2.0.220531.1, restore packages. The project checks `packages.config` and fails the build with a restore message if the package is missing.
4. Select Debug or Release, x64.
5. Build.

The project file is `XReceiver/XReceiver.vcxproj`. If the folder that contains this checkout still uses an older name, rename that outer folder only after you close Cursor and Visual Studio. C++/WinRT generates `XReceiver/Generated Files/module.g.cpp` during the build and the project compiles that file once. Do not commit the generated directory.

Take latency measurements on Release x64. Debug is slower.

## Deploy to Xbox

1. In Visual Studio, set the debugger target to Remote Machine.
2. Enter the Xbox address. Visual Studio stores it in `XReceiver.vcxproj.user`. That file is gitignored. Do not commit it.
3. Deploy from Visual Studio.

Local packaging creates a development certificate, usually a `TemporaryKey.pfx` next to the project. That file is a private key. It is gitignored. Do not commit `.pfx`, `.cer`, or `.snk` files. Another PC creates its own test certificate in the manifest designer (Packaging) or on the first package build. A sideload user must trust the certificate that signed the package they install. This repository does not publish a certificate or an `.appx`. Do not distribute a package signed with a temporary development key.

The package identity name and publisher are unchanged from earlier development builds so an upgrade can keep local settings. The manifest version is `1.0.0.0`. The preview label `v0.1.0-preview` is a GitHub release name, not a package downgrade, and it is not tagged yet.

## UxPlay forwarding

Install UxPlay separately. Replace `XBOX_IP` with the Xbox address. Defaults are video port 5000 and audio port 5002.

```bash
uxplay -n "XReceiver" \
  -vrtp "config-interval=1 pt=96 ! udpsink host=XBOX_IP port=5000 sync=false async=false" \
  -artp "pt=96 ! udpsink host=XBOX_IP port=5002 sync=false async=false"
```

Do not pass `-h265`. Start XReceiver before you start mirroring. On the Apple device, pick the UxPlay receiver from Screen Mirroring.

## First run

Onboarding explains the LAN, the two ports, Screen Mirroring, Menu, View, B, and Video only. Continue dismisses it. Help opens it again. An upgrade that already dismissed onboarding does not show it again.

## Controller

| Control | Action |
|---|---|
| A | Activate the focused control |
| Menu | Open or close the panel |
| View | Toggle diagnostics |
| B | Close the panel and diagnostics. Does not stop playback or exit |
| D-pad | Move focus |

The panel hides after a few seconds of playback if the controller is idle. Ports and the audio toggle change only while stopped. The A/V offset and the overlay mode apply immediately.

## Overlay modes

- **Automatic** (default): status shows briefly after a state change, then hides during steady playback
- **Always visible**: the compact status stays while you receive. Controls can still hide
- **Video only**: no chrome while healthy video is playing. Menu and View open temporary panels. B clears them. Waiting, reconnecting, keyframe wait, and errors bring status back

## A/V offset

The offset is milliseconds. Positive means audio is delayed further (audio later). Negative means audio earlier. The control steps by 10 ms and the value is saved.

A change during playback takes effect on the next audio quantum, about 10 ms. Raising the offset plays that much silence once. Lowering it skips that much buffered audio once. Presses that arrive together are combined into one move to the latest value. Slow clock-drift correction then carries on as before. The audio buffer stays between 20 and 300 ms by default, more on a jittery network, so an offset that would go past either end has no further effect. A saved `+100 ms` stays `+100 ms` across launches and across the XReceiver rename, because the package identity did not change.

## Diagnostics

View, or the diagnostics control, shows video, audio, session, and health counters. Hidden by default for a new install. The Mbps figure is total received video bytes. Accepted packets are counted separately. `recv→submit` is not television or speaker latency. Unavailable values are shown as an em dash. Health words do not restart the session.

## Short validation

On a Release build, with a controller:

1. Clear app data, launch at 720p, finish onboarding, restart, confirm it stays dismissed
2. Walk every control. Check the focus ring
3. Mirror. Confirm Automatic, Always visible, and Video only
4. Menu and View, then B, without stopping playback
5. Confirm Waiting, Receiving, a static picture with audio, and Reconnecting
6. Confirm the saved offset
7. Look at 1080p, and at 4K output only as UI scaling
8. Confirm Release has no Self-test or PoC controls

Host tests do not replace this. The blank form is `docs/XBOX_RC_CHECKLIST.md`. See `CONTRIBUTING.md` for `tools\run-host-selftest.cmd`. The maintainer checklist is `docs/RELEASE_CHECKLIST.md`.

## Troubleshooting

- Stays on Ready to connect: same LAN, no AP isolation, correct `XBOX_IP`
- HEVC error: remove `-h265` from UxPlay
- Port in use: stop the other listener or pick ports from 1024 through 65535
- Strict mode holds the picture until the next keyframe after loss. Tolerant keeps decoding and can show artifacts
- Audio can fail while video continues. The status line says audio is unavailable
- Graph errors: stop and start the receiver after an HDMI or device change

## Security

UDP is unauthenticated and unencrypted. Use a trusted LAN. Do not forward ports 5000 or 5002 from the internet. Details and reporting are in `SECURITY.md`.

## Contributing

See `CONTRIBUTING.md`.

## Releases

The first public preview is a source release. `v0.1.0-preview` is the GitHub name. It is not tagged yet. The UWP package version stays `1.0.0.0`.

A UWP package has to be signed. A local Developer Mode build may use the temporary certificate Visual Studio creates on that PC. Leave that key untracked. A public binary needs a different release certificate, a build from the reviewed tag, and published checksums. Installing that package means trusting the signing certificate. It is not Microsoft Store trust. Binary publication stays blocked until a release certificate exists.

## License

MIT. Copyright (c) 2026 XReceiver contributors. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.

## Acknowledgments

UxPlay is an independent project that can forward RTP. XReceiver does not include it. C++/WinRT is a Microsoft library used under the MIT license. The blank UWP page shell started from the Visual Studio template.
