import subprocess, sys
cmds = [
  ['git','status','--porcelain'],
  ['git','rev-parse','--abbrev-ref','HEAD'],
  ['git','checkout','-B','esp32-api'],
  ['git','add','-A'],
  ['git','commit','-m','Add laptop client and ESP32 trigger/display flow; updated server endpoints and ESP32 sketch'],
  ['git','push','-u','origin','esp32-api']
]
for c in cmds:
  try:
    print('>',' '.join(c))
    p = subprocess.run(c, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print('returncode=', p.returncode)
    if p.stdout:
      print('STDOUT:\n', p.stdout)
    if p.stderr:
      print('STDERR:\n', p.stderr)
  except Exception as e:
    print('EXC', e)
    break
