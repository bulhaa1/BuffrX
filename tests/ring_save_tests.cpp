// The replay ring's save rules that have no libobs in them (src/engine/ring_save.h): the name a save writes, so a
// save in the same second - or a prior save still being read - cannot land on the same path, the claim that makes a
// name this save's own even when another file appears in the same instant, and the verdict the host is handed, so a
// muxer that failed does not leave a valid-looking truncated clip announced as saved and a join with no muxer process
// is not announced as a failure.
//
// Built into buffrx-tests, this needs four calls in tests/engine_tests.cpp's main() beside test_ring():
//     g_failures += test_ring_save_names();
//     g_failures += test_ring_save_claims();
//     g_failures += test_ring_save_result();
//     g_failures += test_ring_save_parts();
// and this file in BUFFRX_TEST_SOURCES (CMakeLists.txt). The publication and settlement checks run inside the claim
// checks, so the harness needs no more calls. Built on its own with -DBUFFRX_RING_SAVE_STANDALONE it is a complete test
// program.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include <windows.h>

#include "../src/engine/ring_parts.h"
#include "../src/engine/ring_publish.h"
#include "../src/engine/ring_save.h"

namespace {

int g_ring_save_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++g_ring_save_failures;
}

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The file system as the claim loop sees it: `resolve_free` is reserve_output_path's own counter for a name already on
// disk, and a claim takes the name only by creating the file, exactly as claim_output_file does. An existing clip,
// another save's name and a race all refuse in the same way.
struct NameSpace {
  std::set<std::string> files;   // names on disk
  std::set<std::string> claims;  // names this process took and still holds (a save still being read or filed)
};

std::string resolve_free(const NameSpace& space, const std::string& stem) {
  std::string name = stem + ".mp4";
  int counter = 1;
  while (space.files.count(lower(name))) name = stem + " (" + std::to_string(counter++) + ").mp4";
  return name;
}

std::string claim_name(NameSpace& space, const std::string& stem) {
  const std::string path = resolve_free(space, stem);
  if (!space.claims.insert(lower(path)).second) return std::string();  // another save in this process holds it
  space.files.insert(lower(path));                                     // the claim creates the file
  return path;
}

std::string make_scratch_dir() {
  char root[MAX_PATH] = {0};
  if (!GetTempPathA(MAX_PATH, root)) return std::string();
  const std::string path = std::string(root) + "buffrx-ring-save-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(GetTickCount64());
  if (!CreateDirectoryA(path.c_str(), nullptr)) return std::string();
  return path;
}

bool write_bytes(const std::string& path, const char* text) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(std::strlen(text));
  const bool ok = WriteFile(file, text, size, &written, nullptr) != 0 && written == size;
  CloseHandle(file);
  return ok;
}

std::string read_bytes(const std::string& path) {
  std::string text;
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return text;
  char buffer[256];
  DWORD got = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) text.append(buffer, got);
  CloseHandle(file);
  return text;
}

bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A file with footage in it, as ring_output.cpp's file_has_bytes reads it: an empty file is not a clip.
bool file_has_bytes(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return false;
  return info.nFileSizeHigh != 0 || info.nFileSizeLow != 0;
}

// The claim ring_output.cpp makes: CREATE_NEW is the whole check, so the name is taken by creating it and a file that
// is already there refuses without being touched.
bool claim_file(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

// Publishing a clip: a rename that never replaces (MOVEFILE_REPLACE_EXISTING is not passed), and MOVEFILE_COPY_ALLOWED
// so the clip can cross to another volume when the parts are beside the ring's own file.
bool publish(const std::string& source, const std::string& target) {
  return MoveFileExA(source.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
}

// reserve_output_path's rule (library.cpp) against the real folder: the configured name, then a counter, until no file
// is there. The stems these checks use need none of its cleaning, so this is the whole of what they exercise.
std::string free_on_disk(const std::string& directory, const std::string& stem, const std::string& suffix) {
  std::string candidate = directory + "\\" + stem + suffix;
  int counter = 1;
  while (file_exists(candidate)) candidate = directory + "\\" + stem + " (" + std::to_string(counter++) + ")" + suffix;
  return candidate;
}

// Removes a folder these checks made, with whatever refusals and staged files it holds.
void remove_tree(const std::string& dir) {
  WIN32_FIND_DATAA entry{};
  HANDLE find = FindFirstFileA((dir + "\\*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::string name = entry.cFileName;
      if (name == "." || name == "..") continue;
      const std::string path = dir + "\\" + name;
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
      else DeleteFileA(path.c_str());
    } while (FindNextFileA(find, &entry));
    FindClose(find);
  }
  RemoveDirectoryA(dir.c_str());
}

// A fixed volume other than the scratch folder's, when this machine has one: a save in parts writes its packet parts
// beside the ring's own file, which need not be in the clips folder, so publishing one can cross volumes. Empty on a
// one-volume machine, and the crossing is then not checked.
std::string other_volume_scratch(const std::string& scratch) {
  if (scratch.empty()) return std::string();
  const char own = static_cast<char>(std::toupper(static_cast<unsigned char>(scratch[0])));
  const DWORD drives = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {  // A and B were floppies
    const char letter = static_cast<char>('A' + i);
    if (!(drives & (1u << i)) || letter == own) continue;
    const std::string root = std::string(1, letter) + ":\\";
    if (GetDriveTypeA(root.c_str()) != DRIVE_FIXED) continue;
    const std::string dir = root + "buffrx-ring-save-x" + std::to_string(GetCurrentProcessId());
    if (!CreateDirectoryA(dir.c_str(), nullptr)) continue;
    return dir;
  }
  return std::string();
}

}  // namespace

// Returns how many checks failed, so the test harness can add it to its own count.
int test_ring_save_names() {
  const int before = g_ring_save_failures;
  // The configured name is kept: the first save of a second writes exactly it, and only a collision adds a counter.
  expect(bx::save_attempt_stem("2026-11-11 10-00-00", 0) == "2026-11-11 10-00-00", "the first attempt is the configured name");
  expect(bx::save_attempt_stem("2026-11-11 10-00-00", 1) == "2026-11-11 10-00-00 (1)", "the second attempt keeps the name and adds (1)");
  expect(bx::save_attempt_stem("2026-11-11 10-00-00", 2) == "2026-11-11 10-00-00 (2)", "and (2) after that");

  // Two saves in one second: the host throttles its requests, a salvage and a save do not. The second save must not take
  // the first's path, even though the first's file is not on disk yet (its muxer has not opened it).
  {
    NameSpace space;
    const std::string stem = "2026-11-11 10-00-00";
    const std::string first = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    const std::string second = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    const std::string third = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    expect(first == "2026-11-11 10-00-00.mp4", "a repeated filename: the first save of the second is the configured name");
    expect(second != first && third != first && third != second, "a repeated filename: every save in the second takes its own path");
    expect(second == "2026-11-11 10-00-00 (1).mp4" && third == "2026-11-11 10-00-00 (2).mp4",
           "a repeated filename: the counter follows the configured name");
  }

  // A prior save still being read: the host filed its clip into the game's folder, which frees the root name, but the
  // process still holds the name while a trim may be reading the file it moved.
  {
    NameSpace space;
    const std::string stem = "2026-11-11 10-00-01";
    const std::string filing = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    space.files.erase(lower(filing));  // the host moved the clip out of the root
    const std::string next = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    expect(next != filing, "overlap with filing: a save still being filed keeps its name");
  }

  // A clip that is already on disk (an unfiled clip left at the root, a save from another engine): the file system's own
  // counter is used, so the join (media_concat) or the muxer never replaces it.
  {
    NameSpace space;
    const std::string stem = "2026-11-11 10-00-02";
    space.files.insert(lower(stem + ".mp4"));
    const std::string path = bx::save_path_for(stem, [&](const std::string& s) { return claim_name(space, s); });
    expect(path == stem + " (1).mp4", "an existing root clip is never the target of a new save");
  }

  // Every name refused (a pathological claim): the save reports that it has no path, rather than being handed a name
  // that belongs to another file.
  {
    const std::string path = bx::save_path_for("2026-11-11 10-00-03", [](const std::string&) { return std::string(); }, 5);
    expect(path.empty(), "a save with no free name gets no path at all");
  }

  return g_ring_save_failures - before;
}

// The publication rules against the real folder (ring_publish.h): a save writes a staging file its own name owns, then
// publishes it with a rename that never replaces. A competing writer that lands between choosing a name and taking it
// keeps every byte, the save takes the next name, and the verdict's path is the file that really holds the footage.
int test_ring_save_publish() {
  const int before = g_ring_save_failures;
  const std::string dir = make_scratch_dir();
  expect(!dir.empty(), "a scratch folder for the publication checks");
  if (dir.empty()) return g_ring_save_failures - before;

  const std::string stem = "2026-11-11 11-00-00";
  const auto free_name = [](const std::string& directory, const std::string& stem, const std::string& suffix) {
    return free_on_disk(directory, stem, suffix);
  };
  const auto no_claims = [](const std::string&) { return false; };

  // The name a save writes while it works: the library's listing drops it (library.cpp skips any name holding ".part"),
  // and it keeps the extension the muxer and the join choose the container from.
  {
    const std::string staged = bx::ring_staging_stem(stem, "4242-1") + ".mp4";
    expect(staged.find(".part") != std::string::npos, "a staging name carries .part, so the library never lists it");
    expect(staged.size() > 4 && staged.substr(staged.size() - 4) == ".mp4",
           "a staging name keeps the extension the container is chosen from");
    expect(staged != stem + ".mp4", "a staging name is never the name the person sees");
  }

  // A plain save: another writer creates a file at the chosen name after the save selected it. That writer's bytes are
  // never touched, the save lands on the next name, and the verdict's path is the file that holds the clip.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem(stem, "4242-1") + ".mp4";
    expect(write_bytes(staged, "our footage"), "the save wrote its staging file");
    bool raced = false;
    const std::string published = bx::ring_publish(
        staged, dir, stem, ".mp4", free_name,
        [&](const std::string& from, const std::string& to) {
          if (!raced) write_bytes(to, "another writer's clip");  // it lands between the choice and the rename
          raced = true;
          return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
        },
        no_claims);
    const std::string taken = dir + "\\" + stem + ".mp4";
    expect(raced, "the competing writer landed after the destination was chosen");
    expect(published == dir + "\\" + stem + " (1).mp4", "the save publishes under the next free name");
    expect(read_bytes(taken) == "another writer's clip", "the competing writer keeps every byte it wrote");
    expect(read_bytes(published) == "our footage", "the save's footage is published whole");
    expect(!file_exists(staged), "the staging name is renamed, not copied: the clip is one file");
    const bx::SaveOutcome result = bx::save_result(1, published, true, true, 0, 0, false, false);
    expect(bx::save_produced_clip(result), "a published clip is a save the host may announce");
    expect(result.path == published && read_bytes(result.path) == "our footage",
           "the verdict's path is the file that holds the clip");
  }

  // Every name refused: nothing is published, the footage stays at the save's own staging name, the save is not
  // announced as saved, and the verdict still names the file the footage is in rather than losing it.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem("busy", "4242-2") + ".mp4";
    expect(write_bytes(staged, "our footage"), "a second save wrote its staging file");
    int competing = 0;
    const std::string published = bx::ring_publish(
        staged, dir, "busy", ".mp4", free_name,
        [&](const std::string&, const std::string& to) {
          write_bytes(to, "another writer's clip");  // every name is taken in the instant it is chosen
          ++competing;
          return false;
        },
        no_claims, 4);
    expect(published.empty(), "every name another writer took: nothing is published");
    expect(competing == 4, "each refused attempt was one try, and no name was written instead");
    expect(read_bytes(staged) == "our footage", "the footage stays at the save's own staging name, lossless");
    const bool published_ok = !published.empty();
    const bx::SaveOutcome result = bx::save_result(2, staged, true, published_ok, 0, 0, false, false);
    expect(!bx::save_produced_clip(result), "a save that could not free a name is not announced as saved");
    expect(result.path == staged && read_bytes(result.path) == "our footage",
           "the failure still names the file the footage was kept in");
  }

  // A name this process already handed out is skipped, not taken again: the host files the clip into the game's folder
  // and frees the root name while a trim can still be reading the file it moved.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem("Held", "4242-4") + ".mp4";
    expect(write_bytes(staged, "our footage"), "a third save wrote its staging file");
    const std::string held = dir + "\\Held.mp4";
    const auto holds_that_name = [&](const std::string& key) { return key == bx::ring_name_key(held); };
    const std::string published = bx::ring_publish(
        staged, dir, "Held", ".mp4", free_name,
        [](const std::string& from, const std::string& to) {
          return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
        },
        holds_that_name);
    expect(published == dir + "\\Held (1).mp4", "a name this process already handed out is skipped");
    expect(!file_exists(held), "nothing is written at the name that is still claimed");
    expect(read_bytes(published) == "our footage", "the clip is published under the next name");
  }

  // A refused join (a failed concatenation): the newest packet part is the clip. It is brought to a staging name in the
  // destination folder - a move, crossing a volume when the part lives on another one - and published from there under
  // the same no-replace rule, with a competing writer landing on the first name it tries.
  {
    std::string part_dir = other_volume_scratch(dir);
    const bool cross_volume = !part_dir.empty();
    if (!cross_volume) part_dir = dir + "\\.buffrx";
    expect(cross_volume || CreateDirectoryA(part_dir.c_str(), nullptr) != 0, "a folder for the packet part");
    const std::string part = part_dir + "\\part-4242-3-0.mp4";
    expect(write_bytes(part, "newest part"), "the newest packet part, beside the ring's file");
    const std::string staged = bx::ring_stage(
        part, [&](int attempt) { return dir + "\\" + bx::ring_staging_stem("Fallback", "4242-3", attempt) + ".mp4"; },
        [](const std::string& from, const std::string& to) {
          return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED) != 0;  // crosses a volume when it must
        });
    expect(!staged.empty() && read_bytes(staged) == "newest part", "the part is staged in the destination folder");
    expect(!file_exists(part), "the part moved: the staged file is the clip, not a second copy");
    bool raced = false;
    const std::string published = bx::ring_publish(
        staged, dir, "Fallback", ".mp4", free_name,
        [&](const std::string& from, const std::string& to) {
          if (!raced) write_bytes(to, "a clip the person already had");
          raced = true;
          return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
        },
        no_claims);
    expect(published == dir + "\\Fallback (1).mp4", "the fallback publishes under the next free name");
    expect(read_bytes(dir + "\\Fallback.mp4") == "a clip the person already had",
           "the clip already at the name keeps its own bytes");
    expect(read_bytes(published) == "newest part", "the fallback clip is the newest part, whole");
    expect(!file_exists(staged), "the staging name is gone: the clip has one name");
    const bx::SaveOutcome result = bx::save_parts_result(3, published, true, false, 0, 7, false, true);
    expect(bx::save_produced_clip(result) && bx::save_left_footage_out(result),
           "the newest part alone is a save that leaves footage out");
    expect(result.path == published && read_bytes(result.path) == "newest part",
           "the verdict's path is the published fallback clip");
    if (cross_volume) RemoveDirectoryA(part_dir.c_str());
  }

  remove_tree(dir);
  return g_ring_save_failures - before;
}

// The writer's verdict decides publication (ring_publish.h's ring_settle): a save whose writer failed never gives its
// file the name a person sees, even when the file is full of footage, because a truncated clip under a normal name is a
// clip the library offers while the verdict says the save failed. Those bytes stay at the save's own staging name - the
// listing drops it - and the verdict names it, so the footage can be recovered. A finished writer is published, and a
// finished writer that found no free name stays at its staging name too, still named and still not announced as saved.
int test_ring_save_settlement() {
  const int before = g_ring_save_failures;
  const std::string dir = make_scratch_dir();
  expect(!dir.empty(), "a scratch folder for the settlement checks");
  if (dir.empty()) return g_ring_save_failures - before;

  const std::string stem = "2026-11-11 12-00-00";
  const std::string seen = dir + "\\" + stem + ".mp4";  // the name a person sees
  const auto free_name = [](const std::string& directory, const std::string& name, const std::string& suffix) {
    return free_on_disk(directory, name, suffix);
  };
  const auto no_claims = [](const std::string&) { return false; };
  const auto remove_empty = [](const std::string& path) { DeleteFileA(path.c_str()); };
  const auto move_never_replaces = [](const std::string& from, const std::string& to) {
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
  };
  const auto publish_here = [&](const std::string& source) {
    return bx::ring_publish(source, dir, stem, ".mp4", free_name, move_never_replaces, no_claims);
  };

  // A failed writer with bytes at its staging name: nothing is published - the publication step is not even asked - and
  // the verdict names the hidden staging file that still holds every byte the writer managed.
  std::string failed_stage;
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem(stem, "4242-9") + ".mp4";
    failed_stage = staged;
    expect(write_bytes(staged, "truncated footage"), "a failed writer left bytes at its staging name");
    bool publish_asked = false;
    const bx::SaveSettlement settled = bx::ring_settle(
        staged, false, file_has_bytes, remove_empty, [&](const std::string& source) {
          publish_asked = true;
          return publish_here(source);
        });
    expect(!publish_asked, "a failed writer never reaches publication: a truncated clip cannot take a clip's name");
    expect(settled.kept && !settled.published, "a failed writer's bytes are kept, not published");
    expect(settled.path == staged && read_bytes(settled.path) == "truncated footage",
           "the verdict names the file the failed writer's bytes are in");
    expect(settled.path.find(".part") != std::string::npos,
           "the kept file is at a staging name the library's listing drops");
    expect(!file_exists(seen), "no clip appears under the name a person sees");
    const bx::SaveOutcome plain = bx::save_result(1, settled.path, true, settled.published, 3, 0, false, false);
    expect(!bx::save_produced_clip(plain), "a muxer that exited non-zero is not a save the host may announce");
    expect(plain.path == settled.path && file_exists(plain.path), "the failed verdict names the kept bytes");
    const bx::SaveOutcome parts = bx::save_parts_result(2, settled.path, true, true, 3, 0, false, false);
    expect(!bx::save_produced_clip(parts), "a part whose write failed is not a save the host may announce");
    expect(parts.path == settled.path && file_exists(parts.path), "the failed part's verdict names its kept bytes");
  }

  // A failed writer that produced nothing: the empty staging file this save created is removed, and no name is announced.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem(stem, "4242-10") + ".mp4";
    expect(claim_file(staged), "the save created its empty staging file");
    const bx::SaveSettlement settled = bx::ring_settle(staged, false, file_has_bytes, remove_empty, publish_here);
    expect(!file_exists(staged), "an empty stage is removed: no 0-byte file is left beside the clips");
    expect(settled.path.empty() && !settled.kept && !settled.published,
           "nothing was written: the verdict has no path and announces nothing");
    expect(!file_exists(seen), "a save that wrote nothing publishes nothing");
  }

  // A finished writer: the file is published under the configured name, and a later save's publication never touches the
  // failed save's kept bytes.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem(stem, "4242-11") + ".mp4";
    expect(write_bytes(staged, "a whole clip"), "a finished writer wrote its staging file");
    const bx::SaveSettlement settled = bx::ring_settle(staged, true, file_has_bytes, remove_empty, publish_here);
    expect(settled.published && !settled.kept, "a finished writer is published");
    expect(settled.path == seen && read_bytes(settled.path) == "a whole clip",
           "the finished clip takes the configured name");
    expect(!file_exists(staged), "publishing moved the file: the clip has one name");
    expect(read_bytes(failed_stage) == "truncated footage",
           "publishing a clip never touches a failed save's kept bytes");
    const bx::SaveOutcome result = bx::save_result(3, settled.path, true, true, 0, 0, false, false);
    expect(bx::save_produced_clip(result) && result.path == settled.path,
           "a published clip is announced at the name it holds");
  }

  // A finished writer no name could be freed for: it stays at the staging name, whole, and is a failed save - the file
  // never reached the name the person asked for.
  {
    const std::string staged = dir + "\\" + bx::ring_staging_stem("Busy", "4242-12") + ".mp4";
    expect(write_bytes(staged, "a clip no name was free for"), "a finished writer wrote its staging file");
    const bx::SaveSettlement settled = bx::ring_settle(staged, true, file_has_bytes, remove_empty,
                                                       [](const std::string&) { return std::string(); });
    expect(!settled.published && !settled.kept, "a refused publication is not a published clip");
    expect(settled.path == staged && read_bytes(settled.path) == "a clip no name was free for",
           "the footage stays at the staging name, whole");
    const bx::SaveOutcome result = bx::save_result(4, settled.path, true, settled.published, 0, 0, false, false);
    expect(!bx::save_produced_clip(result), "a clip no name was freed for is not announced as saved");
    expect(result.path == settled.path && file_exists(result.path), "the failure names where the footage stayed");
  }

  // The newest packet part kept from a refused join is a valid clip: once staged in the destination folder, a finished
  // writer's settlement publishes it like any other clip and leaves no part behind.
  {
    const std::string parts_dir = dir + "\\.buffrx";
    expect(CreateDirectoryA(parts_dir.c_str(), nullptr) != 0, "the folder the packet parts live in");
    const std::string part = parts_dir + "\\part-4242-13-0.mp4";
    expect(write_bytes(part, "the newest part"), "the newest packet part, beside the ring's file");
    const std::string staged = bx::ring_stage(
        part, [&](int attempt) { return dir + "\\" + bx::ring_staging_stem("Fallback", "4242-13", attempt) + ".mp4"; },
        move_never_replaces);
    expect(!staged.empty() && read_bytes(staged) == "the newest part", "the part is staged in the destination folder");
    expect(!file_exists(part), "the part moved: the staged file is the clip, not a second copy");
    const bx::SaveSettlement settled = bx::ring_settle(
        staged, true, file_has_bytes, remove_empty,
        [&](const std::string& source) { return bx::ring_publish(source, dir, "Fallback", ".mp4", free_name, move_never_replaces, no_claims); });
    expect(settled.published && settled.path == dir + "\\Fallback.mp4", "the fallback part is published");
    expect(read_bytes(settled.path) == "the newest part", "the fallback clip holds the newest part, whole");
    expect(!file_exists(staged), "the fallback's staging name is gone: the clip has one name");
    const bx::SaveOutcome result = bx::save_parts_result(5, settled.path, true, false, 0, 7, false, true);
    expect(bx::save_produced_clip(result) && bx::save_left_footage_out(result),
           "the newest part alone is a clip that leaves footage out");
    expect(result.path == settled.path, "the verdict's path is the published fallback clip");
    DeleteFileA(result.path.c_str());
    RemoveDirectoryA(parts_dir.c_str());
  }

  remove_tree(dir);
  return g_ring_save_failures - before;
}

// The claim against the real file system: an existing clip is never touched, a name is taken by creating it, and a
// refusal never leaves a file it found changed.
int test_ring_save_claims() {
  const int before = g_ring_save_failures;
  const std::string dir = make_scratch_dir();
  expect(!dir.empty(), "a scratch folder for the claim checks");
  if (dir.empty()) return g_ring_save_failures - before;

  const std::string stem = "2026-11-11 10-00-00";
  const std::string existing = dir + "\\" + stem + ".mp4";
  expect(write_bytes(existing, "a clip the person already has"), "a clip is already in the folder");
  const auto claim = [&](const std::string& candidate) {
    const std::string path = dir + "\\" + candidate + ".mp4";
    return claim_file(path) ? path : std::string();
  };
  const std::string first = bx::save_path_for(stem, claim);
  expect(first == dir + "\\" + stem + " (1).mp4", "an existing clip is never the save's path");
  expect(read_bytes(existing) == "a clip the person already has", "the claim never writes into the clip already there");

  const std::string second = bx::save_path_for(stem, claim);
  expect(!second.empty() && second != first, "a second save in the same second takes its own name");
  expect(file_exists(first) && file_exists(second), "each save's name is a file it created, not a name it hoped for");

  // A name already taken refuses the claim and keeps its bytes: this is the check-then-write race, closed.
  expect(write_bytes(first, "keep"), "the first save's file has bytes in it");
  expect(!claim_file(first), "a name this process already took is refused");
  expect(read_bytes(first) == "keep", "a refused claim leaves the file it found untouched");

  // Publishing the newest part never replaces a file that appeared in the race, and a rename to a name the save owns lands.
  const std::string part = dir + "\\part.mp4";
  expect(write_bytes(part, "newest part"), "a packet part to publish");
  expect(!publish(part, second), "a rename never replaces the file already at the name");
  expect(read_bytes(second).empty(), "the file at that name keeps its own bytes");
  const std::string target = dir + "\\" + stem + " (3).mp4";
  expect(publish(part, target) && read_bytes(target) == "newest part", "the part lands at a name the save owns");

  DeleteFileA(existing.c_str());
  DeleteFileA(first.c_str());
  DeleteFileA(second.c_str());
  DeleteFileA(target.c_str());
  RemoveDirectoryA(dir.c_str());
  // The publication rules against a real folder: staging, then a rename that never replaces (ring_publish.h).
  test_ring_save_publish();
  // The writer's verdict gates publication, and a failed writer's bytes stay hidden (ring_publish.h's ring_settle).
  test_ring_save_settlement();
  return g_ring_save_failures - before;
}

int test_ring_save_result() {
  const int before = g_ring_save_failures;
  using bx::SaveOutcome;
  // A clean save: the muxer took every packet and exited zero.
  {
    const SaveOutcome result = bx::save_result(1, "C:/clips/a.mp4", true, true, 0, 0, false, false);
    expect(result.finished, "a finished save is marked finished");
    expect(bx::save_produced_clip(result), "a clean muxer is a clip the host may announce");
    expect(!bx::save_left_footage_out(result), "a clean save leaves nothing out");
    expect(result.muxer_exit == 0 && result.id == 1, "a clean save keeps its muxer exit and its id");
  }

  // A muxer that never started: there is no clip, and no exit code to report.
  {
    const SaveOutcome result = bx::save_result(2, "C:/clips/b.mp4", false, false, 0, 0, false, false);
    expect(!bx::save_produced_clip(result), "a save whose muxer never started is not a clip");
    expect(result.muxer_exit == -1, "a muxer that never started reports -1, not a clean zero");
  }

  // A forced muxer failure with a valid-looking partial: every packet was written and a file of the right size is there,
  // and the process still failed. It must not be announced as a saved clip.
  {
    const SaveOutcome result = bx::save_result(3, "C:/clips/c.mp4", true, true, 3, 0, false, false);
    expect(!bx::save_produced_clip(result), "a muxer exit code is the verdict: a truncated clip is not a save");
    expect(!result.ok, "a failed muxer is not ok even when every packet was written");
    expect(result.muxer_exit == 3 && result.path == "C:/clips/c.mp4", "the failure keeps the exit code and the path for the message");
  }

  // A pipe that refused a packet, with a muxer that then exited zero: still not a clip.
  {
    const SaveOutcome result = bx::save_result(4, "C:/clips/d.mp4", true, false, 0, 0, false, false);
    expect(!bx::save_produced_clip(result), "a packet the pipe refused is a failed save even at exit zero");
  }

  // Partial footage that is still a clip: the parts could not be joined and the newest part is the clip, or the disk part
  // could not be read. It is saved and it is said so - never silently.
  {
    const SaveOutcome joined = bx::save_result(5, "C:/clips/e.mp4", true, true, 0, 0, false, true);
    expect(bx::save_produced_clip(joined) && bx::save_left_footage_out(joined), "the newest part alone is a clip that leaves footage out");
    const SaveOutcome gap = bx::save_result(6, "C:/clips/f.mp4", true, true, 0, 12, true, false);
    expect(bx::save_produced_clip(gap) && bx::save_left_footage_out(gap), "an unreadable disk part is a clip with a gap");
    const SaveOutcome counted = bx::save_result(7, "C:/clips/g.mp4", true, true, 0, 3, false, false);
    expect(bx::save_left_footage_out(counted), "records left out make a clip partial even without the flag");
  }

  // The subsequent save: a new id and a clean muxer, so a host that kept the failed verdict for the id it asked about
  // never reads the failure of the save before it as this save's.
  {
    const SaveOutcome failed = bx::save_result(8, "C:/clips/h.mp4", true, true, 5, 0, false, false);
    const SaveOutcome next = bx::save_result(9, "C:/clips/i.mp4", true, true, 0, 0, false, false);
    expect(!bx::save_produced_clip(failed), "the failed save stays failed");
    expect(bx::save_produced_clip(next) && next.id != failed.id, "a subsequent save reports its own clean result");
  }

  return g_ring_save_failures - before;
}

// A save in parts reads whether an output was produced, not whether a muxer process ran: every part can be a Timeline
// chunk file, and the join then writes the clip with no process at all.
int test_ring_save_parts() {
  const int before = g_ring_save_failures;
  using bx::SaveOutcome;
  {
    const SaveOutcome joined = bx::save_parts_result(1, "C:/clips/a.mp4", true, false, 0, 0, false, false);
    expect(bx::save_produced_clip(joined), "a join with no muxer process is still a clip");
    expect(!bx::save_left_footage_out(joined), "a clean join leaves nothing out");
  }
  {
    // The join failed and there was no packet part to keep: no clip, and no invented exit code.
    const SaveOutcome none = bx::save_parts_result(2, "C:/clips/b.mp4", false, true, 0, 0, false, false);
    expect(!bx::save_produced_clip(none), "a save that produced no file is not a clip, even with a clean exit code");
    expect(none.muxer_exit == -1, "a save that produced no file reports -1, not a clean zero");
  }
  {
    // The join was refused and the newest packet part was kept: a clip that leaves footage out.
    const SaveOutcome kept = bx::save_parts_result(3, "C:/clips/c.mp4", true, false, 0, 7, false, true);
    expect(bx::save_produced_clip(kept) && bx::save_left_footage_out(kept), "the newest part alone is a clip that leaves footage out");
  }
  {
    // A part's muxer failed, which is a real failure however many parts had already been written.
    const SaveOutcome broken = bx::save_parts_result(4, "C:/clips/d.mp4", true, true, 3, 0, false, false);
    expect(!bx::save_produced_clip(broken), "a failed part is a failed save");
    expect(broken.muxer_exit == 3, "a failed part keeps its muxer exit for the message");
  }

  // A plan can be valid and hold no part: erase/retention won the race before the save's read guard, so every picture
  // the snapshot names was taken out of RAM for a chunk file that is gone. save_in_parts must not concatenate that plan
  // (it would read the front of an empty vector) and must not report an empty clip as saved; it reports a failure and
  // removes only its own empty staging file. The last shape here is the one that does survive: a packet part after a
  // gone chunk is written exactly as before, with the gone records counted as left out.
  {
    const auto video = [](uint64_t seq, int64_t dts, bool key, bool on_chunk, uint32_t chunk) {
      bx::RingEntry entry;
      entry.video = true;
      entry.video_keyframe = key;
      entry.video_seq = seq;
      entry.dts_usec = dts;
      entry.size = 10;
      entry.on_chunk = on_chunk;
      entry.chunk_id = chunk;
      return entry;
    };
    const auto sound = [](int64_t dts, bool on_chunk, uint32_t chunk) {
      bx::RingEntry entry;
      entry.dts_usec = dts;
      entry.size = 5;
      entry.on_chunk = on_chunk;
      entry.chunk_id = chunk;
      return entry;
    };
    const auto chunk_rec = [](uint32_t id, uint64_t at, uint32_t count) {
      bx::ChunkRec rec;
      rec.id = id;
      rec.file = "chunk-" + std::to_string(id) + ".mp4";
      rec.at = at;
      rec.count = count;
      rec.key_index = {0};
      rec.key_time_s = {0.0};
      return rec;
    };
    const auto gone = [](const bx::ChunkRec&) { return false; };

    // Erased-only chunks: every record of the snapshot was in the one chunk file, and that file is gone.
    {
      std::vector<bx::RingEntry> entries;
      for (uint64_t seq = 10; seq < 14; ++seq) {
        entries.push_back(video(seq, 1000000 + static_cast<int64_t>(seq - 10) * 50000, seq == 10, true, 1));
      }
      entries.push_back(sound(995000, true, 1));   // sound before the first picture: dropped quietly, not a gap
      entries.push_back(sound(1050000, true, 1));
      std::map<uint32_t, bx::ChunkRec> chunks;
      chunks[1] = chunk_rec(1, 10, 4);
      const bx::SavePlan plan = bx::plan_save_parts(entries, chunks, gone);
      expect(plan.valid && plan.parts.empty(), "an erased-only chunk plans no part at all");
      expect(plan.lost == 5, "every record the gone chunk held is counted as left out");
      expect(bx::newest_packet_part(plan) == -1, "an empty plan has no newest packet part to fall back to either");
    }

    // Audio-only remainder: the pictures were all in a gone chunk and what is left of the snapshot is sound.
    {
      std::vector<bx::RingEntry> entries;
      entries.push_back(video(30, 1000000, true, true, 4));
      entries.push_back(video(31, 1050000, false, true, 4));
      entries.push_back(sound(1200000, false, 0));  // sound the ring still holds; no picture follows it
      entries.push_back(sound(1250000, false, 0));
      std::map<uint32_t, bx::ChunkRec> chunks;
      chunks[4] = chunk_rec(4, 30, 2);
      const bx::SavePlan plan = bx::plan_save_parts(entries, chunks, gone);
      expect(plan.valid && plan.parts.empty(), "a snapshot left with sound only plans no part");
      expect(plan.lost == 2, "the pictures from the gone chunk are the gap; remaining sound is not a clip on its own");
    }

    // Surviving packet part: a gone chunk at the front, then pictures the ring still holds in plain packets.
    {
      std::vector<bx::RingEntry> entries;
      entries.push_back(video(40, 1000000, true, true, 5));   // gone chunk
      entries.push_back(video(41, 1033000, false, true, 5));  // gone chunk
      entries.push_back(video(42, 1066000, true, false, 0));  // still a packet, and the seam's keyframe
      entries.push_back(sound(1070000, false, 0));
      entries.push_back(video(43, 1100000, false, false, 0));
      entries.push_back(sound(1105000, false, 0));
      std::map<uint32_t, bx::ChunkRec> chunks;
      chunks[5] = chunk_rec(5, 40, 2);
      const bx::SavePlan plan = bx::plan_save_parts(entries, chunks, gone);
      expect(plan.valid && plan.parts.size() == 1 && !plan.parts[0].file, "a surviving packet part is the plan's one part");
      expect(plan.parts[0].entries.size() == 4, "it holds every packet the ring still has, not the gone chunk's");
      expect(plan.lost == 2 && bx::newest_packet_part(plan) == 0, "the gone records are skipped and the packet part can still be the clip");
      expect(plan.parts[0].entries.front() == 2 && entries[2].video_keyframe,
             "the packet part begins on the video keyframe after the gone chunk");
    }
  }
  return g_ring_save_failures - before;
}

#ifdef BUFFRX_RING_SAVE_STANDALONE
int main() {
  std::printf("ring save\n");
  int failures = test_ring_save_names();
  failures += test_ring_save_claims();
  failures += test_ring_save_result();
  failures += test_ring_save_parts();
  std::printf("%s\n", failures == 0 ? "ring save: every check passed" : "ring save: FAILURES");
  return failures == 0 ? 0 : 1;
}
#endif
