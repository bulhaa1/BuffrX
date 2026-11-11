// The only external pages the native interface may ask the shell to open.
#pragma once

#include <string>

namespace bx {
inline constexpr char kGithubUrl[] = "https://github.com/bulhaa1/BuffrX";
inline constexpr char kReleasesUrl[] = "https://github.com/bulhaa1/BuffrX/releases/latest";
inline constexpr char kDiscordUrl[] = "https://discord.gg/834DEG4wM";
inline constexpr char kSiteUrl[] = "https://buffrx.app/";

inline bool is_public_link(const std::string& url) {
  return url == kGithubUrl || url == kReleasesUrl || url == kDiscordUrl || url == kSiteUrl;
}
}  // namespace bx
