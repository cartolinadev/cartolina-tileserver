# Contributing to cartolina-tileserver

`cartolina-tileserver` is a fork of — and replacement for — the discontinued
`vts-mapproxy`. Contributions are welcome, but the project is not trying to
preserve every legacy code path. Before starting larger work, read
[README.md](README.md) and [AGENTS.md](AGENTS.md), and check the
[documentation index](docs/index.md) and the shared
[cartolina-js wiki](https://github.com/cartolinadev/cartolina-js/tree/main/docs/wiki)
for project-wide architecture and RFCs.

## Code of Conduct

Participation in this project is covered by
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Contributor Terms

By submitting a contribution, you agree that:

- the contribution may be used, modified, sublicensed, and redistributed
  under the project license in [LICENSE](LICENSE)
- you have the right to submit the contribution under those terms
- the contribution is your original work, or is derived from work that you
  have the right to submit under compatible open-source terms
- you will identify any third-party code, data, generated output, or license
  terms that apply to the contribution
- to the extent permitted by law, you will defend and indemnify the project
  maintainers from third-party claims caused by your breach of these terms

These terms are an inbound-equals-outbound contribution policy. Copyright
ownership is not assigned to the project; contributors keep ownership of
their own contributions while granting the project the rights needed to
release them under the project license.

Maintainers may ask for a signed-off commit, a separate written
certification, or clarification of source provenance before accepting a
contribution.

## Contribution Scope

Good contributions include:

- bug reports with a reproducible resource configuration, request URL, or
  dataset
- fixes for current resource drivers and generators
- focused improvements to a driver, generator, or the introspection API
- documentation that records current behavior or non-obvious findings

Out of scope by default:

- restoring legacy `vts-mapproxy` behavior that this fork has deliberately
  dropped or replaced
- speculative abstractions for future resource types
- broad rewrites that are not tied to a tested behavior change

## Development Setup

Building from source, running the dev server, and the repository's other
conventions (submodules, tile URL layout, package builds) are covered in
[AGENTS.md](AGENTS.md) — read it before your first change. In short:

```bash
git clone --recursive https://github.com/cartolinadev/cartolina-tileserver.git
cd cartolina-tileserver
git config core.hooksPath .githooks

cd mapproxy
make -j<N> mapproxy   # adapt <N> to the host's available parallelism
```

## Coding Guidelines

Follow the repository instructions in [AGENTS.md](AGENTS.md) and the current
code near the change. In short:

- clean, modern C++17; the build is `-Wall -Wextra -Werror -pedantic-errors`
  and must compile warning-free
- 80-character hard line limit, in code and prose
- no `else if` chains; prefer guard `if`s, early returns, or `switch`
- keep changes in the right layer: application-specific logic belongs in
  `mapproxy/`, not in the shared `externals/` submodules
- prefer deleting a dead code path over keeping it "just in case"
- match the existing brace, comment, and naming conventions in the file you
  are editing

## Pull Requests

Use a branch with a short descriptive name. Include in the pull request:

- the problem or feature being addressed
- the main implementation decisions
- the tests or manual checks run (e.g. tile requests exercised, generators
  covered)
- any documentation updated (`docs/`, `AGENTS.md`, backlog, session log)

Documentation-only changes do not need a rebuild. Code changes that alter
server behavior should update the relevant operator guide or session log in
the same pull request.

## Reporting Bugs

Report bugs on the project
[issue tracker](https://github.com/cartolinadev/cartolina-tileserver/issues).
Include:

- the `cartolina-tileserver` version or commit
- the resource configuration (or a minimal excerpt) that reproduces the issue
- the request URL and HTTP status/response received
- relevant lines from the mapproxy log
- the expected vs. actual behavior
