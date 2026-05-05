"""
Laptop client for the Robi workflow.
- Polls /next_trigger on the server.
- When a trigger is received, records a short audio chunk and transcribes using Whisper.
- If wake word "hey robi" is detected, records the user's query, transcribes it, POSTs to /chat with session_id.
- Receives AI reply, speaks via pyttsx3 (system default audio device -> your Bluetooth speaker if set as default), and POSTs /display so the ESP32 can fetch the text.

Usage (Windows cmd.exe):
C:/Users/abhin/AppData/Local/Programs/Python/Python311/python.exe laptop_client.py --server http://192.168.29.202:5000

Notes:
- Make sure your Bluetooth speaker is paired and set as the Windows default output device before running this script.
- Whisper model download may occur on first run (it can be large). Use --model tiny for much faster startup (lower accuracy).
"""

import argparse
import time
import requests
import sounddevice as sd
import scipy.io.wavfile as wav
import tempfile
import whisper
import pyttsx3
import os


def record_audio(duration=3, fs=16000, filename=None):
    if filename is None:
        fd, filename = tempfile.mkstemp(suffix='.wav')
        os.close(fd)
    print(f"Recording {duration}s -> {filename}")
    audio = sd.rec(int(duration * fs), samplerate=fs, channels=1, dtype='int16')
    sd.wait()
    wav.write(filename, fs, audio)
    return filename


def transcribe_file(model, filename):
    print('Transcribing...')
    res = model.transcribe(filename)
    text = res.get('text', '')
    print('Transcribed:', text)
    return text.strip()


def speak_text(engine, text):
    print('Speaking:', text)
    engine.say(text)
    engine.runAndWait()


def main(server, model_name='small'):
    server = server.rstrip('/')
    print('Using server:', server)

    print('Loading Whisper model (this may download weights)...')
    model = whisper.load_model(model_name)
    print('Model loaded:', model_name)

    engine = pyttsx3.init()

    poll_interval = 1.0
    while True:
        try:
            r = requests.get(f"{server}/next_trigger", timeout=5)
        except requests.RequestException as e:
            print('Error polling server:', e)
            time.sleep(poll_interval)
            continue

        if r.status_code == 204:
            time.sleep(poll_interval)
            continue

        if r.status_code != 200:
            print('Unexpected status from /next_trigger:', r.status_code, r.text)
            time.sleep(poll_interval)
            continue

        item = r.json()
        session_id = item.get('session_id')
        print('Got trigger, session_id=', session_id)

        # Record short clip for wake word
        wake_file = record_audio(duration=3)
        wake_text = transcribe_file(model, wake_file).lower()

        if 'hey robi' not in wake_text:
            print('Wake word not detected (heard: "%s"). Ignoring.' % wake_text)
            continue

        print('Wake word detected! Recording user query...')
        q_file = record_audio(duration=5)
        query = transcribe_file(model, q_file)

        if not query:
            print('No query detected. Skipping.')
            continue

        # Send to server /chat
        try:
            payload = {'message': query, 'session_id': session_id}
            rc = requests.post(f"{server}/chat", json=payload, timeout=20)
            if rc.status_code != 200:
                print('/chat returned', rc.status_code, rc.text)
                continue
            data = rc.json()
            reply = data.get('reply')
            if not reply:
                print('No reply in /chat response:', data)
                continue

            # Play reply via system default audio (set your Bluetooth speaker as default)
            speak_text(engine, reply)

            # Post display for ESP32
            try:
                dd = {'session_id': session_id, 'text': reply}
                dresp = requests.post(f"{server}/display", json=dd, timeout=5)
                if dresp.status_code not in (200, 204):
                    print('/display returned', dresp.status_code, dresp.text)
            except Exception as e:
                print('Error posting /display:', e)

        except Exception as e:
            print('Error talking to server /chat:', e)

        # small cooldown
        time.sleep(1)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--server', required=True, help='Server base URL, e.g. http://192.168.29.202:5000')
    p.add_argument('--model', default='small', help='Whisper model name (tiny, base, small, medium, large)')
    args = p.parse_args()
    main(args.server, model_name=args.model)
