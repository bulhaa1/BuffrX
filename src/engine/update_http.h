// A bounded WinHTTP request for the update worker. Each operation completes through a callback, so a
// deadline can cancel a server that trickles headers or body bytes without closing a synchronous handle.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>

namespace bx {

class UpdateHttp {
public:
  UpdateHttp() : complete_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
                 closed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  UpdateHttp(const UpdateHttp&) = delete;
  UpdateHttp& operator=(const UpdateHttp&) = delete;

  ~UpdateHttp() {
    close_request();
    if (connection_) WinHttpCloseHandle(connection_);
    if (session_) WinHttpCloseHandle(session_);
    if (complete_) CloseHandle(complete_);
    if (closed_) CloseHandle(closed_);
  }

  bool open(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, bool secure,
            const wchar_t* user_agent, DWORD redirect_policy, int phase_ms, ULONGLONG deadline) {
    if (!complete_ || !closed_) return false;
    deadline_ = deadline;
    phase_ms_ = std::max(1, std::min(phase_ms, 5000));
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    session_ = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
#endif
    if (!session_) session_ = WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session_ || !WinHttpSetTimeouts(session_, phase_ms_, phase_ms_, phase_ms_, phase_ms_)) return false;
    connection_ = WinHttpConnect(session_, host, port, 0);
    if (!connection_) return false;
    request_ = WinHttpOpenRequest(connection_, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request_ || !WinHttpSetOption(request_, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                                       sizeof(redirect_policy))) return false;
    if (WinHttpSetStatusCallback(request_, &UpdateHttp::callback,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK) return false;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
    if (!WinHttpSetOption(request_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return false;
    context_set_ = true;
    return true;
  }

  bool send(const wchar_t* headers) {
    begin();
    if (!WinHttpSendRequest(request_, headers, headers ? static_cast<DWORD>(-1L) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(this))) return false;
    return wait_for(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
  }

  bool receive(DWORD& status) {
    begin();
    if (!WinHttpReceiveResponse(request_, nullptr) || !wait_for(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
      return false;
    DWORD size = sizeof(status);
    return WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
  }

  bool read(void* buffer, DWORD capacity, DWORD& bytes) {
    begin();
    if (!WinHttpReadData(request_, buffer, capacity, nullptr) || !wait_for(WINHTTP_CALLBACK_STATUS_READ_COMPLETE))
      return false;
    bytes = bytes_.load(std::memory_order_acquire);
    return true;
  }

  // The body's declared size, for a progress line. False when the server sent none (a chunked answer).
  bool content_length(int64_t& length) const {
    length = 0;
    if (!request_) return false;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &value, &size, WINHTTP_NO_HEADER_INDEX))
      return false;
    length = value;
    return length > 0;
  }

private:
  static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD length) {
    auto* self = reinterpret_cast<UpdateHttp*>(context);
    if (!self) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
      SetEvent(self->closed_);
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
      self->bytes_.store(length, std::memory_order_relaxed);
      self->status_.store(status, std::memory_order_release);
      SetEvent(self->complete_);
    }
  }

  void begin() {
    status_.store(0, std::memory_order_relaxed);
    bytes_.store(0, std::memory_order_relaxed);
    ResetEvent(complete_);
  }

  bool wait_for(DWORD expected) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline_) {
      close_request();
      return false;
    }
    const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(phase_ms_, deadline_ - now));
    if (WaitForSingleObject(complete_, wait_ms) != WAIT_OBJECT_0) {
      close_request();
      return false;
    }
    return status_.load(std::memory_order_acquire) == expected;
  }

  void close_request() {
    if (!request_) return;
    WinHttpCloseHandle(request_);
    request_ = nullptr;
    // HANDLE_CLOSING is the last callback. The callback context and read buffer must outlive it.
    if (context_set_) WaitForSingleObject(closed_, INFINITE);
    context_set_ = false;
  }

  HANDLE complete_ = nullptr, closed_ = nullptr;
  HINTERNET session_ = nullptr, connection_ = nullptr, request_ = nullptr;
  std::atomic<DWORD> status_{0}, bytes_{0};
  ULONGLONG deadline_ = 0;
  int phase_ms_ = 5000;
  bool context_set_ = false;
};

}  // namespace bx
