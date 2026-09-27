# Xbox release-candidate checklist

About 20 minutes with a controller. Do not mark this passed until it has been run on hardware. This file is a template.

Build: Release x64. Commit under test:

## Upgrade

Install over the previous package. Do not clear app data.

- [ ] Upgrade succeeds
- [ ] Onboarding stays dismissed
- [ ] Overlay mode is unchanged
- [ ] Ports are unchanged
- [ ] Saved offset is still +100 ms and still means audio later

## UI

- [ ] Waiting, Starting, Receiving, Reconnecting, and Stopped are distinct
- [ ] Controller focus order and the focus ring are obvious
- [ ] Menu, View, and B behave as documented
- [ ] Automatic hides status during steady playback
- [ ] Always visible keeps the compact status
- [ ] Video only shows no chrome during healthy playback
- [ ] Menu and View open temporary UI in Video only
- [ ] B returns to an unobstructed picture and does not stop playback
- [ ] With diagnostics open, Menu closes the panel and no hidden control keeps the focus ring
- [ ] After changing the ports, Ready to connect and Setup help show the new ports; defaults show 5000 and 5002
- [ ] Release has no Self-test or PoC controls

## Playback (10–15 minutes, one session)

Include normal motion, high motion, a static picture with audio, one orientation or resolution change, one UxPlay restart, and one brief network interruption.

Pass only if all of these hold:

- [ ] No crash or hang
- [ ] No permanent black picture
- [ ] No permanent audio loss
- [ ] No decoder or source restart loop
- [ ] Queue does not climb for the whole session
- [ ] No health flag stays on without a cause
- [ ] No audio callback or AudioGraph errors
- [ ] Picture returns on a valid keyframe
- [ ] Audio stays clean
- [ ] A/V offset still feels acceptable
- [ ] During playback, +10 ms and -10 ms are heard at once as a brief gap or skip, and several fast presses land on the shown value
- [ ] Video only stays clear during healthy playback

## Result

Date:

Commit:

Pass / fail:

Notes:

Screenshots:
