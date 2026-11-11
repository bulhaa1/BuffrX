// A release body, turned into the rows the changelog sheet draws: headings, bullets and text, with the
// bold spans marked by offset. No HTML is interpreted and nothing is fetched - GitHub's markdown arrives
// as text and leaves as text, shaped for one column of the panel.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace bx {

enum class NoteKind { heading, bullet, text };

struct NoteRow {
  NoteKind kind = NoteKind::text;
  std::string text;  // the markers are already gone
  std::vector<std::pair<size_t, size_t>> bold;  // first = offset in text, second = length
};

// One row's text with its list or heading marker removed; kind says which it was.
inline std::string notes_clean(std::string line, NoteKind& kind) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return std::string();
  line.erase(0, first);

  kind = NoteKind::text;
  if (!line.empty() && line.front() == '#') {
    size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#') ++hashes;
    // A heading marker is hashes then either the end or a space. "#tag" is text, not a heading.
    if (hashes == line.size() || line[hashes] == ' ') {
      kind = NoteKind::heading;
      while (hashes < line.size() && line[hashes] == ' ') ++hashes;
      line.erase(0, hashes);
    }
  } else if (line.size() >= 2 && (line.front() == '-' || line.front() == '*') && line[1] == ' ') {
    kind = NoteKind::bullet;
    line.erase(0, 2);
  }

  // [label](address) keeps its label: the sheet never shows a URL and never makes anything clickable.
  size_t open = line.find('[');
  while (open != std::string::npos) {
    const size_t close = line.find(']', open + 1);
    if (close == std::string::npos) break;
    if (close + 1 < line.size() && line[close + 1] == '(') {
      const size_t end = line.find(')', close + 2);
      if (end != std::string::npos) {
        line.replace(open, end - open + 1, line.substr(open + 1, close - open - 1));
        open = line.find('[', open);
        continue;
      }
    }
    open = line.find('[', open + 1);
  }
  return line;
}

inline std::vector<NoteRow> note_rows(const std::string& body, size_t max_rows = 200) {
  std::vector<NoteRow> rows;
  size_t start = 0;
  while (start <= body.size() && rows.size() < max_rows) {
    size_t end = body.find('\n', start);
    const bool last = end == std::string::npos;
    if (last) end = body.size();

    NoteRow row;
    std::string text = notes_clean(body.substr(start, end - start), row.kind);
    start = end + 1;

    // **bold**: the markers go, the span stays marked for the face that draws it.
    std::string plain;
    plain.reserve(text.size());
    size_t bold_from = std::string::npos;
    for (size_t i = 0; i < text.size();) {
      if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
        if (bold_from == std::string::npos) {
          bold_from = plain.size();
        } else {
          row.bold.emplace_back(bold_from, plain.size() - bold_from);
          bold_from = std::string::npos;
        }
        i += 2;
        continue;
      }
      plain.push_back(text[i]);
      ++i;
    }
    row.text = plain;
    if (!row.text.empty()) rows.push_back(row);
    if (last) break;
  }
  return rows;
}
}  // namespace bx
