# Changelog

## v0.1.0-preview

Release candidate prepared on 2026-09-27. Not tagged. Not a Microsoft Store package. Manifest version stays `1.0.0.0` so an upgrade is not a downgrade. The GitHub name `v0.1.0-preview` is separate from that package version.

XReceiver is an Xbox Developer Mode app. It receives H.264 video (UDP 5000) and L16 stereo audio (UDP 5002) forwarded by a separate UxPlay server on a trusted LAN. It does not implement AirPlay. It is not affiliated with Apple, Microsoft, Xbox, or UxPlay. It is not a native 4K receiver. UDP is unauthenticated and unencrypted. Do not forward the ports from the internet.

### User-facing

- Ten-foot UI, first-run help, and controller focus
- Overlay modes: Automatic, Always visible, and Video only
- Plain-language status for waiting, receiving, reconnecting, and errors
- Manual A/V offset. Positive means audio later
- Diagnostics and health counters, hidden until requested
- Display name XReceiver. Package identity is unchanged, so local settings survive an upgrade

### Known limits

- H.264 up to the existing 1080p60 target. A 4K television may scale that picture
- No HEVC
- No automatic A/V sync
- Host tests do not prove memory safety
- Xbox UI and playback for this candidate have not been signed off
- A public binary is blocked until a release certificate exists. Do not ship a package signed with a temporary development key

### Already in this line

- RTP receiver prototype
- Reliability hardening
- Security hardening and hostile-input tests
- Health diagnostics
- Repository ignore rules
- Rename from the smoke-test project name

## Unreleased

Nothing yet beyond the candidate above.
