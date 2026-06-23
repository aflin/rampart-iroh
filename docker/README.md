# rampart-iroh docker oven

Builds **`rampart-iroh.so`** for the portable `centos7-x86_64` target (glibc 2.17
floor) inside a self-contained [manylinux2014] "oven" container.

The module is a Rust cdylib/staticlib (`iroh` + `tokio` + `rustls`) wrapped by a
small C shim. Rust statically links its std and the entire crate tree, so the
finished `.so` needs only **glibc + libgcc_s** — there is nothing to bundle. The
*only* reason to build it in the oven is the **glibc floor**: built natively it
picks up your host's glibc (e.g. 2.34); built here it drops to **2.17**, so the
module runs on CentOS 7 and everything newer.

The rampart headers are **not** baked in: `build.sh` bind-mounts the installed
`/usr/local/rampart-ml`, and the build resolves `$RAMPART_INCLUDE` from
`rampart -c "console.log(process.installPath)"`.

```
docker/build.sh <stage>
```

## Commands

| Command | What it does |
|---|---|
| `docker/build.sh build` | Compile → `build/oven/rampart-iroh.so` |
| `docker/build.sh install` | Install the module into `/usr/local/rampart-ml/modules` |
| `docker/build.sh shell` | Interactive shell in the oven |
| `docker/build.sh save-image` | Persist the oven image to a `.tar.gz` (see below) |
| `docker/build.sh --rebuild-image [...]` | Force a fresh image first (after a `Dockerfile` edit) |

Typical flow:

```
docker/build.sh build      # -> build/oven/rampart-iroh.so
docker/build.sh install    # -> /usr/local/rampart-ml/modules/rampart-iroh.so
```

`install` also strips the module and (if those dirs exist) copies `iroh-test.js`
to `…/test` and the `licenses/` files to `…/licenses`.

> **First build needs network.** `cargo` fetches the whole crate tree from
> crates.io on the first run; the download cache then persists under
> `build/cargo-home/`, so later builds are offline-ish and incremental.
> `--locked` pins `Cargo.lock` verbatim for reproducibility.

## Mounted directories

Nothing host-facing is baked into the image — it's all bind-mounted at
`docker run` time. `$REPO` is the iroh repo root (`/usr/local/src/rampart-iroh`).

| Stage | Host path → container path | Mode |
|---|---|---|
| **build** | `/usr/local/src/rampart-iroh` → `/iroh` | rw |
| | `/usr/local/rampart-ml` → `/usr/local/rampart-ml` | **ro** |
| | `/etc/passwd` → `/etc/passwd` | ro |
| | `/etc/group` → `/etc/group` | ro |
| **install** | `/usr/local/src/rampart-iroh` → `/iroh` | rw |
| | `/usr/local/rampart-ml` → `/usr/local/rampart-ml` | rw |
| **shell** | `/usr/local/src/rampart-iroh` → `/iroh` | rw |
| | `/usr/local/rampart-ml` → `/usr/local/rampart-ml` | **ro** |

Why each one:

- **Repo (`/iroh`)** — always rw: oven artifacts go to `build/oven/` (the cargo
  target via `CARGO_TARGET_DIR`, the crate cache in `build/cargo-home/`, and the
  module itself). Your native `target/` and the repo-root `rampart-iroh.so` are
  **never** touched. `build/` is gitignored.
- **`/usr/local/rampart-ml`** — the *installed* centos7 rampart. Mounted **ro at
  build** (reads `rampart.h` + runs the binary for `$RAMPART_INCLUDE`) and **rw
  at install** (drops the module into `…/modules`). Only this subdir is mounted.
- **`/etc/passwd` + `/etc/group`** (ro) — only on `build`, which runs as your uid
  (`--user`) so the uid resolves to a name. `install` runs as root (to write the
  system modules dir), so it doesn't mount these.

Everything else (devtoolset-11, the Rust toolchain, cmake/perl for
`ring`/`aws-lc-sys`) lives **inside** the image.

## The oven image

The image (`rampart-iroh-oven`) lives in your local docker store and persists
there across reboots and container runs — you don't need anything else to reuse
it. `build.sh` finds it automatically.

`save-image` additionally writes it to `build/rampart-iroh-oven.image.tar.gz`
(a large file). This is only needed to:

1. **move it to another machine** (`docker load` there),
2. **back it up** before an aggressive prune / docker reinstall,
3. keep a frozen snapshot independent of the daemon.

If that tarball exists, `ensure_image` restores it with `docker load` instead of
rebuilding. After editing the `Dockerfile`, rebuild with `--rebuild-image`.

> A plain `docker image prune -f` only removes **dangling** (untagged) images and
> will not touch `rampart-iroh-oven`. Only `docker rmi`, `docker image prune -a`,
> `docker system prune -a`, or a docker reinstall remove it — and even then the
> `Dockerfile` reproduces it deterministically (needs network).

## Notes

- Built with **Rust stable** (no `rust-toolchain` pin in the repo). Pin a version
  in the `Dockerfile` if you ever need a specific rustc.
- The C `examples/` (which link libevent) are **not** built in the oven — only
  the module is. The module needs the vendored `include/event2/` headers, not a
  system libevent.

[manylinux2014]: https://github.com/pypa/manylinux
