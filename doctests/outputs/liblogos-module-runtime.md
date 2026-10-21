# Running a Real Module Against This liblogos

`logos-liblogos` is the core of the Logos platform — `logos_host` and the
`liblogos_core` C API that every frontend (the `logosctl` CLI, the basecamp
desktop app) builds on. This doc-test exercises **this** liblogos commit
end-to-end through the headless `logosctl` runtime:

1. Build the `logosctl` CLI, **overriding its `logos-liblogos` input with the
   commit under test** — so the runtime you exercise is built against the code
   in this repository, not the latest published release.
2. Build [`test_basic_module`](https://github.com/logos-co/logos-test-modules)
   as an `.lgx` package straight from its own flake.
3. Start a `logosctl` daemon and install the `.lgx` into its session.
4. Load `test_basic_module`, introspect it, and call two of its methods — verifying the module actually runs on top
   of this liblogos.

Because every layer (host, module loader, IPC) comes from the liblogos commit
under test, a green run is real evidence that this change keeps the module
runtime working.

**What you'll build:** A real module — `test_basic_module` — installed into a `logosctl` session and called through its daemon, running on this liblogos commit.

**What you'll learn:**

- How to build `logosctl` against a specific `logos-liblogos` commit via `--override-input`
- How to build a real module's `.lgx` from its own flake
- How to install an `.lgx` into a `logosctl` session
- How to start the `logosctl` daemon, load a module, and call its methods

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- A Linux or macOS machine.

---

## Step 1: Build logosctl against this liblogos

Build the `logosctl` CLI from its published flake, but **override its
`logos-liblogos` input** so it links against the commit under test rather
than the latest release. The result is symlinked to `./logos/`.

> The override URL is what pins liblogos to a specific commit: the doc-test
> runner expands a release placeholder on it to a concrete ref. Locally that
> is this checkout's `HEAD` (see `run.sh`); in CI it is the commit being
> tested. With no pin it falls back to latest `master`.

> The CLI itself tracks `master`, deliberately. Only `logos-liblogos` is
> overridden, so everything else in the closure must be a revision that
> liblogos' current runtime pairs with — the shared-runtime split landed as
> one pin set across logos-protocol, logos-plugin-qt, logos-liblogos and
> this CLI, and an older CLI rev pinned here silently reintroduces a second
> copy of the runtime in the process.

### 1.1 Build logosctl with the liblogos override

```bash
nix build 'github:logos-co/logos-logoscore-cli#ctl' \
  --override-input logos-liblogos 'github:logos-co/logos-liblogos' \
  --out-link ./logos
```

The build produces `logos/bin/logosctl` plus bundled runtime libraries
and a `logos/modules/` directory containing the built-in
`capability_module` (required for the auth handshake when loading
modules). Because `follows` propagates the override, the whole
dependency closure — `logos_host`, `liblogos_core`, every module — is
rebuilt against this liblogos.

---

## Step 2: Build and install a module

Build [`test_basic_module`](https://github.com/logos-co/logos-test-modules)'s
`.lgx` straight from its flake, start a daemon, and install the package into
its session. Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `lgx` output.

### 2.1 Build the module's .lgx

`logos-test-modules` publishes each of its modules under
`modules.<system>.<name>`, so the attribute path carries the Nix system
double — `builtins.currentSystem` supplies it. Link the result as
`./module-lgx`.

```bash
# From inside a single-module repo this is simply: nix build '.#lgx'
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.lgx" -o module-lgx
```

The `.lgx` package is now under `./module-lgx/`:

```bash
ls module-lgx/*.lgx
```

### 2.2 Start the daemon

Start a `logosctl` daemon. Its session directory, `./session`, holds the
daemon's config, its installed modules and their data. `--detach`
returns once the daemon accepts commands.

```bash
logosctl daemon start --detach
```

### 2.3 Install the .lgx

A path ending in `.lgx` installs from disk, with no catalog involved. A
local build is unsigned, which the session's default policy accepts with
a warning. `-y` confirms, since there is no terminal to ask on.

```bash
logosctl install module-lgx/*.lgx -y
```

### 2.4 Confirm the install

List the session's packages and confirm the module landed:

```bash
logosctl package ls
```

---

## Step 3: Call the module

Load `test_basic_module`, introspect it, and call two of its methods with the
client subcommands. The daemon logs to `session/logs/daemon.log`.

### 3.1 Inspect the startup log

Review the daemon's startup output:

```bash
cat session/logs/daemon.log
```

### 3.2 Check daemon status

Verify the daemon is running:

```bash
logosctl daemon status
```

### 3.3 List discovered modules

`test_basic_module` should be visible in the scan directory:

```bash
logosctl module ls
```

### 3.4 Load the module

Load `test_basic_module` into the running daemon:

```bash
logosctl module load test_basic_module
```

### 3.5 Confirm the module is loaded

Re-run `daemon status`; the module that was `not_loaded` before now reports
`loaded`:

```bash
logosctl daemon status
```

### 3.6 Introspect the module with module show

`module show` lists the methods and events the module exposes — the
same methods you can `call`:

```bash
logosctl module show test_basic_module
```

### 3.7 Call a method

`echo` returns its argument unchanged — a string round-trip out to the
module subprocess and back over liblogos' IPC:

```bash
logosctl call test_basic_module echo hello
```

### 3.8 Call a second method

`addInts` adds its two arguments. This exercises an `int` round-trip,
where `echo` exercised a string:

```bash
logosctl call test_basic_module addInts 40 2
```

### 3.9 Stop the daemon

Shut the daemon down cleanly:

```bash
logosctl daemon stop
```

The daemon removes its state file and exits.

### 3.10 Confirm the daemon has stopped

With no daemon running, the client reports `not_running` and exits
non-zero, so we add `|| true` to let the doc-test assert on the output:

```bash
logosctl daemon status
```
