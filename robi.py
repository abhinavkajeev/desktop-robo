"""
Robi Flask Server
=================
Responsibilities:
  - /chat         → receives transcribed query from laptop_client,
                    calls OpenRouter AI, returns reply JSON,
                    and stores reply for ESP32 to fetch.
  - /display      → laptop_client posts reply text here for ESP32.
  - /latest_display → ESP32 polls this to fetch the latest reply text.

The server does NOT do audio recording or TTS — that is laptop_client.py's job.

Run:
    python robi.py
"""

import os
import uuid
from threading import Lock

import requests
from dotenv import load_dotenv
from flask import Flask, jsonify, request

# ===== ENV =====
load_dotenv()
API_KEY = os.getenv("OPENROUTER_API_KEY")

if not API_KEY:
    print("❌  OPENROUTER_API_KEY not found in .env — /chat will fail!")
else:
    print("✅  OPENROUTER_API_KEY loaded.")

app = Flask(__name__)

# ===== IN-MEMORY STORE =====
# Maps session_id → AI reply text (consumed once by ESP32)
display_store: dict[str, str] = {}
# Most recent reply for ESP32 to poll without session tracking
latest_reply: dict = {"text": "", "fresh": False}
# Robot mood state — ESP32 polls to know what animation to play
robot_state: dict = {"state": "idle", "changed": False}
display_lock = Lock()


# ===== AI CALL =====
def ask_ai(user_msg: str) -> str:
    """Call OpenRouter and return the AI assistant reply."""
    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={
            "Authorization": f"Bearer {API_KEY}",
            "Content-Type": "application/json",
        },
        json={
            "model": "openai/gpt-3.5-turbo",
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "You are Robi, a friendly and curious desktop robot companion. "
                        "Keep your answers concise — 2-3 sentences max — because they "
                        "will be read aloud and shown on a small OLED screen."
                    ),
                },
                {"role": "user", "content": user_msg},
            ],
        },
        timeout=20,
    )
    resp.raise_for_status()
    return resp.json()["choices"][0]["message"]["content"].strip()


# ===== ROUTES =====

@app.route("/chat", methods=["POST"])
def chat():
    """
    POST body: { "message": "...", "session_id": "..." }
    Returns:   { "reply": "..." }
    Also stores the reply so ESP32 can fetch it via /latest_display.
    """
    try:
        data = request.get_json(force=True) or {}
        user_msg = data.get("message", "").strip()
        session_id = data.get("session_id") or str(uuid.uuid4())

        if not user_msg:
            return jsonify({"error": "message is empty"}), 400

        print(f"📩  /chat  session={session_id}  msg={user_msg!r}")

        reply = ask_ai(user_msg)
        print(f"🤖  AI reply: {reply!r}")

        # Store for ESP32 (both session-based and latest)
        with display_lock:
            display_store[session_id] = reply
            latest_reply["text"] = reply
            latest_reply["fresh"] = True

        return jsonify({"reply": reply, "session_id": session_id}), 200

    except requests.HTTPError as e:
        print(f"❌  OpenRouter HTTP error: {e}")
        return jsonify({"error": "AI service error", "detail": str(e)}), 502

    except Exception as e:
        print(f"❌  /chat error: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/display", methods=["POST"])
def display():
    """
    POST body: { "session_id": "...", "text": "..." }
    Stores text for ESP32 to fetch.
    """
    data = request.get_json(force=True) or {}
    session_id = data.get("session_id")
    text = data.get("text", "")

    if not session_id:
        return jsonify({"error": "missing session_id"}), 400

    with display_lock:
        display_store[session_id] = text
        latest_reply["text"] = text
        latest_reply["fresh"] = True

    print(f"📺  /display stored for session={session_id}")
    return ("", 204)


@app.route("/latest_display", methods=["GET"])
def latest_display():
    """
    GET ?session_id=...
    Returns { "text": "..." } or 204 if nothing ready yet.
    Deletes the entry after returning it (consumed once).
    """
    session_id = request.args.get("session_id")
    if not session_id:
        return jsonify({"error": "missing session_id"}), 400

    with display_lock:
        text = display_store.get(session_id)
        if not text:
            return ("", 204)
        del display_store[session_id]

    return jsonify({"text": text}), 200


@app.route("/latest", methods=["GET"])
def latest():
    """
    GET /latest — ESP32 polls this (no session_id needed).
    Returns { "text": "..." } if a fresh reply exists, else 204.
    Marks the reply as consumed so ESP32 only gets it once.
    """
    with display_lock:
        if not latest_reply.get("fresh"):
            return ("", 204)
        text = latest_reply["text"]
        latest_reply["fresh"] = False
    return jsonify({"text": text}), 200


@app.route("/state", methods=["GET", "POST"])
def state():
    """
    POST body: { "state": "listening" | "thinking" | "idle" }
        → Laptop pushes Robi's current mood.
    GET  → ESP32 polls. Returns { "state": "..." } if changed, else 204.
    """
    if request.method == "POST":
        data = request.get_json(force=True) or {}
        new_state = data.get("state", "idle")
        with display_lock:
            robot_state["state"]   = new_state
            robot_state["changed"] = True
        print(f"🎭  State → {new_state}")
        return ("", 204)

    # GET
    with display_lock:
        if not robot_state.get("changed"):
            return ("", 204)
        s = robot_state["state"]
        robot_state["changed"] = False
    return jsonify({"state": s}), 200


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


# ===== RUN =====
if __name__ == "__main__":
    print("🚀  Starting Robi server on 0.0.0.0:5001 …")
    app.run(host="0.0.0.0", port=5001, debug=False)