========================================================================
    UwpSmokeTestCpp - low-latency RTP receiver for UxPlay (Xbox, UWP)
========================================================================

This app does NOT speak AirPlay. UxPlay on a Mac is the AirPlay receiver;
it forwards the decrypted mirror stream to the Xbox as plain RTP:

  video  -vrtp  H.264 RTP  UDP 5000  PT 96  90 kHz clock
  audio  -artp  L16 RTP    UDP 5002  PT 96  S16BE 44.1 kHz stereo

The Xbox app receives both, depacketizes H.264 (Single NAL, STAP-A, FU-A)
into a video-only MediaStreamSource played by a real-time MediaPlayer,
and plays audio through AudioGraph with its own small buffer.


------------------------------------------------------------------------
1. Build and deploy (Visual Studio 2022 only)
------------------------------------------------------------------------
- Open UwpSmokeTestCpp.sln, pick Debug or Release, x64, "Remote Machine".
  The remote Xbox address is already set in the .vcxproj.user file.
- Build, deploy and debug from Visual Studio as usual. Take final latency
  numbers on Release x64; the Debug CRT slows the per-packet code.
- On the Xbox, in Dev Home, set the app type to "Game" if the "App"
  resource limits cause hitches (see Troubleshooting).


------------------------------------------------------------------------
2. Running with UxPlay (on the Mac)
------------------------------------------------------------------------
First confirm your UxPlay version supports forwarding (1.73 or later):

  uxplay -h        (look for -vrtp and -artp)

Then (replace XBOX_IP):

  uxplay -n "Xbox AirPlay" \
    -vrtp "config-interval=1 pt=96 ! udpsink host=XBOX_IP port=5000 sync=false async=false" \
    -artp "pt=96 ! udpsink host=XBOX_IP port=5002 sync=false async=false"

- Do NOT pass -h265. The receiver is H.264 only; if HEVC is detected the
  app shows an Error state saying so.
- config-interval=1 makes UxPlay repeat SPS/PPS, which lets the app start
  or recover quickly.
- If the network struggles with IDR bursts, lower the load, for example
  add:  -s 1920x1080 -fps 30

On the Xbox: start the app. With Auto-start on (the default), it begins
listening immediately ("Waiting"). Start mirroring from the iPhone/iPad
to "Xbox AirPlay". The state pill turns green ("Receiving").


------------------------------------------------------------------------
3. Controller
------------------------------------------------------------------------
  D-pad / A      navigate / press; any press shows the panel if hidden
  Menu           show or hide the control panel
  View           show or hide the diagnostics overlay
  B              hide the panel (never exits the app)

The panel auto-hides 5 s after video starts if the controller is idle.
Ports, Audio on/off, Loss recovery (Strict/Tolerant), Auto-start and the
A/V offset are saved and restored on the next launch. Ports can only be
changed while stopped.

States:  Stopped (grey), Starting/Waiting (amber), Receiving (green),
         Reconnecting (orange: neither audio nor video has arrived for
         at least 5 s), Error (red: see the message under the pill).
         A static AirPlay screen sends few or no video frames; while
         audio or video still arrives the state stays Receiving. While
         the picture is held for a keyframe after loss, the line under
         the pill says so and the state stays Receiving.


------------------------------------------------------------------------
4. Network and firewall
------------------------------------------------------------------------
- The Xbox has no user firewall. Access is governed by the manifest
  capability privateNetworkClientServer (added). If milestone 0a shows no
  packets arriving, add internetClientServer as the fallback.
- The macOS firewall only affects inbound traffic; outbound UDP to
  5000/5002 needs no change.
- Mac and Xbox must be on the same subnet with no Wi-Fi client/AP
  isolation. Wired Ethernet on the Xbox is strongly recommended.
- For a Windows PC test receiver: allow inbound UDP 5000/5002 in Windows
  Defender Firewall. For this UWP app on the same PC as the sender, a
  loopback exemption is needed:
    CheckNetIsolation LoopbackExempt -is -n=<PackageFamilyName>


------------------------------------------------------------------------
5. Milestone 0: Xbox spikes (Debug builds only)
------------------------------------------------------------------------
Debug builds show an extra row of buttons in the panel. Results appear
in the diagnostics overlay.

  Self-test     runs the built-in parser/depacketizer/gate/audio tests.
  PoC: sockets  bare DatagramSockets on the two ports that only count
                packets, bytes and sequence gaps (0a: proves the
                capability admits LAN UDP; measure loss in IDR bursts at
                1080p60 with 12-20 Mbps).
  PoC: clip     plays Assets\poc_1080p60.h264 on a loop at 60 fps with
                no networking (0b: decode, RealTimePlayback, SampleLag,
                pts lead, in-band resolution change).
  PoC: tone     0c: a standalone AudioGraph that synthesizes a 440 Hz
                sine inside QuantumStarted (no ring buffer, no timer,
                no drift control). Each press runs the next test and
                the 7th press stops (final numbers stay on screen):
                  1  A native rate, LowestLatency
                  2  B 44.1 kHz,    LowestLatency
                  3  A native rate, ClosestToDesired 240 (5 ms)
                  4  B 44.1 kHz,    ClosestToDesired 240
                  5  A native rate, SystemDefault
                  6  B 44.1 kHz,    SystemDefault
                A = tone at the graph's own rate (is AudioGraph itself
                clean?). B = 44.1 kHz input node, AudioGraph resamples
                (the UxPlay case).

Press a PoC button again to stop it. PoCs stop the receiver first.
Starting the receiver resets the tone sequence to test 1.

Reading the tone diagnostics:
  quantum / latency      actual quantum size and graph latency
  render processing      the processing mode actually granted
  late                   callbacks arriving > 1.5 quanta after the last
  callback time          our own work per callback (should be tiny)
  RequiredSamples        frames per channel per callback; B alternates
                         (e.g. 117/118 for a 128-sample 48 kHz quantum)
  submitted / completed  outstanding should stay 0-2
  unrendered time        wall time the graph did not render. Growth of
                         whole quanta that lines up with audible
                         scratches means device starvation; a slow creep
                         of a few ms per minute is only clock drift.
  errors                 graph = UnrecoverableErrorOccurred (device
                         lost, session disconnected); callback = HRESULT
                         failures in QuantumStarted; bad buffers =
                         AudioFrame smaller than requested.
A quantum mode is "stable" if, over 10 minutes, the tone stays clean,
"late" stays near 0 and "unrendered time" does not grow in steps.
Prefer the smallest stable mode.

Creating the PoC clip (Annex B, AUD + SPS/PPS on every IDR, no B-frames):

  gst-launch-1.0 -e videotestsrc num-buffers=600 pattern=ball \
    ! video/x-raw,width=1920,height=1080,framerate=60/1 ! timeoverlay \
    ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast \
      key-int-max=60 bframes=0 aud=true bitrate=12000 \
    ! video/x-h264,stream-format=byte-stream,alignment=au,profile=high \
    ! h264parse config-interval=-1 ! filesink location=poc_1080p60.h264

To test resolution changes, concatenate a 1280x720 clip made the same
way onto the end (cat a.h264 b.h264 > poc_1080p60.h264).

Copy the file to UwpSmokeTestCpp\Assets\poc_1080p60.h264. The project
packages it automatically when it exists (a conditional item in the
.vcxproj); reload the project in Visual Studio after adding it.


------------------------------------------------------------------------
6. Test senders without AirPlay (Mac, GStreamer)
------------------------------------------------------------------------
Video (hardware encoder; timeoverlay is used for latency photos):

  gst-launch-1.0 videotestsrc is-live=true pattern=ball \
    ! video/x-raw,width=1920,height=1080,framerate=60/1 ! timeoverlay \
    ! videoconvert ! vtenc_h264_hw realtime=true \
      allow-frame-reordering=false max-keyframe-interval=60 bitrate=12000 \
    ! h264parse ! rtph264pay config-interval=1 pt=96 mtu=1400 \
    ! udpsink host=XBOX_IP port=5000 sync=false async=false

  (Software alternative: x264enc tune=zerolatency speed-preset=ultrafast
   key-int-max=60 bframes=0)

Audio:

  gst-launch-1.0 audiotestsrc is-live=true wave=ticks ! audioconvert \
    ! audio/x-raw,format=S16BE,rate=44100,channels=2 ! rtpL16pay pt=96 \
    ! udpsink host=XBOX_IP port=5002 sync=false async=false

Sanity-check UxPlay's forwarded output on another machine first:

  gst-launch-1.0 udpsrc port=5000 \
    caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96" \
    ! rtph264depay ! h264parse ! decodebin ! autovideosink sync=false


------------------------------------------------------------------------
7. Loss, duplication and reordering tests (netsim)
------------------------------------------------------------------------
Insert netsim before udpsink, either in a test sender or inside UxPlay's
-vrtp / -artp strings:

  ... ! netsim drop-probability=0.01 duplicate-probability=0.005 \
        delay-probability=0.02 min-delay=1 max-delay=20 ! udpsink ...

What to expect (diagnostics overlay):
- 1 % loss: no crash and no corrupted pictures in Strict mode: the
  picture holds on the last good frame ("[AWAITING IDR]" in the
  diagnostics) and resumes only on the next complete IDR. There is no
  timeout that resumes on non-IDR frames. The gaps/lost/dup/reordered
  counters roughly match netsim; "AU incomplete" counts every access unit
  that lost a packet and was discarded whole.
- 20 ms reordering: "reordered" rises while "lost" stays near zero once
  videoReorderTimeoutMs is raised to about 25.
- Audio with loss: "concealed" rises; no clicks beyond the short fades.
- Pause the sender for 10 s and 60 s: Reconnecting after 5 s with no
  audio and no video; the sockets, decoder and cached SPS/PPS are kept,
  so the stream continues on its next frame; memory stays flat.
- Kill and restart the sender or UxPlay mid-stream: the last picture is
  held (Reconnecting if nothing arrives for 5 s), then video continues
  on the new stream's first IDR.
- 1-hour soak: memory flat; "pts lead" and "sample lag" stable (no
  latency creep); audio "buffer" stays near "target"; drift ppm shown.


------------------------------------------------------------------------
8. Measuring latency
------------------------------------------------------------------------
Turn on the TV's Game Mode / ALLM for every measurement and note the
TV's own input lag.

A. Without AirPlay: tee the test sender so the same timeoverlay source
   also shows locally (add "t. ! queue ! videoconvert ! osxvideosink
   sync=false"). Photograph the Mac screen and the TV together; take 10
   shots and use the median. Measures encode + network + Xbox
   receive/decode/display.
B. End to end: a millisecond stopwatch on the iPhone, mirrored.
   Photograph the iPhone and the TV. Repeat with UxPlay rendering
   locally on the Mac (no -vrtp). The difference is roughly the Xbox
   path minus the Mac's own render time.
C. In app: the diagnostics overlay shows recv->submit p50/p95 (first
   packet of a frame to hand-off to the decoder), sample lag, pts lead
   and queue depth.

A/V sync: mirror an A/V sync test video (flash + beep) and film it in
slow motion. Adjust with the A/V offset buttons (positive = audio later).
Once a good value is known, it can become videoPipelineLatencyMs.


------------------------------------------------------------------------
9. Tuning
------------------------------------------------------------------------
Defaults live in ReceiverSettings.h. All values are persisted in the
app's LocalSettings the first time the app saves settings, so after
changing a default either uninstall the app or reset its data
(Xbox: Manage app > Reset) for the new default to take effect.

  videoReorderTimeoutMs  10     5-30   wait for a missing packet
  videoReorderWindow     256           packets held while reordering
  audioReorderTimeoutMs  15
  frameQueueDepth        2      1-3    while playing, non-reference
                                       frames above this are dropped
  maxFrameAgeMs          100    50-200 while playing, non-reference
                                       frames older than this are dropped
  openQueueCap           60            playing cap: a frame arriving at
                                       a full queue is rejected (queued
                                       frames are kept); a rejected
                                       reference frame applies the loss
                                       policy ("back-pressure")
  startupQueueCap        600    >=cap  same, from the source's IDR until
                                       its first frame is rendered and
                                       the start-up backlog has drained
                                       to half of openQueueCap; also
                                       bounded to 64 MB
  lossPolicy             Strict        Strict or Tolerant (UI toggle)
  rebuildOnFormatChange  true          new source on SPS size change
  socketBufferBytes      2 MB          socket inbound buffer
  audioMinDelayMs        20            audio buffer floor
  audioMaxDelayMs        300           audio hard cap (flush above)
  videoPipelineLatencyMs 60            used for A/V alignment
  idleTimeoutMs          5000   >=5000 no audio AND no video ->
                                       Reconnecting (display only)
  ssrcTakeoverMs         1000          accept a new sender after this

Video integrity rules (not configurable):
- An access unit (all RTP packets with one SSRC and timestamp) reaches
  the decoder only if its marker packet arrived, every sequence number
  from its first to its last packet was received, and every FU-A NAL
  has a valid start, contiguous middle and valid end fragment. Anything
  else is discarded whole ("AU incomplete"); a timestamp change before
  the marker counts as "no marker".
- Strict: after a damaged or dropped reference picture, every non-IDR
  picture is discarded ("awaiting IDR") until a complete IDR arrives.
  The latest valid SPS/PPS are cached and prepended to IDRs that lack
  them. Tolerant: only the damaged pictures are dropped.
- Timestamp epoch: each source generation maps the RTP timestamp of
  its first IDR to the start position set in MediaStreamSource
  Starting (0), and every later picture to that plus its 90 kHz RTP
  delta converted to 100 ns ticks. PlaybackSession.Position uses the
  same stream-relative epoch; samples are never aligned to wall-clock
  time. Durations are the RTP delta to the previous picture, capped at
  100 ms. "pts discont" counts genuine RTP timestamp discontinuities (a
  timestamp that does not advance, or one that jumps more than 2 s
  ahead of the pictures' real arrival spacing); the picture then gets
  lastPts + lastDuration and later pictures follow from there. A normal
  stream shows 0.
- Delivery is a pull model. Each SampleRequested is answered at once
  with the oldest queued complete frame, or its deferral is kept until
  the next frame arrives. A kept request is only completed without a
  sample (which ends that stream) when its source is replaced, closed
  or stopped ("ended").
- Start-up: from the IDR that opens a source until its first frame is
  rendered ("delivery opening/starting", then "playing"), every
  complete frame is kept: the IDR with its SPS/PPS and the pictures
  that follow it wait for the player, bounded by startupQueueCap.
  Latency trimming and ageing apply only while playing, and only to
  non-reference frames.
- Capacity is never loss of what is already queued: a frame arriving
  at a full queue is rejected, the queued frames (including the IDR)
  are still delivered in order. A rejected reference frame is a
  genuinely skipped picture, so Strict awaits the next IDR exactly as
  after network damage. The overlay separates the two: "IDR waits:
  network" (damaged input) and "back-pressure" (rejected for capacity),
  with "startup-full" / "playing-full" counting rejected frames.
- Delivery diagnostics: "requests" SampleRequested events, "deferred"
  requests that had to wait, "pending" requests waiting now,
  "overlapping" requests that arrived while another was still waiting
  (the pipeline normally never does this), "last request" time since
  the last SampleRequested, "processed" / "unprocessed" Processed
  callbacks received / still outstanding (diagnostic only; not used to
  limit delivery), "rendered" SampleRendered events. Debug builds also
  write one line per request, deferral, submission, Processed callback,
  Starting, first rendered sample and Closed event to the Visual Studio
  Output window, tagged "video: src <source> req <request> sample <n>
  pts <ms>". "startup peak" is the largest queue while starting.


------------------------------------------------------------------------
10. Troubleshooting
------------------------------------------------------------------------
- Stays "Waiting": check XBOX_IP, same subnet, no AP isolation. Run
  "PoC: sockets" to see whether any UDP arrives at all.
- Error "Could not bind video UDP port ..." (or "Audio port ... bind
  failed" under the pill; video keeps running): another app or a PoC
  holds the port; stop it or choose other ports (1024-65535).
- Error mentions HEVC: remove -h265 from the UxPlay command.
- Frequent freezes in Strict mode: every lost packet in a reference
  picture holds the picture until the next IDR; compare "IDR interval"
  with how often "gaps" rises. Improve the network (wired Ethernet) or
  switch Loss recovery to Tolerant, which keeps decoding complete
  pictures and accepts visible artifacts after a loss.
- High "lost" during scene changes: IDR bursts overflow the socket;
  use wired Ethernet or lower the UxPlay bitrate/resolution.
- "real-time off" in diagnostics: RealTimePlayback was rejected; latency
  will be higher.
- Black picture with "submitted" stuck: look at "pending" and "last
  request". pending 1 with a growing last request age and "awaiting
  IDR" means the player is waiting for a frame the loss policy is
  holding back (check which "IDR waits" counter rose); pending 0 with
  a growing last request age means the player itself stopped asking
  (see the Output window log for the last submitted sample and whether
  its Processed callback ever arrived). "delivery starting" that never
  becomes "playing" means no SampleRendered event arrived.
- High latency right after connecting: frames that arrived while the
  decoder started are all shown rather than dropped; "startup peak"
  shows how many. The backlog drains only as fast as the player pulls.
- Audio "not running": video continues; the message under the state
  pill says why (for example, no audio device).
- Audio path: AudioGraph runs at its native rate with the SystemDefault
  quantum (the only mode that stayed clean in the Xbox tone tests); the
  app converts 44.1 kHz to that rate with a windowed-sinc resampler.
  "output" in the AUDIO line is the graph latency used for A/V sync.
- "graph err" > 0: the audio device or session was lost (for example
  an HDMI/receiver change). Stop and Start the receiver.
- "callback err" > 0: AudioGraph rejected a frame; report it with the
  diagnostics text.


------------------------------------------------------------------------
11. What the diagnostics measure
------------------------------------------------------------------------
The overlay is a receiver-health view, not a measurement of the television
or soundbar. "recv→submit" is the time from the first packet of an access
unit until that access unit is handed to MediaStreamSource. It does not
include decoder, HDMI, display or eARC delay.

Clocks that may be compared:
- Local elapsed time uses a monotonic clock (packet age, submit age,
  stall, IDR wait, uptime, state age).
- RTP timestamps are unwrapped per stream and are not subtracted from
  the monotonic clock directly.
- PTS lead compares the last submitted video PTS with the player's
  position. It is an estimate of how far ahead the queue is, not lip-sync
  error at the speakers.
- Audio "buffer" is the PCM ring fill. "output" is AudioGraph's reported
  latency. Drift ppm is the resampler correction.

"accepted pkts" counts datagrams that passed admission. The Mbps figure
is still total received video bytes, including datagrams that were later
rejected. Memory is the UWP app-memory counter when the platform provides
it; -1 means it was not available. Health words are diagnostics only and
do not restart the session.

Health thresholds (internal):
- queue: frame queue at or above 8 frames for 2 s (soft depth is 2).
- no-submit: an accepted video packet in the last 500 ms, no sample
  submitted for 2 s, and the receiver is not waiting for a keyframe.
- audio-low / audio-high: ring fill below half or above twice the target
  delay for 2 s.
- idr-wait: current keyframe wait is at least 2 s.
- restarts: 3 or more source builds inside 10 s.
- pts-jumps: 8 or more timestamp discontinuities since the previous
  diagnostics tick.
- peak-rising: the frame-queue high-water mark increases again after it
  has already reached 8 frames.
- stall: while Receiving, video and audio (if enabled) have both been
  quiet for 1 s. Audio keeps a static picture from counting as a stall.
- clock: a backward local timestamp was ignored.

Per Start, counters and high-water marks reset with the session. The
saved A/V offset is not part of these counters.


------------------------------------------------------------------------
12. Short Xbox validation (about 30-45 minutes)
------------------------------------------------------------------------
This procedure has not been run by the implementation. Fill the template
after a real Xbox session. A 20 minute run cannot prove long-term memory
stability.

Preparation. Record the commit, Debug or Release, Xbox model, display
resolution and refresh, UxPlay version and the exact command, the Apple
source, which parts of the network are wired or wireless, the saved A/V
offset, and the starting diagnostics (including memory if it is not -1).

Use Release x64 for the timed sections.

A. Normal mirroring, 10 minutes. Mixed motion and audio at 1080p60 when
   the source can send it. Note start and end frame rate, bitrates,
   jitter, queue level and peaks, stalls, decoder errors, audio buffer,
   underruns, callback and graph errors, and memory. The saved +100 ms
   offset must still be +100 ms.

B. High motion, 5 minutes. Watch queue, drops, sample lag, PTS jumps and
   decoder errors. Afterward the queue should fall back toward a few
   frames.

C. Static picture with audio, 5 minutes. State stays Receiving. Audio
   stays clean. The quiet picture does not become Reconnecting.

D. Recovery, about 5 minutes. Change orientation or resolution a few
   times, restart UxPlay once, interrupt the network once, and send a
   short bounded burst of junk UDP. The picture returns on a valid
   keyframe. No permanent black frame, decoder loop, or stuck request.

E. Lifecycle, about 5 minutes. Five Start/Stop cycles, one Stop during
   Starting, three suspend/resume cycles. Ports bind again. The session
   does not stay stuck.

F. Optional combined stability, 20 minutes. This can replace A-C when
   the same session includes normal, high-motion and static sections, so
   the whole manual pass stays near 30-45 minutes. Compare start and end
   memory, queues and error counters. There should be no continuous
   rise, no growing A/V drift, no crash, no hang, no permanent black
   picture, no audio loss and no restart loop.

Pass when all of these hold:
- no crash or hang
- no permanent video or audio loss
- no continuously growing queue
- no unexplained sustained memory increase during this short test
- no AudioGraph or callback errors
- no decoder restart loop
- no persistent queue-full condition
- recovery succeeds after the planned disruptions
- diagnostics stay consistent (ages are not negative except -1 for
  "not yet", health is not stuck on clock)
- the +100 ms offset is still saved
- Release shows no Self-test or PoC controls

Result template:

  Commit:
  Build:
  Xbox / display:
  UxPlay command:
  Network:
  A/V offset before / after:
  A start -> end (fps, Mbps, queue, peak, stalls, underruns, memory):
  B queue after high motion:
  C stayed Receiving with audio:
  D recovery:
  E lifecycle:
  F 20 min trend (or "replaced by A-C"):
  Pass / fail:
  Notes:


------------------------------------------------------------------------
13. Resolution boundary
------------------------------------------------------------------------
This receiver decodes H.264 and is aimed at up to 1080p60. A 1080p
picture on a 4K television can be scaled by the Xbox or the display.
That is not native 4K, and the app does not advertise a 4K mode.

UxPlay's native 4K mirror on supported Apple devices is HEVC/H.265.
Supporting that would be a later feature: HEVC RTP depacketization,
VPS/SPS/PPS handling, HEVC keyframe and corruption recovery, Xbox HEVC
capability detection, an HEVC MediaStreamSource, hostile-input tests,
UxPlay rtph265pay forwarding, and a performance pass whose first target
would be 4K30. None of that is in this build.
