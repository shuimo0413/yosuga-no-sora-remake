#!/usr/bin/env bash
# Final verification for the annotated audit fixes.
set -u
cd "$(dirname "$0")/.."
pass=0; fail=0
ok() { echo "PASS: $1"; pass=$((pass+1)); }
no() { echo "FAIL: $1"; fail=$((fail+1)); }

echo "== 1. Python syntax =="
for f in tools/package_data_release.py tools/generate_notices.py tools/pack_data_xp3.py \
         tools/generate_content_manifest.py tools/setup_ohos_project.py tools/ohos/autosign/autosign.py; do
  python -m py_compile "$f" 2>/dev/null && ok "py_compile $f" || no "py_compile $f"
done

echo "== 2. Workflow YAML =="
for f in .github/workflows/*.yml; do
  python -c "import yaml,io,sys; yaml.safe_load(io.open(sys.argv[1],encoding='utf-8'))" "$f" 2>/dev/null \
    && ok "yaml $f" || no "yaml $f"
done

echo "== 3. JSON5 files parse (app.json5 via regex sanity) =="
grep -q '"bundleName": "com.shuimo0413.yosuganosora.hdremake"' ohos-project/AppScope/app.json5 \
  && ok "app.json5 bundleName" || no "app.json5 bundleName"
grep -q '"versionName": "0.0.0"' ohos-project/AppScope/app.json5 && ok "app.json5 versionName 0.0.0" || no "app.json5 versionName"

echo "== 4. Version extraction behaviour (tag -> numeric, shared by all platforms) =="
ver() {
  printf '%s' "$1" | sed -nE 's/^v?([0-9]+(\.[0-9]+)*).*/\1/p'
}
[[ "$(ver v0.1.0)" == "0.1.0" ]] && ok "v0.1.0 -> 0.1.0" || no "v0.1.0 -> $(ver v0.1.0)"
[[ "$(ver v1.2.3)" == "1.2.3" ]] && ok "v1.2.3 -> 1.2.3" || no "v1.2.3 -> $(ver v1.2.3)"
[[ "$(ver v2)" == "2" ]] && ok "v2 -> 2" || no "v2 -> $(ver v2)"
[[ "$(ver v1.01)" == "1.01" ]] && ok "v1.01 -> 1.01 (leading zero kept)" || no "v1.01 -> $(ver v1.01)"
[[ "$(ver v123)" == "123" ]] && ok "v123 -> 123" || no "v123 -> $(ver v123)"
[[ "$(ver v0.1.0-ohos-x)" == "0.1.0" ]] && ok "v0.1.0-ohos-x -> 0.1.0" || no "v0.1.0-ohos-x -> $(ver v0.1.0-ohos-x)"
[[ "$(ver v1.2.3-test.1)" == "1.2.3" ]] && ok "v1.2.3-test.1 -> 1.2.3" || no "v1.2.3-test.1 -> $(ver v1.2.3-test.1)"
[[ -z "$(ver vabc)" ]] && ok "vabc rejected" || no "vabc -> $(ver vabc)"
python tools/inject_game_version.py --tag v1.2.3-rc.4 --dry-run | grep -q "1.2.3" && ok "inject tool extraction" || no "inject tool extraction"

echo "== 5. Bundle-id consistency =="
want='com.shuimo0413.yosuganosora.hdremake'
grep -q "applicationId = \"$want\"" android-project/app/build.gradle && ok "gradle applicationId" || no "gradle applicationId"
grep -q "namespace = \"$want\"" android-project/app/build.gradle && ok "gradle namespace" || no "gradle namespace"
grep -q "KRKRSDL2_BUNDLE_IDENTIFIER \"$want\"" CMakeLists.txt && ok "CMake default bundle id" || no "CMake default bundle id"
grep -q "IOS_BUNDLE_IDENTIFIER:-$want" ios-project/generate.sh && ok "generate.sh default" || no "generate.sh default"
grep -q "com.lightwinder" ohos-project/AppScope/app.json5 && no "app.json5 legacy id" || ok "app.json5 legacy id gone"

echo "== 6. Actions pinned =="
grep -rn "uses: actions/[a-z-]*@v" .github/workflows/ >/dev/null 2>&1 && no "mutable tag reference left" || ok "no mutable action tags"
n=$(grep -rh "uses: .*@[0-9a-f]\{40\}" .github/workflows/ | wc -l)
[[ "$n" -ge 20 ]] && ok "pinned refs count=$n" || no "pinned refs count=$n"

echo "== 7. Identity leftovers (source tree only) =="
left=$(grep -rln "lightwinder\|WarSkyGod" --include="*.java" --include="*.gradle" --include="*.yml" \
  --include="*.ets" --include="*.cpp" --include="*.h" --include="*.py" --include="*.sh" \
  --include="*.cf" --include="*.xml" --include="*.md" \
  . 2>/dev/null | grep -v "external/" | grep -v "\.hvigor/" | grep -v "entry/build/" \
  | grep -v "entry/\.cxx" | grep -v "\.git/" | grep -v "tools/verify_audit_fixes.sh")
if [[ -z "$left" ]]; then ok "no identity leftovers"; else no "leftovers: $left"; fi

echo "== 8. Icon rename =="
[[ -f icon.ico && ! -f "图标.ico" ]] && ok "icon.ico present, old name gone" || no "icon file state"
grep -rn "图标.ico" .github/workflows/ >/dev/null 2>&1 && no "workflow icon ref stale" || ok "workflow icon refs updated"

echo "== 9. Index.ets split sanity =="
[[ -f ohos-project/entry/src/main/ets/workers/DataWorkers.ets ]] && ok "DataWorkers.ets exists" || no "DataWorkers.ets missing"
grep -q "from '../workers/DataWorkers'" ohos-project/entry/src/main/ets/pages/Index.ets && ok "Index imports workers" || no "Index import missing"
n=$(grep -c "^@Concurrent" ohos-project/entry/src/main/ets/workers/DataWorkers.ets)
[[ "$n" == 6 ]] && ok "6 @Concurrent workers moved" || no "@Concurrent count=$n"
python - <<'EOF' && ok "Index.ets braces balanced" || no "Index.ets braces unbalanced"
import io
t = io.open('ohos-project/entry/src/main/ets/pages/Index.ets', encoding='utf-8').read()
import re
t = re.sub(r'//.*', '', t)
t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
t = re.sub(r"'(?:[^'\\]|\\.)*'", "''", t)
t = re.sub(r'"(?:[^"\\]|\\.)*"', '""', t)
import sys; sys.exit(0 if t.count('{') == t.count('}') and t.count('(') == t.count(')') else 1)
EOF

echo "== 10. debugwin + atomic + zip cap =="
grep -q 'debugwin="\\x6E\\x6F"' "platform/windows-krkrz/Yosuga no Sora-HD Remake.cf" && ok "debugwin=no" || no "debugwin value"
grep -q "std::atomic<bool> gExtractRunning" src/core/sdl2/AndroidDataBridge.cpp && ok "atomic latch" || no "atomic latch"
grep -q "gExtractRunning.exchange(true)" src/core/sdl2/AndroidDataBridge.cpp && ok "exchange test-and-set" || no "exchange"
grep -q "4 GiB extraction limit" src/core/sdl2/zip_extract.cpp && ok "zip 4GiB cap" || no "zip cap"

echo "== 11. notices tool =="
python tools/generate_notices.py --output /tmp/notices-v.txt >/dev/null 2>&1 && ok "generate_notices runs" || no "generate_notices"
grep -q "THIRD-PARTY NOTICES" /tmp/notices-v.txt && ok "notices header" || no "notices header"
n=$(grep -rh "generate_notices" .github/workflows/ | wc -l)
[[ "$n" -ge 4 ]] && ok "notices wired into $n workflow spots" || no "notices wiring=$n"

echo "== 12. BUILD-INFO carries SHA-256 =="
n=$(grep -rl "SHA-256" .github/workflows/ | wc -l)
[[ "$n" -ge 3 ]] && ok "SHA-256 in $n workflows" || no "SHA-256 workflows=$n"

echo
echo "TOTAL: pass=$pass fail=$fail"
[[ "$fail" == 0 ]]
