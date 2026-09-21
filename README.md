# Kelp

Kelp is the project manager for Kelyra.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

./build/kelp new hello
cd hello
kelp build
kelp run
```

Projects are described by `kelp.toml`:

```toml
[project]
name = "hello"
version = "0.1.0"
entry = "src/main.kly"

[build]
compiler = "kelyra"
kind = "executable"
output = "build/hello"
optimization = 0
safe-level = 0
c-sources = []
c-args = []

[package]
output = "build/hello-0.1.0.tar.gz"

[test]
sources = []

[dependencies.kstd]
repository = "git@github.com:z8z6/kstd.git"
revision = "main"
```

Supported commands are `new`, `init`, `members`, `check`, `build`, `output`,
`run`, `test`, and `package`. `package` builds the project and creates the
configured `.tar.gz` source archive.

`build --debug` overrides optimization with `-O0` without changing `kelp.toml`.
`output` prints the absolute configured artifact path without building or
fetching dependencies; editor integrations use it to configure a debugger.
Builds report preparation, compilation, and completion on stderr, including the
entry/output paths. Kelyra's `--progress` lists every loaded `.kly` module, C header,
C source, and the code-generation/link stages. These are phase counters, not
time-based percentages; a failed build never reports successful completion.

Every command reports wall-clock durations on stderr: a per-project
`checked`/`built`/`tested`/`packaged` line, plus a workspace total when the
selection covers more than one project. Durations use `s`/`ms` units and cover
dependency resolution and compilation, so they are useful for spotting slow
members and for CI trend tracking.

## Subprojects

A `kelp.toml` may list subprojects under `[workspace] members`. Each member is a
directory with its own `kelp.toml`, and members may nest recursively:

```toml
[workspace]
members = ["libs/math", "apps/demo"]
```

A workspace root may also be a project itself. `kelp members` prints every
project in the tree as `<path> <name> <kind> <output>`; a workspace root without
its own `[project]` is listed with `-` for both name and output.

Commands accept an optional `<member>` selector, matched by project name or by
the member's path relative to the workspace root, and a `--workspace` flag:

```sh
kelp build                   # the current project, or every member of a pure workspace
kelp build --workspace       # the current project and every member
kelp build math              # only the member named math
kelp output apps/demo        # by relative path
kelp check --workspace
kelp test --workspace
kelp package --workspace
```

Members are visited before their parent so dependencies build first. `run`
always targets a single executable project, and `output` always prints a single
artifact path.

`build.kind` selects what `kelp build` produces:

- `executable` (default) links an executable at `build.output`;
- `library` compiles the project to an object file (default `build/<name>.o`)
  and rejects `kelp run`.

A library is compiled once and linked, not copied into every consumer. When a
project depends on a library, Kelp builds that library's object, passes its
source directory to Kelyra as an `--external-path` (so the consumer emits only
declarations for its modules) and passes the object as a `--link-input`. The
library's configured C sources are still linked into the final executable. That
is how subprojects reference each other: put the shared code in a `library`
project and depend on it; executable-kind dependencies stay source-level. A
library dependency builds with the consumer's compiler and shares its
dependency cache.

## Dependencies

Dependencies use Git repositories or local paths. Kelp clones Git dependencies
into `.kelp/dependencies`, checks out `revision` when provided, and passes each
dependency source directory to Kelyra as a `--module-path` search directory.
Their configured C sources are compiled into the final executable. Sources are
compiled where they live: Kelp never copies them into a staging tree, so
diagnostics and debug information point at the real files. Modules in the
project's own source directory take priority, followed by dependency
directories in resolution order.

A path dependency is used in place and is never cloned, which keeps a sibling
checkout visible without a Git round trip:

```toml
[dependencies.math]
path = "../libs/math"
```

The path is resolved relative to the manifest (absolute paths are allowed) and
must contain a `kelp.toml`. Either `repository`/`revision` or `path` may be
given for one dependency, never both. Path and Git dependencies may mix in one
project, and both participate in cycle detection.

In a workspace, every member shares the workspace root's `.kelp/dependencies`
cache, even when a command runs from inside a member, so a Git dependency is
cloned and fetched once for the whole tree instead of once per member. If two
members request the same dependency name from different repositories or
revisions, Kelp reports a conflict rather than silently repointing the shared
cache.

Kelp searches parent directories for `kelp.toml`, so commands also work from a
project subdirectory. The parser intentionally supports the TOML values used
above: tables, quoted strings, non-negative integers, booleans, and string
arrays.
