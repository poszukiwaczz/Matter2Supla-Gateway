import re, ssl, urllib.request, json
from pathlib import Path

b = sorted(Path(r"M:\Matter2Supla-Gemini-Version\backups").glob("before-033gemini2-serial-*"))[-1]
log = (b / "boot-uart-private.log").read_text(encoding="utf-8")
token = re.search(r"Panel access token: ([0-9a-f]{64})", log).group(1)
ip = "192.168.1.191"
ctx = ssl._create_unverified_context()

def get(p, raw=False):
    req = urllib.request.Request(f"https://{ip}{p}", headers={"Host": ip, "Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, context=ctx, timeout=8) as r:
        data = r.read().decode("utf-8", errors="replace")
        return data if raw else json.loads(data)

st = get("/api/status")
dg = get("/api/gateway/diagnostics")
dv = get("/api/devices")
lg = get("/api/logs", raw=True)
print("=== STATUS ===", json.dumps(st, indent=2))
print("=== DIAGNOSTICS ===", json.dumps(dg, indent=2))
print("=== DEVICES SUMMARY ===")
for d in dv["devices"]:
    t = d.get("telemetry", {})
    print(d["node"], d["name"], "sub:", t.get("subscription_active"), "reports:", t.get("report_count"), "last_age:", t.get("last_report_age_s"), "thread:", t.get("thread"))
print("=== /api/logs ===")
print(lg)
