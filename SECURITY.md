# Security policy

XReceiver is preview software for Xbox Developer Mode. It is not production-certified, and the host tests do not prove memory safety.

## Supported versions

Security fixes apply to the current preview line on the default branch. There are no older supported release branches yet.

## Reporting a vulnerability

Use GitHub private vulnerability reporting (Security Advisories) on this repository. Do not open a public issue for an unfixed vulnerability.

Do not include private keys, `.pfx` files, `.vcxproj.user`, your public IP address, or personal media in the report.

Useful reports include:

- XReceiver commit
- Debug or Release
- What an attacker on the LAN can do
- Whether the issue needs a malicious RTP packet, a crash, or a settings file
- The smallest packet or steps that reproduce it

There is no separate security email address for this project.

## Manifest capabilities

The package requests `internetClient`, `privateNetworkClientServer`, and `internetClientServer`. Those capabilities are how this UWP build is allowed to receive LAN UDP. Removing them would need a new Xbox test and is not part of the preview candidate.

## Network assumptions

XReceiver accepts unauthenticated, unencrypted UDP. It is meant for a trusted private LAN.

- Video port default: UDP 5000
- Audio port default: UDP 5002
- Do not forward those ports from the internet
- Do not expose the Xbox or the UxPlay computer to an untrusted network
- Prefer a wired segment or a network you control
- The app does not authenticate the sender

Phase 3 added bounds checks and hostile-input tests for the parsers. That reduces several crash and overflow cases. It is not a guarantee that every malformed packet is safe, and it does not protect against a malicious host that is already on your LAN.
