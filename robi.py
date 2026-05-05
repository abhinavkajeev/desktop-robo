from flask import Flask, request, jsonify
import requests
import os
from dotenv import load_dotenv
import whisper
import sounddevice as sd
import scipy.io.wavfile as wav
import pyttsx3
import uuid
from threading import Lock

# ===== LOAD ENV =====
load_dotenv()

app = Flask(__name__)

API_KEY = os.getenv("OPENROUTER_API_KEY")

model = whisper.load_model("base")
engine = pyttsx3.init()

if not API_KEY:
    print("❌ API KEY NOT FOUND")
else:
    print("✅ API KEY LOADED")

# ===== RECORD AUDIO =====
def record_audio(duration=4, filename="input.wav"):
    fs = 16000
    print("🎤 Listening...")
    audio = sd.rec(int(duration * fs), samplerate=fs, channels=1)
    sd.wait()
    wav.write(filename, fs, audio)
    return filename

# ===== AI CALL =====
def ask_ai(user_msg):
    res = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={
            "Authorization": f"Bearer {API_KEY}",
            "Content-Type": "application/json"
        },
        json={
            "model": "openai/gpt-3.5-turbo",
            "messages": [
                {"role": "system", "content": "You are a cute AI companion robot."},
                {"role": "user", "content": user_msg}
            ]
        },
        timeout=15
    )

    data = res.json()
    return data["choices"][0]["message"]["content"]

# ===== SERVER QUEUE / LAPTOP WORKFLOW =====

# In-memory trigger queue and display store
trigger_queue = []
queue_lock = Lock()
display_store = {}
display_lock = Lock()


@app.route('/trigger', methods=['POST'])
def trigger():
    # ESP32 calls this when voice is detected
    payload = request.json or {}
    session_id = payload.get('session_id') or str(uuid.uuid4())

    with queue_lock:
        trigger_queue.append({'session_id': session_id})

    print('Trigger received, session_id=', session_id)
    return jsonify({'session_id': session_id}), 200


@app.route('/next_trigger', methods=['GET'])
def next_trigger():
    # Laptop polls this endpoint to see if there's a pending trigger
    with queue_lock:
        if not trigger_queue:
            return ('', 204)
        item = trigger_queue.pop(0)

    return jsonify(item), 200


@app.route('/chat', methods=['POST'])
def chat():
    # Laptop posts: { "message": "...", "session_id": "..." }
    try:
        data = request.json or {}
        user_msg = data.get('message', '')
        session_id = data.get('session_id')

        print('Chat request:', user_msg, 'session:', session_id)

        reply = ask_ai(user_msg)

        # Optionally store reply for ESP32 to fetch
        if session_id:
            with display_lock:
                display_store[session_id] = reply

        return jsonify({'reply': reply}), 200

    except Exception as e:
        print('ERROR in /chat:', e)
        return jsonify({'error': str(e)}), 500


@app.route('/display', methods=['POST'])
def display():
    # Laptop posts final text for ESP32 to display: { session_id, text }
    data = request.json or {}
    session_id = data.get('session_id')
    text = data.get('text', '')
    if not session_id:
        return jsonify({'error': 'missing session_id'}), 400

    with display_lock:
        display_store[session_id] = text

    print('Display stored for', session_id)
    return ('', 204)


@app.route('/latest_display', methods=['GET'])
def latest_display():
    session_id = request.args.get('session_id')
    if not session_id:
        return jsonify({'error': 'missing session_id'}), 400

    with display_lock:
        text = display_store.get(session_id)
        if not text:
            return ('', 204)
        # Optionally delete after fetching
        del display_store[session_id]

    return jsonify({'text': text}), 200

# ===== RUN =====
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)