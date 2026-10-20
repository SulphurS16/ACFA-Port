from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess,json,time
s=Path(__file__).resolve().parents[1]
r=s.parent;out=r/'local/native-source-audit';out.mkdir(parents=True,exist_ok=True)
# Renderer/UI/device Windows backends are a separately identified integration task.
files=sorted(p for p in (s/'src/runtime').rglob('*.cpp') if 'gpu' not in p.parts and 'ui' not in p.parts)
excluded={'main.cpp','bindings.cpp','gamepads.cpp','input.cpp','sysdialog.cpp','audio_out.cpp','input_hooks.cpp','menu_mouse.cpp'}
files=[p for p in files if p.name not in excluded]
flags=['clang++','-std=c++20','-fsyntax-only','-fno-strict-aliasing','-DACFA_CONTROLLER_ONLY=1','-DACFA_GAME_JP=1','-DACFA_DEV=0','-I'+str(s/'src'),'-I'+str(s/'third_party/simde')]
def run(p):
    begin=time.time(); proc=subprocess.run(flags+[str(p)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    log=out/(str(p.relative_to(s)).replace('/','__')+'.log');log.write_text(proc.stdout)
    errors=[l.strip() for l in proc.stdout.splitlines() if 'error:' in l]
    return {'path':str(p.relative_to(s)),'passed':proc.returncode==0,'first_error':errors[0] if errors else '', 'seconds':round(time.time()-begin,2),'log':str(log.relative_to(r))}
with ThreadPoolExecutor(max_workers=4) as pool: results=list(pool.map(run,files))
report={'scope':'nongraphics runtime translation-unit Apple ARM64 syntax validation; not link or game acceptance','created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'count':len(results),'passed':sum(x['passed'] for x in results),'failed':sum(not x['passed'] for x in results),'excluded_windows_or_in_progress':sorted(excluded),'results':results}
(r/'reports').mkdir(exist_ok=True)
(r/'reports/native-macos-source-audit.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='results'},ensure_ascii=False))
for x in results:
    if not x['passed']:print(x['path']+': '+x['first_error'])
