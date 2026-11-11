#pragma once
// The clip the panel plays: the preview page (ui/dist, the same player the preview window used) running in WebView2, as a
// child of the panel's own window. It exists only while a clip is open. The browser starts with the first clip and is
// gone when the last one closes, so an idle panel runs nothing, and a covered picture (a sheet is up) is suspended and
// asked to keep little memory. Everything runs on the window thread: WebView2 needs that thread's message loop.

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bx::ui {

// What the picture shows now: nothing, the page still starting, the clip playing, or the page could not play it.
enum class ClipPicture { off, loading, playing, failed };

class WebClip {
public:
  WebClip();
  ~WebClip();
  WebClip(const WebClip&) = delete;
  WebClip& operator=(const WebClip&) = delete;

  // Where the page's files are (the folder that holds index.html) and where the browser keeps its profile.
  void configure(std::string web_root, std::string profile_dir);
  // The page asked for the clip to close (Escape in the page). Runs on the window thread; the caller releases later.
  void on_close(std::function<void()> close) { close_ = std::move(close); }
  // The picture changed state with no frame of its own (ready, failed): the window should draw.
  void on_change(std::function<void()> change) { change_ = std::move(change); }

  // The window thread, every frame a clip is open: `file` plays in `rect` (client pixels of `parent`), hidden while
  // `covered` is set (a sheet is over it). `roots` are the folders the page may read media from.
  ClipPicture show(HWND parent, const std::string& file, const RECT& rect, bool covered, const std::vector<std::string>& roots,
                   const std::string& theme);
  // Closes the page and its browser, so the memory goes back. Safe to call with nothing open.
  void release();

private:
  struct Session;
  std::unique_ptr<Session> session_;
  std::string web_root_, profile_dir_;
  unsigned generations_ = 0;
  std::function<void()> close_, change_;
};

}  // namespace bx::ui
