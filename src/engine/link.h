// BuffrX links: a clip sent straight from this PC to a friend's BuffrX, with no server of ours.
//
// The networking lives in `buffrx-share.exe` (src-share/, iroh over QUIC), a helper this service starts
// only while a link is live or a clip is arriving and stops a little after the last one ends. It talks
// to the helper over pipes, one JSON object per line, and the helper ends when its stdin closes, so it
// never outlives the engine. Everything the user decides - which file, for how long, for how many
// people, whether a large clip may arrive - is decided here; the helper only carries bytes.
//
// What is kept, in <data root>\links.json: the helper's secret key, every live link's token, and the
// resolved short link with the ticket it was made from (both bearer credentials, link_secret.h), all
// sealed to this Windows account (protect_secret), so a link keeps working after a restart until it
// expires and a copied links.json opens on no other account. Received clips are written to
// <data root>\incoming\ first, checked, and only then moved into the library's "Shared with me".
//
// State changes are published as `link.changed` ({links, receives}); a clip that arrived as
// `link.received` ({id, file}). Nothing here takes the engine lock.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../shared/json.h"
#include "link_idle.h"

namespace bx {

// A receive's own rules, pure so they can be checked without a network (link_receive_self_check).
constexpr int kReceiveTries = 5;                 // fetches per receive once the sharer answered (resume makes them cheap)
constexpr int kConnectTries = 3;                 // fetches per receive that never reached the sharer (it is off or asleep)
// How many fetches a receive gets: more once it has reached the sharer, because a retry then resumes.
int receive_tries(bool reached_sharer);
constexpr uint64_t kConnectLimitMs = 180000;      // a receive that never gets as far as the file; a 4 GB
                                                  // clip is hashed on the sharer before the header is sent
constexpr uint64_t kStallLimitMs = 60000;        // a receive that stops arriving part-way
// A failure another attempt can fix. Everything the user or the sender decided (declined, expired,
// refused, too large, no space, not a clip) is final, and so is a disk error: trying again cannot help.
bool receive_retryable(const std::string& reason);
// How long to wait before the attempt after `attempt`: 1 s, 3 s, 8 s, 15 s, 30 s.
uint64_t receive_backoff_ms(int attempt);
// The watchdog's verdict on a receive under way: "" while it still has time, else the reason to fail
// with. `last_progress` is when bytes last arrived, or when the attempt started while connecting.
// "asking" is left alone: that wait is the user's to take.
std::string receive_watchdog(const std::string& state, uint64_t last_progress, uint64_t now);
// Whether a progress line moves the stall clock: new bytes do, and so does the helper's hash pass over the
// finished file (`checking`), which counts from zero again and so never looks like new bytes.
bool receive_clock_moves(int64_t& held, int64_t incoming, bool checking);
// A received picture is filed only when its first bytes are the kind its name says (PNG or JPEG).
bool received_image_ok(const std::string& name, const uint8_t* head, size_t size);
// A name from the sender, as it is about to be filed: one level of the clips folder, no path, no
// alternate data stream, an extension BuffrX writes.
bool receive_name_is_safe(const std::string& name);
// A link is still live at `now` (unix seconds), so it may be offered to the helper again. An expired
// link is never re-served: the helper would carry it after the expiry the sharer chose.
bool link_serve_live(int64_t expires, int64_t now);
// A file that already has a live link gets it back only while the helper that serves it is ready,
// running and reachable. A stopped or dead helper's endpoint is not one to hand out.
bool link_reuse_allowed(bool ready, bool running, const std::string& endpoint, bool online);
// A link is reused only for the lifetime it was made with: 1 hour and 24 hours never stand in for each other.
bool link_window_matches(int64_t created, int64_t expires, int64_t requested_seconds);
// Checks the rules above against themselves when the service starts - in a debug build only: `assert` is
// compiled out of every build that ships, so tests/engine_tests.cpp is what actually holds these rules.
void link_receive_self_check();

class LinkService {
public:
  LinkService(std::string data_root, std::string helper_exe);
  ~LinkService();

  std::function<void(const std::string& name, const Json& data)> emit;

  // Decodes the first frame of a file that is still arriving, so the sheet can show what is coming.
  // Installed by the engine (main.cpp) because the decoder lives outside this file's link graph.
  std::function<bool(const std::string& part, const std::string& out, std::string& error)> make_poster;

  // Settings the engine passes on whenever they change: the page links point to (empty: buffrx://
  // links), the name the page shows as "Shared by" (empty: none), and the upload cap in kbit/s (0:
  // Unlimited; any cap is lowered to 25 Mbit/s while a game has the focus).
  void configure(const std::string& page, const std::string& sender_name, int upload_kbps);

  // A link to one library file, live for `minutes`. A file that already has a live link gets it back.
  bool share(const std::string& file, int minutes, Json& reply, std::string& error);

  // The short link the service handed back, kept so the sheet and Copy link use it without asking
  // again, and kept in links.json so a restart does not hand out the long link for it. `fragment` is the
  // ticket that was shortened: the service drops the code by that text, which is not the ticket the link
  // would carry today (the relay in it can change under a restarted helper).
  void set_short_url(const std::string& id, const std::string& url, const std::string& fragment);
  bool stop(const std::string& id, std::string& error);
  Json list();
  // A link someone sent: the clip is fetched into the library ("Shared with me").
  bool open(const std::string& url, const std::string& clips_dir, const std::string& screenshots_dir, Json& reply,
            std::string& error);
  bool answer(const std::string& id, bool accept, std::string& error);
  bool cancel(const std::string& id, std::string& error);

  // A game has focus: the upload cap drops so a friend's download never costs the game its connection.
  void set_gaming(bool gaming);
  // Once a second, on the service's own thread: expires links and stops the helper when nothing
  // needs it. Public so a test can drive it.
  void tick();
  // One line of the helper's own protocol, as the reader thread hands it over. Public so a test can drive
  // the events that end a link - the helper's own "expired" - without a real helper.
  void on_event(const Json& event);
  void shutdown();

private:
  struct Link {
    std::string id;     // hex of the share id
    std::string token;  // hex
    std::string file, name;
    std::string short_url;       // the resolved short link, once the service answered; kept in links.json
    std::string short_fragment;  // the ticket that short link was made from, for the service's revoke
    // What links.json held for those two fields when neither opened on this account: written back exactly
    // as it was, so a migration never overwrites a credential that is not this PC's to read.
    std::string kept_short_url, kept_short_fragment;
    std::string poster;     // base64url JPEG of the first frame, sent to the receiver in the header
    int64_t size = 0;
    int64_t created = 0;  // unix seconds; 0 in a links.json written before it was kept
    int64_t expires = 0;
    int viewers = 0;    // downloads that finished
    int watching = 0;   // downloads under way
    std::string via;    // "direct" or "relay", as the last transfer went; empty until one starts
  };
  struct Receive {
    std::string id, state, name, error, path, part;
    int64_t size = 0, bytes = 0;
    std::string clips_dir;
  std::string screenshots_dir;
    std::string via;
    // The ticket, kept in memory only and never logged, so an automatic retry dials the same link
    // again and the sheet's Retry button can hand it back to link.open. It is gone once the entry is
    // dismissed, and it is never written to links.json.
    std::string url, peer, relay, share, token;
    int64_t expires = 0;
    int attempt = 0;        // fetches sent so far
    std::string helper_id;
    int tries = 0;          // failures handled for this receive, so a dead helper cannot loop forever
    uint64_t progress = 0;  // when a byte last arrived, or when the attempt was sent
    uint64_t retry_at = 0;  // when the next attempt is due while retrying
    // The first frame of the file as it arrives, so the sheet shows what is coming before it is here.
    // BuffrX writes fragmented MP4, so the first fragment is already playable.
    std::string thumbnail;
    // What the sharer said the file is (its size and time). A retry hands it back with the partial file,
    // and the helper asks only for the rest when the sharer's file still matches.
    std::string tag;
    bool accepted = false;  // the person said yes to a large clip: a retry does not ask again
  };

  // Starts the helper if needed and waits for it to be on the network (up to 35 s).
  bool open_link(const std::string& url, const std::string& clips_dir, const std::string& screenshots_dir, Json& reply,
                 std::string& error);
  bool ensure_helper(std::string& error);
  // Starts it without waiting; the caller holds the lock.
  bool start_helper_locked(std::string& error);
  bool helper_dead();
  void stop_helper();
  void reader_loop(void* read_pipe);
  bool send_locked(const Json& command);
  // False when the helper did not take the command: the link is then not being served.
  bool serve_locked(const Link& link);
  void finish_receive(const std::string& id, const std::string& via);
  // Sends one fetch for this receive (its first, or a retry) and starts its watchdog clock.
  void send_fetch_locked(Receive& receive);
  // A receive has stopped: another attempt if the reason is transient and the link is still valid,
  // else "failed" with that reason. Never leaves the partial file behind.
  void receive_failed_locked(Receive& receive, const std::string& reason);
  void trim_finished_locked();
  void load();
  // Removes a link for good: the active one and every carried links.json entry with its id.
  void drop_link_locked(const std::string& id);
  void save_locked();
  void publish();
  Json state_json_locked();
  std::string url_for_locked(const Link& link);
  int upload_kbps_locked() const;

  std::string data_root_;
  std::string helper_exe_;
  std::string page_;
  std::string sender_name_;
  int upload_kbps_ = 0;
  bool gaming_ = false;

  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::string secret_;    // hex, the helper's identity
  std::string endpoint_;  // hex, the running helper's; empty while none runs
  // This PC's public key, kept while the helper is stopped: a live link's address and "is this link mine"
  // do not depend on a helper being up. Whether a link may be handed out again still asks endpoint_.
  std::string identity_;
  std::string relay_;
  bool ready_ = false;
  // A helper being torn down is joined outside the lock; no new helper may start until that join ends,
  // or two reader threads would share `reader_` and the old one's cleanup would clear the new one's state.
  bool helper_stopping_ = false;
  bool online_ = false;     // the helper reached a relay: a link of this PC's can be found
  uint64_t last_tick_ = 0;   // the tick clock at the last tick, sleep included
  uint64_t last_awake_ = 0;  // the unbiased clock then, sleep left out: the difference is a sleep
  void* process_ = nullptr;
  void* job_ = nullptr;
  void* stdin_ = nullptr;
  std::thread reader_;
  std::atomic<bool> reader_done_{true};
  // The helper's stdout, read by `reader_`. Owned here, not by the reader: stop_helper cancels a read on it
  // that will not end by itself and closes it after the join, so the handle is never cancelled after its
  // value could have been reused.
  void* out_read_ = nullptr;
  // Expiry and the helper's idle stop run here, once a second, never on the engine's window thread.
  std::thread ticker_;
  std::condition_variable ticker_cv_;
  bool stopping_ = false;
  LinkIdleClock idle_;
  uint64_t last_start_ = 0;
  bool restart_pending_ = false;
  uint64_t next_id_ = 0;
  std::map<std::string, Link> links_;
  // links.json entries this run did not bring back because they cannot be served right now: a file on a
  // drive that is not plugged in, a token or short link sealed to another account. They are not active -
  // nothing serves them and the sheet does not show them - and every write of the file carries them
  // through unchanged, so sealing one entry never erases another.
  std::vector<Json> carried_;
  std::map<std::string, Receive> receives_;
};

}  // namespace bx
