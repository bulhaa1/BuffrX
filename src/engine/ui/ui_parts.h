// Pieces the panel, the quick panel and Settings share: the actions (each a protocol
// command whose failure becomes a toast), the titlebar, thumbnails, the clip card's menu, the rename and
// delete dialogs and the hotkey recorder.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui_format.h"
#include "ui_views.h"
#include "ui_widgets.h"

namespace bx::ui::parts {

// The actions every surface offers.
void toggle_buffer(ViewEnv& env);
void save_clip(ViewEnv& env);
bool draw_save_feedback(ViewEnv& env, ImDrawList* draw, ImVec2 icon, float size, ImU32 color);
void quick_clip(ViewEnv& env);
void screenshot(ViewEnv& env);
void toggle_recording(ViewEnv& env);
// `preview`: the clip plays first, in the editor process; Edit there turns it into the editor.
void open_editor(ViewEnv& env, const std::vector<std::string>& files, bool preview = false);
void set(ViewEnv& env, const char* key, Json value);  // one setting

// The Timeline: the panel's and the tray's TIMELINE entry opens (or closes) the preview, and the notices a writer
// state change earns once - started, paused outside the game, stopped for disk space. Called once per frame.
void timeline_action(ViewEnv& env);
void timeline_notices(UiStore& store);

// The frameless window's titlebar: mark, wordmark, title, the caller's buttons, then minimise /
// close. Registers itself as the window's drag handle.
void titlebar(ViewEnv& env, const std::string& title, const std::function<void()>& buttons);
// A titlebar button: an icon button the window does not drag on.
bool bar_button(ViewEnv& env, const char* id, const char* icon, const std::string& tip, bool active = false);
// The update's one-line state, or "" when nothing is on offer: "UPDATE READY", "GETTING UPDATE",
// "UPDATE FAILED" or "UPDATE AVAILABLE". The badge and the titlebar's label share it, so they never disagree.
std::string update_state_text(const ViewEnv& env);
// The update badge and its drop-down: the version running and the one on offer, nothing else. Draws nothing
// when there is no newer release; true when it drew, so the caller can lay the next button after it. `compact`
// makes it small enough for the quick panel's hint bar; `with_label` draws the state text immediately before it.
bool update_badge(ViewEnv& env, bool compact = false, bool with_label = false);
// The restart prompt, once per downloaded version: restart now, or later (the next launch applies it).
void update_restart_dialog(ViewEnv& env);

ImU32 tone_color(Tone tone);
void apply_profile(ViewEnv& env, int index);  // profiles.apply, then the profiles and config read again
// A thumbnail (or the screenshot itself) filling `size`, cropped to cover; an icon while it loads.
// `zoom` shows 1/zoom of the picture, centred (a hovered card's picture grows a little); 1 is the whole picture.
void thumbnail(ViewEnv& env, const UiMedia& item, ImVec2 min, ImVec2 size, float rounding, float zoom = 1.0f);
// A picture from a file already on disk (a Timeline session's poster), fitted the same way; `icon` while it loads.
void picture(ViewEnv& env, const std::string& path, const char* icon, ImVec2 min, ImVec2 size, float rounding, float zoom = 1.0f);

const UiMedia* find_media(const ViewEnv& env, const std::string& file);

std::vector<LinkView> link_views(const Json& state);
void copy_link(UiStore& store, const std::string& url);
void link_stop_button(ViewEnv& env, const char* id, const LinkView& link, float size, float icon_size, bool framed);
void open_preview(ViewEnv& env, const UiMedia& item, const std::vector<UiMedia>& list);
// The card's "…" menu: preview, edit, copy, show in folder, open, rename, delete.
void media_menu(ViewEnv& env, const char* popup_id, const UiMedia& item, const std::vector<UiMedia>& list);
// A Timeline card's menu: preview the footage, trim a clip out of it, erase it (asked twice, as Settings asks), show it.
void session_menu(ViewEnv& env, const char* popup_id, bool armed, const std::function<void(const char* action)>& run);
void run_media_action(ViewEnv& env, const std::string& action, const UiMedia& item, const std::vector<UiMedia>& list);

// The share sheet: opens for one library file and starts fitting it to the upload limit (the setting,
// or `limit_mib`). Drawn by media_dialogs; closing it cancels a job nobody used.
void open_share(UiStore& store, PanelState& panel, const UiMedia& item, int limit_mib = 0);
inline void open_share(ViewEnv& env, const UiMedia& item, int limit_mib = 0) { open_share(env.store, env.panel, item, limit_mib); }
void close_share(UiStore& store, PanelState& panel);

std::string hotkey_label(const std::string& action);
void hotkey_recorder(ViewEnv& env);
void media_dialogs(ViewEnv& env);

// The once-after-an-install changelog sheet: this build's release notes, or a button that asks GitHub for
// them. Also the one place version_seen moves forward.
void whats_new(ViewEnv& env);

}  // namespace bx::ui::parts
