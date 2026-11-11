// The Timeline save's publication rules (src/engine/save_publish.h) against a real folder: a copy staged under
// a name of its own - claimed by creating it, with the join's sidecars, so a name another writer takes in between is
// refused rather than truncated - is published by a rename that never replaces, a name another writer took in the
// meantime refuses that rename and the copy lands under the next configured name, and a save that could not be
// published or could not claim a name keeps its footage instead of deleting it. A part an earlier save left is never
// opened over or removed, a save that runs out of final names names no file, and a released claim is only ever the
// save's own.
//
// Built into buffrx-tests, it needs one line in tests/engine_tests.cpp's main(), beside the other named suites:
//     g_failures += test_save_publish();
// and this file in BUFFRX_TEST_SOURCES (CMakeLists.txt). Built on its own with -DBUFFRX_SAVE_PUBLISH_STANDALONE
// it is a complete test program; link it with the target's other unit objects (BUFFRX_TEST_SOURCES) for
// reserve_output_path.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "../src/engine/library.h"
#include "../src/engine/media.h"
#include "../src/engine/save_publish.h"

namespace {

int g_save_publish_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++g_save_publish_failures;
}

bool write_file(const std::string& path, const std::string& bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

std::string read_file(const std::string& path) {
  std::string bytes;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return bytes;
  char buffer[512];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, got);
  std::fclose(file);
  return bytes;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The name reserve_output_path would hand out for one attempt: the configured stem, then " (n)" for a name that
// is already on disk. It is the caller's own counter step, exactly as commands.cpp's claim loop does it.
std::string free_name(const std::string& directory, const std::string& stem, const std::string& extension) {
  return bx::reserve_output_path(directory, stem, extension);
}

// A rename that never replaces, exactly the call the engine makes.
bool move_no_replace(const std::string& from, const std::string& to, unsigned long* error) {
  if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
  if (error) *error = GetLastError();
  return false;
}

// A file created only when nothing holds the name, exactly the call the engine makes: the creation is the claim.
bool create_new_file(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

void remove_file(const std::string& path) { DeleteFileA(path.c_str()); }

// The whole set one staged copy owns, as commands.cpp asks for it: the copy first, then the two sidecars a join
// child writes beside it. media.h names the sidecars, so the test and the engine cannot drift apart.
std::vector<std::string> staging_set(const std::string& copy, bool join_sidecars = true) {
  std::vector<std::string> files{copy};
  if (join_sidecars) {
    files.push_back(bx::media_join_result_path(copy));
    files.push_back(bx::media_join_spec_path(copy));
  }
  return files;
}

struct TempFolder {
  std::string folder;
  bool ok = false;

  TempFolder() {
    char base[MAX_PATH] = {};
    const DWORD got = GetTempPathA(MAX_PATH, base);
    if (got == 0 || got >= MAX_PATH) return;
    folder = std::string(base) + "buffrx-save-publish-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64());
    ok = CreateDirectoryA(folder.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
  }

  ~TempFolder() {
    if (folder.empty()) return;
    WIN32_FIND_DATAA entry{};
    HANDLE find = FindFirstFileA((folder + "\\*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileA((folder + "\\" + entry.cFileName).c_str());
      } while (FindNextFileA(find, &entry));
      FindClose(find);
    }
    RemoveDirectoryA(folder.c_str());
  }
};

// The next configured name, one counter step per call, held so two saves of one process never take one name.
struct NameClaim {
  std::string stem;
  int attempt = 0;
  std::string next(const std::string& directory, const std::string& extension) {
    std::string name = stem;
    if (attempt > 0) name += " (" + std::to_string(attempt) + ")";
    ++attempt;
    return free_name(directory, name, extension);
  }
};

// A final name another writer takes between this save's claim and its rename: the rename is refused, that
// writer's file keeps every byte, and the copy is published under the next configured name instead.
void test_competing_final_creator() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string staged = bx::reserve_output_path(temp.folder, bx::save_staging_stem("Clip", 4242, 1), ".mp4");
  expect(staged.find(".part") != std::string::npos, "a staged copy carries the marker the library drops");
  expect(staged.size() > 4 && staged.compare(staged.size() - 4, 4, ".mp4") == 0, "a staged copy keeps the container extension");
  expect(write_file(staged, "STAGED-FOOTAGE"), "the staged copy is written");
  // The replay saver (or any other program) takes the name this save would have published under.
  const std::string taken = temp.folder + "\\Clip.mp4";
  expect(write_file(taken, "OTHER-WRITERS-CLIP"), "the competing clip is written");

  NameClaim claim{"Clip"};
  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged, [&] { return claim.next(temp.folder, ".mp4"); }, move_no_replace);

  expect(published.path == temp.folder + "\\Clip (1).mp4", "the copy is published under the next configured name");
  expect(read_file(taken) == "OTHER-WRITERS-CLIP", "the competing clip keeps every byte");
  expect(read_file(published.path) == "STAGED-FOOTAGE", "the published clip is the staged footage");
  expect(!exists(staged), "the staged copy is gone once it is published");
}

// A part an earlier save left under the name this save would stage under is not this save's: the reservation
// moves to a name of its own, and the stranger's part is still there with its bytes after the save publishes.
void test_keeps_foreign_part() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  // The name attempt 1 would take, held by a part an earlier crash left.
  const std::string foreign = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 1) + ".mp4";
  expect(write_file(foreign, "EARLIER-PART"), "the earlier part is written");

  const std::string staged = bx::reserve_output_path(temp.folder, bx::save_staging_stem("Clip", 4242, 1), ".mp4");
  expect(staged != foreign, "a staged name another file holds is not taken");
  expect(write_file(staged, "THIS-SAVES-FOOTAGE"), "this save writes its own staged copy");

  NameClaim claim{"Clip"};
  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged, [&] { return claim.next(temp.folder, ".mp4"); }, move_no_replace);

  expect(published.path == temp.folder + "\\Clip.mp4", "the copy is published under the free configured name");
  expect(read_file(published.path) == "THIS-SAVES-FOOTAGE", "the published clip is this save's footage");
  expect(exists(foreign) && read_file(foreign) == "EARLIER-PART", "the earlier part is never deleted or opened over");
}

// Every name refused for a reason other than another file being there (a sharing violation, a folder that changed
// permissions): no name is published, and the verified footage is still in the staging file.
void test_failed_publication_keeps_staged() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string staged = bx::reserve_output_path(temp.folder, bx::save_staging_stem("Clip", 4242, 3), ".mp4");
  expect(write_file(staged, "VERIFIED-FOOTAGE"), "the staged copy is written");

  NameClaim claim{"Clip"};
  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged, [&] { return claim.next(temp.folder, ".mp4"); },
      [](const std::string&, const std::string&, unsigned long* error) {
        if (error) *error = ERROR_ACCESS_DENIED;
        return false;
      });

  expect(published.path.empty(), "a refused publication has no name");
  expect(published.error == ERROR_ACCESS_DENIED, "the refusal's own error is reported");
  expect(exists(staged) && read_file(staged) == "VERIFIED-FOOTAGE", "the verified footage is kept, not deleted");
  expect(!exists(temp.folder + "\\Clip.mp4"), "no empty clip is left under the configured name");
}

// Running out of names is the same failure: every name offered is another writer's, and the copy still has its bytes.
void test_every_name_taken() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  expect(write_file(temp.folder + "\\Clip.mp4", "TAKEN"), "the competing clip is written");
  const std::string staged = bx::reserve_output_path(temp.folder, bx::save_staging_stem("Clip", 4242, 4), ".mp4");
  expect(write_file(staged, "FOOTAGE"), "the staged copy is written");

  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged, [&] { return temp.folder + "\\Clip.mp4"; },  // the one name every attempt is refused on
      move_no_replace, 4);

  expect(published.path.empty(), "no name is published when every attempt is refused");
  expect(exists(staged) && read_file(staged) == "FOOTAGE", "the copy keeps its bytes when no name is free");
  expect(read_file(temp.folder + "\\Clip.mp4") == "TAKEN", "no competing clip is replaced");
}

// A staged name another writer's part already holds is not this save's: the creation refuses it, that part keeps its
// bytes, and the whole set - the copy and both sidecars - is claimed under a name of this save's own.
void test_foreign_staged_copy_refused() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string foreign = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 20) + ".mp4";
  expect(write_file(foreign, "OTHER-WRITERS-PART"), "the other writer's part is written");
  const std::string mine = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 21) + ".mp4";

  int asked = 0;
  const std::string staged = bx::claim_staging_path(
      [&] { return staging_set(asked++ == 0 ? foreign : mine); }, create_new_file, remove_file);

  expect(!staged.empty() && staged != foreign, "a staged name another file holds is never taken");
  expect(exists(foreign) && read_file(foreign) == "OTHER-WRITERS-PART", "the other writer's part keeps every byte");
  expect(exists(staged), "the claimed copy is this save's own file");
  expect(exists(bx::media_join_result_path(staged)) && exists(bx::media_join_spec_path(staged)),
         "the join's sidecars are claimed with the copy, so the child writes only this save's files");
}

// The actual race the reservation used to lose: the name is chosen, another writer creates that exact name before this
// save's create. The create refuses, the other writer's bytes stay, and the next attempt is claimed.
void test_staging_race_between_choice_and_create() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string raced = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 22) + ".mp4";
  const std::string mine = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 23) + ".mp4";

  int attempt = 0;
  const std::string staged = bx::claim_staging_path(
      [&] {
        const std::string copy = attempt++ == 0 ? raced : mine;
        if (copy == raced) expect(write_file(copy, "RACED-IN"), "another writer takes the chosen name first");
        return staging_set(copy);
      },
      create_new_file, remove_file);

  expect(staged == mine, "the attempt moves to a name this save could create");
  expect(exists(raced) && read_file(raced) == "RACED-IN", "the writer that won the race keeps its bytes");
  expect(exists(staged) && read_file(staged).empty(), "the claimed copy exists and is empty until the join writes it");
}

// A copy name is free but a sidecar beside it is not: the attempt is refused as a whole, the one file it made is
// removed again, and the stranger's sidecar is never opened over or deleted.
void test_foreign_sidecar_refused() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string first = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 24) + ".mp4";
  const std::string foreign_sidecar = bx::media_join_result_path(first);
  expect(write_file(foreign_sidecar, "EARLIER-OUTCOME"), "a sidecar an earlier writer left is written");
  const std::string mine = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 25) + ".mp4";

  int asked = 0;
  const std::string staged = bx::claim_staging_path(
      [&] { return staging_set(asked++ == 0 ? first : mine); }, create_new_file, remove_file);

  expect(staged == mine, "a set with a taken sidecar is refused whole and the next one is claimed");
  expect(!exists(first), "the copy the refused attempt made is removed again, so nothing empty is left behind");
  expect(exists(foreign_sidecar) && read_file(foreign_sidecar) == "EARLIER-OUTCOME",
         "the stranger's sidecar keeps every byte");
}

// Every staged name is another writer's file: no name is claimed, nothing is written and nothing is deleted.
void test_staging_names_exhausted() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  std::vector<std::string> taken;
  for (int index = 0; index < 3; ++index) {
    const std::string copy = temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 30u + static_cast<unsigned>(index)) + ".mp4";
    expect(write_file(copy, "TAKEN-" + std::to_string(index)), "a candidate name is another writer's file");
    taken.push_back(copy);
  }
  int asked = 0;
  const std::string staged = bx::claim_staging_path(
      [&] {
        // Every attempt lands on a name another writer holds.
        return staging_set(taken[static_cast<size_t>(asked++) % taken.size()]);
      },
      create_new_file, remove_file);

  expect(staged.empty(), "no staged path is claimed when every name is taken");
  expect(asked == bx::kSavePublishAttempts, "every attempt was made before giving up");
  for (size_t index = 0; index < taken.size(); ++index) {
    expect(read_file(taken[index]) == "TAKEN-" + std::to_string(index), "no taken file is opened over or deleted");
  }
}

// A save that runs out of final names names no file at all: the old loop handed the last refused name back, and the
// claim's destructor then erased a name another save held.
void test_save_name_claims_exhausted() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  // Every name this save will ask for is held by another save in flight.
  std::set<std::string> in_flight;
  for (int attempt = 0; attempt < 4; ++attempt) {
    in_flight.insert(free_name(temp.folder, bx::save_name_attempt("Clip", attempt), ".mp4"));
  }
  std::vector<std::string> asked;
  std::vector<std::string> mine;
  const std::string claimed = bx::claim_save_name(
      "Clip",
      [&](const std::string& name) {
        asked.push_back(name);
        return free_name(temp.folder, name, ".mp4");
      },
      [&](const std::string& path) {
        if (!in_flight.insert(path).second) return false;  // another save's hold
        mine.push_back(path);
        return true;
      },
      4);

  expect(claimed.empty(), "running out of final names names no file");
  expect(mine.empty(), "no name another save held is recorded as this save's own");
  expect(asked.size() == 4 && asked[0] == "Clip" && asked[1] == "Clip (1)" && asked[3] == "Clip (3)",
         "each attempt asks for the next configured name");
}

// A name held for one save is only released by the save that held it: a hold that another save refused leaves nothing
// to release, so a claim's destructor cannot free a name it never took.
void test_save_name_releases_only_its_own() {
  const std::string mine = "C:\\clips\\Clip.mp4";
  std::set<std::string> in_flight{mine};
  std::vector<std::string> released_by_me;
  const std::string refused = bx::claim_save_name(
      "Clip", [](const std::string& name) { return "C:\\clips\\" + name + ".mp4"; },
      [&](const std::string& candidate) {
        if (!in_flight.insert(candidate).second) return false;
        released_by_me.push_back(candidate);  // the save's own record: only names whose hold succeeded
        return true;
      },
      1);
  expect(refused.empty(), "a name every other save holds is not claimed");
  expect(released_by_me.empty(), "a refused name is never recorded as this save's own");
  expect(in_flight.size() == 1 && in_flight.count(mine) == 1, "the other save's hold is untouched");
}

// A range export's own publication: the staged copy is this export's, another writer takes the configured name while
// the range joins, and the verified file settles on the next name with both writers keeping every byte.
void test_range_export_publication() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  int asked = 0;
  const std::string staged = bx::claim_staging_path(
      [&] {
        return staging_set(temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 40u + static_cast<unsigned>(asked++)),
                           /*join_sidecars=*/false);
      },
      create_new_file, remove_file);
  expect(!staged.empty(), "the export owns a staged name");
  expect(write_file(staged, "EXPORTED-FOOTAGE"), "the export writes and verifies its staged copy");
  const std::string taken = temp.folder + "\\Clip.mp4";
  expect(write_file(taken, "REPLAY-SAVE"), "another writer takes the configured name first");

  std::set<std::string> in_flight;
  std::vector<std::string> claim;
  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged,
      [&] {
        return bx::claim_save_name(
            "Clip", [&](const std::string& name) { return free_name(temp.folder, name, ".mp4"); },
            [&](const std::string& path) {
              if (!in_flight.insert(path).second) return false;
              claim.push_back(path);
              return true;
            });
      },
      move_no_replace);

  expect(published.path == temp.folder + "\\Clip (1).mp4", "the export publishes under the next configured name");
  expect(read_file(published.path) == "EXPORTED-FOOTAGE", "the published clip is the export's verified footage");
  expect(read_file(taken) == "REPLAY-SAVE", "the writer that took the configured name keeps every byte");
  expect(!exists(staged), "the staged copy is gone once it is published");
}

// A publication that ran out of final names keeps the verified footage at its staging name instead of deleting it.
void test_exhausted_publication_keeps_staged() {
  TempFolder temp;
  if (!temp.ok) {
    expect(false, "the test folder is created");
    return;
  }
  const std::string staged = bx::claim_staging_path(
      [&] { return staging_set(temp.folder + "\\" + bx::save_staging_stem("Clip", 4242, 50), false); },
      create_new_file, remove_file);
  expect(!staged.empty() && write_file(staged, "VERIFIED-FOOTAGE"), "the export's staged copy is written");

  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged,
      [&] {
        return bx::claim_save_name("Clip",
                                   [&](const std::string& name) { return free_name(temp.folder, name, ".mp4"); },
                                   [](const std::string&) { return false; });  // every name is another save's
      },
      move_no_replace);

  expect(published.path.empty(), "no name is published when every final name is held elsewhere");
  expect(exists(staged) && read_file(staged) == "VERIFIED-FOOTAGE", "the verified footage is kept, not deleted");
}

}  // namespace

int test_save_publish() {
  test_competing_final_creator();
  test_keeps_foreign_part();
  test_failed_publication_keeps_staged();
  test_every_name_taken();
  test_foreign_staged_copy_refused();
  test_staging_race_between_choice_and_create();
  test_foreign_sidecar_refused();
  test_staging_names_exhausted();
  test_save_name_claims_exhausted();
  test_save_name_releases_only_its_own();
  test_range_export_publication();
  test_exhausted_publication_keeps_staged();
  if (g_save_publish_failures == 0) std::printf("save publish: all checks passed\n");
  return g_save_publish_failures;
}

#ifdef BUFFRX_SAVE_PUBLISH_STANDALONE
int main() { return test_save_publish() == 0 ? 0 : 1; }
#endif
