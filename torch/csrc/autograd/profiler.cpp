#include <torch/csrc/autograd/profiler.h>
#include <torch/csrc/autograd/function.h>
#include <torch/csrc/jit/frontend/code_template.h>

#include <torch/csrc/jit/runtime/operator.h>

#include <ATen/core/op_registration/op_registration.h>
#include <torch/library.h>

#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <ATen/ThreadLocalDebugInfo.h>

namespace torch { namespace autograd { namespace profiler {

namespace {

CUDAStubs default_stubs;
constexpr CUDAStubs* default_stubs_addr = &default_stubs;
// Constant initialization, so it is guaranteed to be initialized before
// static initialization calls which may invoke registerCUDAMethods
static CUDAStubs* cuda_stubs = default_stubs_addr;

// We decompose the profiler logic into the following components:
//
// ThreadLocalDebugInfo:
//
// ThreadLocalDebugInfo is a thread local mapping from slots into
// the debug information structs.
// ThreadLocalDebugInfo is automatically propagated across thread
// boundaries, including the cases of:
//  - launching async jobs with at::launch
//  - executing JIT continuations
//  - moving from the forward threads into autograd (backward) threads
//
// Entries in ThreadLocalDebugInfo are managed by DebugInfoGuard
// which can be used to add or overwrite an entry in the thread local
// mapping. A corresponding entry is removed when the guard is destroyed,
// potentially revealing the previously set value for the same slot.
//
// For the async tasks, slots previuosly set in the main thread before
// launching of an async task are shared and visible in the async task.
//
// On the other hand, any adding or overwriting of the mapping by the
// async task is not visible to the main thread and any modification
// (including removal of the entries) in the main thread is not visible
// to the async task if it happends after launching the task.
//
// We use ThreadLocalDebugInfo (slot PROFILER_STATE) to store profiler config,
// as well as a list of events that happen during profiling.
// An instance of ThreadLocalDebugInfo is created each time we enter
// profiler (i.e. enter profiling context manager/call enableConfig) and
// uniquely identifies a profiling run.
//
// We automatically propagate ThreadLocalDebugInfo into async tasks,
// as well as across JIT continuations and autograd thread, so all
// the operations that happen between profiling start and end
// (not necessarily within the same thread) are recorded.
// Unless the profiling slot is overwritten as in the case of nested
// profiling ranges (in this case events for the subrange are handled
// by the nested profiler)
//
// When we exit a profiling range (either by exiting profiling context
// manager or by calling disableProfiler), we remove the previously set
// profiling entry for the given thread local mapping, and consolidate
// events in the profiling result
//
//
// ThreadLocalState:
//
// ThreadLocalState takes a 'snapshot' of thread local variables
// using provided getters. It is used together with ThreadLocalStateGuard
// to transfer the snapshot across thread boundary and set the thread local
// values as in the parent task.
//
// Profiler uses ThreadLocalState to propagate profiler's thread local state.
// ThreadLocalState also automatically propagates profiler callbacks.
//
//
// RecordFunction and observers
//
// Profiler uses observers mechanism to add a pair of thread local callbacks
// that are executed on a number of predetermined ranges, including:
//  - c10/ATen ops
//  - TorchScript functions/methods
//  - user defined named ranges (see `record_function` python context manager)
//
// Profiler setups a pair of callbacks that record profiling events and save them
// into the thread local profiler struct (ThreadLocalDebugInfo, PROFILER_STATE slot)
//
//
// Thus, the overall logic is:
//
// enableProfiler:
//  - checks that profiler is not enabled (otherwise throws)
//  - pushes new ThreadLocalDebugInfo (slot PROFILER_STATE) as the profiler
//    config for the current thread
//  - pushes profiling callbacks for the current thread
//
// disableProfiler:
//  - pops PROFILER_STATE slot from the current ThreadLocalDebugInfo and
//    consolidates events
//  - removes profiling callbacks
//
// ThreadLocalState:
//  - propagates ThreadLocalDebugInfo across threads
//  - propagates profiler callbacks across threads
//
// Profiler callbacks:
//  - get the current profiling state (PROFILER slot in ThreadLocalDebugInfo)
//  - save profiling events into the profiling state
//

// Profiler state
struct ProfilerThreadLocalState : public at::DebugInfoBase {
  explicit ProfilerThreadLocalState(
      ProfilerState state,
      bool report_input_shapes)
    : config_(state, report_input_shapes) {}
  explicit ProfilerThreadLocalState(
      const ProfilerConfig& config)
    : config_(config) {}

  inline const ProfilerConfig& config() const {
    return config_;
  }

  thread_event_lists consolidate() {
    std::lock_guard<std::mutex> g(mutex_);
    thread_event_lists result;
    for (auto it = event_lists_map_.begin(); it != event_lists_map_.end(); ++it) {
      auto & list = it->second;
      result.emplace_back(list->consolidate());
    }
    return result;
  }

  void mark(
    std::string name,
    bool include_cuda = true) {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      cuda_stubs->nvtxMarkA(name.c_str());
    } else {
      std::lock_guard<std::mutex> guard(mutex_);
      auto& list = getEventList();
      list.record(
          EventKind::Mark,
          StringView(std::move(name)),
          RecordFunction::currentThreadId(),
          include_cuda && config_.state == ProfilerState::CUDA);
    }
  }

  void pushRange(
    const StringView& name,
    const char* msg = "",
    int64_t sequence_nr = -1,
    std::vector<std::vector<int64_t>>&& shapes = {}) {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      if (sequence_nr >= 0 || shapes.size() > 0) {
        std::stringstream s;
        if (sequence_nr >= 0) {
          s << name.str() << msg << sequence_nr;
        }
        if (shapes.size() > 0) {
          s << ", sizes = [";
          for (int i = 0; i < shapes.size(); i++) {
            if (shapes[i].size() > 0) {
              s << "[";
              for (int dim = 0; dim < shapes[i].size(); dim++) {
                s << shapes[i][dim];
                if (dim < shapes[i].size() - 1) {
                  s << ", ";
                }
              }
              s << "]";
            } else {
              s << "[]";
            }
            if (i < shapes.size() - 1) {
              s << ", ";
            }
          }
          s << "]";
        }
        cuda_stubs->nvtxRangePushA(s.str().c_str());
      } else {
        cuda_stubs->nvtxRangePushA(name.str());
      }
    } else {
      std::lock_guard<std::mutex> guard(mutex_);
      auto& list = getEventList();
      list.record(
          EventKind::PushRange,
          name,
          RecordFunction::currentThreadId(),
          config_.state == ProfilerState::CUDA,
          std::move(shapes));
    }
  }

  void popRange(uint64_t orig_thread_id) {
    if (config_.state == ProfilerState::Disabled) {
      return;
    }
    if (config_.state == ProfilerState::NVTX) {
      cuda_stubs->nvtxRangePop();
    } else {
      std::lock_guard<std::mutex> guard(mutex_);
      auto& list = getEventList(orig_thread_id);
      list.record(
          EventKind::PopRange,
          StringView(""),
          orig_thread_id,
          config_.state == ProfilerState::CUDA);
    }
  }

  void setCallbackHandle(CallbackHandle handle) {
    handle_ = handle;
  }

  CallbackHandle callbackHandle() const {
    return handle_;
  }

 private:
  // not thread safe
  RangeEventList& getEventList(int64_t thread_id = -1) {
    if (thread_id < 0) {
      thread_id = RecordFunction::currentThreadId();
    }
    auto it = event_lists_map_.find(thread_id);
    if (it != event_lists_map_.end()) {
      return *(it->second);
    } else {
      auto event_list = std::make_shared<RangeEventList>();
      event_lists_map_[thread_id] = event_list;
      return *(event_list.get());
    }
  }

  std::mutex mutex_;
  std::unordered_map<uint16_t, std::shared_ptr<RangeEventList>>
      event_lists_map_;
  ProfilerConfig config_ = ProfilerConfig(ProfilerState::Disabled, false);
  CallbackHandle handle_;
};

ProfilerThreadLocalState* getProfilerTLSState() {
  const auto& state = at::ThreadLocalDebugInfo::get(at::DebugInfoKind::PROFILER_STATE);
  return static_cast<ProfilerThreadLocalState*>(state.get());
}

void pushProfilingCallbacks() {
  auto state_ptr = getProfilerTLSState();
  TORCH_INTERNAL_ASSERT(state_ptr, "Expected profiler state set");
  auto handle = addThreadLocalCallback(RecordFunctionCallback(
      [](const RecordFunction& fn) {
        auto state_ptr = getProfilerTLSState();
        if (!state_ptr || state_ptr->config().state == ProfilerState::Disabled) {
          return;
        }

        auto* msg = (fn.seqNr() >= 0) ? ", seq = " : "";
        if (state_ptr->config().report_input_shapes) {
          std::vector<std::vector<int64_t>> inputSizes;
          inputSizes.reserve(fn.inputs().size());
          for (const c10::IValue& input : fn.inputs()) {
            if (!input.isTensor()) {
              inputSizes.emplace_back();
              continue;
            }
            const at::Tensor& tensor = input.toTensor();
            if (tensor.defined()) {
              inputSizes.push_back(input.toTensor().sizes().vec());
            } else {
              inputSizes.emplace_back();
            }
          }
          state_ptr->pushRange(fn.name(), msg, fn.seqNr(), std::move(inputSizes));
        } else {
          state_ptr->pushRange(fn.name(), msg, fn.seqNr(), {});
        }
      },
      [](const RecordFunction& fn) {
        auto state_ptr = getProfilerTLSState();
        if (!state_ptr || state_ptr->config().state == ProfilerState::Disabled) {
          return;
        }
        state_ptr->popRange(fn.getStartCallbacksThreadId());
      })
    .needsInputs(state_ptr->config().report_input_shapes)
    .scopes({RecordScope::FUNCTION, RecordScope::USER_SCOPE}));
  state_ptr->setCallbackHandle(handle);
}

void removeProfilingCallbacks() {
  auto state_ptr = getProfilerTLSState();
  TORCH_INTERNAL_ASSERT(state_ptr, "Expected profiler state set");
  removeCallback(state_ptr->callbackHandle());
}

const int kCUDAWarmupStart = 5;

// temp. workaround for dispatcher ::Profiler key
thread_local std::vector<std::shared_ptr<RecordFunctionGuard>> g_;

} // namespace

void registerCUDAMethods(CUDAStubs* stubs) {
  cuda_stubs = stubs;
}

ProfilerConfig::~ProfilerConfig() = default;

bool profilerEnabled() {
  auto state_ptr = getProfilerTLSState();
  return state_ptr && state_ptr->config().state != ProfilerState::Disabled;
}

void enableProfiler(const ProfilerConfig& new_config) {
  TORCH_CHECK(new_config.state != ProfilerState::NVTX || cuda_stubs->enabled(),
    "Can't use NVTX profiler - PyTorch was compiled without CUDA");

  auto state_ptr = getProfilerTLSState();
  TORCH_CHECK(!state_ptr, "Profiler is already enabled on this thread");

  auto state = std::make_shared<ProfilerThreadLocalState>(new_config);
  at::ThreadLocalDebugInfo::_push(at::DebugInfoKind::PROFILER_STATE, state);


  pushProfilingCallbacks();
  g_.emplace_back(std::make_shared<RecordFunctionGuard>());

  if (new_config.state == ProfilerState::CUDA) {
    // event recording appears to have some startup overhead, so we need to
    // to generate some dummy events first before recording synchronization events
    for (int idx = 0; idx < kCUDAWarmupStart; ++idx) {
      cuda_stubs->onEachDevice([state](int /* unused */) {
          state->mark("__cuda_startup");
          cuda_stubs->synchronize();
      });
    }

    // cuda events must be on the same device, so we need a start event recorded
    // for each gpu. we then use this event to synchronize time on the GPU
    // with the CPU clock.
    cuda_stubs->onEachDevice([state](int d) {
        state->mark("__cuda_start_event");
    });
  }
  state->mark("__start_profile", false);
}

thread_event_lists disableProfiler() {
  // all the DebugInfoBase objects are scope based and supposed to use DebugInfoGuard
  auto state = at::ThreadLocalDebugInfo::_pop(at::DebugInfoKind::PROFILER_STATE);
  TORCH_CHECK(state && state->config().state != ProfilerState::Disabled,
      "Can't disable profiler when it's not running");

  g_.pop_back();
  removeProfilingCallbacks();

  if (state->config().state == ProfilerState::NVTX) {
    return thread_event_lists();
  }

  state->mark("__stop_profile");

  return state->consolidate();
}

void Event::record(bool record_cuda) {
  if (record_cuda) {
    cuda_stubs->record(&device_, &event, &cpu_ns_);
    return;
  }
  cpu_ns_ = getTime();
}

double Event::cuda_elapsed_us(const Event & e) {
  if(!e.has_cuda() || !has_cuda()) {
    throw std::logic_error("Events were not recorded for CUDA");
  }
  if(e.device() != device()) {
    throw std::logic_error("Events are not on the same device");
  }
  return cuda_stubs->elapsed(event, e.event);
}

CUDAStubs::~CUDAStubs() = default;


static jit::CodeTemplate event_template(R"(
{
  "name": "${name}",
  "ph": "X",
  "ts": ${ts},
  "dur": ${dur},
  "tid": ${tid},
  "pid": "CPU Functions",
  "args": {}
})");


RecordProfile::RecordProfile(std::ostream& out)
: out_(out) {
  init();
}

RecordProfile::RecordProfile(const std::string& filename)
: file_(new std::ofstream(filename)), out_(*file_) {
  init();
}

void RecordProfile::init() {
  enableProfiler(ProfilerConfig(ProfilerState::CPU, false /* report shapes */));
}

RecordProfile::~RecordProfile() {
  thread_event_lists event_lists = disableProfiler();
  std::vector<Event*> events;
  for(auto& l : event_lists) {
    for(auto& e : l) {
        events.push_back(&e);
    }
  }
  processEvents(events);
  if (file_){
    file_->close();
  }
}

void RecordProfile::processEvents(const std::vector<Event*>& events) {
  TORCH_CHECK(out_, "could not open file");
  Event* start = nullptr;
  for (Event* e : events) {
    if(0 == strcmp(e->name(), "__start_profile")) {
      start = e;
      break;
    }
  }
  TORCH_CHECK(start, "could not find start?");
  std::vector<Event*> stack;
  out_ << "[\n";
  bool first = true;
  for(Event* e : events) {
    if(e->kind() == "push") {
      stack.push_back(e);
    } else if(e->kind() == "pop") {
      if(!first) {
        out_ << ",\n";
      }
      first = false;
      Event* e_start = stack.back();
      stack.pop_back();
      jit::TemplateEnv env;
      env.s("name", e_start->name());
      env.d("ts", start->cpu_elapsed_us(*e_start));
      env.d("dur", e_start->cpu_elapsed_us(*e));
      env.d("tid", e_start->thread_id());
      out_ << event_template.format(env);
    }
  }
  out_ << "]\n";
}

}}}

void profile_wrapper(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  c10::impl::ExcludeDispatchKeyGuard key_guard(c10::DispatchKey::Profiler);
#if !defined(CAFFE2_IS_XPLAT_BUILD) && !defined(C10_MOBILE)
  RECORD_FUNCTION(op.schema().name(), *stack, torch::autograd::Node::peek_at_next_sequence_nr());
#else
  RECORD_FUNCTION(op.schema().name(), *stack);
#endif
  op.callBoxed(stack);
}

TORCH_LIBRARY_IMPL(_, Profiler, m) {
  m.fallback(torch::CppFunction::makeFromBoxedFunction<&profile_wrapper>());
}
