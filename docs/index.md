# Tileserver documentation

This directory contains documentation specific to
`cartolina-tileserver`.

Project-wide architecture, RFCs, the shared backlog, frontend documentation,
and frontend/backend interface documentation remain in the `cartolina-js`
wiki:
<https://github.com/cartolinadev/cartolina-js/tree/main/docs/wiki>.

## Operator guides

- [Resource definitions](resources.md) — configuration for established surface,
  tms, and geodata drivers
- [Tileserver tools](tileserver-tools.md) — installed and in-tree command-line
  tools and their roles.
- [Metanode store](metanode-store-operations.md) — create, migrate, verify,
  and roll back DEM resources backed by the metanode store.

## Backend internals

- [Metatile production](tileserver-metatile-production.md) — current and
  legacy DEM metatile generation paths.
- [Tile index](tile-index.md) — what a VTS tile index carries, how
  `mapproxy-tiling` produces one, and how the served index is assembled.

## Reference

- [Developer notes](../AGENTS.md) — building, running, repository conventions,
  and documentation ownership.
- [Backlog](backlog.md) — deferred work confined to the tileserver.
- [Session log](session-log.md) — significant tileserver work and findings.

## Design history

RFCs remain in the shared project wiki. The implemented
[RFC 7: the metanode store](https://github.com/cartolinadev/cartolina-js/blob/main/docs/wiki/rfc-metanode-store.md)
records the design and implementation history behind the current
metanode-store documentation.
