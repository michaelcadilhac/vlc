# Local-file DLNA casting and Samsung interoperability

The code in this change was provided by **GPT-6.1 Sol**. Development and
validation used VLC 4 on Arch Linux and a Samsung renderer advertised as
“Samsung LED50”. The exact TV model was not recorded.

## Problem and evidence

The renderer rejected VLC's output as unsupported even though ReadyMedia
could play the same local media. Several independent problems contributed:

- `DLNA.ORG_PN` parsing started one character before the profile name, retaining
  the `=`. Explicit renderer profiles consequently failed to match.
- MIME wildcard expansion concealed that parser failure. Selection returned
  the first matching codec pair, allowing streaming MP4 to win over a suitable
  explicit transport-stream profile. Wildcards also inherited named profiles
  they did not actually advertise.
- Codec IDs alone did not establish compliance with a named AVC profile.
- Initial elementary-stream discovery could start an audio-only or video-only
  renderer session before the other track appeared.
- Live HTTP output advertised a zero-length HEAD representation and lacked
  the DLNA response headers used by the working server. Correcting headers
  and selecting an ISO TS profile still did not make live remuxing play on the
  tested TV.

The decisive comparison was finite-file serving: both the original MP4 and a
finite, codec-copy MPEG-TS file played through ReadyMedia. Serving the original
MP4 through VLC with its actual length and byte-range support also played.
The implemented local-file path therefore serves the original container when
its MIME type is advertised without a named-profile restriction.

After playback worked, VLC pause initially caused the TV to report a network
error, and seeking to the midpoint took about a minute. Preserving the HTTP
resource and forwarding transport controls fixed these observed failures.
Seeking while paused also needed explicit pause restoration because Samsung's
asynchronous seek resumed playback.

## Capability parsing and profile selection

`modules/stream_out/dlna/protocol_info.hpp` contains helpers shared by the
renderer and the regression test. Parsing matches whole `DLNA.ORG_PN`
parameters, handles the final parameter without a semicolon, and rejects
empty, malformed and duplicate profile names. An absent name is not a
wildcard.

Each parsed capability preserves its renderer declaration and whether it
matches an explicit named profile. Compatible explicit profiles outrank MIME
wildcard expansions. Wildcard protocols omit `DLNA.ORG_PN` in outgoing metadata;
when choosing between wildcard video remuxes, ISO MPEG-TS is preferred over
streaming MP4. Timestamped TS profiles are excluded from the ISO TS mux path.

Two profiles are added: `AVC_TS_MP_HD_AC3_ISO` and
`AVC_TS_MP_HD_AAC_MULT5_ISO`. The validator deliberately accepts a conservative
subset: AVC Main profile, level 4, up to 1920×1080, up to 30 fps at HD or 60 fps
at 1280×720 and below, and at most 20 Mb/s. Visible dimensions take precedence
over macroblock-padded dimensions. Audio must use 32, 44.1 or 48 kHz. AC-3 is
limited to six channels and 640 kb/s; the AAC path accepts validated AAC-LC
stereo at up to 576 kb/s, rejecting HE-AAC and unsupported configurations.
Unknown constraints on named AVC profiles cause selection to fall back to a
renderer MIME wildcard when one is available. Other legacy profile families
are not comprehensively validated by this change.

## Finite-file HTTP serving

The input supplies its URI through `SOUT_STREAM_SET_INPUT_URI`. For a readable,
nonempty regular `file://` resource with an advertised MIME-only capability,
the DLNA output creates one HTTP endpoint and serves the original bytes.
Matroska extensions have MIME mappings; the older `video/x-mkv` alias is used
when that is the renderer's advertised form.

The new exported `httpd_FileNewFromFd()` API duplicates the descriptor and
copies supplied headers. Its caller keeps ownership of the original descriptor.
The resource must remain unchanged while it is served. The endpoint provides:

- HEAD and GET with the actual file size and MIME type;
- full-file GET, single bounded, open-ended and suffix byte ranges;
- HTTP 206 with `Content-Range`, or 416 for invalid/unsatisfiable ranges;
- `Accept-Ranges`, `contentFeatures.dlna.org` and `transferMode.dlna.org`;
- bounded 64 KiB reads rather than loading the whole file into memory.

DIDL metadata includes the actual resource size and advertises byte-range
support. Stream arrival order does not replace this endpoint. Local input
pacing keeps the casting session alive while the TV reads independently.
Active finite-file transfers bypass the normal idle timeout so a paused TV
can retain its connection; disconnecting or deleting the resource still
closes the transfer.

The live-stream fallback remains available. It now supplies DLNA HTTP headers
through a private output option, rejects CR/LF in that header value, omits a
false zero Content-Length on HEAD, and does not advertise range support it
cannot provide. Conversion output has unknown final size: this change neither
precomputes it nor advertises an estimated Content-Length.

## Playback controls and lifetime

Optional stream-output queries forward VLC pause/resume, time or position
seek, and explicit stop. Modules that do not implement these queries continue
using their existing controls. The DLNA file path maps them to AVTransport
Pause, Play, Seek (`REL_TIME`) and Stop.

A demux seek does not invalidate the original-file endpoint. Paused seeks
poll transport state at 100 ms intervals for up to five seconds, returning
when the renderer is already paused or can accept Pause after transitioning.
The same handling applies to an initial seek while paused. An explicit stop
closes the file endpoint and prevents queued packets during decoder teardown
from reopening it; a new input URI resets that guard.

This polling is synchronous and the compiler warns that short delays should
use proper event handling. An event-driven follow-up would avoid blocking the
input control path during pause restoration.

## Validation and reproduction

With an already configured build directory, build and run the committed test:

```sh
make -C _build/test test_modules_stream_out_dlna
_build/test/test_modules_stream_out_dlna
```

It covers profile-name edge cases, malformed protocol entries, exact versus
wildcard ranking in both declaration orders, codec/format constraint mismatches,
AAC configuration handling, video-only fallback, byte-range validation and
UPnP seek-time formatting. Test registration is included for Autotools and
Meson; validation here used Autotools.

Additional validation performed during development:

- A complete VLC build with 437 plugins, using the compiler and linker flags
  from the archived Arch `vlc 3.0.23_2-16` package, adapted to VLC 4's available
  configure options. Local playback and the native Qt interface were checked.
- The regression test with the configured flags and ASan/UBSan. Leak detection
  was disabled because the sandbox did not support LeakSanitizer's process
  inspection.
- A temporary mock AVTransport renderer verified a 20-second unread HTTP
  response followed by byte-for-byte comparison of 16 MiB, pause/resume, time
  and position seeks, asynchronous paused-seek restoration, one stable URI,
  and clean stop without reopening. The local harness is not a committed test.
- Samsung playback of the original H.264/AAC MP4 and H.264/AC-3 MKV. A 20-second
  pause remained `PAUSED_PLAYBACK` with status `OK`; resume returned `PLAYING`;
  midpoint seeking completed within five seconds. The user confirmed pause,
  resume and seeking visually. A subsequent paused seek remained stationary
  at the midpoint for ten seconds, with status `OK`, until explicitly resumed.
- A style-only review, affected-component rebuild, regression rerun and
  `git diff --check` after removing generated translation changes.

To reproduce manually, start the modified VLC with a local MP4 or MKV, select
an available DLNA renderer, then exercise VLC's pause, resume and seek controls,
including seeking while paused. For diagnostics that avoid the separate
hardware decode/encode failure observed during initial experiments:

```sh
_build/vlc -vvv --no-hw-dec --no-avcodec-dr --avcodec-threads=1 /path/to/video.mkv
```

Compare AVTransport metadata and HTTP HEAD/Range responses with a working
finite-file DLNA server if playback fails. Test a paused interval longer than
the server's usual idle timeout and confirm seeking does not replace the URI.

## Scope and remaining limits

The successful Samsung path is original local-file serving. MIME advertisement
is a renderer capability declaration, not an inspection of every codec inside
the container. A MIME-compatible file can still contain unsupported tracks.
Files requiring conversion, non-file inputs, and renderers without a suitable
MIME-only declaration use the existing streaming fallback, which was not
validated as playable on this TV. The separate hardware-acceleration crash is
not fixed here. Broader program-aware discovery stabilization, additional
profile validation, HTTP multipart ranges and DLNA time-range HTTP requests
are outside this change.
