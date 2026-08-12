#!/usr/bin/env bash
#
# run_shipped_engines_test.sh -- every standalone sign-off engine this fork adds
# must be DECLARED, and the image must carry the ones declared shipped.
#
# vibeic-prereq: self
# (the tree half always runs; the image half is attempted and reports its own
#  named partial-skip when no image is reachable)
#
# THE DEFECT
# ----------
# Twelve standalone engine entry points -- antenna_check.py, metal_fill.py,
# metal_cheese.py, caa.py, mp_color.py, lvs_recon.py, pattern_match.py,
# perc_latchup.py, cmp_gradient.py, xcheck_router.py, gds_antenna_deck_check.py,
# metal_fill_emit.py -- were in `git log` and in no shipped artefact. The publish
# stage of tools/klayout/Dockerfile is `FROM scratch` + `COPY --from=build
# /klayout/bld`, and /klayout/bld is build.sh's OUTPUT directory; build.sh never
# installs repo-root Python. Measured on the released image 0.2.89, built from
# this fork's HEAD: all twelve absent, and $VIBEIC_KLAYOUT_TOOLS unset.
#
# TWO HALVES, BOTH REAL
# ---------------------
#   TREE  -- plugin-wiring/SHIPPED_ENGINES.json declares every engine directory
#            and its entry points; this asserts they exist, and that no engine
#            directory in the tree is UNDECLARED. An engine added next to its
#            feature and shipped by nothing is how all twelve got here, so a new
#            directory must say ship=true or ship=false with a reason.
#   IMAGE -- if an image is reachable AND was built from a fork ref that carries
#            this declaration, every entry point declared ship=true must be
#            inside it under $VIBEIC_KLAYOUT_TOOLS, and the variable must be set.
#            This is the half that would have caught the defect.
#
#            The ref check is not a loophole, it is what stops a permanent
#            false-red: an image built before the declaration existed cannot
#            honour it, and blaming it would leave the suite red for something
#            no commit in this repository can fix. The image says which fork ref
#            it was built from -- /vibeic/provenance/klayout.json -- and an
#            older one produces a NAMED skip naming the rebuild that would
#            answer the question. Measured all three ways 2026-08-12:
#              published 0.2.89 (ref 1d657c0b7, predates it)  -> named SKIP
#              same image, provenance rewritten to a ref that
#                carries it, engines still absent               -> FAIL, all 12
#              image built with the tools/klayout/Dockerfile
#                staging step                                   -> PASS
#
# Env: EDA_IMAGE (default ghcr.io/vibeic/vibeic-eda:latest)
# Exit: 0 PASS   1 FAIL
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NAME=run_shipped_engines_test
MANIFEST="$ROOT/plugin-wiring/SHIPPED_ENGINES.json"

[ -f "$MANIFEST" ] || { echo "FAIL $NAME: $MANIFEST is missing from the tree"; exit 1; }

rc=0

# ---------------------------------------------------------------- tree half --
tree_report="$(python3 - "$ROOT" "$MANIFEST" <<'PY'
import json, os, sys, glob
root, manifest = sys.argv[1], sys.argv[2]
m = json.load(open(manifest))
engines = m.get("engines") or []
if not engines:
    print("FAIL: SHIPPED_ENGINES.json declares no engines"); sys.exit(1)
declared = {e["dir"] for e in engines} | {d["dir"] for d in m.get("not_engines") or []}
bad = []
for e in engines:
    d = os.path.join(root, e["dir"])
    if not os.path.isdir(d):
        bad.append("%s: declared but not in the tree" % e["dir"]); continue
    for entry in e.get("entry") or []:
        if not os.path.isfile(os.path.join(d, entry)):
            bad.append("%s/%s: declared entry point missing" % (e["dir"], entry))
    if not e.get("ship") and not e.get("why"):
        bad.append("%s: ship=false with no reason" % e["dir"])
# Anything that LOOKS like a standalone engine: a top-level dir with its own
# tests/ and its own *.py. That is what every one of the twelve looks like.
found = set()
for d in sorted(os.listdir(root)):
    p = os.path.join(root, d)
    if not os.path.isdir(p) or d.startswith("."):
        continue
    if os.path.isdir(os.path.join(p, "tests")) and glob.glob(os.path.join(p, "*.py")):
        found.add(d)
for d in sorted(found - declared):
    bad.append("%s: looks like a standalone engine (own tests/ and *.py) but is "
               "in neither `engines` nor `not_engines`, so nothing decides "
               "whether the image should carry it" % d)
print("  declared engines: %d (%d shipped)   engine-shaped dirs in the tree: %d"
      % (len(engines), sum(1 for e in engines if e.get("ship")), len(found)))
for b in bad:
    print("  ! " + b)
sys.exit(1 if bad else 0)
PY
)"; tree_rc=$?
printf '%s\n' "$tree_report"
[ $tree_rc -eq 0 ] || { echo "FAIL $NAME (tree)"; rc=1; }

# --------------------------------------------------------------- image half --
IMG="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:latest}"
have_image=0
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  docker image inspect "$IMG" >/dev/null 2>&1 && have_image=1
  [ $have_image -eq 0 ] && docker pull "$IMG" >/dev/null 2>&1 && have_image=1
fi

# An image built BEFORE this declaration existed cannot be blamed for not
# honouring it, and calling that a failure would leave the suite permanently red
# for a reason nobody can act on from this repository. So the image half asks the
# image what fork ref it was built from -- /vibeic/provenance/klayout.json, which
# the tool image writes into itself -- and only judges an image whose ref
# actually carries plugin-wiring/SHIPPED_ENGINES.json. Anything else is a NAMED
# skip that says which rebuild would answer the question.
ref=""
if [ $have_image -eq 1 ]; then
  ref="$(docker run --rm --entrypoint /bin/bash "$IMG" -c \
        'cat /vibeic/provenance/klayout.json 2>/dev/null' 2>/dev/null \
        | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("ref",""))
except Exception: print("")' 2>/dev/null)"
  if [ -z "$ref" ]; then
    echo "  SKIP image checks: $IMG carries no /vibeic/provenance/klayout.json, so"
    echo "       there is no way to tell which fork state it was built from."
    have_image=0
  elif ! git -C "$ROOT" cat-file -e "$ref:plugin-wiring/SHIPPED_ENGINES.json" 2>/dev/null; then
    echo "  SKIP image checks: $IMG was built from klayout $ref, which predates"
    echo "       plugin-wiring/SHIPPED_ENGINES.json. Rebuild the tool image from a"
    echo "       ref that carries it (and with the tools/klayout/Dockerfile staging"
    echo "       step) to make this half answerable."
    have_image=0
  fi
fi

if [ $have_image -eq 0 ]; then
  [ -n "$ref" ] || echo "  SKIP image checks: no reachable EDA image ($IMG)"
  echo "       -- the tree half above still ran and still decides this suite's verdict."
else
  echo "  image $IMG was built from klayout $ref, which carries the declaration"
  want="$(python3 - "$MANIFEST" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
for e in m.get("engines") or []:
    if e.get("ship"):
        for entry in e.get("entry") or []:
            print("%s/%s" % (e["dir"], entry))
PY
)"
  out="$(docker run --rm --entrypoint /bin/bash "$IMG" -c '
    root="${VIBEIC_KLAYOUT_TOOLS:-}"
    echo "VIBEIC_KLAYOUT_TOOLS=[${root:-<unset>}]"
    [ -n "$root" ] && [ -d "$root" ] && (cd "$root" && find . -name "*.py" | sed "s#^\./##" | sort)
  ' 2>&1)"
  printf '  %s\n' "$(printf '%s' "$out" | head -n 1)"
  case "$out" in
    *"VIBEIC_KLAYOUT_TOOLS=[<unset>]"*)
      echo "  ! the image does not set \$VIBEIC_KLAYOUT_TOOLS, so nothing can find"
      echo "    the engines even if they are in it"
      rc=1 ;;
  esac
  missing=""
  for f in $want; do
    case "$out" in *"$f"*) ;; *) missing="$missing $f" ;; esac
  done
  if [ -n "$missing" ]; then
    echo "  ! the image does not carry these engines declared ship=true:"
    for f in $missing; do echo "      $f"; done
    rc=1
  else
    echo "  every engine declared ship=true is present in $IMG"
  fi
fi

[ $rc -eq 0 ] && echo "PASS $NAME" || echo "FAIL $NAME"
exit $rc
