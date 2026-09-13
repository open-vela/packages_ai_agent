#!/bin/bash
# fix_gemini_s1.sh - Minimal audio-framework patch for ai_agent / mini_memo
#                    on the Gemini-s1 board (Allwinner R528, r528s3-gemini-s1)
#
# Usage:
#   cd <project-root>   # e.g. contest-2026/
#   bash packages/ai_agent/fix_gemini_s1.sh
#   ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/ \
#       -e -Wno-error -j$(nproc)
#
# Unlike fix_esp32s3.sh this script does NOT need to run in the background:
# it can run once before the build. It does two things:
#
#   1. Rewrites the board's media graph.conf / criteria.txt with a minimal
#      capture + playback pipeline (see below).
#   2. Applies mini_memo_ptt_record_fix.patch - the cross-repo C fixes for the
#      "recording receives no frames / PTT hangs" root cause (DMA IRQ teardown,
#      media server accept pump, ffmpeg abufsink format negotiation).
#
# Why the graph patch exists
# --------------------------
# The stock Gemini-s1 media graph wires up the full smart-speaker pipeline
# (Ring/Music/Alarm movie sources, Video0/Video1, A2DP sink, a 10-input amix,
# VOIP/WWE/MDS capture sinks, voice-change, ...). ai_agent / mini_memo only
# needs a plain microphone capture path and a plain PCM playback path. Running
# the full graph on this board pulls in services the demo does not use and
# makes the record / playback path harder to bring up.
#
# It replaces the board's graph.conf and criteria.txt with a minimal pair that
# exposes exactly two routes through the media server:
#
#   * capture : adevsrc(/dev/audio/pcm0c) -> abufsink@acap
#   * playback: abufsrc@apb            -> adevsink(/dev/audio/pcm0p)
#
# Both run at 48 kHz / stereo, which matches what the ai_agent audio capture
# and playback code requests from the media framework on this board. Nothing
# outside the audio framework is touched.
#
# The graph changes cannot be expressed in defconfig because graph.conf /
# criteria.txt are plain resource files, not Kconfig options.

set -euo pipefail

# Resolve project root (parent of packages/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

MEDIA_DIR="$ROOT_DIR/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/src/etc/media"
GRAPH="$MEDIA_DIR/graph.conf"
CRITERIA="$MEDIA_DIR/criteria.txt"

if [ ! -d "$MEDIA_DIR" ]; then
    echo "[fix] ERROR: media config dir not found: $MEDIA_DIR"
    echo "[fix]        Is this the Gemini-s1 (r528s3-gemini-s1) source tree?"
    exit 1
fi

echo "[fix] Applying minimal audio-framework graph for ai_agent / mini_memo..."

# ---- Fix 1: minimal media graph --------------------------------
# Two independent routes: mic capture -> abufsink, abufsrc -> speaker.
if [ -f "$GRAPH" ] && [ ! -f "$GRAPH.ai_agent.bak" ]; then
    cp "$GRAPH" "$GRAPH.ai_agent.bak"
fi
cat > "$GRAPH" <<'EOF'
adevsrc@pcm0c=format=nuttx:sample_rate=48000:ch_layout=stereo:devname=/dev/audio/pcm0c:map=1,volume@VolAcap=precision=fixed[acap0],
[acap0]abufsink@acap;

abufsrc@apb=map=1,volume@VolApb=precision=fixed[apb0],
[apb0]adevsink@pcm0p=format=nuttx:sample_rate=48000:ch_layout=stereo:devname=/dev/audio/pcm0p
EOF
echo "  [1/2] graph.conf: minimal capture + playback routes written"

# ---- Fix 2: matching policy criteria ---------------------------
# Point Capture/Ring/Music/Alarm at the minimal abufsink/abufsrc nodes so the
# policy engine can resolve them against the new graph.
if [ -f "$CRITERIA" ] && [ ! -f "$CRITERIA.ai_agent.bak" ]; then
    cp "$CRITERIA" "$CRITERIA.ai_agent.bak"
fi
cat > "$CRITERIA" <<'EOF'
InclusiveCriterion UsingDevices     : mic a2dpsnk = mic
InclusiveCriterion ActiveStreams    : acap VOIPtx WWEtx MDStx
ExclusiveCriterion Capture          : abufsink@acap
ExclusiveCriterion Ring             : abufsrc@apb
ExclusiveCriterion Music            : abufsrc@apb
ExclusiveCriterion Alarm            : abufsrc@apb
ExclusiveCriterion Video Video0     : movie_async@Video0
ExclusiveCriterion VtunVideo Video1 : movie_async@Video1
ExclusiveCriterion TalkCap          : amoviesink_async@acap
ExclusiveCriterion VoipCap          : amoviesink_async@VOIPtx
ExclusiveCriterion WWEtx            : amoviesink_async@WWEtx
ExclusiveCriterion MDStx            : amoviesink_async@MDStx
NumericalCriterion persist.media.AcapGain AcapGain : [1,10] = 10
NumericalCriterion persist.media.AlarmVolume AlarmVolume : [0,20] = 10
NumericalCriterion persist.media.A2dpsnkVolume A2dpsnkVolume RingVolume MusicVolume MediaVolume : [0,20] = 10
ExclusiveCriterion VoiceChangeMode  : normal dashu xiaochou = normal
EOF
echo "  [2/2] criteria.txt: minimal policy criteria written"

echo "[fix] Media graph done. Original files saved as *.ai_agent.bak in:"
echo "      $MEDIA_DIR"

# ---- Fix 3: cross-repo PTT recording fixes ---------------------
# Apply mini_memo_ptt_record_fix.patch. It bundles three independent repo
# diffs, separated by "===== REPO: <path> =====" markers. We split it and
# git-apply each segment in the matching repo. Each apply is idempotent: if a
# segment already reverse-applies (i.e. is already present), it is skipped.
PATCH_FILE="$SCRIPT_DIR/defconfigs/gemini-s1/mini_memo_ptt_record_fix.patch"

apply_repo_segment() {
    # $1 = repo path (relative to ROOT_DIR), $2 = segment file
    local repo="$ROOT_DIR/$1"
    local seg="$2"

    if [ ! -s "$seg" ]; then
        return 0
    fi
    if [ ! -d "$repo/.git" ] && [ ! -f "$repo/.git" ]; then
        echo "  [skip] $1: not a git repo, cannot apply"
        return 0
    fi

    if git -C "$repo" apply --reverse --check "$seg" >/dev/null 2>&1; then
        echo "  [skip] $1: already applied"
        return 0
    fi
    if ! git -C "$repo" apply --check "$seg" >/dev/null 2>&1; then
        echo "  [WARN] $1: patch does not apply cleanly, skipping."
        echo "         Apply manually: git -C $1 apply <segment>"
        return 0
    fi
    git -C "$repo" apply "$seg"
    echo "  [ok]   $1: PTT recording fix applied"
}

if [ -f "$PATCH_FILE" ]; then
    echo "[fix] Applying mini_memo_ptt_record_fix.patch..."
    TMPDIR_PTT="$(mktemp -d)"
    trap 'rm -rf "$TMPDIR_PTT"' EXIT

    # Split the bundle into per-repo segments by the REPO marker.
    awk -v out="$TMPDIR_PTT" '
        /^===== REPO: / {
            repo = $3
            gsub(/[^A-Za-z0-9_]/, "_", repo)
            seg = out "/" repo ".patch"
            next
        }
        seg { print > seg }
    ' "$PATCH_FILE"

    apply_repo_segment "vendor/allwinnertech"        "$TMPDIR_PTT/vendor_allwinnertech.patch"
    apply_repo_segment "frameworks/multimedia/media" "$TMPDIR_PTT/frameworks_multimedia_media.patch"
    apply_repo_segment "external/ffmpeg/ffmpeg"      "$TMPDIR_PTT/external_ffmpeg_ffmpeg.patch"

    echo "[fix] PTT recording fix done."
else
    echo "[fix] NOTE: $PATCH_FILE not found, skipping PTT recording fix."
fi

echo "[fix] All done."
