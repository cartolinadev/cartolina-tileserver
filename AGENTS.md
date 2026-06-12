# AGENTS.md — cartolina-tileserver

Developer/agent notes for working on this repository from source. The
[README.md](README.md) covers the user-facing side (apt install, features);
this file covers building, running, and conventions that are easy to get
wrong. Everything here was verified in-tree; correct it if it drifts.

`cartolina-tileserver` is a fork of vts-mapproxy. The buildable program is
`mapproxy`; most server code lives under
[mapproxy/src/mapproxy/](mapproxy/src/mapproxy/).


## Documentation

A large body of cartolina documentation lives in the wiki of the sister
`cartolina-js` project (the client side of cartolina), under `docs/wiki/` in
that repository (`index.md` is the entry point). The wiki is shared across the
whole project; some of it pertains specifically to the tileserver. Examine it
for relevant information before diving into the codebase.

**Always read the wiki before answering conceptual questions and before any 
non-trivial change. The wiki is where ongoing work is tracked across both repos; 
the tileserver git log alone does not tell you what is planned or in flight. 
At minimum, consult:

- `docs/wiki/backlog.md` — deferred bugs and follow-up work, including
  tileserver items (e.g. the pre-built metatile index and `mapproxy-tiling`
  redesign).
- `docs/wiki/session-log.md` — chronological record of significant work and
  non-obvious findings; this is where to confirm whether a paired client-side
  change (e.g. a "renderer update needed") has actually been done.
- Any topic doc named for the area in play. 

Do not infer current obligations from a stale changelog line; cross-check it
against the session log and the code before reporting it as outstanding.

That said, documentation is only a secondary source of truth. The codebase is
the bible and empirical verification the holy grail: never optimise a weak or
unverified hypothesis — test and verify, including information the user
provides, whenever that is technically possible.


## Building from source

- CMake source root is [mapproxy/](mapproxy/); the configured build tree is
  `mapproxy/build/` and the binary is `mapproxy/build/bin/mapproxy`.
- Incremental build: from `mapproxy/build/` run `make -j<N> mapproxy`. Only
  the `mapproxy` target is needed to test the server; building everything
  pulls in many tools you usually don't need.
- Keep `<N>` at no more than **half** the host's cores; the compile is
  memory-hungry and over-subscribing the machine can stall it.
- The server runs the **last successful** build. If a build fails, the old
  binary stays in place — rebuild and restart before trusting any test.

### Build type matters — Debug vs Release

The shipped `.deb` is built **Release** (`-O3 -DNDEBUG`). A from-source dev
tree may be configured **Debug** (`-g`, no `NDEBUG`). This is not cosmetic:

- Boost uBLAS consistency checks (`BOOST_UBLAS_CHECK`, e.g. the
  `size1 == size2` vector/matrix size assertion) are compiled out when
  `NDEBUG` is defined. A **Debug** build enables them; a size mismatch that
  Release silently tolerates will throw (`bad argument`) and surface as an
  HTTP 422.
- Consequence: a crash you see in Debug may be invisible in production, and
  vice-versa. Match production with `RelWithDebInfo`/`Release`; use `Debug`
  deliberately to flush out latent dimension bugs. Check the active type with
  `grep CMAKE_BUILD_TYPE mapproxy/build/CMakeCache.txt`.


## Running the dev server

```
cd mapproxy/build
./bin/mapproxy -f <path-to>/mapproxy.conf
```

- The listen port comes from the config's `[http] listen`. A from-source dev
  instance typically uses a different port from the packaged system service
  (`/usr/bin/vts-backend-mapproxy`), which is long-running — do **not** kill
  it when iterating.
- The process command line is the relative `./bin/mapproxy -f ...`, so to find
  or kill the dev instance match on `bin/mapproxy -f` (and exclude
  `vts-backend`); `pkill -f build/bin/mapproxy` will **not** match.
- Resource roots and datasets are driven entirely by the `-f` config; follow
  it when you need to inspect inputs.
- One place to find the conf file is
  `~/install/etc/vts/mapproxy/mapproxy.conf`; if you're unsure which config
  to use, ask the user.
- One place to find the log file is `~/install/var/log/vts/mapproxy.log`; the
  conf file usually states the log path explicitly, so check it there.


## Tile URL conventions (for testing generators)

Tiles are generated on demand (surface tilesets are not pre-rendered on disk),
so to exercise a generator you request a tile over HTTP.

The easiest way to find real URLs is the **introspection API**: browse
`http://<host>:<port>/` — a navigable index of reference frames, groups and
resources, with an embedded viewer per resource. Each resource serves a
`mapConfig.json` (at `.../<id>/mapConfig.json`) whose `meshUrl`/`metaUrl`/
`navUrl` are the exact tile-URL templates — e.g.
`{lod}-{x}-{y}.bin?3gr=2&r=3`, relative to the resource path (this is where
the `?...` query strings come from).

The typical path shape (introspection or the resource configuration are
authoritative; anything inferred otherwise is guesswork):

```
http://<host>:<port>/<refframe>/surface/<group>/<id>/<lod>-<x>-<y>[-<sub>].<ext>
```

- `<host>:<port>` — the query port is whatever the conf file's `[http] listen`
  sets; there is no fixed default.
- `<refframe>/<group>/<id>` are not fixed either: they follow from the resource
  configuration (the config files), so read those to learn the real paths. A
  typical path might look like:

```
http://<host>/melown2015/surface/topoearth/viewfinder-dem1/14-4345-2867-0.nm
```

- `<ext>`: `bin` = mesh, `nm` = normal map, `nav` = navtile, `meta` = metatile.
- The `-<sub>` suffix is a **submesh index**. Per-submesh files (`.nm`, atlas)
  use it: `14-4345-2867-0.nm`. The mesh has none: `14-4345-2867.bin`. Getting
  this wrong returns 404 "Unrecognized filename".
- The server renders tiles fresh on every request — there's no rendered-tile
  cache (production caching is external, e.g. a reverse proxy). So requesting
  a tile always runs the generator and emits its logs; no cache-busting query
  string is needed. Internal caches do exist (notably GDAL's), but they don't
  make rendered output stale.


## Submodules

The externals under [externals/](externals/) are git submodules (see
`.gitmodules`). The superproject pins each to a specific commit (gitlink).

- `git checkout` of a superproject branch does **not** move submodule working
  trees unless you pass `--recurse-submodules`; after switching, run
  `git submodule status` (leading `+`/`-`/space tells you checked-out vs
  pinned) and reconcile deliberately.
- To carry a change across branches consistently: commit it in the submodule
  on the matching branch, then commit the superproject with the bumped
  gitlink. A superproject commit records the submodule SHA, not its branch.
- Don't trust a branch name to imply a commit: verify with
  `git -C externals/<sub> rev-parse HEAD` against
  `git ls-tree <branch> externals/<sub>`.


## Commits

- Never commit to `main`/`master` (or any integration branch) without an
  explicit request from the user. This applies to submodules too.
- Don't create micro-commits; group a logical unit of work into one commit.
- After a fix, wait for the user to verify it manually before committing —
  even on feature or other non-main branches.


## Code style

- Write clean, modern C++17. The build is `-std=c++17` with
  `-Wall -Wextra -Werror -pedantic-errors`, so it must compile warning-free;
  favour standard-library and RAII idioms over hand-rolled or C-style code.
  Aim high — this codebase should rock.
- 80-character hard line limit, in code and prose.
- Comment style: javadoc `/** ... */` for function/member documentation;
  `/* ... */` for multi-line non-body notes; `//` for in-body comments. Keep
  it consistent — don't mix styles for the same kind of comment.
- Every new public method gets a javadoc `/** */` block with `@param` and
  `@return`; obligatory unless the method is absolutely trivial.
- Descriptive identifiers; avoid single-character names (`index` not `i`,
  `weight` not `w`). Match the surrounding code where it already deviates.
- Private data members take a trailing underscore (`foo_`); private member
  functions do **not**.
- Keep changes in the right layer and minimal; don't introduce unrelated
  functionality into a module just because it's a convenient spot. Prefer
  reusing an existing helper over duplicating logic.
- Before reimplementing anything, read the docs of the supporting libraries,
  especially GDAL and PROJ, and dig deep; much of what you need already
  exists. Write as little code as possible.
- Respect module boundaries. The `externals/` submodules (libgeo, libmath,
  vts-libs, ...) are reusable libraries; before adding code to one, ask what
  that module is for. Application-specific logic belongs in the application
  (`mapproxy/`), not in a shared module. When in doubt put it in the
  application — you can always lift it into a module later once a clean,
  reusable abstraction has emerged.
- New modules (files created in this fork) carry the copyright line
  `Copyright (c) YYYY Montevallo Consulting, s.r.o.` (current year) in the
  standard BSD header block.

