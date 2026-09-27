# v0.1.0-preview release checklist

Do not tag or publish until every required line is true. The first public preview is a source release. A binary is blocked until a release certificate exists. Do not sign a public package with a temporary development key, and do not commit that key.

- [ ] Working tree clean
- [ ] Reviewed commit recorded
- [ ] `tools\run-host-selftest.cmd` passed
- [ ] Debug x64 build clean
- [ ] Release x64 build clean
- [ ] Warning-as-error still enabled
- [ ] Release has no Self-test or PoC controls
- [ ] Xbox UI checklist in `docs/XBOX_RC_CHECKLIST.md` passed
- [ ] Short playback and recovery session passed
- [ ] Video only checked on the Xbox
- [ ] Settings upgrade checked (onboarding, overlay, ports, +100 ms)
- [ ] README, license, and third-party notices reviewed
- [ ] No secrets, certificates, or private paths in the tree
- [ ] GitHub Actions host-selftest passed on the commit that will be tagged
- [ ] Opus review finished
- [ ] If a binary is published: built from the tag, signed with a release certificate, checksums published
- [ ] Tag `v0.1.0-preview` created only after the lines above

Manifest version stays `1.0.0.0`. Package identity name and publisher stay as they are so upgrades keep local settings.
