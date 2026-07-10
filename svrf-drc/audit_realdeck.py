import sys, time
from collections import Counter
root = globals().get("root"); deck_path = globals().get("deck")
sys.path.insert(0, root)
import pya
from svrf_klayout.svrf_parse import parse_deck
from svrf_klayout.run_svrf_drc import Engine

text = open(deck_path, encoding="utf-8", errors="replace").read()
deck = parse_deck(text)
ly = pya.Layout(); ly.dbu=0.001; top=ly.create_cell("TOP")
for (num,dt) in deck.layers.values(): ly.layer(num,dt)
gds="/tmp/empty_layers.gds"; ly.write(gds)

t0=time.time()
eng = Engine(gds, text)
res = eng.execute()
t1=time.time()

dv = Counter(v for v,_,_ in res)
derivs = eng.deck.derivations
built = sum(1 for d in derivs if d.supported and not d.edge_typed)
edge  = sum(1 for d in derivs if d.edge_typed)
notb  = sum(1 for d in derivs if not d.supported)
print(f"derivations: {len(derivs)}  built={built}  edge-typed={edge}  not-built={notb}")
nbreasons=Counter(d.reason.split(':')[0][:34] for d in derivs if not d.supported)
for r,c in nbreasons.most_common(6): print(f"    not-built: {c:5d}  {r}")
print(f"\nstatements executed: {len(res)}  ({t1-t0:.1f}s)")
for v,c in dv.most_common(): print(f"    {v:6s} {c}")
# skip reasons
skips=Counter(str(i)[:46] for v,r,i in res if v=="SKIP")
print("  SKIP reasons (top):")
for s,c in skips.most_common(8): print(f"    {c:5d}  {s}")
errs=Counter(str(i)[:60] for v,r,i in res if v=="ERROR")
if errs:
    print("  ERROR sigs (top):")
    for e,c in errs.most_common(6): print(f"    {c:4d}  {e}")
clean=dv.get('PASS',0)+dv.get('FAIL',0)
print(f"\nEXECUTES-CLEAN (PASS+FAIL): {clean}/{len(res)} = {100*clean/len(res):.1f}%")
