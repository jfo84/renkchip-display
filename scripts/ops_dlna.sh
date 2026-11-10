#!/bin/bash
# ops_dlna.sh — put a status page on the TV through the stick's stock DLNA
# renderer (no display-takeover, no vendor stack surgery). Renders the page
# on the host (Brave headless 1920x1400) -> ffmpeg scale to EXACT 1080p ->
# ops.mp4 on the desktop :8000 -> SOAP SetAVTransportURI+Play on the stick
# :38400. Loops forever: whenever the stick stays up long enough for the
# renderer to answer, the page appears; re-plays every REPLAY_S while up.
#
# Config (env):
#   DONGLE     base URL of the stick          (default http://192.168.8.160 — my lab IP)
#   SERVE      base URL serving ops.mp4       (default http://192.168.8.219:8000)
#   OPS_API    host:port of the data API the page polls, tunneled to
#              localhost:9701 via SSH_HOST. Empty = page is self-contained.
#   SSH_HOST   ssh alias used for the tunnel and the scp push (default desktop)
set -u
cd "$(dirname "$0")"
D="${DONGLE:-http://192.168.8.160}"
DIP="${D#http://}"
U="${SERVE:-http://192.168.8.219:8000}"
OPS_API="${OPS_API:-}"
SSH_HOST="${SSH_HOST:-desktop}"
BRAVE="/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"
REPLAY_S=900
LOG=ops_dlna.log

log() { echo "$(date '+%m-%d %H:%M:%S') $*" | tee -a "$LOG"; }

render() {
  # Keep the data-API tunnel alive if one is configured.
  if [ -n "$OPS_API" ]; then
    curl -s -m 4 -o /dev/null http://localhost:9701/peers || {
      ssh -N -f -L "9701:${OPS_API}" "$SSH_HOST" 2>/dev/null; sleep 2; }
  fi
  # Screenshot at the page's real height, scale to exact 1080p in ffmpeg.
  # NEVER use --force-device-scale-factor with window-size: it emits
  # CSS×DSF pngs (e.g. 2074×1166) and the player silently refuses to parse
  # anything that isn't exactly 1920×1080 (looks exactly like broken
  # transfers — BrokenPipe bursts — but isn't).
  "$BRAVE" --headless --disable-gpu --screenshot=/tmp/ops_r.png \
    --window-size=1920,1400 --virtual-time-budget=10000 \
    --host-resolver-rules="MAP $SSH_HOST 127.0.0.1" \
    "http://localhost:8088/?tv=1#/ops" >/dev/null 2>&1
  [ -s /tmp/ops_r.png ] || return 1
  # -an: video-only. A wedged player + an audio track = full-volume TV
  # speaker noise; with no track it is structurally impossible. faststart
  # is mandatory or the player sees moov as 00:00 and retries forever.
  ffmpeg -y -loglevel error -loop 1 -f image2 -i /tmp/ops_r.png \
    -t 894 -vf scale=1920:1080 \
    -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -r 5 -an \
    -x264-params keyint=250:min-keyint=250:scenecut=0 \
    -movflags +faststart ops.mp4 && \
  scp -q ops.mp4 "$SSH_HOST:ops/ops.mp4"
}
soap_state() {
  curl -s -m 6 -X POST "$D:38400/serviceControl/AVTransport" \
    -H 'Content-Type: text/xml; charset="utf-8"' \
    -H 'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo"' \
    --data-binary '<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID></u:GetTransportInfo></s:Body></s:Envelope>' \
    | tr '<' '\n<' | grep -i CurrentTransportState | head -1
}

soap_play() {
  curl -s -m 10 -X POST "$D:38400/serviceControl/AVTransport" \
    -H 'Content-Type: text/xml; charset="utf-8"' \
    -H 'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI"' \
    --data-binary '<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID><CurrentURI>'$U'/ops.mp4</CurrentURI><CurrentURIMetaData></CurrentURIMetaData></u:SetAVTransportURI></s:Body></s:Envelope>' \
    -o /dev/null -w '%{http_code}'
  sleep 2
  curl -s -m 10 -X POST "$D:38400/serviceControl/AVTransport" \
    -H 'Content-Type: text/xml; charset="utf-8"' \
    -H 'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#Play"' \
    --data-binary '<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:Play xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><InstanceID>0</InstanceID><Speed>1</Speed></u:Play></s:Body></s:Envelope>' \
    -o /dev/null -w '%{http_code}'
}

log "ops_dlna driver starting (waits for a stable stick, then DLNA-plays ops.mp4)"
while :; do
  # wait for the stick to be up AND stay up 60s
  ping -c 1 -t 2 "$DIP" >/dev/null 2>&1 || { sleep 15; continue; }
  sleep 60
  ping -c 1 -t 2 "$DIP" >/dev/null 2>&1 || { log "box flapped — waiting"; continue; }

  # DLNA renderer up? (starts with the media stack; probe up to 3 min)
  UP=""
  for t in $(seq 1 18); do
    C=$(curl -s -m 4 -o /dev/null -w '%{http_code}' "$D:38400/deviceDescription/MediaRenderer" 2>/dev/null)
    [ "$C" = "200" ] && { UP=1; break; }
    ping -c 1 -t 2 "$DIP" >/dev/null 2>&1 || break
    sleep 10
  done
  if [ -z "$UP" ]; then log "no DLNA renderer this boot — retrying"; continue; fi

  render && log "rendered ops.mp4 ($(stat -f%z ops.mp4) B)" || log "render failed (serving last good)"
  # Hold while the renderer is PLAYING — a mid-play URI switch wedges the
  # media stack (TRANSITIONING forever; Stop resets the transport but not
  # the stack; only a reboot cures). EOS lands as STOPPED, so the page
  # still refreshes every cycle.
  S=$(soap_state)
  if [ "$S" = "CurrentTransportState>PLAYING" ]; then
    log "holding stream ($S)"
  else
    R=$(soap_play)
    log "SOAP seturi+play: $R (was $S)"
  fi
  sleep "$REPLAY_S"
done
