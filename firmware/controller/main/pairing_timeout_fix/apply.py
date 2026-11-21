import hashlib,json,os,re,sys
from pathlib import Path
build,overlay,sdk=(Path(x).resolve() for x in sys.argv[1:4])
for name,expected in json.loads(overlay.with_suffix('.json').read_text())['upstream_sha256'].items():
 if hashlib.sha256((sdk/name).read_bytes()).hexdigest()!=expected:raise SystemExit('Review pairing timeout correction after SDK change: '+name)
matches=[]
for p in build.rglob('*.ninja'):
 text=p.read_text()
 pattern=r'(?m)^(build [^\r\n]*SetUpCodePairer\.cpp\.o: cxx )(\S+)(.*)$'
 if re.search(pattern,text):matches.append((p,text,pattern))
if len(matches)!=1:raise SystemExit('Expected exactly one SetUpCodePairer compile rule')
p,text,pattern=matches[0]
replacement=os.path.relpath(overlay,build).replace('\\','/').replace('$','$$').replace(' ','$ ').replace(':','$:')
new=re.sub(pattern,lambda m:m[1]+replacement+m[3],text)
if new!=text:p.write_text(new)
print('CHIP uses project-local discovery timeout completion correction')
