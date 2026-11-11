// The editor is a WebView2 page, and the runtime that draws it is Microsoft's, not ours. A PC without it starts
// buffrx-ui.exe, loses it a moment later, and shows nothing at all - which is how the editor came to be the one
// silent failure in the product. This is the rule that decides whether it can open, kept pure so the answer can be
// checked without a browser on the machine.
#pragma once

namespace buffrx {

// `query_result` is the HRESULT the loader returned (negative means the query itself failed); `version` is the
// runtime's own version string, or null when there is no runtime. An empty answer means absent, not unknown:
// WebView2 always names a version when it is installed.
inline bool webview2_present(long query_result, const wchar_t* version) {
  return query_result >= 0 && version != nullptr && version[0] != L'\0';
}

}  // namespace buffrx
