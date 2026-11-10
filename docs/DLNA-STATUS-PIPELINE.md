# The DLNA status pipeline

*Nov 2026. The practical answer to "get my status page onto the TV at
1080p, today" — using the stick's completely stock media path, no
root required.*

## The idea

The stick ships with a working DLNA MediaRenderer. Instead of fighting the
cast stack (see `FINDINGS.md` for that war), render the status page into a
video file on a real computer and let the stick's own player display it:

    Brave headless (host) -> localhost:8088/?tv=1   [1920x1400 capture]
      -> ffmpeg scale=1920:1080, 5 fps, GOP 250, video-only, faststart
      -> scp ops.mp4 -> wired desktop :8000 (serve_ops.ps1)
      -> ops_dlna.sh SOAP-plays it on the stick :38400

The driver loops forever: wait for the stick to be up and stable, wait for
the renderer, push a fresh render, play. When the stick reboots or flaps,
the loop simply waits and resumes. A ~15-minute video ends, lands as
STOPPED, and the next cycle replays — the page refreshes itself at the
cycle cadence (a dark gap of up to ~2 min per cycle is normal).

## What the player actually wants (paid for in hours)

1. **Exactly 1920×1080, nothing else.** The player silently refuses
   anything else. The trap: `--force-device-scale-factor` combined with
   `--window-size` emits CSS×DSF pixels (2074×1166 at DSF 1.08), and a
   moov that claims non-1080p parses as 00:00 duration — the player
   retries forever while the HTTP server logs BrokenPipe bursts. It looks
   *exactly* like broken transfers. It isn't. Screenshot at real CSS size,
   scale in ffmpeg.
2. **Never switch URI mid-play.** A `SetAVTransportURI` while PLAYING
   wedges the media stack in TRANSITIONING forever; `Stop` resets the
   transport but not the stack; only a reboot cures it. So the driver is
   state-gated: it calls `GetTransportInfo` first and only plays when the
   transport is not already PLAYING.
3. **No audio track.** A wedged player plus an AAC track equals
   full-volume TV-speaker noise. Video-only files parse fine, and with no
   track the failure mode is structurally impossible.
4. **`+faststart`** or the player sees moov as 00:00 and retries forever.
5. **File size was a red herring.** Small files, a few hundred KB, 1080p —
   all fine once the geometry and flags were right.

## The desktop side

`serve_ops.ps1` is a raw `TcpListener` on :8000 serving one file — no
`HttpListener`, so no admin URL ACL. `setup_ops.ps1` adds the firewall rule
and starts the server hidden, once, from an elevated Task Scheduler job;
after a desktop reboot, re-run the task and the pipeline heals itself.

## Why this is the right milestone

The cast/decode path (`FINDINGS.md`) is the interesting engineering, but
this pipeline is the one that survived a 9-hour soak with zero flaps — the
stick streaming DLNA continuously on TV-USB power, which the cast-path
load class never managed. It's the plumbing for the real endgame: the same
loop playing whatever a display node should show.
