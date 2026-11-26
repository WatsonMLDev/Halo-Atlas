# Halo-Atlas

Native PebbleOS binary compatibility runtime and local developer harness for running unmodified Pebble applications on Brilliant Labs Halo.

## Language

**Watchapp**:
A packaged `.pbw` application targeting PebbleOS, bundling an executable binary, optional resources, metadata, and an optional companion script.
_Avoid_: Pebble app, watch binary, applet

**Pebble Protocol**:
The framed binary wire protocol spoken between the PebbleOS runtime and an attached companion client.
_Avoid_: custom bridge protocol, watch wire format

**Dictionary**:
Pebble's typed collection of integer-keyed tuples carried inside an `AppMessage` or synchronized via `AppSync`.
_Avoid_: tuple map, KV payload

**AppMessage**:
A transactional, UUID-scoped `Dictionary` exchange (`PUSH` with `ACK` or `NACK`) between a running Watchapp and a companion client over Pebble Protocol.
_Avoid_: JS packet, dict message

**PebbleKit JS Companion**:
The host-side JavaScript environment that runs a Watchapp's bundled `pebble-js-app.js` script to provide network access, geolocation, storage, and user configuration.
_Avoid_: JS bridge, phone script
