#include "rime/automation/uia_service.hpp"

#include <windows.h>

// WIN32_LEAN_AND_MEAN skips the OLE headers that define the `interface`
// macro; UIAutomationCore.h's `typedef interface ...` forward declarations
// need it (MSVC C4430 cascade otherwise).
#include <objbase.h>

#include <initguid.h>
#include <UIAutomation.h>

#include <oleauto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rime::automation {

using rime::core::Error;

namespace {

using Code = rime::core::Error::Code;

// MSVC spells the enumerator TreeScope_Subtree; MinGW uses TreeScope_SubTree.
#if defined(_MSC_VER)
constexpr TreeScope k_subtree = TreeScope_Subtree;
#else
constexpr TreeScope k_subtree = TreeScope_SubTree;
#endif

// Move-only COM holder: AddRef'd pointers are owned exactly once and
// released on the MTA thread (or wherever the ComRef itself lives - the
// registry never leaves that thread).
template <typename T>
class ComRef final {
 public:
  ComRef() = default;
  ~ComRef() { reset(); }
  ComRef(const ComRef&) = delete;
  ComRef& operator=(const ComRef&) = delete;
  ComRef(ComRef&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComRef& operator=(ComRef&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  void reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }
  T** put() {
    reset();
    return &ptr_;
  }
  // Adopts an already AddRef'd pointer.
  void attach(T* ptr) {
    reset();
    ptr_ = ptr;
  }
  // Returns a borrowed pointer after taking its own reference.
  T* clone() const {
    if (ptr_) ptr_->AddRef();
    return ptr_;
  }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  T* ptr_{nullptr};
};

// Control-type names shared by find queries and read snapshots. Values come
// from the UIA headers, so the table cannot drift from the platform.
struct ControlTypeName {
  const char* name;
  int id;
};

constexpr ControlTypeName k_control_types[] = {
    {"button", UIA_ButtonControlTypeId},
    {"calendar", UIA_CalendarControlTypeId},
    {"checkbox", UIA_CheckBoxControlTypeId},
    {"combobox", UIA_ComboBoxControlTypeId},
    {"custom", UIA_CustomControlTypeId},
    {"document", UIA_DocumentControlTypeId},
    {"edit", UIA_EditControlTypeId},
    {"group", UIA_GroupControlTypeId},
    {"hyperlink", UIA_HyperlinkControlTypeId},
    {"image", UIA_ImageControlTypeId},
    {"list", UIA_ListControlTypeId},
    {"listitem", UIA_ListItemControlTypeId},
    {"menu", UIA_MenuControlTypeId},
    {"menubar", UIA_MenuBarControlTypeId},
    {"menuitem", UIA_MenuItemControlTypeId},
    {"pane", UIA_PaneControlTypeId},
    {"progressbar", UIA_ProgressBarControlTypeId},
    {"radio", UIA_RadioButtonControlTypeId},
    {"slider", UIA_SliderControlTypeId},
    {"spinner", UIA_SpinnerControlTypeId},
    {"statusbar", UIA_StatusBarControlTypeId},
    {"tab", UIA_TabControlTypeId},
    {"tabitem", UIA_TabItemControlTypeId},
    {"text", UIA_TextControlTypeId},
    {"toolbar", UIA_ToolBarControlTypeId},
    {"tooltip", UIA_ToolTipControlTypeId},
    {"tree", UIA_TreeControlTypeId},
    {"treeitem", UIA_TreeItemControlTypeId},
    {"window", UIA_WindowControlTypeId},
};

bool control_type_from_name(const std::string& name, int& out) {
  for (const auto& entry : k_control_types) {
    if (name == entry.name) {
      out = entry.id;
      return true;
    }
  }
  return false;
}

const char* control_type_name(int id) {
  for (const auto& entry : k_control_types) {
    if (id == entry.id) return entry.name;
  }
  return "";
}

std::string widen_error(const char* what, const HRESULT hr) {
  char text[160]{};
  std::snprintf(text, sizeof(text), "%s (hr=0x%08lX)", what,
                static_cast<unsigned long>(hr));
  return std::string(text);
}

// UTF-8 -> UTF-16 for property conditions. Tests use ASCII, but names come
// from user queries so the conversion must be complete.
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                      needed);
  return wide;
}

// UTF-16 -> UTF-8 for element properties.
std::string narrow_utf8(const BSTR text) {
  if (!text) return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  out.pop_back();  // drop the embedded NUL written by the -1 source length
  return out;
}

// UIA reports a dead element (window destroyed) with this exact HRESULT;
// everything else is a real failure.
bool is_gone(const HRESULT hr) {
  return hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
}

}  // namespace

struct UiaService::Impl final {
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool quit{false};
  std::atomic<UiaServiceState> state{UiaServiceState::Created};
  std::atomic<std::size_t> element_count{0};
  std::thread thread;
  // Serializes start/stop so state transitions never race.
  std::mutex lifecycle;

  // --- MTA thread only -------------------------------------------------
  ComRef<IUIAutomation> automation;
  std::unordered_map<std::uint64_t, ComRef<IUIAutomationElement>> elements;
  std::uint64_t next_id{1};

  // True while executing on the service's own MTA thread; enqueueing a job
  // from there would wait on itself forever, so it is refused instead.
  static bool on_service_thread() { return thread_local_flag(); }
  static bool& thread_local_flag() {
    static thread_local bool flag = false;
    return flag;
  }

  Error enqueue(std::function<void()> job) {
    if (on_service_thread()) {
      return {Code::InvalidState, "automation service cannot be called from its own thread"};
    }
    if (state.load() != UiaServiceState::Running) {
      return {Code::InvalidState, "automation service is not running"};
    }
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (state.load() != UiaServiceState::Running) {
        return {Code::InvalidState, "automation service is not running"};
      }
      jobs.push_back([job = std::move(job), done]() mutable {
        try {
          job();
        } catch (...) {
        }
        try {
          done->set_value();
        } catch (...) {
        }
      });
    }
    cv.notify_one();
    finished.wait();
    return Error::none();
  }

  void thread_main(std::promise<Error> ready) {
    thread_local_flag() = true;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) {
      ready.set_value({Code::ExecutionFailed, widen_error("CoInitializeEx failed", co)});
      thread_local_flag() = false;
      return;
    }
    const HRESULT created =
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IUIAutomation, reinterpret_cast<void**>(automation.put()));
    if (FAILED(created)) {
      CoUninitialize();
      ready.set_value(
          {Code::ExecutionFailed, widen_error("UIA client creation failed", created)});
      thread_local_flag() = false;
      return;
    }
    ready.set_value(Error::none());

    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      cv.wait(lock, [this] { return quit || !jobs.empty(); });
      if (quit && jobs.empty()) break;
      std::deque<std::function<void()>> batch;
      batch.swap(jobs);
      lock.unlock();
      for (auto& job : batch) job();
      lock.lock();
    }
    lock.unlock();

    // Teardown stays on the MTA thread: element references and the UIA
    // client are released before CoUninitialize.
    elements.clear();
    automation.reset();
    CoUninitialize();
    thread_local_flag() = false;
  }

  // --- job bodies (MTA thread) ----------------------------------------

  bool snapshot_of(IUIAutomationElement* element, std::uint64_t id, ElementSnapshot& out,
                   Error& error) {
    BSTR name = nullptr;
    HRESULT hr = element->get_CurrentName(&name);
    if (FAILED(hr)) {
      if (name) SysFreeString(name);
      error = is_gone(hr)
                  ? Error{Code::TargetGone, "element " + std::to_string(id) + " is gone"}
                  : Error{Code::ExecutionFailed, widen_error("reading element name failed", hr)};
      return false;
    }
    out.id = id;
    if (name) {
      out.name = narrow_utf8(name);
      SysFreeString(name);
    }

    int control_type = 0;
    hr = element->get_CurrentControlType(&control_type);
    if (FAILED(hr)) {
      error = is_gone(hr)
                  ? Error{Code::TargetGone, "element " + std::to_string(id) + " is gone"}
                  : Error{Code::ExecutionFailed,
                          widen_error("reading element control type failed", hr)};
      return false;
    }
    out.control_type = control_type_name(control_type);

    BSTR automation_id = nullptr;
    hr = element->get_CurrentAutomationId(&automation_id);
    if (FAILED(hr)) {
      if (automation_id) SysFreeString(automation_id);
      error = is_gone(hr)
                  ? Error{Code::TargetGone, "element " + std::to_string(id) + " is gone"}
                  : Error{Code::ExecutionFailed,
                          widen_error("reading element automation id failed", hr)};
      return false;
    }
    if (automation_id) {
      out.automation_id = narrow_utf8(automation_id);
      SysFreeString(automation_id);
    }

    BOOL enabled = FALSE;
    hr = element->get_CurrentIsEnabled(&enabled);
    if (FAILED(hr)) {
      error = is_gone(hr)
                  ? Error{Code::TargetGone, "element " + std::to_string(id) + " is gone"}
                  : Error{Code::ExecutionFailed,
                          widen_error("reading element enabled state failed", hr)};
      return false;
    }
    out.enabled = enabled != FALSE;

    RECT rect{};
    hr = element->get_CurrentBoundingRectangle(&rect);
    if (FAILED(hr)) {
      error = is_gone(hr)
                  ? Error{Code::TargetGone, "element " + std::to_string(id) + " is gone"}
                  : Error{Code::ExecutionFailed,
                          widen_error("reading element bounds failed", hr)};
      return false;
    }
    out.x = rect.left;
    out.y = rect.top;
    out.width = rect.right - rect.left;
    out.height = rect.bottom - rect.top;
    return true;
  }

  std::uint64_t register_element(IUIAutomationElement* element) {
    const std::uint64_t id = next_id++;
    element->AddRef();
    elements.emplace(id, ComRef<IUIAutomationElement>{});
    elements.at(id).attach(element);
    element_count.store(elements.size(), std::memory_order_relaxed);
    return id;
  }

  void find_job(const FindQuery& query, std::vector<ElementSnapshot>& out, Error& error) {
    error = Error::none();
    out.clear();
    const bool has_criterion =
        !query.name.empty() || !query.control_type.empty() || !query.automation_id.empty();
    if (!has_criterion) {
      error = {Code::InvalidContract,
               "find requires at least one of name, controlType or automationId"};
      return;
    }
    if (query.max_results == 0) {
      error = {Code::InvalidContract, "find maxResults must be positive"};
      return;
    }
    int control_type_id = 0;
    if (!query.control_type.empty() && !control_type_from_name(query.control_type, control_type_id)) {
      error = {Code::InvalidContract, "unknown control type: " + query.control_type};
      return;
    }
    if (!automation) {
      error = {Code::InvalidState, "automation client is not available"};
      return;
    }

    ComRef<IUIAutomationElement> scope;
    if (query.from_id != 0) {
      const auto found = elements.find(query.from_id);
      if (found == elements.end()) {
        error = {Code::TargetGone, "element " + std::to_string(query.from_id) + " is gone"};
        return;
      }
      scope.attach(found->second.clone());
    } else {
      const HRESULT hr = automation->GetRootElement(scope.put());
      if (FAILED(hr) || !scope) {
        error = {Code::ExecutionFailed, widen_error("desktop root unavailable", hr)};
        return;
      }
    }

    ComRef<IUIAutomationCondition> match;
    const HRESULT true_hr = automation->CreateTrueCondition(match.put());
    if (FAILED(true_hr) || !match) {
      error = {Code::ExecutionFailed, widen_error("UIA true condition unavailable", true_hr)};
      return;
    }
    auto add_condition = [&](int property, VARIANT value) -> bool {
      ComRef<IUIAutomationCondition> child;
      const HRESULT hr = automation->CreatePropertyCondition(property, value, child.put());
      if (FAILED(hr) || !child) return false;
      ComRef<IUIAutomationCondition> combined;
      const HRESULT combine_hr =
          automation->CreateAndCondition(match.get(), child.get(), combined.put());
      if (FAILED(combine_hr) || !combined) return false;
      match = std::move(combined);
      return true;
    };

    if (!query.name.empty()) {
      const std::wstring wide = widen(query.name);
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_BSTR;
      value.bstrVal = SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
      const bool ok = add_condition(UIA_NamePropertyId, value);
      VariantClear(&value);
      if (!ok) {
        error = {Code::ExecutionFailed, "UIA name condition was rejected"};
        return;
      }
    }
    if (!query.control_type.empty()) {
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_I4;
      value.lVal = control_type_id;
      if (!add_condition(UIA_ControlTypePropertyId, value)) {
        error = {Code::ExecutionFailed, "UIA control type condition was rejected"};
        return;
      }
    }
    if (!query.automation_id.empty()) {
      const std::wstring wide = widen(query.automation_id);
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_BSTR;
      value.bstrVal = SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
      const bool ok = add_condition(UIA_AutomationIdPropertyId, value);
      VariantClear(&value);
      if (!ok) {
        error = {Code::ExecutionFailed, "UIA automation id condition was rejected"};
        return;
      }
    }

    ComRef<IUIAutomationElementArray> found;
    const HRESULT find_hr = scope->FindAll(k_subtree, match.get(), found.put());
    if (FAILED(find_hr) || !found) {
      if (is_gone(find_hr)) {
        if (query.from_id != 0) {
          elements.erase(query.from_id);
          element_count.store(elements.size(), std::memory_order_relaxed);
        }
        error = {Code::TargetGone,
                 "element " + std::to_string(query.from_id) + " is gone"};
      } else {
        error = {Code::ExecutionFailed, widen_error("element search failed", find_hr)};
      }
      return;
    }

    int length = 0;
    found->get_Length(&length);
    const int limit =
        length < static_cast<int>(query.max_results) ? length : static_cast<int>(query.max_results);
    out.reserve(static_cast<std::size_t>(limit));
    for (int index = 0; index < limit; ++index) {
      IUIAutomationElement* element = nullptr;
      const HRESULT hr = found->GetElement(index, &element);
      if (FAILED(hr) || !element) continue;
      ElementSnapshot snapshot;
      Error snapshot_error;
      const std::uint64_t id = register_element(element);
      const bool read_ok = snapshot_of(element, id, snapshot, snapshot_error);
      element->Release();
      if (!read_ok) {
        elements.erase(id);
        element_count.store(elements.size(), std::memory_order_relaxed);
        if (snapshot_error.code == Code::TargetGone) continue;
        error = snapshot_error;
        return;
      }
      out.push_back(std::move(snapshot));
    }
  }

  ComRef<IUIAutomationElement> take_element(std::uint64_t id, Error& error) {
    const auto found = elements.find(id);
    if (found == elements.end()) {
      error = {Code::TargetGone, "element " + std::to_string(id) + " is gone"};
      return {};
    }
    ComRef<IUIAutomationElement> element;
    element.attach(found->second.clone());
    return element;
  }

  void drop_element(std::uint64_t id) {
    elements.erase(id);
    element_count.store(elements.size(), std::memory_order_relaxed);
  }

  void read_job(std::uint64_t id, ElementSnapshot& out, Error& error) {
    error = Error::none();
    ComRef<IUIAutomationElement> element = take_element(id, error);
    if (!element) return;
    ElementSnapshot snapshot;
    if (!snapshot_of(element.get(), id, snapshot, error)) {
      if (error.code == Code::TargetGone) drop_element(id);
      return;
    }
    out = std::move(snapshot);
  }

  void invoke_job(std::uint64_t id, Error& error) {
    error = Error::none();
    ComRef<IUIAutomationElement> element = take_element(id, error);
    if (!element) return;

    IUnknown* raw_pattern = nullptr;
    const HRESULT pattern_hr = element->GetCurrentPattern(UIA_InvokePatternId, &raw_pattern);
    ComRef<IUIAutomationInvokePattern> pattern;
    if (raw_pattern) {
      raw_pattern->QueryInterface(IID_IUIAutomationInvokePattern,
                                  reinterpret_cast<void**>(pattern.put()));
      raw_pattern->Release();
    }
    if (FAILED(pattern_hr) || !pattern) {
      if (is_gone(pattern_hr)) {
        drop_element(id);
        error = {Code::TargetGone, "element " + std::to_string(id) + " is gone"};
      } else {
        error = {Code::Unsupported, "element " + std::to_string(id) + " does not support invoke"};
      }
      return;
    }

    const HRESULT invoke_hr = pattern->Invoke();
    if (FAILED(invoke_hr)) {
      if (is_gone(invoke_hr)) {
        drop_element(id);
        error = {Code::TargetGone, "element " + std::to_string(id) + " is gone"};
      } else {
        error = {Code::ExecutionFailed, widen_error("element invoke failed", invoke_hr)};
      }
    }
  }

  void release_job(std::uint64_t id, bool& out) {
    out = elements.erase(id) > 0;
    element_count.store(elements.size(), std::memory_order_relaxed);
  }
};

UiaService::UiaService() : impl_(std::make_unique<Impl>()) {}

UiaService::~UiaService() { (void)stop(); }

Error UiaService::start() {
  std::lock_guard<std::mutex> guard(impl_->lifecycle);
  if (impl_->state.load() != UiaServiceState::Created) {
    return {Code::InvalidState, "automation service can only be started once"};
  }
  std::promise<Error> ready;
  std::future<Error> ready_future = ready.get_future();
  impl_->thread = std::thread([impl = impl_.get(), ready = std::move(ready)]() mutable {
    impl->thread_main(std::move(ready));
  });
  const Error init = ready_future.get();
  if (!init.ok()) {
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->state.store(UiaServiceState::Stopped);
    return init;
  }
  impl_->state.store(UiaServiceState::Running);
  return Error::none();
}

Error UiaService::stop() {
  std::lock_guard<std::mutex> guard(impl_->lifecycle);
  const UiaServiceState current = impl_->state.load();
  if (current == UiaServiceState::Stopped) return Error::none();
  if (current == UiaServiceState::Created) {
    impl_->state.store(UiaServiceState::Stopped);
    return Error::none();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->quit = true;
    impl_->state.store(UiaServiceState::Stopping);
  }
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->state.store(UiaServiceState::Stopped);
  return Error::none();
}

UiaServiceState UiaService::state() const { return impl_->state.load(); }

Error UiaService::find(const FindQuery& query, std::vector<ElementSnapshot>& out) {
  out.clear();
  Error job_error = Error::none();
  const Error queued = impl_->enqueue([&] {
    try {
      impl_->find_job(query, out, job_error);
    } catch (...) {
      job_error = {Code::ExecutionFailed, "automation find failed"};
    }
  });
  if (!queued.ok()) return queued;
  return job_error;
}

Error UiaService::read(const std::uint64_t id, ElementSnapshot& out) {
  out = ElementSnapshot{};
  Error job_error = Error::none();
  const Error queued = impl_->enqueue([&] {
    try {
      impl_->read_job(id, out, job_error);
    } catch (...) {
      job_error = {Code::ExecutionFailed, "automation read failed"};
    }
  });
  if (!queued.ok()) return queued;
  return job_error;
}

Error UiaService::invoke(const std::uint64_t id) {
  Error job_error = Error::none();
  const Error queued = impl_->enqueue([&] {
    try {
      impl_->invoke_job(id, job_error);
    } catch (...) {
      job_error = {Code::ExecutionFailed, "automation invoke failed"};
    }
  });
  if (!queued.ok()) return queued;
  return job_error;
}

bool UiaService::release(const std::uint64_t id) {
  if (impl_->state.load() != UiaServiceState::Running) return false;
  bool released = false;
  const Error queued = impl_->enqueue([&] {
    try {
      impl_->release_job(id, released);
    } catch (...) {
      released = false;
    }
  });
  if (!queued.ok()) return false;
  return released;
}

std::size_t UiaService::element_count() const {
  return impl_->element_count.load(std::memory_order_relaxed);
}

}  // namespace rime::automation
