import requests
s='http://127.0.0.1:5000'
print('POST /trigger')
r=requests.post(s+'/trigger', json={})
print(r.status_code, r.text)
print('GET /next_trigger')
r=requests.get(s+'/next_trigger')
print(r.status_code, r.text)
print('POST /chat')
if r.status_code==200:
    sid=r.json().get('session_id')
else:
    sid=None
print('using sid', sid)
r=requests.post(s+'/chat', json={'message':'Hello from test', 'session_id':sid})
print(r.status_code, r.text)
print('GET /latest_display')
r=requests.get(s+'/latest_display', params={'session_id':sid})
print(r.status_code, r.text)
