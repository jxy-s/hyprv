# src/shared/ — IPC wire contract

`RdpIpc.h` is the IPC wire contract shared by `src/app/` (client, `rdp/RdpHostClient`) and `src/rdphost/` (server). **UTF-8 only on the wire.** **Coordinate ANY edit through a single agent — both processes need a matching update.** Frame format / version negotiation / message table: `docs/IPC.md`. Trap detail: rdphost/IPC section of `docs/GOTCHAS.md`.
