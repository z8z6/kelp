#!/bin/sh
set -eu

kelp=$1
tmp=${TMPDIR:-/tmp}/kelp-test-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"
cd "$tmp"

"$kelp" --version | grep '^kelp 0.1.0$'

"$kelp" new demo
test -f demo/kelp.toml
test -f demo/src/main.kly

cat > fake-kelyra <<'EOF'
#!/bin/sh
set -eu
output=
if [ "${FAKE_KELYRA_FAIL:-0}" = 1 ]; then exit 1; fi
if [ -n "${FAKE_KELYRA_LOG:-}" ]; then
  printf '%s\n' "$@" > "$FAKE_KELYRA_LOG"
fi
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-o" ]; then
    output=$2
    shift 2
  else
    shift
  fi
done
if [ -n "$output" ]; then
  mkdir -p "$(dirname "$output")"
  printf '#!/bin/sh\nprintf "kelp-run-ok\\n"\n' > "$output"
  chmod +x "$output"
fi
EOF
chmod +x fake-kelyra

fake=$tmp/fake-kelyra
sed -i "s|compiler = \"kelyra\"|compiler = \"$fake\"|" demo/kelp.toml
cd demo
"$kelp" check
"$kelp" build
test -x build/demo
test "$("$kelp" run)" = kelp-run-ok
"$kelp" test

test "$("$kelp" output)" = "$tmp/demo/build/demo"
sed -i 's/optimization = 0/optimization = 3/' kelp.toml
export FAKE_KELYRA_LOG=$tmp/debug-arguments
"$kelp" build --debug 2>"$tmp/progress"
grep -q '\[1/3\] Preparing demo' "$tmp/progress"
grep -q '\[2/3\] Building src/main.kly' "$tmp/progress"
grep -q '\[3/3\] Finished build/demo' "$tmp/progress"
grep -qx -- '--progress' "$FAKE_KELYRA_LOG"
grep -qx -- '-O0' "$FAKE_KELYRA_LOG"
"$kelp" build
grep -qx -- '-O3' "$FAKE_KELYRA_LOG"
if FAKE_KELYRA_FAIL=1 "$kelp" build 2>"$tmp/failed-progress"; then
  echo "compiler failure was ignored" >&2
  exit 1
fi
grep -q 'Build failed (exit 1)' "$tmp/failed-progress"
if grep -q 'Finished' "$tmp/failed-progress"; then exit 1; fi
if "$kelp" build --invalid >/dev/null 2>&1; then
  echo "invalid build option was accepted" >&2
  exit 1
fi
cd src
test "$("$kelp" output)" = "$tmp/demo/build/demo"
cd ..

printf '\nunknown = true\n' >> kelp.toml
if "$kelp" check >/dev/null 2>&1; then
  echo "unknown TOML key was accepted" >&2
  exit 1
fi

cd "$tmp"
mkdir -p dependency/src/demo
cat > dependency/kelp.toml <<'EOF'
[project]
name = "dependency"
entry = "src/dependency.kly"

[build]
c-sources = ["src/demo/runtime.c"]
EOF
cat > dependency/src/dependency.kly <<'EOF'
pub fn main() -> i32 { return 0; }
EOF
cat > dependency/src/demo/lib.kly <<'EOF'
module demo.lib;
pub fn answer() -> i32 { return 42; }
EOF
cat > dependency/src/demo/runtime.c <<'EOF'
int demo_runtime(void) { return 42; }
EOF
git -C dependency init -q
git -C dependency add .
git -C dependency -c user.name=Kelp -c user.email=kelp@example.invalid \
  commit -qm initial
revision=$(git -C dependency rev-parse HEAD)

"$kelp" new consumer
sed -i "s|compiler = \"kelyra\"|compiler = \"$fake\"|" consumer/kelp.toml
cat >> consumer/kelp.toml <<EOF

[dependencies.demo]
repository = "$tmp/dependency"
revision = "$revision"
EOF
export FAKE_KELYRA_LOG=$tmp/compiler-arguments
cd consumer
"$kelp" build
test -f .kelp/stage/src/demo/lib.kly
grep -q -- "--c-source=$tmp/consumer/.kelp/dependencies/demo/src/demo/runtime.c" \
  "$FAKE_KELYRA_LOG"
"$kelp" package
test -f build/consumer-0.1.0.tar.gz
tar -tzf build/consumer-0.1.0.tar.gz | grep -q '^kelp.toml$'
tar -tzf build/consumer-0.1.0.tar.gz | grep -q '^src/main.kly$'
