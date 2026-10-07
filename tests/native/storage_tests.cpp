// Realism: L5 - a real filesystem fixture under %TEMP%, the real INI profile
// API, the real environment block, the real recycle bin, a real loopback HTTP
// transfer and the real Kernel + storage executor. Every expectation is
// observed through raw Win32 calls (never through the service under test),
// and the fixture is removed before the first assert that could abort.

#include "rime/action/kernel.hpp"
#include "rime/core/cancellation.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/json.hpp"
#include "rime/win32/storage.hpp"
#include "rime/win32/storage_executor.hpp"

#include <winsock2.h>  // the download test owns a loopback server, so winsock
#include <ws2tcpip.h>  // has to be included (and linked) before windows.h
#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using rime::win32::DriveInfo;
using rime::win32::FileStatInfo;
using rime::win32::StorageService;
using ErrorCode = rime::core::Error::Code;

std::uint64_t deadline_ms() {
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         20000;
}

std::string to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

// Escapes backslashes for a payload the fixture path is pasted into.
std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char value : text) {
    if (value == '\\') out += "\\\\";
    else out += value;
  }
  return out;
}

// ---- raw Win32 observation -------------------------------------------------
// Nothing below calls StorageService: the assertions compare the service's
// output against what the operating system itself reports.

bool os_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::vector<std::uint8_t> os_read(const std::wstring& path) {
  std::vector<std::uint8_t> out;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return out;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < out.size()) {
      const std::size_t remaining = out.size() - total;
      const DWORD want = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
      DWORD got = 0;
      if (!ReadFile(file, out.data() + total, want, &got, nullptr) || got == 0) break;
      total += got;
    }
    out.resize(total);
  }
  CloseHandle(file);
  return out;
}

std::string os_read_text(const std::wstring& path) {
  const std::vector<std::uint8_t> bytes = os_read(path);
  return std::string(bytes.begin(), bytes.end());
}

bool os_write(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  std::size_t total = 0;
  while (total < bytes.size()) {
    const std::size_t remaining = bytes.size() - total;
    const DWORD want = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + total, want, &written, nullptr) || written == 0) {
      ok = false;
      break;
    }
    total += written;
  }
  CloseHandle(file);
  return ok;
}

bool os_write_text(const std::wstring& path, const std::string& text) {
  return os_write(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

DWORD os_attribs(const std::wstring& path) { return GetFileAttributesW(path.c_str()); }

// Golden Windows epoch offset: 100 ns intervals from 1601-01-01 to
// 1970-01-01. This is a published constant, not something the service under
// test computes for the test.
constexpr std::uint64_t kFileTimeUnixEpoch = 116444736000000000ull;

std::int64_t os_mtime_ms(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
  ULARGE_INTEGER value{};
  value.LowPart = data.ftLastWriteTime.dwLowDateTime;
  value.HighPart = data.ftLastWriteTime.dwHighDateTime;
  if (value.QuadPart < kFileTimeUnixEpoch) return -1;
  return static_cast<std::int64_t>((value.QuadPart - kFileTimeUnixEpoch) / 10000ull);
}

bool os_set_mtime_ms(const std::wstring& path, const std::int64_t unix_ms) {
  ULARGE_INTEGER value{};
  value.QuadPart = static_cast<std::uint64_t>(unix_ms) * 10000ull + kFileTimeUnixEpoch;
  FILETIME modified{};
  modified.dwLowDateTime = value.LowPart;
  modified.dwHighDateTime = value.HighPart;
  HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const BOOL ok = SetFileTime(file, nullptr, nullptr, &modified);
  CloseHandle(file);
  return ok != 0;
}

// Recursive fixture removal that clears read-only/hidden/system first, so a
// fixture the test itself hardened still deletes.
bool os_delete_tree(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return true;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) != 0;
  }
  const std::wstring pattern = path + L"\\*";
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW(pattern.c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (std::wstring(found.cFileName) == L"." || std::wstring(found.cFileName) == L"..") continue;
      os_delete_tree(path + L"\\" + found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  return RemoveDirectoryW(path.c_str()) != 0;
}

bool os_env_set(const wchar_t* name, const wchar_t* value) {
  return SetEnvironmentVariableW(name, value) != 0;
}

std::string os_env_get(const wchar_t* name) {
  const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) return {};
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(name, buffer.data(), needed);
  if (written == 0) return {};
  buffer.resize(written);
  return to_utf8(buffer);
}

// Reads one INI key through the profile API with a sentinel default, so a
// missing key is distinguishable from an empty value.
bool os_ini_key_equals(const std::wstring& file, const wchar_t* section, const wchar_t* key,
                       const std::string& expected) {
  wchar_t buffer[1024];
  const DWORD written =
      GetPrivateProfileStringW(section, key, L"\x01RIME-MISSING", buffer, 1024, file.c_str());
  const std::wstring value(buffer, written);
  if (!value.empty() && value[0] == L'\x01') return false;
  return to_utf8(value) == expected;
}

// Enumerates the section names the profile API reports for the file.
bool os_ini_section_exists(const std::wstring& file, const std::wstring& section) {
  std::wstring buffer(4096, L'\0');
  const DWORD written = GetPrivateProfileSectionNamesW(buffer.data(), 4096, file.c_str());
  if (written == 0) return false;
  const wchar_t* cursor = buffer.c_str();
  const wchar_t* const end = buffer.c_str() + written;
  while (cursor < end && *cursor != L'\0') {
    if (section == cursor) return true;
    cursor += std::wcslen(cursor) + 1;
  }
  return false;
}

UINT os_drive_type(const char letter) {
  wchar_t root[4] = {static_cast<wchar_t>(letter), L':', L'\\', L'\0'};
  return GetDriveTypeW(root);
}

// ---- loopback HTTP server --------------------------------------------------

// Serves `expected` connections, answering GET /ok with a fixed body and any
// other path with 404. stop() closes the listener, which unblocks an accept
// that would otherwise wait forever for a connection the test never makes.
class LoopbackServer {
 public:
  bool start(const std::string& body, const int expected) {
    body_ = body;
    expected_ = expected;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) !=
        0) {
      return false;
    }
    if (listen(listener_, 4) != 0) return false;
    int length = static_cast<int>(sizeof(address));
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
    port_ = ntohs(address.sin_port);
    worker_ = std::thread(&LoopbackServer::serve, this, listener_);
    return true;
  }

  void stop() {
    if (listener_ != INVALID_SOCKET) {
      const SOCKET listener = listener_;
      listener_ = INVALID_SOCKET;
      closesocket(listener);
    }
    if (worker_.joinable()) worker_.join();
  }

  [[nodiscard]] int port() const { return port_; }

 private:
  void serve(const SOCKET listener) {
    for (int served = 0; served < expected_; ++served) {
      const SOCKET client = accept(listener, nullptr, nullptr);
      if (client == INVALID_SOCKET) return;
      timeval timeout{};
      timeout.tv_sec = 5;
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 static_cast<int>(sizeof(timeout)));
      std::string request;
      char buffer[1024];
      while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        const int got = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
      }
      const std::string response =
          request.compare(0, 8, "GET /ok ") == 0
              ? "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_.size()) +
                    "\r\nConnection: close\r\n\r\n" + body_
              : std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                            "Connection: close\r\n\r\n");
      std::size_t sent = 0;
      while (sent < response.size()) {
        const int chunk = send(client, response.data() + sent,
                               static_cast<int>(response.size() - sent), 0);
        if (chunk <= 0) break;
        sent += static_cast<std::size_t>(chunk);
      }
      closesocket(client);
    }
  }

  SOCKET listener_{INVALID_SOCKET};
  int port_{0};
  int expected_{0};
  std::string body_;
  std::thread worker_;
};

}  // namespace

int main() {
  // Executors require the worker lane; this harness executes on the main thread.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());

  // The fixture lives in the temp directory under a per-pid name, so parallel
  // runs never collide. Nothing below has touched it yet when this asserts.
  wchar_t temp[MAX_PATH];
  const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
  assert(temp_length > 0 && temp_length < MAX_PATH);
  const std::wstring fixture = std::wstring(temp, temp_length) + L"rime_storage_test_" +
                               std::to_wstring(GetCurrentProcessId());
  if (os_exists(fixture)) os_delete_tree(fixture);
  assert(CreateDirectoryW(fixture.c_str(), nullptr) != 0);
  const std::string fixture_u8 = to_utf8(fixture);

  const std::wstring p_hello = fixture + L"\\hello.txt";
  const std::string hello_path = fixture_u8 + "\\hello.txt";
  const std::wstring p_cjk = fixture + L"\\cjk.txt";
  const std::string cjk_path = fixture_u8 + "\\cjk.txt";
  const std::wstring p_plain = fixture + L"\\plain.bin";
  const std::string plain_path = fixture_u8 + "\\plain.bin";
  const std::wstring p_missing = fixture + L"\\missing.txt";
  const std::string missing_path = fixture_u8 + "\\missing.txt";
  const std::wstring p_listing = fixture + L"\\listing";
  const std::string listing_path = fixture_u8 + "\\listing";
  const std::wstring p_append = fixture + L"\\append.txt";
  const std::string append_path = fixture_u8 + "\\append.txt";
  const std::wstring p_bom = fixture + L"\\bom.txt";
  const std::string bom_path = fixture_u8 + "\\bom.txt";
  const std::wstring p_utf16 = fixture + L"\\utf16.txt";
  const std::string utf16_path = fixture_u8 + "\\utf16.txt";
  const std::wstring p_utf16be = fixture + L"\\utf16be.txt";
  const std::string utf16be_path = fixture_u8 + "\\utf16be.txt";
  const std::wstring p_trunc = fixture + L"\\trunc.txt";
  const std::string trunc_path = fixture_u8 + "\\trunc.txt";
  const std::wstring p_copy = fixture + L"\\copy.txt";
  const std::string copy_path = fixture_u8 + "\\copy.txt";
  const std::wstring p_moved = fixture + L"\\moved.txt";
  const std::string moved_path = fixture_u8 + "\\moved.txt";
  const std::wstring p_nested = fixture + L"\\nested\\deep\\leaf.txt";
  const std::wstring p_tree = fixture + L"\\tree";
  const std::string tree_path = fixture_u8 + "\\tree";
  const std::wstring p_tree_copy = fixture + L"\\tree-copy";
  const std::string tree_copy_path = fixture_u8 + "\\tree-copy";
  const std::wstring p_tree_move = fixture + L"\\tree-move";
  const std::wstring p_tree_moved = fixture + L"\\tree-moved";
  const std::string tree_moved_path = fixture_u8 + "\\tree-moved";
  const std::wstring p_dirdelete = fixture + L"\\dirdelete";
  const std::string dirdelete_path = fixture_u8 + "\\dirdelete";
  const std::wstring p_ini = fixture + L"\\data.ini";
  const std::string ini_path = fixture_u8 + "\\data.ini";
  const std::wstring p_ini_parent = fixture + L"\\no-such-dir\\data.ini";
  const std::string ini_parent_path = fixture_u8 + "\\no-such-dir\\data.ini";
  // Raw OS evidence captured before any service call: both paths are absent
  // on disk, so the later "file not found"/"path not found" results cannot be
  // explained by anything this test itself created.
  const bool missing_absent_on_disk = !os_exists(p_missing);
  const bool ini_parent_absent_on_disk = !os_exists(p_ini_parent);
  const std::wstring p_recycle = fixture + L"\\recycle-me.txt";
  const std::string recycle_path = fixture_u8 + "\\recycle-me.txt";
  const std::wstring p_link = fixture + L"\\link.lnk";
  const std::string link_path = fixture_u8 + "\\link.lnk";
  const std::wstring p_installed = fixture + L"\\installed.txt";
  const std::string installed_path = fixture_u8 + "\\installed.txt";
  const std::wstring p_download = fixture + L"\\download.bin";
  const std::string download_path = fixture_u8 + "\\download.bin";
  const std::wstring p_download_deadline = fixture + L"\\download-deadline.bin";
  const std::string download_deadline_path = fixture_u8 + "\\download-deadline.bin";
  const std::wstring p_handle = fixture + L"\\handle.txt";
  const std::string handle_path = fixture_u8 + "\\handle.txt";
  const std::wstring p_exec_append = fixture + L"\\exec-append.txt";
  const std::string exec_append_path = fixture_u8 + "\\exec-append.txt";
  const std::wstring p_exec_time = fixture + L"\\exec-time.txt";
  const std::string exec_time_path = fixture_u8 + "\\exec-time.txt";
  const std::wstring p_exec_denied = fixture + L"\\exec-denied.txt";
  const std::string exec_denied_path = fixture_u8 + "\\exec-denied.txt";

  StorageService service;

  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"filesystem.write"}));
  const auto executor = std::make_shared<rime::win32::StorageExecutor>(service);
  assert(kernel.register_executor("storage.write", executor).ok());

  // --- Capture phase ---------------------------------------------------
  // Everything below mutates the real filesystem WITHOUT asserting: assert()
  // aborts and runs no destructors, so a failure here would leave the fixture
  // behind. Every outcome is recorded, the fixture is deleted, and only then
  // are they asserted.

  const std::string hello_text = "rime-storage-hello";
  const std::string cjk_text = "rime-\xE6\xB5\x8B\xE8\xAF\x95-\xE2\x9C\x93";
  const std::vector<std::uint8_t> plain_bytes = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};

  const bool wrote_hello = os_write_text(p_hello, hello_text);
  const bool wrote_cjk = os_write_text(p_cjk, cjk_text);
  const bool wrote_plain = os_write(p_plain, plain_bytes);

  // Whole-file reads decode exactly what the OS holds.
  std::string text;
  const auto read_hello = service.read_text(hello_path, text);
  const bool hello_matches = read_hello.ok() && text == hello_text;
  const auto read_cjk = service.read_text(cjk_path, text);
  const bool cjk_matches = read_cjk.ok() && text == cjk_text;
  std::vector<std::uint8_t> bytes;
  const auto read_plain = service.read_bytes(plain_path, bytes);
  const bool plain_matches = read_plain.ok() && bytes == plain_bytes;
  const auto read_missing = service.read_text(missing_path, text);
  const auto read_directory = service.read_text(fixture_u8, text);
  const auto read_empty_path = service.read_text("", text);

  // stat: the stamp and the attribute bits are written with raw Win32 first,
  // so the service is read against values it never produced.
  const std::int64_t stamped_mtime = 1700000000000ll;  // 2023-11-14T22:13:20Z
  const bool stamped = os_set_mtime_ms(p_hello, stamped_mtime);
  const bool hardened =
      SetFileAttributesW(p_hello.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY) != 0;
  FileStatInfo stat_info;
  const auto stat_hello = service.stat(hello_path, stat_info);
  const bool stat_matches =
      stat_hello.ok() && stat_info.size == hello_text.size() &&
      stat_info.mtime_unix_ms == stamped_mtime && !stat_info.is_dir &&
      stat_info.attrib_string == "RA" &&
      (stat_info.attrib_bits & FILE_ATTRIBUTE_READONLY) != 0 &&
      (stat_info.attrib_bits & FILE_ATTRIBUTE_ARCHIVE) != 0;
  const std::int64_t observed_mtime = os_mtime_ms(p_hello);
  const auto stat_missing = service.stat(missing_path, stat_info);
  FileStatInfo dir_stat;
  const auto stat_dir = service.stat(fixture_u8, dir_stat);
  const bool dir_stat_ok = stat_dir.ok() && dir_stat.is_dir;

  // list: three children created with raw Win32, sorted by byte order.
  const bool made_listing = CreateDirectoryW(p_listing.c_str(), nullptr) != 0;
  const bool wrote_listing_a = os_write_text(p_listing + L"\\a.txt", "alpha");
  const bool wrote_listing_b = os_write_text(p_listing + L"\\b.bin", "beta");
  const bool made_listing_sub = CreateDirectoryW((p_listing + L"\\sub").c_str(), nullptr) != 0;
  std::vector<rime::win32::DirEntryInfo> entries;
  const auto list_listing = service.list(listing_path, entries);
  const bool listing_matches = list_listing.ok() && entries.size() == 3 &&
                               entries[0].name == "a.txt" && !entries[0].is_dir &&
                               entries[1].name == "b.bin" && !entries[1].is_dir &&
                               entries[2].name == "sub" && entries[2].is_dir;
  const auto list_file = service.list(hello_path, entries);
  const auto list_missing = service.list(missing_path, entries);

  // Environment block: one variable seeded by the OS, one written by the
  // service and observed back through GetEnvironmentVariableW.
  const bool env_seeded = os_env_set(L"RIME_STORAGE_TEST_OBSERVED", L"seeded-by-os");
  std::string env_value;
  const auto env_read = service.env_get("RIME_STORAGE_TEST_OBSERVED", env_value);
  const bool env_matches = env_read.ok() && env_value == "seeded-by-os";
  std::string env_unset;
  const auto env_read_unset = service.env_get("RIME_STORAGE_TEST_NEVER_SET", env_unset);
  const bool env_unset_ok = env_read_unset.ok() && env_unset.empty();
  const auto env_set_result = service.env_set("RIME_STORAGE_TEST_SERVICE", "set-by-service");
  const std::string env_service_observed = os_env_get(L"RIME_STORAGE_TEST_SERVICE");
  const auto env_bad_name = service.env_set("A=B", "value");

  // Encoding: bytes are crafted (or compared) byte for byte, so no
  // expectation is produced by the encoder under test.
  const auto enc_utf16 = service.set_encoding("utf-16");
  const bool enc_utf16_ok = enc_utf16.ok() && service.encoding() == "utf-16";
  const std::string utf16_text = "rime-UTF16";
  const bool wrote_utf16 = service.write_text(utf16_path, utf16_text).ok();
  std::vector<std::uint8_t> utf16_expected = {0xFF, 0xFE};
  for (const char value : utf16_text) {
    utf16_expected.push_back(static_cast<std::uint8_t>(value));
    utf16_expected.push_back(0x00);
  }
  const std::vector<std::uint8_t> utf16_observed = os_read(p_utf16);
  std::string utf16_read_back;
  const auto utf16_roundtrip = service.read_text(utf16_path, utf16_read_back);
  const bool utf16_matches = utf16_roundtrip.ok() && utf16_read_back == utf16_text;

  const bool wrote_utf16be = os_write(p_utf16be, {0xFE, 0xFF, 0x00, 0x41, 0x00, 0x42});
  const auto enc_utf16be = service.set_encoding("utf-16-be");
  std::string utf16be_read;
  const auto utf16be_roundtrip = service.read_text(utf16be_path, utf16be_read);
  const bool utf16be_matches = utf16be_roundtrip.ok() && utf16be_read == "AB";

  const auto enc_bom = service.set_encoding("utf-8-bom");
  const bool wrote_bom = service.write_text(bom_path, "first").ok();
  const auto bom_append = service.append_text(bom_path, "second");
  const std::vector<std::uint8_t> bom_observed = os_read(p_bom);
  const auto enc_bad = service.set_encoding("gbk");
  const auto enc_restore = service.set_encoding("UTF-8");
  const bool encoding_restored = enc_restore.ok() && service.encoding() == "utf-8";

  // append_text creates the file when it is missing and never re-adds a mark.
  const auto append_first = service.append_text(append_path, "one");
  const auto append_second = service.append_text(append_path, "two");
  const std::string append_observed = os_read_text(p_append);

  // Handle table: ids are opaque, stale ids fail, and the raw file only
  // changes once the handle is used.
  std::uint64_t handle = 0;
  std::uint64_t hello_length = 0;
  std::uint64_t length = 0;
  const std::size_t handles_before = service.open_handle_count();
  const auto open_hello = service.file_open(hello_path, "r", handle, hello_length);
  std::vector<std::uint8_t> chunk;
  bool eof = false;
  const auto read_chunk = service.file_read(handle, 5, chunk, eof);
  const bool chunk_matches = read_chunk.ok() && !eof && chunk.size() == 5 &&
                             std::string(chunk.begin(), chunk.end()) == hello_text.substr(0, 5);
  std::uint64_t pos = 0;
  std::uint64_t file_length = 0;
  const auto stat_handle = service.file_stat(handle, pos, file_length);
  const bool stat_handle_ok =
      stat_handle.ok() && pos == 5 && file_length == hello_text.size();
  std::uint64_t seek_pos = 0;
  const auto seek_begin = service.file_seek(handle, 0, 0, seek_pos);
  const auto read_rest = service.file_read(handle, 100, chunk, eof);
  const bool rest_matches =
      read_rest.ok() && eof && chunk.size() == hello_text.size() &&
      std::string(chunk.begin(), chunk.end()) == hello_text;
  const auto seek_bad_whence = service.file_seek(handle, 0, 7, seek_pos);
  const auto read_too_many = service.file_read(handle, (64u << 20) + 1u, chunk, eof);
  const auto close_hello = service.file_close(handle);
  const auto read_stale = service.file_read(handle, 1, chunk, eof);
  const auto close_stale = service.file_close(handle);
  const std::size_t handles_after = service.open_handle_count();

  std::uint64_t scratch_handle = 0;
  std::uint64_t scratch_length = 0;
  const auto open_bad_mode = service.file_open(hello_path, "x", scratch_handle, scratch_length);
  const auto open_missing_file = service.file_open(missing_path, "r", scratch_handle, scratch_length);
  const auto open_directory = service.file_open(fixture_u8, "r", scratch_handle, scratch_length);
  const auto open_append = service.file_open(append_path, "a", handle, length);
  const auto write_handle = service.handle_write(handle, std::vector<std::uint8_t>{0x21});
  const auto close_append = service.file_close(handle);
  const std::string append_after_write = os_read_text(p_append);
  const auto write_stale = service.handle_write(handle, std::vector<std::uint8_t>{0x78});
  std::uint64_t trunc_handle = 0;
  const auto open_trunc = service.file_open(trunc_path, "w", trunc_handle, length);
  const auto close_trunc = service.file_close(trunc_handle);
  const bool trunc_is_empty = os_read(p_trunc).empty();

  // Copy / move / delete, all observed with raw reads.
  const auto copy_ok = service.file_copy(hello_path, copy_path, false);
  const std::vector<std::uint8_t> copy_bytes = os_read(p_copy);
  const auto copy_again = service.file_copy(hello_path, copy_path, false);
  // copy_ok carried hello's READONLY bit over (CopyFile copies attributes).
  // Win32 and AHK both refuse to overwrite a read-only destination, so that
  // bit would make the overwrite below test the refusal rather than the
  // overwrite; the destination is reset to a plain file first.
  const bool copy_made_writable = SetFileAttributesW(p_copy.c_str(), FILE_ATTRIBUTE_ARCHIVE) != 0;
  const auto copy_over = service.file_copy(hello_path, copy_path, true);
  const auto copy_missing = service.file_copy(missing_path, fixture_u8 + "\\copy-never.txt", false);
  const auto move_ok = service.file_move(copy_path, moved_path, false);
  const std::vector<std::uint8_t> hello_bytes(hello_text.begin(), hello_text.end());
  const bool move_observable =
      os_exists(p_moved) && !os_exists(p_copy) && os_read(p_moved) == hello_bytes;
  const auto move_missing = service.file_move(missing_path, fixture_u8 + "\\moved-never.txt", false);
  const auto delete_missing = service.file_delete(missing_path);
  const auto delete_directory = service.file_delete(fixture_u8);

  // Nested directory creation, two new levels deep.
  const auto make_nested = service.dir_create(fixture_u8 + "\\nested\\deep");
  const bool nested_ok = os_attribs(fixture + L"\\nested") != INVALID_FILE_ATTRIBUTES &&
                         os_attribs(fixture + L"\\nested\\deep") != INVALID_FILE_ATTRIBUTES;
  const bool wrote_leaf = os_write_text(p_nested, "leaf");

  const bool made_tree = CreateDirectoryW(p_tree.c_str(), nullptr) != 0;
  const bool wrote_tree_child = os_write_text(p_tree + L"\\child.txt", "tree-child");
  const bool made_tree_sub = CreateDirectoryW((p_tree + L"\\sub").c_str(), nullptr) != 0;
  const bool wrote_tree_grand = os_write_text(p_tree + L"\\sub\\grand.txt", "tree-grand");
  const bool made_tree_move = CreateDirectoryW(p_tree_move.c_str(), nullptr) != 0;
  const bool wrote_tree_move_child = os_write_text(p_tree_move + L"\\only.txt", "only");
  const bool made_dirdelete = CreateDirectoryW(p_dirdelete.c_str(), nullptr) != 0;
  const bool wrote_dirdelete_child = os_write_text(p_dirdelete + L"\\keep.txt", "keep");
  const std::string tree_move_path = fixture_u8 + "\\tree-move";

  const auto dir_delete_non_recursive = service.dir_delete(dirdelete_path, false);
  const bool dirdelete_survived = os_exists(p_dirdelete);
  const auto dir_delete_recursive = service.dir_delete(dirdelete_path, true);
  const bool dirdelete_removed = !os_exists(p_dirdelete);
  const auto dir_delete_missing = service.dir_delete(dirdelete_path, true);
  const auto dir_delete_on_file = service.dir_delete(hello_path, true);

  const auto dir_copy_ok = service.dir_copy(tree_path, tree_copy_path, false);
  const bool dir_copy_observable =
      os_exists(p_tree_copy) && os_read_text(p_tree_copy + L"\\child.txt") == "tree-child" &&
      os_read_text(p_tree_copy + L"\\sub\\grand.txt") == "tree-grand";
  const auto dir_copy_missing = service.dir_copy(dirdelete_path, tree_copy_path, true);
  const auto dir_copy_onto_file = service.dir_copy(tree_path, hello_path, false);
  const auto dir_move_ok = service.dir_move(tree_move_path, tree_moved_path, false);
  const bool dir_move_observable =
      os_exists(p_tree_moved) && !os_exists(p_tree_move) &&
      os_read_text(p_tree_moved + L"\\only.txt") == "only";
  const auto dir_move_missing = service.dir_move(dirdelete_path, tree_moved_path, true);

  // setAttrib: raw attribute bits in, raw attribute bits out.
  const auto attrib_add = service.set_attrib(plain_path, "R", "");
  const DWORD attrib_after_add = os_attribs(p_plain);
  const auto attrib_remove = service.set_attrib(plain_path, "", "R");
  const DWORD attrib_after_remove = os_attribs(p_plain);
  const auto attrib_none = service.set_attrib(plain_path, "", "");
  const auto attrib_unknown = service.set_attrib(plain_path, "Z", "");
  FileStatInfo plain_stat;
  const auto attrib_stat = service.stat(plain_path, plain_stat);
  const bool attrib_string_ok = attrib_stat.ok() && plain_stat.attrib_string == "A";

  // setTime: the stamp is verified through the raw FILETIME, using the
  // golden epoch offset above, never through the service.
  const std::int64_t file_mtime = 1600000000000ll;  // 2020-09-13T12:26:40Z
  const auto time_ok = service.set_time(plain_path, "mtime", file_mtime);
  const std::int64_t time_observed = os_mtime_ms(p_plain);
  const auto time_bad_which = service.set_time(plain_path, "created", file_mtime);
  const auto time_out_of_range = service.set_time(plain_path, "mtime", -1);
  const auto time_missing = service.set_time(missing_path, "mtime", file_mtime);

  // INI: every observation goes through the profile API directly.
  const std::string ini_missing_path = fixture_u8 + "\\no-such.ini";
  const auto ini_create = service.ini_write(ini_path, "section", "key", "value");
  const bool ini_observed = os_ini_key_equals(p_ini, L"section", L"key", "value");
  std::string ini_value;
  const auto ini_read_ok = service.ini_read(ini_path, "section", "key", ini_value);
  const bool ini_read_matches = ini_read_ok.ok() && ini_value == "value";
  const auto ini_overwrite = service.ini_write(ini_path, "section", "key", "updated");
  const bool ini_updated = os_ini_key_equals(p_ini, L"section", L"key", "updated");
  const auto ini_second = service.ini_write(ini_path, "section", "second", "other");
  const auto ini_read_missing_key = service.ini_read(ini_path, "section", "nope", ini_value);
  const auto ini_read_missing_section = service.ini_read(ini_path, "nosuch", "key", ini_value);
  const auto ini_read_missing_file = service.ini_read(ini_missing_path, "section", "key", ini_value);
  const auto ini_read_empty_section = service.ini_read(ini_path, "", "key", ini_value);
  const auto ini_bad_parent = service.ini_write(ini_parent_path, "section", "key", "value");
  const auto ini_delete_key = service.ini_delete(ini_path, "section", "key");
  const bool ini_key_gone = !os_ini_key_equals(p_ini, L"section", L"key", "updated") &&
                            os_ini_key_equals(p_ini, L"section", L"second", "other");
  const auto ini_delete_missing_key = service.ini_delete(ini_path, "section", "nope");
  const auto ini_delete_missing_section = service.ini_delete(ini_path, "nosuch", "key");
  const auto ini_delete_missing_file = service.ini_delete(ini_missing_path, "section", "key");
  const auto ini_delete_section = service.ini_delete(ini_path, "section", "");
  const bool ini_section_gone = !os_ini_section_exists(p_ini, L"section");

  // Drives: only parameter validation and a letter with no root. No drive is
  // ever ejected, locked or relabelled by this test.
  const std::uint32_t drive_mask = GetLogicalDrives();
  char present_letter = 0;
  char absent_letter = 0;
  for (char letter = 'A'; letter <= 'Z'; ++letter) {
    const bool present = (drive_mask & (1u << (letter - 'A'))) != 0;
    if (present && present_letter == 0) present_letter = letter;
    if (!present && absent_letter == 0) absent_letter = letter;
  }
  const std::string present_drive = std::string(1, present_letter) + ":";
  const std::string absent_drive = std::string(1, absent_letter) + ":";
  const std::wstring present_root =
      std::wstring(1, static_cast<wchar_t>(present_letter)) + L":\\";

  DriveInfo drive;
  const auto drive_type_result = service.drive_get("type", present_drive, drive);
  const UINT raw_type = os_drive_type(present_letter);
  std::string expected_type = "Unknown";
  if (raw_type == DRIVE_REMOVABLE) expected_type = "Removable";
  else if (raw_type == DRIVE_FIXED) expected_type = "Fixed";
  else if (raw_type == DRIVE_REMOTE) expected_type = "Network";
  else if (raw_type == DRIVE_CDROM) expected_type = "CDROM";
  else if (raw_type == DRIVE_RAMDISK) expected_type = "RAMDisk";
  const bool drive_type_matches = drive_type_result.ok() && present_letter != 0 &&
                                  drive.letter == present_drive && drive.type == expected_type;

  DriveInfo list_all;
  const auto drive_list_all = service.drive_get("list", "", list_all);
  bool drive_list_matches = drive_list_all.ok();
  for (char letter = 'A'; letter <= 'Z'; ++letter) {
    const bool reported = os_drive_type(letter) != DRIVE_NO_ROOT_DIR;
    bool in_list = false;
    for (const std::string& entry : list_all.list) {
      if (entry.size() == 1 && entry[0] == letter) in_list = true;
    }
    if (reported != in_list) drive_list_matches = false;
  }

  DriveInfo list_fixed;
  const auto drive_list_fixed = service.drive_get("list", "Fixed", list_fixed);
  bool drive_fixed_matches = drive_list_fixed.ok();
  for (char letter = 'A'; letter <= 'Z'; ++letter) {
    const bool reported = os_drive_type(letter) == DRIVE_FIXED;
    bool in_list = false;
    for (const std::string& entry : list_fixed.list) {
      if (entry.size() == 1 && entry[0] == letter) in_list = true;
    }
    if (reported != in_list) drive_fixed_matches = false;
  }
  const auto drive_list_bad = service.drive_get("list", "bogus", list_fixed);

  ULARGE_INTEGER raw_available{};
  ULARGE_INTEGER raw_total{};
  const bool raw_space_ok =
      GetDiskFreeSpaceExW(present_root.c_str(), &raw_available, &raw_total, nullptr) != 0;
  DriveInfo space;
  const auto drive_space = service.drive_get("spacefree", present_drive, space);
  const bool drive_space_matches = drive_space.ok() && raw_space_ok &&
                                   space.free_bytes == raw_available.QuadPart &&
                                   space.total_bytes == 0 && space.capacity_percent == -1;

  DriveInfo capacity;
  const auto drive_capacity = service.drive_get("capacity", present_drive, capacity);
  const std::uint64_t expected_percent =
      raw_total.QuadPart == 0
          ? 0
          : ((raw_total.QuadPart - raw_available.QuadPart) * 100ull) / raw_total.QuadPart;
  const bool drive_capacity_matches =
      drive_capacity.ok() && raw_space_ok && capacity.total_bytes == raw_total.QuadPart &&
      capacity.free_bytes == raw_available.QuadPart &&
      capacity.capacity_percent == static_cast<int>(expected_percent);

  wchar_t raw_label[261] = {};
  wchar_t raw_filesystem[261] = {};
  DWORD raw_serial = 0;
  const bool raw_volume_ok = GetVolumeInformationW(present_root.c_str(), raw_label, 261, &raw_serial,
                                                   nullptr, nullptr, raw_filesystem, 261) != 0;
  DriveInfo serial;
  const auto drive_serial = service.drive_get("serial", present_drive, serial);
  const bool drive_serial_matches =
      drive_serial.ok() && raw_volume_ok && serial.serial == raw_serial;
  DriveInfo label_info;
  const auto drive_label_read = service.drive_get("label", present_drive, label_info);
  const bool drive_label_matches =
      drive_label_read.ok() && raw_volume_ok && label_info.label == to_utf8(raw_label);
  DriveInfo filesystem_info;
  const auto drive_filesystem = service.drive_get("filesystem", present_drive, filesystem_info);
  const bool drive_filesystem_matches = drive_filesystem.ok() && raw_volume_ok &&
                                        filesystem_info.filesystem == to_utf8(raw_filesystem);

  DWORD sectors_per_cluster = 0;
  DWORD bytes_per_sector = 0;
  DWORD free_clusters = 0;
  DWORD total_clusters = 0;
  const bool raw_status_ok = GetDiskFreeSpaceW(present_root.c_str(), &sectors_per_cluster,
                                               &bytes_per_sector, &free_clusters,
                                               &total_clusters) != 0;
  DriveInfo status_info;
  const auto drive_status = service.drive_get("status", present_drive, status_info);
  const bool drive_status_matches =
      drive_status.ok() && raw_status_ok && status_info.status == "Ready";
  DriveInfo statuscd_info;
  const auto drive_statuscd = service.drive_get("statuscd", present_drive, statuscd_info);
  const bool drive_statuscd_matches = drive_statuscd.ok() && drive_status.ok() &&
                                      statuscd_info.status == status_info.status;

  DriveInfo absent_info;
  const auto drive_absent_type = service.drive_get("type", absent_drive, absent_info);
  const auto drive_absent_space = service.drive_get("spacefree", absent_drive, absent_info);
  const auto drive_bad_field = service.drive_get("bogus", present_drive, absent_info);
  const auto drive_empty_letter = service.drive_get("type", "", absent_info);
  const auto drive_bad_letter = service.drive_get("type", "1:", absent_info);
  const auto drive_label_absent = service.drive_set_label(absent_drive, "nope");
  const auto drive_lock_absent = service.drive_lock(absent_drive);
  const auto drive_unlock_absent = service.drive_unlock(absent_drive);
  const auto drive_eject_absent = service.drive_eject(absent_drive);
  const auto drive_retract_absent = service.drive_retract(absent_drive);

  // Recycle: the file leaves the fixture and lands in the recycle bin (one
  // tiny test file the user's bin keeps until it is emptied).
  const bool wrote_recycle = os_write_text(p_recycle, "recycle-me");
  const auto recycle_ok = service.recycle(recycle_path);
  const bool recycle_removed = !os_exists(p_recycle);
  const auto recycle_missing = service.recycle(missing_path);

  // Shortcut: the .lnk is inspected through its own magic bytes, not by
  // resolving it with the service under test.
  const auto shortcut_ok =
      service.make_shortcut(link_path, hello_path, "--arg", fixture_u8, hello_path, "rime link");
  const std::vector<std::uint8_t> link_bytes = os_read(p_link);
  const bool link_magic = link_bytes.size() >= 4 && link_bytes[0] == 0x4C && link_bytes[1] == 0x00 &&
                          link_bytes[2] == 0x00 && link_bytes[3] == 0x00;
  const auto shortcut_no_target = service.make_shortcut(link_path, "", "", "", "", "");

  // FileInstall shares FileCopy's semantics in this runtime.
  const auto install_ok = service.file_install(hello_path, installed_path, false);
  const bool install_matches = os_read_text(p_installed) == hello_text;

  // --- Download over a real loopback socket -----------------------------
  WSADATA winsock_data{};
  const bool winsock_ready = WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0;
  LoopbackServer server;
  const std::string download_body = "rime-storage-download-payload";
  const bool server_started = winsock_ready && server.start(download_body, 3);
  const std::string ok_url = "http://127.0.0.1:" + std::to_string(server.port()) + "/ok";
  const std::string missing_url = "http://127.0.0.1:" + std::to_string(server.port()) + "/missing";
  const std::wstring p_download_404 = fixture + L"\\download-404.bin";
  rime::core::CancellationToken token;
  std::uint64_t downloaded = 0;
  const auto download_ok = service.download(ok_url, download_path, token, 0, downloaded);
  const bool download_matches =
      download_ok.ok() && downloaded == download_body.size() &&
      os_read(p_download) == std::vector<std::uint8_t>(download_body.begin(), download_body.end());
  const auto download_404 = service.download(missing_url, fixture_u8 + "\\download-404.bin", token,
                                             0, downloaded);
  const bool download_404_left_no_file = !os_exists(p_download_404);
  const auto download_scheme =
      service.download("ftp://example.com/file", download_path, token, 0, downloaded);
  rime::core::CancellationSource cancelled_source;
  cancelled_source.cancel();
  const auto download_cancelled =
      service.download(ok_url, download_path, cancelled_source.token(), 0, downloaded);
  // A deadline already in the past trips inside the transfer loop, after the
  // destination was created, so the partial file has to be removed again.
  const auto download_deadline = service.download(ok_url, download_deadline_path, token, 1, downloaded);
  const bool download_deadline_left_no_file = !os_exists(p_download_deadline);
  server.stop();
  if (winsock_ready) WSACleanup();

  // --- Selectors: a headless embed refuses before any dialog can open ----
  std::vector<std::string> selected;
  const auto select_no_ui = service.select_file("Text (*.txt)", "", false, selected, token);
  std::string selected_dir;
  const auto select_dir_no_ui = service.select_dir("", selected_dir, token);
  const auto select_dir_cancelled = service.select_dir("", selected_dir, cancelled_source.token());
  std::vector<std::string> selected_cancelled;
  const auto select_file_cancelled =
      service.select_file("*.txt", "", false, selected_cancelled, cancelled_source.token());

  // stop() reclaims whatever the test left open and is repeatable.
  std::uint64_t stop_handle = 0;
  std::uint64_t stop_length = 0;
  const auto open_for_stop = service.file_open(hello_path, "r", stop_handle, stop_length);
  const std::size_t handles_before_stop = service.open_handle_count();
  const std::size_t stopped = service.stop();
  const std::size_t handles_after_stop = service.open_handle_count();
  const std::size_t stopped_again = service.stop();

  // --- Executor round trip ----------------------------------------------
  const bool wrote_exec_time = os_write_text(p_exec_time, "time-target");
  rime::action::Action append_action;
  append_action.id = 1;
  append_action.source = {"test", "storage_tests"};
  append_action.type = "storage.write";
  append_action.capability = "filesystem.write";
  append_action.target = {"storage", "fs"};
  append_action.deadline_unix_ms = deadline_ms();
  append_action.payload = "{\"op\":\"append\",\"path\":\"" + json_escape(exec_append_path) +
                          "\",\"text\":\"executed\"}";
  const auto exec_append = kernel.execute(append_action);
  const bool exec_append_matches = os_read_text(p_exec_append) == "executed";

  rime::action::Action time_action = append_action;
  time_action.id = 2;
  time_action.payload = "{\"op\":\"setTime\",\"path\":\"" + json_escape(exec_time_path) +
                        "\",\"which\":\"mtime\",\"unixMs\":1600000000000}";
  const auto exec_time = kernel.execute(time_action);
  const bool exec_time_matches = os_mtime_ms(p_exec_time) == 1600000000000ll;

  std::uint64_t exec_handle = 0;
  std::uint64_t exec_length = 0;
  const auto exec_open = service.file_open(handle_path, "w", exec_handle, exec_length);
  rime::action::Action handle_action = append_action;
  handle_action.id = 3;
  handle_action.payload = "{\"op\":\"handleWrite\",\"handle\":" + std::to_string(exec_handle) +
                          ",\"data\":\"via-executor\"}";
  const auto exec_handle_write = kernel.execute(handle_action);
  const auto exec_handle_close = service.file_close(exec_handle);
  const bool exec_handle_matches = os_read_text(p_handle) == "via-executor";

  // A policy without the capability refuses before the executor runs, so the
  // file the denied action wanted is never created.
  rime::action::Kernel denied(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{}));
  const bool denied_registered = denied.register_executor("storage.write", executor).ok();
  rime::action::Action denied_action = append_action;
  denied_action.id = 4;
  denied_action.payload = "{\"op\":\"append\",\"path\":\"" + json_escape(exec_denied_path) +
                          "\",\"text\":\"denied\"}";
  const auto refused = denied.execute(denied_action);
  const bool denied_untouched = !os_exists(p_exec_denied);

  // --- Contract violations ----------------------------------------------
  rime::action::Action missing_op = append_action;
  missing_op.id = 5;
  missing_op.payload = "{\"path\":\"" + json_escape(exec_append_path) + "\"}";
  const auto missing_op_result = kernel.execute(missing_op);

  rime::action::Action unknown_op = append_action;
  unknown_op.id = 6;
  unknown_op.payload = "{\"op\":\"rename\",\"path\":\"" + json_escape(exec_append_path) + "\"}";
  const auto unknown_op_result = kernel.execute(unknown_op);

  rime::action::Action extra_field = append_action;
  extra_field.id = 7;
  extra_field.payload = "{\"op\":\"append\",\"path\":\"" + json_escape(exec_append_path) +
                        "\",\"text\":\"x\",\"mode\":\"r\"}";
  const auto extra_field_result = kernel.execute(extra_field);

  rime::action::Action no_path = append_action;
  no_path.id = 8;
  no_path.payload = "{\"op\":\"append\",\"text\":\"x\"}";
  const auto no_path_result = kernel.execute(no_path);

  rime::action::Action bad_json = append_action;
  bad_json.id = 9;
  bad_json.payload = "not-json";
  const auto bad_json_result = kernel.execute(bad_json);

  rime::action::Action wrong_kind = append_action;
  wrong_kind.id = 10;
  wrong_kind.target = {"window", "fs"};
  const auto wrong_kind_result = kernel.execute(wrong_kind);

  rime::action::Action wrong_id = append_action;
  wrong_id.id = 11;
  wrong_id.target = {"storage", "other"};
  const auto wrong_id_result = kernel.execute(wrong_id);

  rime::action::Action bad_which = append_action;
  bad_which.id = 12;
  bad_which.payload = "{\"op\":\"setTime\",\"path\":\"" + json_escape(exec_time_path) +
                      "\",\"which\":\"created\",\"unixMs\":1}";
  const auto bad_which_result = kernel.execute(bad_which);

  rime::action::Action bad_handle = append_action;
  bad_handle.id = 13;
  bad_handle.payload = "{\"op\":\"handleWrite\",\"handle\":0,\"data\":\"x\"}";
  const auto bad_handle_result = kernel.execute(bad_handle);

  rime::action::Action bad_unix_ms = append_action;
  bad_unix_ms.id = 14;
  bad_unix_ms.payload = "{\"op\":\"setTime\",\"path\":\"" + json_escape(exec_time_path) +
                        "\",\"which\":\"mtime\",\"unixMs\":-5}";
  const auto bad_unix_ms_result = kernel.execute(bad_unix_ms);

  // The executor's own type guard is only reachable by calling it directly:
  // the Kernel dispatches exactly the type the executor was registered for.
  rime::action::Action wrong_type = append_action;
  wrong_type.id = 15;
  wrong_type.type = "registry.write";
  const auto wrong_type_result = executor->execute(wrong_type, rime::core::CancellationToken{});

  rime::action::Action pre_cancelled_action = append_action;
  pre_cancelled_action.id = 16;
  rime::core::CancellationSource pre_cancelled;
  pre_cancelled.cancel();
  const auto cancelled_result = kernel.execute(pre_cancelled_action, pre_cancelled.token());

  // --- Cleanup ---------------------------------------------------------
  // From here on a failing assert can no longer leave filesystem or
  // environment state behind.
  if (os_exists(fixture)) os_delete_tree(fixture);
  os_env_set(L"RIME_STORAGE_TEST_OBSERVED", nullptr);
  os_env_set(L"RIME_STORAGE_TEST_SERVICE", nullptr);

  // --- Assert phase ------------------------------------------------------
  // Reads and stats.
  assert(wrote_hello);
  assert(wrote_cjk);
  assert(wrote_plain);
  assert(read_hello.ok());
  assert(hello_matches);
  assert(read_cjk.ok());
  assert(cjk_matches);
  assert(read_plain.ok());
  assert(plain_matches);
  assert(read_missing.code == ErrorCode::ExecutionFailed);
  assert(read_missing.message == "file not found: " + missing_path);
  assert(missing_absent_on_disk);
  assert(read_directory.code == ErrorCode::ExecutionFailed);
  assert(read_directory.message == "read failed for " + fixture_u8 + " (Win32 error 5)");
  assert(read_empty_path.code == ErrorCode::InvalidContract);
  assert(read_empty_path.message == "storage path must not be empty");

  assert(stamped);
  assert(hardened);
  assert(stat_matches);
  assert(observed_mtime == stamped_mtime);
  assert(stat_missing.code == ErrorCode::ExecutionFailed);
  assert(stat_missing.message == "file not found: " + missing_path);
  assert(stat_dir.ok());
  assert(dir_stat_ok);
  assert(dir_stat.size == 0);

  assert(made_listing);
  assert(wrote_listing_a);
  assert(wrote_listing_b);
  assert(made_listing_sub);
  assert(list_listing.ok());
  assert(listing_matches);
  assert(list_file.code == ErrorCode::ExecutionFailed);
  assert(list_file.message == "path is not a directory: " + hello_path);
  assert(list_missing.code == ErrorCode::ExecutionFailed);
  assert(list_missing.message == "file not found: " + missing_path);

  // Environment block.
  assert(env_seeded);
  assert(env_matches);
  assert(env_unset_ok);
  assert(env_set_result.ok());
  assert(env_service_observed == "set-by-service");
  assert(env_bad_name.code == ErrorCode::InvalidContract);
  assert(env_bad_name.message == "environment variable name must not contain '=': A=B");

  // Encoding session.
  assert(enc_utf16_ok);
  assert(wrote_utf16);
  assert(utf16_observed == utf16_expected);
  assert(utf16_matches);
  assert(wrote_utf16be);
  assert(enc_utf16be.ok());
  assert(utf16be_matches);
  assert(enc_bom.ok());
  assert(wrote_bom);
  assert(bom_append.ok());
  const std::vector<std::uint8_t> bom_expected = {0xEF, 0xBB, 0xBF, 'f', 'i', 'r', 's', 't',
                                                  's', 'e', 'c', 'o', 'n', 'd'};
  assert(bom_observed == bom_expected);
  assert(enc_bad.code == ErrorCode::InvalidContract);
  assert(enc_bad.message ==
         "encoding must be one of utf-8, utf-8-bom, utf-16, utf-16-be, cp0, cp1252, latin1");
  assert(encoding_restored);

  assert(append_first.ok());
  assert(append_second.ok());
  assert(append_observed == "onetwo");

  // Handle table.
  assert(handles_before == 0);
  assert(open_hello.ok());
  assert(hello_length == hello_text.size());
  assert(chunk_matches);
  assert(stat_handle_ok);
  assert(seek_begin.ok());
  assert(seek_pos == 0);
  assert(rest_matches);
  assert(seek_bad_whence.code == ErrorCode::InvalidContract);
  assert(seek_bad_whence.message == "file seek whence must be 0, 1 or 2");
  assert(read_too_many.code == ErrorCode::InvalidContract);
  assert(read_too_many.message == "file read count exceeds the 64 MiB limit");
  assert(close_hello.ok());
  assert(read_stale.code == ErrorCode::InvalidState);
  assert(read_stale.message == "file handle is not open");
  assert(close_stale.code == ErrorCode::InvalidState);
  assert(close_stale.message == "file handle is not open");
  assert(handles_after == 0);

  assert(open_bad_mode.code == ErrorCode::InvalidContract);
  assert(open_bad_mode.message == "file mode must be \"r\", \"a\" or \"w\"");
  assert(open_missing_file.code == ErrorCode::ExecutionFailed);
  assert(open_missing_file.message == "file not found: " + missing_path);
  assert(open_directory.code == ErrorCode::ExecutionFailed);
  assert(open_directory.message.rfind("open failed for ", 0) == 0);
  assert(open_append.ok());
  assert(write_handle.ok());
  assert(close_append.ok());
  assert(append_after_write == "onetwo!");
  assert(write_stale.code == ErrorCode::InvalidState);
  assert(write_stale.message == "file handle is not open");
  assert(open_trunc.ok());
  assert(close_trunc.ok());
  assert(trunc_is_empty);

  // Copy / move / delete.
  assert(copy_ok.ok());
  assert(copy_bytes == hello_bytes);
  assert(copy_again.code == ErrorCode::ExecutionFailed);
  assert(copy_again.message == "destination already exists: " + copy_path);
  assert(copy_made_writable);
  assert(copy_over.ok());
  assert(copy_missing.code == ErrorCode::ExecutionFailed);
  assert(copy_missing.message == "file not found: " + missing_path);
  assert(move_ok.ok());
  assert(move_observable);
  assert(move_missing.code == ErrorCode::ExecutionFailed);
  assert(move_missing.message == "file not found: " + missing_path);
  assert(delete_missing.code == ErrorCode::ExecutionFailed);
  assert(delete_missing.message == "file not found: " + missing_path);
  assert(delete_directory.code == ErrorCode::ExecutionFailed);
  assert(delete_directory.message == "path is a directory: " + fixture_u8);

  // Directories.
  assert(make_nested.ok());
  assert(nested_ok);
  assert(wrote_leaf);
  assert(made_tree);
  assert(wrote_tree_child);
  assert(made_tree_sub);
  assert(wrote_tree_grand);
  assert(made_tree_move);
  assert(wrote_tree_move_child);
  assert(made_dirdelete);
  assert(wrote_dirdelete_child);
  assert(dir_delete_non_recursive.code == ErrorCode::ExecutionFailed);
  assert(dir_delete_non_recursive.message == "directory is not empty: " + dirdelete_path);
  assert(dirdelete_survived);
  assert(dir_delete_recursive.ok());
  assert(dirdelete_removed);
  assert(dir_delete_missing.code == ErrorCode::ExecutionFailed);
  assert(dir_delete_missing.message == "file not found: " + dirdelete_path);
  assert(dir_delete_on_file.code == ErrorCode::ExecutionFailed);
  assert(dir_delete_on_file.message == "path is not a directory: " + hello_path);
  assert(dir_copy_ok.ok());
  assert(dir_copy_observable);
  assert(dir_copy_missing.code == ErrorCode::ExecutionFailed);
  assert(dir_copy_missing.message == "file not found: " + dirdelete_path);
  assert(dir_copy_onto_file.code == ErrorCode::ExecutionFailed);
  assert(dir_copy_onto_file.message == "destination already exists as a file: " + hello_path);
  assert(dir_move_ok.ok());
  assert(dir_move_observable);
  assert(dir_move_missing.code == ErrorCode::ExecutionFailed);
  assert(dir_move_missing.message == "file not found: " + dirdelete_path);

  // Attributes and timestamps.
  assert(attrib_add.ok());
  assert((attrib_after_add & FILE_ATTRIBUTE_READONLY) != 0);
  assert((attrib_after_add & FILE_ATTRIBUTE_ARCHIVE) != 0);
  assert(attrib_remove.ok());
  assert((attrib_after_remove & FILE_ATTRIBUTE_READONLY) == 0);
  assert((attrib_after_remove & FILE_ATTRIBUTE_ARCHIVE) != 0);
  assert(attrib_none.code == ErrorCode::InvalidContract);
  assert(attrib_none.message ==
         "setAttrib requires at least one attribute letter to add or remove");
  assert(attrib_unknown.code == ErrorCode::InvalidContract);
  assert(attrib_unknown.message == "unknown attribute letter: Z");
  assert(attrib_string_ok);
  assert(time_ok.ok());
  assert(time_observed == file_mtime);
  assert(time_bad_which.code == ErrorCode::InvalidContract);
  assert(time_bad_which.message == "setTime which must be one of mtime, atime, ctime");
  assert(time_out_of_range.code == ErrorCode::InvalidContract);
  assert(time_out_of_range.message == "setTime unix_ms must be between 0 and 253402300799999");
  assert(time_missing.code == ErrorCode::ExecutionFailed);
  assert(time_missing.message == "file not found: " + missing_path);

  // INI profile API.
  assert(ini_create.ok());
  assert(ini_observed);
  assert(ini_read_ok.ok());
  assert(ini_read_matches);
  assert(ini_overwrite.ok());
  assert(ini_updated);
  assert(ini_second.ok());
  assert(ini_read_missing_key.code == ErrorCode::ExecutionFailed);
  assert(ini_read_missing_key.message == "ini key not found: section/nope");
  assert(ini_read_missing_section.code == ErrorCode::ExecutionFailed);
  assert(ini_read_missing_section.message == "ini section not found: nosuch");
  assert(ini_read_missing_file.code == ErrorCode::ExecutionFailed);
  assert(ini_read_missing_file.message == "file not found: " + ini_missing_path);
  assert(ini_read_empty_section.code == ErrorCode::InvalidContract);
  assert(ini_read_empty_section.message == "ini section must not be empty");
  assert(ini_bad_parent.code == ErrorCode::ExecutionFailed);
  assert(ini_bad_parent.message == "path not found: " + ini_parent_path);
  assert(ini_parent_absent_on_disk);
  assert(ini_delete_key.ok());
  assert(ini_key_gone);
  assert(ini_delete_missing_key.code == ErrorCode::ExecutionFailed);
  assert(ini_delete_missing_key.message == "ini key not found: section/nope");
  assert(ini_delete_missing_section.code == ErrorCode::ExecutionFailed);
  assert(ini_delete_missing_section.message == "ini section not found: nosuch");
  assert(ini_delete_missing_file.code == ErrorCode::ExecutionFailed);
  assert(ini_delete_missing_file.message == "file not found: " + ini_missing_path);
  assert(ini_delete_section.ok());
  assert(ini_section_gone);

  // Drives: parameter validation, an OS-observed present letter and a
  // letter with no root. No media is ejected and no label is written.
  assert(present_letter != 0);
  assert(absent_letter != 0);
  assert(drive_type_matches);
  assert(drive_list_all.ok());
  assert(drive_list_matches);
  assert(drive_list_fixed.ok());
  assert(drive_fixed_matches);
  assert(drive_list_bad.code == ErrorCode::InvalidContract);
  // The field is valid; the *type filter* is not, so this is the filter rule
  // and not the field rule asserted below for drive_bad_field.
  assert(drive_list_bad.message ==
         "drive list type must be CDROM, Removable, Fixed, Network, RAMDisk or Unknown");
  assert(drive_space_matches);
  assert(drive_capacity_matches);
  assert(drive_serial_matches);
  assert(drive_label_matches);
  assert(drive_filesystem_matches);
  assert(drive_status_matches);
  assert(drive_statuscd_matches);
  assert(drive_absent_type.code == ErrorCode::TargetGone);
  assert(drive_absent_type.message == "drive not found: " + std::string(1, absent_letter));
  assert(drive_absent_space.code == ErrorCode::TargetGone);
  assert(drive_absent_space.message == "drive not found: " + std::string(1, absent_letter));
  assert(drive_bad_field.code == ErrorCode::InvalidContract);
  assert(drive_bad_field.message ==
         "drive field must be one of type, list, serial, spacefree, status, statuscd, "
         "filesystem, label, capacity");
  assert(drive_empty_letter.code == ErrorCode::InvalidContract);
  assert(drive_empty_letter.message == "drive must not be empty");
  assert(drive_bad_letter.code == ErrorCode::InvalidContract);
  // The third accepted form is "X:\" (letter, colon, backslash), so the colon
  // belongs inside the quoted example too.
  assert(drive_bad_letter.message == "drive must be a letter, \"X:\" or \"X:\\\": 1:");
  assert(drive_label_absent.code == ErrorCode::TargetGone);
  assert(drive_label_absent.message == "drive not found: " + std::string(1, absent_letter));
  assert(drive_lock_absent.code == ErrorCode::TargetGone);
  assert(drive_unlock_absent.code == ErrorCode::TargetGone);
  assert(drive_eject_absent.code == ErrorCode::TargetGone);
  assert(drive_retract_absent.code == ErrorCode::TargetGone);

  // Recycle, shortcut and install.
  assert(wrote_recycle);
  assert(recycle_ok.ok());
  assert(recycle_removed);
  assert(recycle_missing.code == ErrorCode::ExecutionFailed);
  assert(recycle_missing.message == "file not found: " + missing_path);
  assert(shortcut_ok.ok());
  assert(link_magic);
  assert(shortcut_no_target.code == ErrorCode::InvalidContract);
  assert(shortcut_no_target.message == "shortcut target must not be empty");
  assert(install_ok.ok());
  assert(install_matches);

  // Download over the loopback server.
  assert(winsock_ready);
  assert(server_started);
  assert(download_ok.ok());
  assert(download_matches);
  assert(download_404.code == ErrorCode::ExecutionFailed);
  assert(download_404.message == "download failed with HTTP status 404: " + missing_url);
  assert(download_404_left_no_file);
  assert(download_scheme.code == ErrorCode::InvalidContract);
  assert(download_scheme.message ==
         "download URL must start with http:// or https://: ftp://example.com/file");
  assert(download_cancelled.code == ErrorCode::Cancelled);
  assert(download_cancelled.message == "download was cancelled before it started");
  assert(download_deadline.code == ErrorCode::Timeout);
  assert(download_deadline.message == "download exceeded its deadline");
  assert(download_deadline_left_no_file);

  // Selectors stay shut on a headless embed.
  assert(select_no_ui.code == ErrorCode::InvalidState);
  assert(select_no_ui.message == "storage selector has no UI thread");
  assert(select_dir_no_ui.code == ErrorCode::InvalidState);
  assert(select_dir_no_ui.message == "storage selector has no UI thread");
  assert(select_dir_cancelled.code == ErrorCode::Cancelled);
  assert(select_dir_cancelled.message == "selection was cancelled before it started");
  assert(select_file_cancelled.code == ErrorCode::Cancelled);
  assert(select_file_cancelled.message == "selection was cancelled before it started");

  // Teardown contract: stop() closes the one handle left open, returns how
  // many it closed and can run again.
  assert(open_for_stop.ok());
  assert(handles_before_stop == 1);
  assert(stopped == 1);
  assert(handles_after_stop == 0);
  assert(stopped_again == 0);

  // Executor round trip through the real Kernel.
  assert(wrote_exec_time);
  assert(exec_append.succeeded);
  assert(exec_append.detail == "storage write applied");
  {
    const rime::core::json::Value* op = exec_append.value.find("op");
    assert(op != nullptr && op->is_string() && op->as_string() == "append");
  }
  assert(exec_append_matches);
  assert(exec_time.succeeded);
  assert(exec_time_matches);
  assert(exec_open.ok());
  assert(exec_handle_write.succeeded);
  assert(exec_handle_close.ok());
  assert(exec_handle_matches);

  assert(denied_registered);
  assert(!refused.succeeded);
  assert(refused.error.code == ErrorCode::CapabilityDenied);
  assert(denied_untouched);

  // Contract violations reject before any service call.
  assert(!missing_op_result.succeeded);
  assert(missing_op_result.error.code == ErrorCode::InvalidContract);
  assert(missing_op_result.error.message == "storage.write payload requires a non-empty string op");
  assert(!unknown_op_result.succeeded);
  assert(unknown_op_result.error.code == ErrorCode::InvalidContract);
  assert(unknown_op_result.error.message == "storage.write payload does not support op: rename");
  assert(!extra_field_result.succeeded);
  assert(extra_field_result.error.code == ErrorCode::InvalidContract);
  assert(extra_field_result.error.message ==
         "storage.write payload op append does not accept field: mode");
  assert(!no_path_result.succeeded);
  assert(no_path_result.error.code == ErrorCode::InvalidContract);
  assert(no_path_result.error.message == "storage.write payload requires a string path");
  assert(!bad_json_result.succeeded);
  assert(bad_json_result.error.code == ErrorCode::InvalidContract);
  assert(bad_json_result.error.message == "storage.write payload must be a JSON object");
  assert(!wrong_kind_result.succeeded);
  assert(wrong_kind_result.error.code == ErrorCode::InvalidContract);
  assert(wrong_kind_result.error.message.find("target kind 'storage'") != std::string::npos);
  assert(!wrong_id_result.succeeded);
  assert(wrong_id_result.error.code == ErrorCode::InvalidContract);
  assert(wrong_id_result.error.message == "storage.write target id must be 'fs'");
  assert(!bad_which_result.succeeded);
  assert(bad_which_result.error.code == ErrorCode::InvalidContract);
  assert(bad_which_result.error.message ==
         "storage.write payload which must be one of mtime, atime, ctime");
  assert(!bad_handle_result.succeeded);
  assert(bad_handle_result.error.code == ErrorCode::InvalidContract);
  assert(bad_handle_result.error.message ==
         "storage.write payload requires an integer handle between 1 and 9223372036854775807");
  assert(!bad_unix_ms_result.succeeded);
  assert(bad_unix_ms_result.error.code == ErrorCode::InvalidContract);
  assert(bad_unix_ms_result.error.message ==
         "storage.write payload unixMs must be an integer between 0 and 253402300799999");
  assert(!wrong_type_result.succeeded);
  assert(wrong_type_result.error.code == ErrorCode::InvalidContract);
  assert(wrong_type_result.error.message == "unsupported action type: registry.write");
  assert(!cancelled_result.succeeded);
  assert(cancelled_result.cancelled);
  assert(cancelled_result.error.code == ErrorCode::Cancelled);

  // Shortcut round trip: build a real .lnk through IShellLink in the test
  // (independent observation, not the service under test), then read it back.
  // Own temp directory (the main fixture is already deleted by this phase).
  wchar_t shortcut_temp[MAX_PATH] = {0};
  assert(GetTempPathW(MAX_PATH, shortcut_temp) != 0);
  const std::wstring shortcut_dir =
      std::wstring(shortcut_temp) + L"rime_shortcut_test_" + std::to_wstring(GetCurrentProcessId());
  if (os_exists(shortcut_dir)) os_delete_tree(shortcut_dir);
  assert(CreateDirectoryW(shortcut_dir.c_str(), nullptr) != 0);
  char shortcut_narrow[MAX_PATH * 2] = {0};
  assert(WideCharToMultiByte(CP_UTF8, 0, shortcut_dir.c_str(), -1, shortcut_narrow,
                             sizeof(shortcut_narrow), nullptr, nullptr) != 0);
  const std::string shortcut_dir_u8(shortcut_narrow);
  {
    const std::wstring shortcut_path = shortcut_dir + L"\\target.lnk";
    const std::string shortcut_u8 = shortcut_dir_u8 + "\\target.lnk";
    wchar_t sys_dir[MAX_PATH] = {0};
    assert(GetSystemDirectoryW(sys_dir, MAX_PATH) != 0);
    const HRESULT co_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    assert(co_init == S_OK || co_init == S_FALSE || co_init == RPC_E_CHANGED_MODE);
    bool made = false;
    HRESULT step = S_OK;
    const char* step_name = "none";
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(step = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IShellLinkW,
                                          reinterpret_cast<void**>(&link)))) {
      IPersistFile* persist = nullptr;
      if (SUCCEEDED(step = link->QueryInterface(
                        IID_IPersistFile, reinterpret_cast<void**>(&persist)))) {
        const std::wstring target_exe = std::wstring(sys_dir) + L"\\notepad.exe";
        step_name = "SetPath";
        if (SUCCEEDED(step = link->SetPath(target_exe.c_str()))) {
          step_name = "SetWorkingDirectory";
          if (SUCCEEDED(step = link->SetWorkingDirectory(shortcut_dir.c_str()))) {
            step_name = "SetArguments";
            if (SUCCEEDED(step = link->SetArguments(L"--rime-test"))) {
              step_name = "Save";
              if (SUCCEEDED(step = persist->Save(shortcut_path.c_str(), TRUE))) {
                made = true;
              }
            }
          }
        }
        persist->Release();
      } else {
        step_name = "QI";
      }
      link->Release();
    } else {
      step_name = "CoCreate";
    }
    CoUninitialize();
    if (!made) {
      std::fprintf(stderr, "shortcut fixture failed at %s (hr=0x%08lX)\n", step_name,
                   static_cast<unsigned long>(step));
    }
    assert(made);
    rime::win32::ShortcutInfo shortcut;
    assert(service.read_shortcut(shortcut_u8, shortcut).ok());
    assert(shortcut.target.size() >= 11 &&
           shortcut.target.compare(shortcut.target.size() - 11, 11, "notepad.exe") == 0);
    assert(shortcut.args == "--rime-test");
    assert(!shortcut.working_dir.empty());
    // A corrupt link fails instead of guessing.
    assert(os_write_text(shortcut_dir + L"\\broken.lnk", "not a link"));
    rime::win32::ShortcutInfo broken;
    assert(!service.read_shortcut(shortcut_dir_u8 + "\\broken.lnk", broken).ok());
    assert(!service.read_shortcut(shortcut_dir_u8 + "\\gone.lnk", broken).ok());
  }

  // Version resource: a system binary carries one, a plain text file reads
  // back empty (AHK rule), a missing file errors.
  {
    wchar_t sys_dir2[MAX_PATH] = {0};
    assert(GetSystemDirectoryW(sys_dir2, MAX_PATH) != 0);
    std::string system_kernel32;
    {
      const std::wstring kernel_w = std::wstring(sys_dir2) + L"\\kernel32.dll";
      char narrow[MAX_PATH * 2] = {0};
      WideCharToMultiByte(CP_UTF8, 0, kernel_w.c_str(), -1, narrow, sizeof(narrow), nullptr,
                          nullptr);
      system_kernel32.assign(narrow);
    }
    std::string version;
    assert(service.read_version(system_kernel32, version).ok());
    assert(!version.empty() && version.find('.') != std::string::npos);
    assert(os_write_text(shortcut_dir + L"\\plain.txt", "no version here"));
    std::string plain_version = "x";
    const std::string plain_u8 = shortcut_dir_u8 + "\\plain.txt";
    assert(service.read_version(plain_u8, plain_version).ok() && plain_version.empty());
    std::string missing_version;
    assert(!service.read_version(shortcut_dir_u8 + "\\gone.dll", missing_version).ok());
    assert(os_delete_tree(shortcut_dir));
  }
  return 0;
}
