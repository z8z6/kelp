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

Supported commands are `new`, `init`, `check`, `build`, `output`, `run`, `test`, and
`package`. `package` builds the project and creates the configured `.tar.gz`
source archive.

`build --debug` overrides optimization with `-O0` without changing `kelp.toml`.
`output` prints the absolute configured executable path without building or
fetching dependencies; editor integrations use it to configure a debugger.
Builds report preparation, compilation, and completion on stderr, including the
entry/output paths. Kelyra's `--progress` lists every loaded `.kly` module, C header,
C source, and the code-generation/link stages. These are phase counters, not
time-based percentages; a failed build never reports successful completion.

Dependencies use Git repositories. Kelp clones them into
`.kelp/dependencies`, checks out `revision` when provided, and passes each
dependency source directory to Kelyra as a `--module-path` search directory.
Their configured C sources are compiled into the final executable. Sources are
compiled where they live: Kelp never copies them into a staging tree, so
diagnostics and debug information point at the real files. Modules in the
project's own source directory take priority, followed by dependency
directories in resolution order.

Kelp searches parent directories for `kelp.toml`, so commands also work from a
project subdirectory. The parser intentionally supports the TOML values used
above: tables, quoted strings, non-negative integers, booleans, and string
arrays.
