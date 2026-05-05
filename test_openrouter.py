import os
import requests
from dotenv import load_dotenv

load_dotenv()
API_KEY = os.getenv('OPENROUTER_API_KEY')

if not API_KEY:
    print('NO_KEY')
else:
    try:
        headers = {
            'Authorization': f'Bearer {API_KEY}',
            'Content-Type': 'application/json'
        }
        payload = {
            'model': 'openai/gpt-3.5-turbo',
            'messages': [
                {'role': 'system', 'content': 'You are a cute AI companion robot.'},
                {'role': 'user', 'content': 'Hello robot'}
            ]
        }
        r = requests.post('https://openrouter.ai/api/v1/chat/completions', headers=headers, json=payload, timeout=20)
        print('STATUS', r.status_code)
        print(r.text)
    except Exception as e:
        print('EXC', repr(e))
