// The arithmetic and the text behind sharing a clip, kept away from the network and from FFmpeg so it
// can be tested anywhere: how a clip is made to fit a chat's upload limit, which addresses a clip may
// be posted to, and the parts of a webhook post that are not the file itself.
//
// Sharing is a user-pressed outbound request (AGENTS.md, Private): the destination is one the user
// pasted, checked here before anything is sent, and nothing listens for a connection.
#pragma once

#include <cstdint>
#include <string>

namespace bx {

// A limit in MiB as Discord counts it, in bytes. Zero or less means Discord's free default (20 MiB);
// anything above 500 MiB (Nitro's) is capped there.
int64_t share_limit_bytes(int limit_mib);

// How a clip is sent so it fits under `limit_bytes`.
struct ShareFit {
  bool fits = false;     // false: even the smallest picture would be under the floor rate
  bool copy = false;     // the file already fits and is sent as it is
  int width = 0;         // the picture sent, even, never larger than the source
  int height = 0;
  int video_kbps = 0;    // the rate to encode at; 0 for a copy
  int audio_kbps = 0;    // one track (the mix, track 1: the one Discord's player plays)
};

// The largest picture whose floor rate the budget still covers: 1440p, 1080p, 720p, 540p, 480p, 360p,
// on the clip's short side so a portrait clip stays portrait, never scaled up. The budget keeps 8%
// headroom for the container and the encoder's overshoot.
ShareFit share_fit(double duration_seconds, int64_t source_bytes, int source_width, int source_height,
                   int64_t limit_bytes);

// The next smaller picture in the same ladder, for a second attempt after an encoder refused the rate it
// was asked for: a rate below what the picture costs is ignored rather than honoured, so stepping the
// picture down is the only thing that makes a clip smaller. Same rates, same aspect, never larger than
// `fit`; width and height stay 0 when the ladder has no step left.
ShareFit share_fit_smaller(const ShareFit& fit);

// True only for Discord's own webhook address: https, discord.com / ptb / canary / discordapp.com on the
// default port, /api[/vN]/webhooks/<digits>/<token>, and nothing else around it - no credentials, no
// query, no path walk, no spaces.
bool discord_webhook_valid(const std::string& url);
// Full webhook id, without its token; empty for an invalid address.
std::string discord_webhook_id(const std::string& url);
// "webhook …5678": enough to recognise which one is set, never the token. "" for an invalid address.
std::string discord_webhook_hint(const std::string& url);

// The webhook's payload_json part: the message, with every mention disabled so a clip named after
// @everyone pings nobody.
std::string discord_payload_json(const std::string& content);
// The multipart body around the streamed file: the payload part and the file part's header, then the
// closing boundary. A quote or a line break in the file name is replaced, so it cannot end the header.
std::string multipart_head(const std::string& boundary, const std::string& payload_json, const std::string& filename);
std::string multipart_tail(const std::string& boundary);

}  // namespace bx
