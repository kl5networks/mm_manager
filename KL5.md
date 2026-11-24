# mm_manager, KL5 branch

This is [hharte/mm_manager](https://github.com/hharte/mm_manager) with the
fixes KL5 Networks runs in production, one commit per fix, on the `kl5`
branch. `main` follows upstream untouched.

mm_manager programs Nortel Millennium payphones. All of the program is
Howard M. Harte's work; these commits change how it behaves on a VoIP line
(an ATA, a soft modem, a long-distance path), where packets get cut, muted or
lost far more often than on the copper lines it was written for.

## Building

Exactly as upstream:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Windows builds with MSVC the same way (`-A x64`). MinGW is not supported
upstream either.

## The fixes

The branch starts at upstream `2a23e8c` (v0.8.3), the commit these fixes
were proven against, and adds:

| # | Commit | Why |
|---|---|---|
| 0001 | Terminal reports to database | Keeps what each phone reports, for a manager front end to show |
| 0002 | No carrier ends a call | A pty has no carrier detect, so an in-band `NO CARRIER` now ends the session |
| 0003 | Terminal dirs group writable | A front end running as another user can write a terminal's tables |
| 0004 | Routine call-in sends changed tables | A routine call-in also gets tables changed since the last download, sent after `END_DATA` |
| 0005 | Cut packet ends at once | A packet that stops mid-way is ended after a second, so the resend goes out in time |
| 0006 | Lost ACK resends sooner | An ACK that never starts is resent after six seconds, not twelve |
| 0007 | Resend after lost ACK is a retry | That resend is marked RETRY |
| 0008 | Retry of a garbled packet is processed | A RETRY of a packet that was never processed is answered, not dropped |
| 0009 | Stale table ACK is acknowledged | An ACK for an earlier table is acknowledged and the download goes on |
| 0010 | A terminal's own call-in schedule | A terminal's own `mm_table_23.bin` replaces the generated call-in schedule |
| 0011 | Shared database waits for its lock | Several managers can share one database |
| 0012 | Modem over TCP | `-f tcp:host:port` reaches a modem over TCP, with no pty or COM port |
| 0013 | Cash box value and percent kept straight | The cash-box total keeps its cents, and "% full" and status go in the right columns |
| 0014 | Table data in smaller pieces and more retries | Table data goes in 128-byte pieces (`MM_TABLE_CHUNK`, 16-245) with ten tries per packet |
| 0015 | Damaged packet flags are not obeyed | A packet that failed its CRC is asked for again instead of hanging up the call |

Each commit message explains its fix. A fork that wants only some of them
can cherry-pick those.

Upstream has moved past `2a23e8c`. Rebasing onto it is not done yet: its new
time sync on every call-in changes the reply that 0004 builds on, and that
needs its own testing.

## Licence and credits

MIT, as upstream: see `LICENSE`, Copyright (c) Howard M. Harte. The
changes on this branch are Copyright (c) 2026 White Clay Technologies LLC,
under the same MIT licence. By John Walker, for White Clay Technologies LLC.

With thanks to Howard M. Harte and the upstream contributors; to the
US Payphones wiki ([payphones.telesfor.org](https://payphones.telesfor.org/),
Piotr Czerniak, with PhilTel and danc256) and to muCCC
([wiki.muc.ccc.de](https://wiki.muc.ccc.de/millennium:start)) for what is known
about the Millennium; and to the participants in the Millennium and Protel
channels on the PhilTel Discord.

Not affiliated with Nortel or any payphone maker. No firmware is included.
For phones you own.
