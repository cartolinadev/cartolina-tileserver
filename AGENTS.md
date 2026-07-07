# AGENTS.md — cartolina-tileserver

Developer/agent notes for working on this repository from source. The
[README.md](README.md) covers the user-facing side (apt install, features);
this file covers building, running, and conventions that are easy to get
wrong. Everything here was verified in-tree; correct it if it drifts.

`cartolina-tileserver` is a fork of vts-mapproxy. The buildable program is
`mapproxy`; most server code lives under
[mapproxy/src/mapproxy/](mapproxy/src/mapproxy/).


## Documentation

Cartolina consists of the `cartolina-js` frontend and the
`cartolina-tileserver` backend. Documentation is split by scope.

Documentation specific to the tileserver lives under [docs/](docs/), with
[docs/index.md](docs/index.md) as its entry point. This includes operator
guides, implementation notes, tool documentation, and the tileserver
[session log](docs/session-log.md). When a code change makes this
documentation inaccurate, update the affected document in the same change.

Documentation for the project as a whole lives in the wiki of the sister
`cartolina-js` repository. The wiki also contains frontend documentation,
frontend/backend interface documentation, all RFCs, the shared backlog, and
work that spans both repositories. Start at the shared wiki index:
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/index.md>.

Record significant work confined to this repository in
`docs/session-log.md`. Record project-wide or cross-repository work in the
`cartolina-js` session log (`docs/wiki/session-log.md`):
<https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>.
For work that materially changes both repositories, add concise entries to
both logs and link them instead of duplicating the full account.

**Always read both documentation indexes before answering conceptual
questions and before any non-trivial change.** The shared wiki is where
project-wide work is tracked; the tileserver documentation is where mature
tileserver behavior is documented. At minimum, consult:

- `docs/index.md` and the relevant tileserver topic document;
- `docs/backlog.md` for deferred work confined to the tileserver;
- `docs/session-log.md` for recent tileserver work;
- the shared backlog for deferred bugs and follow-up work, including
  tileserver items:
  <https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/backlog.md>;
- the `cartolina-js` session log (`docs/wiki/session-log.md`) for significant
  work and non-obvious findings, including the status of paired client-side
  changes:
  <https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/session-log.md>;
- Any topic doc named for the area in play. 

Do not infer current obligations from a stale changelog line; cross-check it
against the session log and the code before reporting it as outstanding.

Add new entries to `docs/backlog.md` directly below its introduction, newest
first. Add new entries to `docs/session-log.md` directly below its
introduction, newest first.

### Documentation style

Keep documentation light, direct, and useful. State the rule, command, or
result in plain language. Use established terms and define any specialized
term the reader must know.

Do not use jargon to make a simple point sound important. Do not invent
smart-sounding terminology, hand-wave over missing evidence, or include
numbers that cannot be traced to a durable validation record. Do not repeat
the same rationale across the backlog, session log, operator guide, source
comments, and command help.

Bad:

> Partially retained siblings leave a permanently delivery-incomplete
> parent that must render through a materialized coverage mask.

Good:

> Pruning never removes only some of a parent's children.

The bad version hides one simple rule behind invented terms and irrelevant
implementation detail. The good version gives the reader the fact they need.

This is a public open-source repository. Public documentation, including the
backlog and session log, may refer to:

- this software, its public formats and interfaces, and its public
  dependencies; and
- URLs, resource identifiers, filesystem paths, and datasets used by examples
  or demos committed to this repository.

Do not record private application names, internal or customer APIs, private
hostnames or URLs, customer or partner names, proprietary dataset names,
credentials, tokens, or filesystem paths learned from a private installation.
Do not record private validation details such as coordinates, view parameters,
screenshots, or performance measurements tied to private data.

When private testing informs public work, document only the generic software
conclusion. Keep identifying details in agent memory or a private repository.

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
  concrete path might look like:

```
http://<host>/<rf>/surface/<group>/<id>/14-4345-2867-0.nm
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


## Data-writing tools

A tool that writes data (as opposed to a mere inspection/diagnostic tool)
must override `service::Program::operatingDirectory()` (declared in the
`libservice` submodule) to point at the directory it operates in. That
hook is what gives every real run a durable, self-describing record:

- an unspecified `--log.file` defaults to
  `<dir>/log/<tool>-<timestamp>.log` instead of going nowhere;
- the tool logs a startup banner (complete command line, working
  directory, and the resolved configuration in ini form) before anything
  else it logs.

Gate the override on whatever flag distinguishes a real run from a dry
run or diagnostic report (e.g. `mapproxy-tiling`'s `--apply`), so
read-only invocations stay silent. `mapproxy-tiling`
(`mapproxy/src/tiling/main.cpp`) is the reference implementation.


## Commits

- The committed pre-commit hook requires `docs/session-log.md` to be staged.
  Enable it in a fresh clone with `git config core.hooksPath .githooks`. For a
  trivial change that does not warrant a session-log entry, commit with
  `SKIP_SESSION_LOG=1`. The hook also blocks common PII, secrets, and recovery
  codes in staged additions; use `SKIP_SENSITIVE=1` only for a known false
  positive.
- A package-version-bump commit (changelog only, see "Building a package"
  below) is exempt from both checks: it never carries a session-log entry
  (`SKIP_SESSION_LOG=1`), and its maintainer line always re-adds the known,
  already-published packager email (`SKIP_SENSITIVE=1` for that one line).
- Never commit to `main`/`master` (or any integration branch) without an
  explicit request from the user. This applies to submodules too.
- Don't create micro-commits; group a logical unit of work into one commit.
- After a fix, wait for the user to verify it manually before committing —
  even on feature or other non-main branches.


## Building a package

When asked to build a package, do the following:

1. Find the previous changelog stanza in `mapproxy/debian/changelog` (the
   topmost entry) and the commit that introduced it (search the commit log
   around the version-bump commit message, e.g. `git log --oneline -- \
   mapproxy/debian/changelog`). Review every commit between that commit and
   `HEAD` and write one bullet per logical, user-visible change; skip
   commits that only record backlog/planning notes with no behavior change.
2. Compute the new version: increment the numeric upstream part of the
   previous version by 1 (e.g. `1.119` -> `1.120`). Do not use
   `dch --increment` for this — it does not understand this project's
   suffixed version scheme and corrupts it (verified: it mangled
   `1.119metanodestore` into `1.119metanodestorf`). Edit
   `mapproxy/debian/changelog` by hand instead.
3. Branch suffix: on any branch other than `main`/`master`, append the
   branch's final path component (after the last `/`, if any) with all
   non-alphanumeric characters stripped and lowercased, e.g.
   `feature/metanode-store` -> `metanodestore`, giving `1.120metanodestore`.
   On `main`/`master`, use the bare incremented number with no suffix.
4. Write the new stanza at the top of `mapproxy/debian/changelog`:
   `cartolina-tileserver (<version>) testing; urgency=medium`, the bullets
   from step 1, then a blank line and a maintainer line copying the name
   and email from the entry directly below (format:
   ` -- Name <email>  <date -R>`).
5. On a non-main branch: commit the changelog alone with message
   `new package version <version>`, then tag that commit
   `debian/<version>` (plain `git tag`, not `make dtag` — that target is
   declared but not implemented). On `main`/`master`: prepare the
   changelog but do not commit or tag unless the user explicitly asks.
6. Build with `make deb` from `mapproxy/`. The resulting `.deb` files land
   one directory up (the repository root), one each for
   `cartolina-tileserver`, `cartolina-tileserver-dbg` and
   `cartolina-tileserver-tools`, named
   `<package>_<version>-0<DEB_RELEASE>_<arch>.deb`.


## Code and refactoring philosophy

Common to the entire project (shared with
[cartolina-js](https://github.com/cartolinadev/cartolina-js/blob/main/AGENTS.md)).

Code is liability. Less code means fewer bugs and easier maintenance. We
like to delete code.

Complexity that exists for its own sake is a bug. When two approaches
solve the problem equally well, choose the one with fewer moving parts,
fewer special cases, and less code. A design that eliminates a concept
is better than one that models it more precisely.

- **Knuth's rule: premature optimization is the root of all evil.**
  Do not buy speculative performance with code complexity. Optimize when
  a measurement shows the need, against that measurement; until then,
  prefer the simpler design and record the deferred idea in
  [docs/backlog.md](docs/backlog.md) instead of the codebase.
- **Write as little code as possible.** Before writing new code, search
  for existing functionality to reuse — read the docs of the supporting
  libraries, especially GDAL and PROJ, and dig deep; much of what you
  need already exists. When duplication is unavoidable, abstract, but
  only once the duplication is real and the right abstraction is clear.
- **Dead code removal is encouraged.** When a decision removes a code
  path's reason to exist, remove the path in the same change; a tool or
  documented procedure that covers the need beats built-in logic kept
  "just in case". When in doubt, remove and verify the build and
  selftests still pass.


## Code style

- Write clean, modern C++17. The build is `-std=c++17` with
  `-Wall -Wextra -Werror -pedantic-errors`, so it must compile warning-free;
  favour standard-library and RAII idioms over hand-rolled or C-style code.
  Aim high — this codebase should rock.
- 80-character hard line limit, in code and prose.
- No `else if` chains. Prefer independent guard `if`s (each with its own
  reason), early returns, or a `switch` over cascading `else if`. A plain
  `if`/`else` is fine; it's the chained `else if` ladder to avoid.
- No braces around a single statement: write `if (cond) statement;`, or put
  the statement alone on the next line when the condition is long. Braces
  mark multi-statement blocks only.
- Empty lines inside blocks, the symmetric rule (shared with the
  [cartolina-js coding style](https://github.com/cartolinadev/cartolina-js/blob/main/AGENTS.md)):
  when `{` ends a line with preceding content (an `if`, `else`, loop,
  `switch`, function signature, lambda), place an empty line immediately
  inside it; when a closing `}` starts a line that continues (`} else {`),
  place an empty line immediately before it. Omit both when the whole body
  is a single line. Apply to every new or edited multi-line block.
- Comment style: javadoc `/** ... */` for function/member documentation;
  `/* ... */` for multi-line non-body notes; `//` for in-body comments. Keep
  it consistent — don't mix styles for the same kind of comment.
- Comments explain what the code **does**, in present-tense positive terms —
  never what it used to do or does not do. No arguing with replaced code
  ("rather than the old X", "instead of the previous approach", "unlike
  before", "no longer …"): the reader sees only the current code. Before/after
  rationale belongs in the commit message, not the source.
- Every new public method gets a javadoc `/** */` block with `@param` and
  `@return`; obligatory unless the method is absolutely trivial.
- Descriptive identifiers; avoid single-character names (`index` not `i`,
  `weight` not `w`). Match the surrounding code where it already deviates.
- Private data members take a trailing underscore (`foo_`); private member
  functions do **not**.
- Keep changes in the right layer and minimal; don't introduce unrelated
  functionality into a module just because it's a convenient spot. Prefer
  reusing an existing helper over duplicating logic.
- Respect module boundaries. The `externals/` submodules (libgeo, libmath,
  vts-libs, ...) are reusable libraries; before adding code to one, ask what
  that module is for. Application-specific logic belongs in the application
  (`mapproxy/`), not in a shared module. When in doubt put it in the
  application — you can always lift it into a module later once a clean,
  reusable abstraction has emerged.
- New modules (files created in this fork) carry the copyright line
  `Copyright (c) YYYY Montevallo Consulting, s.r.o.` (current year) in the
  standard BSD header block.
