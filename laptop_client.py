"""
Robi Laptop Client — Always-On Wake Word Listener
==================================================
Say "Hey Robi <your question>" in one sentence and it will:
  1. Detect the wake phrase (fuzzy — catches "hero be", "hey roby", etc.)
  2. Extract your question from the same clip
  3. Send to the Flask AI server
  4. Speak the reply via macOS `say` (Bluetooth speaker if set as default)
  5. Push the reply to the ESP32 OLED display

Usage:
    python laptop_client.py --server http://192.168.29.209:5001

Flags:
    --model    tiny|base|small|medium|large  (default: base)
    --mic      device index                  (default: 1 = MacBook Air mic)
    --list-mics                              show all input devices and exit
"""

import argparse
import difflib
import os
import tempfile
import time
import uuid
import subprocess

import numpy as np
import requests
import scipy.io.wavfile as wav
import sounddevice as sd
import whisper

# ─────────────────────────── constants ──────────────────────────────────────
WAKE_PHRASE   = "hey robi"
LISTEN_SEC    = 5.0          # window to catch "hey robi <question>"
QUERY_SEC     = 6.0          # follow-up window if no question in wake clip
RMS_THRESHOLD = 150          # skip silent clips (stops hallucinations)
FUZZY_THRESH  = 0.55         # 0=anything matches, 1=exact only
COOLDOWN_SEC  = 5            # ignore re-triggers after speaking

# Whisper prompt — primes the model to expect these words
WHISPER_PROMPT = "Hey Robi, tell me about"

# ─────────────────────────── audio helpers ───────────────────────────────────

def record_clip(duration: float, fs: int = 16000, device: int | None = None):
    """Record audio. Returns (wav_path, rms_level)."""
    fd, path = tempfile.mkstemp(suffix=".wav")
    os.close(fd)
    audio = sd.rec(int(duration * fs), samplerate=fs, channels=1, dtype="int16", device=device)
    sd.wait()
    rms = int(np.sqrt(np.mean(audio.astype(np.float32) ** 2)))
    wav.write(path, fs, audio)
    return path, rms


def transcribe(model, path: str) -> str:
    """Transcribe wav file with Whisper, biased toward the wake phrase."""
    result = model.transcribe(path, fp16=False, initial_prompt=WHISPER_PROMPT)
    return result.get("text", "").strip()


def is_wake_word(text: str, wake: str = WAKE_PHRASE, threshold: float = FUZZY_THRESH) -> bool:
    """
    Fuzzy sliding-window match.
    Returns True if text contains something close enough to the wake phrase.
    Catches: 'hero be', 'hey roby', 'HEROBUIT', 'hey Robi' etc.
    """
    lower = text.lower()
    # Fast exact check first
    if wake in lower:
        return True

    words   = lower.split()
    w_words = wake.split()
    n       = len(w_words)
    for i in range(max(1, len(words) - n + 1)):
        window = " ".join(words[i : i + n])
        ratio  = difflib.SequenceMatcher(None, window, wake).ratio()
        if ratio >= threshold:
            print(f"  🔍 Fuzzy match: {window!r} ≈ {wake!r}  (score={ratio:.2f})")
            return True
    return False


def extract_query(text: str, wake: str = WAKE_PHRASE) -> str:
    """
    Pull out everything said AFTER the wake phrase in the same clip.
    Works even if Whisper mis-spelled the wake phrase.
    """
    lower = text.lower()
    words   = lower.split()
    w_words = wake.split()
    n       = len(w_words)

    best_idx = -1
    best_ratio = 0.0
    for i in range(max(1, len(words) - n + 1)):
        window = " ".join(words[i : i + n])
        ratio  = difflib.SequenceMatcher(None, window, wake).ratio()
        if ratio > best_ratio:
            best_ratio = ratio
            best_idx   = i

    if best_idx >= 0:
        # Return original-case text after the matched window
        orig_words = text.split()
        after = " ".join(orig_words[best_idx + n:]).strip(" ,.-!?")
        return after
    return ""


# ─────────────────────────── I/O helpers ─────────────────────────────────────

def speak(text: str):
    """Speak via macOS `say` → goes to default audio output (Bluetooth speaker)."""
    print(f"  🔊 Speaking: {text!r}")
    try:
        subprocess.run(["say", "-r", "175", text], check=True)
    except Exception as e:
        print(f"  ⚠️  TTS error: {e}")


def ask_ai(server: str, query: str, session_id: str) -> str | None:
    """POST /chat and return the AI reply string."""
    try:
        r = requests.post(
            f"{server}/chat",
            json={"message": query, "session_id": session_id},
            timeout=30,
        )
        if r.status_code != 200:
            print(f"  ⚠️  /chat → {r.status_code}: {r.text}")
            return None
        reply = r.json().get("reply", "").strip()
        print(f"  🤖 AI: {reply!r}")
        return reply
    except Exception as e:
        print(f"  ⚠️  /chat error: {e}")
        return None


def push_display(server: str, session_id: str, text: str):
    """POST /display so the ESP32 can show the reply on its OLED."""
    try:
        r = requests.post(
            f"{server}/display",
            json={"session_id": session_id, "text": text},
            timeout=5,
        )
        if r.status_code in (200, 204):
            print("  📺 Sent to ESP32 display.")
        else:
            print(f"  ⚠️  /display → {r.status_code}")
    except Exception as e:
        print(f"  ⚠️  /display error: {e}")


def push_state(server: str, state: str):
    """Tell ESP32 what animation to play: listening, thinking, idle."""
    try:
        requests.post(f"{server}/state", json={"state": state}, timeout=3)
        print(f"  🎭 State → {state}")
    except Exception:
        pass


# ─────────────────────────── main loop ───────────────────────────────────────

def main(server: str, model_name: str, mic_device: int | None):
    server = server.rstrip("/")
    dev_name = sd.query_devices(mic_device)["name"] if mic_device is not None else "system default"

    print(f"\n{'='*52}")
    print(f"  🤖  Robi — Wake Word Listener")
    print(f"  Server : {server}")
    print(f"  Mic    : [{mic_device}] {dev_name}")
    print(f"  Model  : {model_name}")
    print(f"  Wake   : \"{WAKE_PHRASE}\"  (fuzzy, threshold={FUZZY_THRESH})")
    print(f"  RMS    : skip clips below {RMS_THRESHOLD}")
    print(f"{'='*52}\n")

    print("⏳ Loading Whisper … (downloads on first run)")
    model = whisper.load_model(model_name)
    print("✅ Whisper ready.\n")

    print(f'👂 Say  "Hey Robi <your question>"  to activate.\n')

    cooldown_until = 0.0

    while True:
        # ── 1. Record a clip ──────────────────────────────────────────────────
        path, rms = record_clip(LISTEN_SEC, device=mic_device)

        # ── 2. Skip silent / noise-only clips ─────────────────────────────────
        if rms < RMS_THRESHOLD:
            print(f"  🤫 Silent (RMS={rms}), skipping.\n")
            try:
                os.remove(path)
            except OSError:
                pass
            continue

        # ── 3. Transcribe ─────────────────────────────────────────────────────
        text = transcribe(model, path)
        try:
            os.remove(path)
        except OSError:
            pass

        print(f"  📝 Heard: {text!r}  (RMS={rms})")

        if not text:
            continue

        # ── 4. Check for wake word ────────────────────────────────────────────
        if not is_wake_word(text):
            print(f"  💤 No wake word, ignoring.\n")
            continue

        if time.time() < cooldown_until:
            print("  ⏸  Cooldown active.\n")
            continue

        print(f"\n🟢 Wake word detected!")
        push_state(server, "listening")   # 🎭 eyes light up

        # ── 5. Extract query from same clip ───────────────────────────────────
        query = extract_query(text)

        if query and len(query.split()) >= 2:
            print(f"  ❓ Question (same clip): {query!r}")
        else:
            # Record a follow-up question clip
            print(f"  ❓ Listening for your question ({QUERY_SEC}s) …")
            q_path, q_rms = record_clip(QUERY_SEC, device=mic_device)
            if q_rms < RMS_THRESHOLD:
                print("  ⚠️  No speech heard. Try: 'Hey Robi what time is it?'\n")
                push_state(server, "idle")
                try:
                    os.remove(q_path)
                except OSError:
                    pass
                continue
            query = transcribe(model, q_path)
            try:
                os.remove(q_path)
            except OSError:
                pass

        if not query:
            print("  ⚠️  No question detected. Try again.\n")
            push_state(server, "idle")
            continue

        # ── 6. Ask AI ─────────────────────────────────────────────────────────
        push_state(server, "thinking")  # 🎭 eyes look up, dots animation
        session_id = str(uuid.uuid4())
        print(f"  📡 Sending: {query!r}")
        reply = ask_ai(server, query, session_id)

        if not reply:
            speak("Sorry, I couldn't get a response.")
            push_state(server, "idle")
            cooldown_until = time.time() + 3
            continue

        # ── 7. Speak + display ────────────────────────────────────────────────
        # /chat already stored the reply in latest_reply for ESP32 to pick up.
        speak(reply)

        cooldown_until = time.time() + COOLDOWN_SEC
        push_state(server, "idle")      # 🎭 back to chill eyes
        print(f"\n👂 Listening again …\n")


# ─────────────────────────── entry point ─────────────────────────────────────

if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Robi wake-word listener")
    p.add_argument("--server", required=True, help="Flask server URL e.g. http://192.168.29.209:5001")
    p.add_argument("--model",  default="base", choices=["tiny","base","small","medium","large"])
    p.add_argument("--mic",    type=int, default=1, help="Mic device index (default 1 = MacBook Air mic)")
    p.add_argument("--list-mics", action="store_true", help="List audio input devices and exit")
    args = p.parse_args()

    if args.list_mics:
        print("\nAudio input devices:")
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0:
                tag = ">>>" if i == sd.default.device[0] else "   "
                print(f"  {tag} [{i}] {d['name']}")
        raise SystemExit(0)

    main(server=args.server, model_name=args.model, mic_device=args.mic)
