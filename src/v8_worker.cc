// Native V8 query worker for the opt-in backend. Speaks the same bounded
// newline-delimited JSON protocol as scripts/query-worker.mjs so js_worker.c
// can use either host without changing the engine-side contract.
//
// V8 is initialized as a single-threaded embedder: one isolate, entered by the
// main thread only. A watchdog thread calls TerminateExecution() when a
// request exceeds its budget. Confinement and process-lifetime policy stay in
// the parent (src/js_worker.c).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/libplatform/libplatform.h"
#include "include/v8-array-buffer.h"
#include "include/v8-context.h"
#include "include/v8-container.h"
#include "include/v8-function.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-json.h"
#include "include/v8-local-handle.h"
#include "include/v8-object.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"
#include "include/v8-template.h"

namespace {

constexpr size_t kMaxFrame = 8 * 1024 * 1024;
constexpr size_t kMaxSource = 4 * 1024 * 1024;
constexpr int kMaxLogs = 64;
constexpr size_t kMaxLogChars = 512;
v8::Isolate *g_isolate = nullptr;
v8::Global<v8::Context> g_json_context;
v8::Global<v8::Context> g_context;
std::string g_bundle;
std::vector<std::string> g_pending_logs;

struct Watchdog {
  std::atomic<bool> stop{false};
  std::atomic<bool> active{false};
  std::atomic<bool> terminated{false};
  std::atomic<long long> deadline_us{0};
  std::thread thread;

  ~Watchdog() {
    stop = true;
    if (thread.joinable()) thread.join();
  }

  void start() {
    thread = std::thread([this] {
      while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!active.load()) continue;
        long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now >= deadline_us.load() && !terminated.exchange(true) && g_isolate)
          g_isolate->TerminateExecution();
      }
    });
  }

  void begin(int budget_ms) {
    terminated = false;
    long long now = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    deadline_us = now + (long long)budget_ms * 1000;
    active = true;
  }

  void end() {
    active = false;
    deadline_us = 0;
    if (g_isolate) g_isolate->CancelTerminateExecution();
  }
};
Watchdog g_watchdog;

const char *kPrelude = R"JS(
/* V8 16 defines `window` in a plain context; the pinned bundle's browser
   guards then try to use `document`. Node's vm context leaves it undefined. */
delete globalThis.window;
globalThis.__workerLogs = [];
globalThis.__workerLog = value => {
  if (__workerLogs.length >= 64) return;
  const text = String(value);
  __workerLogs.push(text.length > 512 ? text.slice(0, 512) : text);
};
delete globalThis.window;
globalThis.performance = { now: () => Date.now() };
globalThis.console = { log(){}, warn(){}, error(){}, info(){}, debug(){} };
globalThis.__workerSliders = [];
globalThis.__workerBinds = [];
globalThis.__hostSlider = v => v;
globalThis.__hostSliderBind = () => __workerBinds.length ? __workerBinds.shift() : -1;
globalThis.__hostSliderRead = i => __workerSliders[i];
globalThis.__midiCC = new Map();
globalThis.__hostMidiOpen = () => 1;
globalThis.__hostMidiRead = (cc, chan) => __midiCC.get(String(cc) + ':' + String(chan ?? 0)) ?? 0;
globalThis.__hostMidiKeys = () => { throw new Error('midikeys are unavailable in the query worker'); };
globalThis.__hostMidiManifest = () => { throw new Error('MIDI manifests require the QuickJS host'); };
globalThis.__csoundLoads = 0;
globalThis.__hostCsound = (kind, text) => {
  if (++__csoundLoads > 16) throw new Error('too many Csound loads');
  if (typeof kind !== 'string' || typeof text !== 'string' ||
      (kind === 'code' && text.length > 65536) ||
      (kind === 'url' && text.length > 480)) throw new Error('invalid Csound load');
};
globalThis.__prebakeLoadingAllowed = true;
globalThis.__hostPrebake = name => {
  if (!__prebakeLoadingAllowed) throw new Error('prebake loads are closed after evaluation');
  if (!Object.hasOwn(__prebakes, name)) throw new Error('prebake unavailable in the query worker');
  return __prebakes[name];
};
)JS";

const char *kHelpers = R"JS(
globalThis.__setCompileState = frame => {
  globalThis.__source = frame.source;
  globalThis.__bpm = (typeof frame.bpm === 'number' && isFinite(frame.bpm) && frame.bpm > 0) ? frame.bpm : 120;
  globalThis.__prebake = frame.prebake || '';
  globalThis.__prebakes = frame.prebakes || {};
  globalThis.__workerBinds = (frame.sliders && Array.isArray(frame.sliders.binds)) ? frame.sliders.binds.slice() : [];
  globalThis.__workerSliders = (frame.sliders && Array.isArray(frame.sliders.values)) ? frame.sliders.values.slice() : [];
  globalThis.__midiCC = new Map();
};
globalThis.__setQueryState = frame => {
  globalThis.__begin = frame.begin;
  globalThis.__end = frame.end;
  globalThis.__cps = (typeof frame.cps === 'number' && isFinite(frame.cps) && frame.cps > 0) ? frame.cps : undefined;
  if (Array.isArray(frame.sliders)) globalThis.__workerSliders = frame.sliders.slice();
  if (Array.isArray(frame.midi)) {
    globalThis.__midiCC = new Map(frame.midi.map(e => [String(e[0]) + ':' + String(e[1]), e[2]]));
  }
};
globalThis.__compileRunner = () => {
  globalThis.__compileResult = undefined;
  globalThis.__compileError = undefined;
  __compile(__source, __bpm, __prebake).then(
    value => { globalThis.__compileResult = value; },
    error => { globalThis.__compileError = String(error); });
  return '';
};
globalThis.__queryRunner = () => __query(__begin, __end, __cps, false);
globalThis.__probeRunner = () => __query(__begin, __end, __cps, true);
)JS";

std::string ToStd(v8::Isolate *isolate, v8::Local<v8::Value> value) {
  if (value.IsEmpty()) return std::string();
  v8::String::Utf8Value utf8(isolate, value);
  return utf8.length() ? std::string(*utf8, (size_t)utf8.length()) : std::string();
}

v8::Local<v8::String> FromStd(v8::Isolate *isolate, const std::string &text) {
  return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal,
                                 (int)text.size())
      .ToLocalChecked();
}

std::string JsonQuote(v8::Local<v8::Context> context, const std::string &text) {
  v8::Local<v8::Value> quoted;
  if (!v8::JSON::Stringify(context, FromStd(g_isolate, text))
           .ToLocal(&quoted))
    return "\"\"";
  return ToStd(g_isolate, quoted);
}

v8::Local<v8::Value> GetGlobal(v8::Local<v8::Context> context, const char *name);

void CollectLogs(v8::Local<v8::Context> context) {
  g_pending_logs.clear();
  v8::Local<v8::Value> logs = GetGlobal(context, "__workerLogs");
  if (!logs->IsArray()) return;
  v8::Local<v8::Array> array = logs.As<v8::Array>();
  uint32_t count = array->Length();
  for (uint32_t i = 0; i < count && g_pending_logs.size() < kMaxLogs; ++i) {
    v8::Local<v8::Value> entry =
        array->Get(context, i).ToLocalChecked();
    std::string text = ToStd(g_isolate, entry);
    if (text.size() > kMaxLogChars) text.resize(kMaxLogChars);
    g_pending_logs.push_back(std::move(text));
  }
}

bool RunScript(v8::Local<v8::Context> context, v8::Local<v8::Script> script,
               v8::Local<v8::Value> *result, std::string *error) {
  v8::Isolate *isolate = g_isolate;
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> value;
  if (!script->Run(context).ToLocal(&value)) {
    *error = try_catch.HasTerminated()
                 ? std::string("script timeout")
                 : ToStd(isolate, try_catch.Exception());
    if (!try_catch.HasTerminated()) {
      v8::Local<v8::Value> stack;
      if (try_catch.StackTrace(context).ToLocal(&stack)) {
        std::string trace = ToStd(isolate, stack);
        if (!trace.empty()) *error += " | " + trace.substr(0, 2000);
      }
    }
    if (error->empty()) *error = "script failed";
    return false;
  }
  if (result) *result = value;
  return true;
}

std::unique_ptr<v8::Global<v8::Script>> Compile(v8::Local<v8::Context> context,
                                                const std::string &source,
                                                const char *name) {
  v8::Isolate *isolate = g_isolate;
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, FromStd(isolate, source)).ToLocal(&script)) {
    std::cerr << "v8-worker: cannot compile " << name << ": "
              << ToStd(isolate, try_catch.Exception()) << '\n';
    return nullptr;
  }
  return std::make_unique<v8::Global<v8::Script>>(isolate, script);
}

bool RunFresh(v8::Local<v8::Context> context, const std::string &source,
              std::string *error) {
  auto script = Compile(context, source, "runtime script");
  if (!script) {
    *error = "script compilation failed";
    return false;
  }
  return RunScript(context, script->Get(g_isolate), nullptr, error);
}

v8::Local<v8::Value> GetGlobal(v8::Local<v8::Context> context, const char *name) {
  return context->Global()
      ->Get(context, FromStd(g_isolate, name))
      .ToLocalChecked();
}

bool CallGlobal(v8::Local<v8::Context> context, const char *name,
                v8::Local<v8::Value> argument,
                v8::Local<v8::Value> *result, std::string *error) {
  v8::Isolate *isolate = g_isolate;
  v8::Local<v8::Value> function = GetGlobal(context, name);
  if (!function->IsFunction()) {
    *error = std::string(name) + " is not a function";
    return false;
  }
  v8::Local<v8::Value> argv[1] = {argument};
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> value;
  if (!function.As<v8::Function>()
           ->Call(context, context->Global(), 1, argv)
           .ToLocal(&value)) {
    *error = try_catch.HasTerminated() ? std::string("script timeout")
                                       : ToStd(isolate, try_catch.Exception());
    if (error->empty()) *error = "call failed";
    return false;
  }
  if (result) *result = value;
  return true;
}

bool NewContext(std::string *error) {
  v8::Isolate *isolate = g_isolate;
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  if (context.IsEmpty()) {
    *error = "cannot create context";
    return false;
  }
  v8::Context::Scope context_scope(context);
  if (!RunFresh(context, kPrelude, error) ||
      (!g_bundle.empty() && !RunFresh(context, g_bundle, error)) ||
      !RunFresh(context, kHelpers, error))
    return false;
  g_context.Reset(isolate, context);
  return true;
}

void WriteResponse(int id, bool ok, const std::string &field,
                   const std::string &value, const std::string &error,
                   const std::string &version) {
  v8::Isolate::Scope isolate_scope(g_isolate);
  v8::HandleScope handle_scope(g_isolate);
  v8::Local<v8::Context> context = g_json_context.Get(g_isolate);
  v8::Context::Scope context_scope(context);
  std::string json = "{\"id\":" + std::to_string(id) + ",\"ok\":" +
                     (ok ? "true" : "false");
  if (!field.empty())
    json += ",\"" + field + "\":" + JsonQuote(context, value);
  if (!error.empty()) json += ",\"error\":" + JsonQuote(context, error);
  if (!version.empty()) json += ",\"v8\":" + JsonQuote(context, version);
  json += ",\"logs\":[";
  for (size_t i = 0; i < g_pending_logs.size(); ++i) {
    if (i) json += ',';
    json += JsonQuote(context, g_pending_logs[i]);
  }
  json += "]}\n";
  if (json.size() > kMaxFrame)
    json = "{\"id\":" + std::to_string(id) +
           ",\"ok\":false,\"error\":\"response exceeds the frame limit\",\"logs\":[]}\n";
  std::fwrite(json.data(), 1, json.size(), stdout);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv) {
  if (!v8::V8::InitializeICUDefaultLocation(argv[0])) {
    std::cerr << "v8-worker: cannot initialize ICU; place icudtl.dat beside the binary\n";
    return 1;
  }
  v8::V8::InitializeExternalStartupData(argv[0]);
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate *isolate = v8::Isolate::New(params);
  g_isolate = isolate;
  g_watchdog.start();

  int status = 0;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> boot = v8::Context::New(isolate);
    v8::Context::Scope context_scope(boot);
    g_json_context.Reset(isolate, boot);
  }

  std::string line;
  while (status == 0 && std::getline(std::cin, line)) {
    if (line.empty()) continue;
    if (line.size() > kMaxFrame) {
      const char *reply =
          "{\"id\":null,\"ok\":false,\"error\":\"frame limit\",\"logs\":[]}\n";
      std::fwrite(reply, 1, std::char_traits<char>::length(reply), stdout);
      std::fflush(stdout);
      status = 2;
      break;
    }
    g_pending_logs.clear();
    int id = 0;
    std::string op, error, field, value;
    bool close = false;

    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> json_context = g_json_context.Get(isolate);
    v8::Context::Scope json_scope(json_context);

    g_watchdog.begin(4000);
    v8::Local<v8::Value> parsed;
    v8::Local<v8::Object> frame;
    if (!v8::JSON::Parse(json_context, FromStd(isolate, line)).ToLocal(&parsed) ||
        !parsed->IsObject()) {
      error = "malformed frame";
    } else {
      frame = parsed.As<v8::Object>();
      v8::Local<v8::Value> id_value =
          frame->Get(json_context, FromStd(isolate, "id")).ToLocalChecked();
      if (id_value->IsInt32()) id = id_value->Int32Value(json_context).FromMaybe(0);
      op = ToStd(isolate,
                 frame->Get(json_context, FromStd(isolate, "op")).ToLocalChecked());
    }
    if (error.empty() && op == "query") g_watchdog.begin(2000);

    if (error.empty()) {
      if (op == "init") {
        v8::Local<v8::Value> bundle =
            frame->Get(json_context, FromStd(isolate, "bundle")).ToLocalChecked();
        if (!bundle->IsString() || ToStd(isolate, bundle).empty() ||
            ToStd(isolate, bundle).size() > kMaxFrame) {
          error = "invalid bundle";
        } else {
          g_bundle = ToStd(isolate, bundle);
          if (!NewContext(&error))
            error = error.empty() ? "cannot initialize runtime" : error;
          else
            value = v8::V8::GetVersion();
        }
      } else if (op == "compile") {
        if (g_context.IsEmpty()) {
          error = "not initialized";
        } else if (ToStd(isolate,
                         frame->Get(json_context, FromStd(isolate, "source"))
                             .ToLocalChecked()).size() > kMaxSource) {
          error = "source limit";
        } else if (!NewContext(&error)) {
          if (error.empty()) error = "cannot rebuild context";
        } else {
          v8::Local<v8::Context> context = g_context.Get(isolate);
          v8::Context::Scope request_scope(context);
          if (CallGlobal(context, "__setCompileState", frame, nullptr, &error) &&
              CallGlobal(context, "__compileRunner", v8::Undefined(isolate),
                         nullptr, &error)) {
            g_isolate->PerformMicrotaskCheckpoint();
            v8::Local<v8::Value> failure = GetGlobal(context, "__compileError");
            if (!failure->IsUndefined() && failure->IsString()) {
              std::string text = ToStd(isolate, failure);
              error = text.substr(0, 512);
            } else {
              v8::Local<v8::Value> result = GetGlobal(context, "__compileResult");
              if (!result->IsString()) {
                error = "compile did not settle";
              } else {
                field = "metadata";
                value = ToStd(isolate, result);
              }
            }
          }
        }
      } else if (op == "query") {
        if (g_context.IsEmpty()) {
          error = "not initialized";
        } else {
          v8::Local<v8::Context> context = g_context.Get(isolate);
          v8::Context::Scope request_scope(context);
          v8::Local<v8::Value> result;
          v8::Local<v8::Value> probe =
              frame->Get(json_context, FromStd(isolate, "probes")).ToLocalChecked();
          bool probes = probe->IsBoolean() && probe->BooleanValue(isolate);
          const char *runner = probes ? "__probeRunner" : "__queryRunner";
          if (CallGlobal(context, "__setQueryState", frame, nullptr, &error) &&
              CallGlobal(context, runner, v8::Undefined(isolate), &result, &error)) {
            if (!result->IsString()) {
              error = "event output limit";
            } else {
              value = ToStd(isolate, result);
              if (value.size() > kMaxFrame) error = "event output limit";
            }
          }
        }
      } else if (op == "close") {
        close = true;
      } else {
        error = "unknown op";
      }
    }
    if (!g_context.IsEmpty()) {
      v8::HandleScope logs_scope(isolate);
      v8::Local<v8::Context> active = g_context.Get(isolate);
      v8::Context::Scope active_scope(active);
      CollectLogs(active);
    }
    g_watchdog.end();

    if (!error.empty()) field.clear();
    if (op == "init") {
      WriteResponse(id, error.empty(), "v8", value, error, "");
    } else if (op == "compile") {
      WriteResponse(id, error.empty(), error.empty() ? "metadata" : "", value, error, "");
    } else if (op == "query") {
      WriteResponse(id, error.empty(), error.empty() ? "events" : "", value, error, "");
    } else {
      WriteResponse(id, true, "", "", "", "");
    }
    if (close) break;
  }

  g_watchdog.end();
  g_context.Reset();
  g_json_context.Reset();
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete params.array_buffer_allocator;
  return status;
}
