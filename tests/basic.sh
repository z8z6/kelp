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
# Generated manifests name the artifact, not a build directory, and the layout
# ignores only the shared cache.
grep -qx 'output = "demo"' demo/kelp.toml
grep -qx '/.kelp/' demo/.gitignore

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
test -x .kelp/build/demo
test ! -e build
test "$("$kelp" run)" = kelp-run-ok
"$kelp" test

test "$("$kelp" output)" = "$tmp/demo/.kelp/build/demo"
sed -i 's/optimization = 0/optimization = 3/' kelp.toml
export FAKE_KELYRA_LOG=$tmp/debug-arguments
"$kelp" build --debug 2>"$tmp/progress"
grep -q '\[1/3\] Preparing demo' "$tmp/progress"
grep -q '\[2/3\] Building src/main.kly' "$tmp/progress"
grep -q '\[3/3\] Finished .kelp/build/demo' "$tmp/progress"
grep -qx -- '--progress' "$FAKE_KELYRA_LOG"
grep -qx -- '-O0' "$FAKE_KELYRA_LOG"
"$kelp" build
grep -qx -- '-O3' "$FAKE_KELYRA_LOG"
sed -i '/optimization = 3/a runtime = "freestanding"' kelp.toml
"$kelp" build
grep -qx -- '--runtime=freestanding' "$FAKE_KELYRA_LOG"
sed -i '/runtime = "freestanding"/a target = "x86_64-pc-windows-msvc"' kelp.toml
"$kelp" build
grep -qx -- '--target=x86_64-pc-windows-msvc' "$FAKE_KELYRA_LOG"
sed -i '/target = "x86_64-pc-windows-msvc"/a windows-import-libraries = ["Synchronization.lib"]' kelp.toml
"$kelp" build
grep -qx -- '--link-input=Synchronization.lib' "$FAKE_KELYRA_LOG"
sed -i 's/target = "x86_64-pc-windows-msvc"/target = "x86_64-unknown-linux-gnu"/' kelp.toml
"$kelp" build
if grep -qx -- '--link-input=Synchronization.lib' "$FAKE_KELYRA_LOG"; then
  echo "Windows import library leaked into Linux build" >&2
  exit 1
fi
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
test "$("$kelp" output)" = "$tmp/demo/.kelp/build/demo"
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
test ! -e .kelp/stage
grep -qx -- 'src/main.kly' "$FAKE_KELYRA_LOG"
grep -qx -- "--module-path=$tmp/consumer/src" "$FAKE_KELYRA_LOG"
grep -qx -- "--module-path=$tmp/consumer/.kelp/dependencies/demo/src" \
  "$FAKE_KELYRA_LOG"
grep -q -- "--c-source=$tmp/consumer/.kelp/dependencies/demo/src/demo/runtime.c" \
  "$FAKE_KELYRA_LOG"
"$kelp" package
test -f .kelp/build/consumer-0.1.0.tar.gz
tar -tzf .kelp/build/consumer-0.1.0.tar.gz | grep -q '^kelp.toml$'
tar -tzf .kelp/build/consumer-0.1.0.tar.gz | grep -q '^src/main.kly$'
cd "$tmp"

# A workspace nests subprojects, each with its own kelp.toml. A library project
# builds an object artifact; an executable consumes a sibling through a local
# path dependency, so nothing is cloned.
mkdir -p workspace/libs/math/src workspace/app/src
cat > workspace/kelp.toml <<'EOF'
[workspace]
members = ["libs/math", "app"]
EOF
cat > workspace/libs/math/kelp.toml <<EOF
[project]
name = "math"
entry = "src/math.kly"

[build]
compiler = "$fake"
kind = "library"
windows-import-libraries = ["Synchronization.lib"]
EOF
cat > workspace/libs/math/src/math.kly <<'EOF'
pub fn answer() -> i32 { return 0; }
EOF
cat > workspace/app/kelp.toml <<EOF
[project]
name = "app"
entry = "src/main.kly"

[build]
compiler = "$fake"
target = "x86_64-pc-windows-msvc"
c-sources = ["src/runtime.c"]
c-libraries = ["src/prebuilt-c.a"]

[dependencies.math]
path = "../libs/math"
EOF
printf 'pub fn main() -> i32 { return 0; }\n' > workspace/app/src/main.kly
printf 'int app_runtime(void) { return 0; }\n' > workspace/app/src/runtime.c
printf 'prebuilt C library\n' > workspace/app/src/prebuilt-c.a

cd workspace
"$kelp" members | grep -qx 'libs/math math library .kelp/build/libs/math/math.o'
"$kelp" members | grep -qx 'app app executable .kelp/build/app/app'
"$kelp" members | grep -qx '. - workspace -'

# A workspace root without [project] builds every member by default.
"$kelp" build
test -f .kelp/build/libs/math/math.o
test -x .kelp/build/app/app
# Only the shared build directory is written; projects stay clean.
test ! -e libs/math/build
test ! -e app/build

# A selector builds or queries only that member.
rm -f .kelp/build/libs/math/math.o
"$kelp" build math
test -f .kelp/build/libs/math/math.o
test "$("$kelp" output app)" = "$tmp/workspace/.kelp/build/app/app"
if "$kelp" build missing >/dev/null 2>&1; then
  echo "unknown workspace member was accepted" >&2
  exit 1
fi
if "$kelp" run math >/dev/null 2>&1; then
  echo "library project was run" >&2
  exit 1
fi

# The path dependency is used in place: its source directory becomes a module
# search path and its C sources are linked, without a dependency cache. Because
# it is a library, the consumer only declares its modules and links its object.
export FAKE_KELYRA_LOG=$tmp/workspace-arguments
rm -rf app/build
"$kelp" build app
grep -qx -- 'src/main.kly' "$FAKE_KELYRA_LOG"
grep -qx -- "--module-path=$tmp/workspace/libs/math/src" "$FAKE_KELYRA_LOG"
grep -qx -- "--external-path=$tmp/workspace/libs/math/src" "$FAKE_KELYRA_LOG"
grep -qx -- "--link-input=$tmp/workspace/.kelp/build/libs/math/math.o" \
  "$FAKE_KELYRA_LOG"
grep -q -- "--c-source=src/runtime.c" "$FAKE_KELYRA_LOG"
grep -qx -- "--link-input=$tmp/workspace/app/src/prebuilt-c.a" "$FAKE_KELYRA_LOG"
grep -qx -- '--link-input=Synchronization.lib' "$FAKE_KELYRA_LOG"
test -f .kelp/build/libs/math/math.o
test ! -e app/.kelp

# An explicitly provided library replaces compilation of the dependency.
printf 'prebuilt Kelyra library\n' > libs/math/prebuilt.o
printf '\nlibrary = "prebuilt.o"\n' >> app/kelp.toml
rm -f .kelp/build/libs/math/math.o
"$kelp" build app
grep -qx -- "--link-input=$tmp/workspace/libs/math/prebuilt.o" "$FAKE_KELYRA_LOG"
test ! -e .kelp/build/libs/math/math.o

# check, test, and package accept --workspace.
"$kelp" check --workspace
"$kelp" test --workspace
"$kelp" package --workspace
test -f .kelp/build/libs/math/math.o
test -f .kelp/build/app/app-0.1.0.tar.gz

# Commands report wall-clock durations on stderr. A multi-project selection adds
# a total line; a single project reports only its own duration.
duration='[0-9]+\.[0-9]{3}s'
timing=$("$kelp" check --workspace 2>&1 >/dev/null)
echo "$timing" | grep -Eq "^checked (math|app) in $duration$"
echo "$timing" | grep -Eq "^checked 2 projects in $duration$"
echo "$("$kelp" build --workspace 2>&1 >/dev/null)" |
  grep -Eq "^built 2 projects in $duration$"
echo "$("$kelp" test --workspace 2>&1 >/dev/null)" |
  grep -Eq "^tested 2 projects in $duration$"
echo "$("$kelp" package --workspace 2>&1 >/dev/null)" |
  grep -Eq "^packaged 2 projects in $duration$"
if "$kelp" check math 2>&1 >/dev/null | grep -q 'projects in'; then
  echo "a single-project check reported a workspace total" >&2
  exit 1
fi
cd "$tmp"

# A project may be both a project and a workspace: the default builds only the
# root, and --workspace adds the members.
mkdir -p combined/src combined/child/src
cat > combined/kelp.toml <<EOF
[project]
name = "combined"
entry = "src/main.kly"

[build]
compiler = "$fake"

[workspace]
members = ["child"]
EOF
cat > combined/child/kelp.toml <<EOF
[project]
name = "child"
entry = "src/main.kly"

[build]
compiler = "$fake"
EOF
printf 'pub fn main() -> i32 { return 0; }\n' > combined/src/main.kly
printf 'pub fn main() -> i32 { return 0; }\n' > combined/child/src/main.kly
cd combined
"$kelp" build
test -x .kelp/build/combined
test ! -e child/build
"$kelp" build --workspace
test -x .kelp/build/child/child
cd "$tmp"

# Path dependencies participate in cycle detection.
mkdir -p cycle/a/src cycle/b/src
cat > cycle/kelp.toml <<'EOF'
[workspace]
members = ["a", "b"]
EOF
cat > cycle/a/kelp.toml <<EOF
[project]
name = "a"
entry = "src/main.kly"

[build]
compiler = "$fake"

[dependencies.b]
path = "../b"
EOF
cat > cycle/b/kelp.toml <<EOF
[project]
name = "b"
entry = "src/main.kly"

[build]
compiler = "$fake"

[dependencies.a]
path = "../a"
EOF
printf 'pub fn main() -> i32 { return 0; }\n' > cycle/a/src/main.kly
printf 'pub fn main() -> i32 { return 0; }\n' > cycle/b/src/main.kly
cd cycle
if "$kelp" build --workspace 2>"$tmp/cycle-error"; then
  echo "cyclic path dependency was accepted" >&2
  exit 1
fi
grep -q 'cyclic dependency' "$tmp/cycle-error"
cd "$tmp"

# Workspace members share one Git dependency cache: the dependency is fetched
# once at the workspace root instead of once per member.
mkdir -p shared/a/src shared/b/src
cat > shared/kelp.toml <<'EOF'
[workspace]
members = ["a", "b"]
EOF
for member in a b; do
  cat > "shared/$member/kelp.toml" <<EOF
[project]
name = "$member"
entry = "src/main.kly"

[build]
compiler = "$fake"

[dependencies.demo]
repository = "$tmp/dependency"
revision = "$revision"
EOF
  printf 'pub fn main() -> i32 { return 0; }\n' > "shared/$member/src/main.kly"
done
cd shared
"$kelp" build --workspace
test -e .kelp/dependencies/demo
test ! -e a/.kelp
test ! -e b/.kelp
# The cached dependency is reused by a later run without a second checkout.
"$kelp" build --workspace
# A build started inside one member still uses the enclosing workspace cache.
(cd a && "$kelp" build)
test ! -e a/.kelp
cd "$tmp"

# A library dependency is built with the consumer's compiler, resolved against
# the consumer root, so a relative path or a stale dependency manifest works.
mkdir -p toolchain/lib/src toolchain/app/src
cp "$fake" toolchain/toolchain
cat > toolchain/kelp.toml <<'EOF'
[workspace]
members = ["lib", "app"]
EOF
cat > toolchain/lib/kelp.toml <<'EOF'
[project]
name = "lib"
entry = "src/lib.kly"

[build]
compiler = "missing-compiler"
kind = "library"
EOF
printf 'pub fn value() -> i32 { return 1; }\n' > toolchain/lib/src/lib.kly
cat > toolchain/app/kelp.toml <<'EOF'
[project]
name = "app"
entry = "src/main.kly"

[build]
compiler = "../toolchain"

[dependencies.lib]
path = "../lib"
EOF
printf 'pub fn main() -> i32 { return 0; }\n' > toolchain/app/src/main.kly
cd toolchain
"$kelp" build app
test -f .kelp/build/lib/lib.o
test -x .kelp/build/app/app
cd "$tmp"

# A path dependency outside the workspace keeps a distinct subtree instead of
# colliding with a same-named directory inside it.
mkdir -p ext/ws/app/src ext/lib/src
cat > ext/ws/kelp.toml <<'EOF'
[workspace]
members = ["app"]
EOF
cat > ext/lib/kelp.toml <<EOF
[project]
name = "lib"
entry = "src/lib.kly"

[build]
compiler = "$fake"
kind = "library"
EOF
printf 'pub fn value() -> i32 { return 0; }\n' > ext/lib/src/lib.kly
cat > ext/ws/app/kelp.toml <<EOF
[project]
name = "app"
entry = "src/main.kly"

[build]
compiler = "$fake"

[dependencies.lib]
path = "../../lib"
EOF
printf 'pub fn main() -> i32 { return 0; }\n' > ext/ws/app/src/main.kly
cd ext/ws
"$kelp" build
test -f .kelp/build/__external/lib/lib.o
test -x .kelp/build/app/app
test ! -e ../lib/.kelp
cd "$tmp"
