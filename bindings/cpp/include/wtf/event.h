#ifndef TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_EVENT_H_
#define TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_EVENT_H_

#include <functional>
#include <string>

#include "wtf/buffer.h"
#include "wtf/config.h"
#include "wtf/platform.h"

namespace wtf {

// Class of events.
enum class EventClass {
  // Instance events (one shot).
  kInstance = 0,

  // Scoped event.
  kScoped = 1,
};

// Flags that can be passed to events.
struct EventFlags {
  // Flags passed to built-in events. Omitted the ones we don't use.
  static constexpr int kInternal = 1 << 3;
  static constexpr int kBuiltin = 1 << 5;
};

// ArgTypeDef for each supported type provides the WTF type name and a
// function for emitting values of the type.
template <typename ArgType>
struct ArgTypeDef {};
template <>
struct ArgTypeDef<const char*> {
  static const char* name;
  static void Emit(EventBuffer* b, const char* value) {
    int string_id = value ? b->string_table()->GetStringId(value)
                          : StringTable::kEmptyStringId;
    b->AddEntry(string_id);
  }
};
template <>
struct ArgTypeDef<uint16_t> {
  static const char* name;
  static void Emit(EventBuffer* b, uint16_t value) { b->AddEntry(value); }
};
template <>
struct ArgTypeDef<uint32_t> {
  static const char* name;
  static void Emit(EventBuffer* b, uint32_t value) { b->AddEntry(value); }
};
template <>
struct ArgTypeDef<int16_t> {
  static const char* name;
  static void Emit(EventBuffer* b, uint16_t value) { b->AddEntry(value); }
};
template <>
struct ArgTypeDef<int32_t> {
  static const char* name;
  static void Emit(EventBuffer* b, uint32_t value) { b->AddEntry(value); }
};

// Value type that can be used to generate an event argument signature. This
// defers the entire cost of generating the signature until it is needed and
// uses template code generation to handle arbitrary types.
class EventDefinition {
 public:
  // Callback function that will be invoked to append a typed arg list
  // to output.
  using ArgumentZipperCallback = void (*)(std::string* output,
                                          const char* arg_names);

  EventDefinition() = default;

  // Create an EventDefinition for an arbitrary list of types and argument
  // name string.
  template <typename... ArgTypes>
  static EventDefinition Create(int wire_id, EventClass event_class, int flags,
                                const char* name_spec) {
    return EventDefinition{wire_id, event_class, flags, name_spec,
                           &EventDefinition::ArgumentZipper<ArgTypes...>};
  }

  // Appends the argument name to the given string.
  void AppendName(std::string* output) const;

  // Appends the argument signature to the given string.
  void AppendArguments(std::string* output) const;

  // Shortcut to construct the name with AppendName. Useful for tests.
  // Production code should try to avoid re-allocating the string by using
  // AppendName directly.
  std::string name() const {
    std::string output;
    AppendName(&output);
    return output;
  }

  // Shortcut to construct a string with AppendArguments. Useful for tests.
  // Production code should try to avoid re-allocating the string by using
  // AppendArguments directly.
  std::string arguments() const {
    std::string output;
    AppendArguments(&output);
    return output;
  }

  // Returns the next event id.
  static int NextEventId() { return next_event_id_.fetch_add(1); }

  int wire_id() const { return wire_id_; }
  EventClass event_class() const { return event_class_; }
  int flags() const { return flags_; }

 private:
  EventDefinition(int wire_id, EventClass event_class, int flags,
                  const char* name_spec, ArgumentZipperCallback argument_zipper)
      : wire_id_(wire_id),
        event_class_(event_class),
        flags_(flags),
        name_spec_(name_spec),
        argument_zipper_(argument_zipper) {}

  // Template that will zip a string of arg names and types into a valid
  // argument signature.
  template <typename... ArgTypes>
  static void ArgumentZipper(std::string* output, const char* arg_names) {
    ZipArgumentsHelper<0, sizeof...(ArgTypes)>::template Zip<ArgTypes...>(
        output, &arg_names);
  }

  // Iterates from 0 .. (kSize - 1), peeling off an ArgType at each depth and
  // processing it. Note that there are ways to write this without using
  // dependent templates, but it is very awkward to declare the variadic
  // argument peeling with that syntax.
  template <size_t k, size_t kSize, typename Enable = void>
  struct ZipArgumentsHelper {
    template <typename FirstType, typename... ArgTypes>
    static void Zip(std::string* output, const char** arg_names) {
      const char* arg_type_name = ArgTypeDef<FirstType>::name;
      ZipArgument(output, k, arg_type_name, arg_names);
      ZipArgumentsHelper<k + 1, kSize>::template Zip<ArgTypes...>(output,
                                                                  arg_names);
    }
  };

  // Terminal iteration specialization that is selected when k == kSize, at
  // which point, the ArgTypes should be empty.
  template <size_t k, size_t kSize>
  struct ZipArgumentsHelper<k, kSize,
                            typename std::enable_if<k == kSize>::type> {
    template <typename... ArgTypes>
    static void Zip(std::string* output, const char** arg_names) {
      static_assert(sizeof...(ArgTypes) == 0,
                    "Should be the terminal specialization.");
    }
  };

  // Appends the next_arg_type to the output signature, peeling an arg name
  // from the *arg_names specification, or generating a name by index.
  static void ZipArgument(std::string* output, size_t index,
                          const char* next_arg_type, const char** arg_names);

  // Hands out event ids.
  static platform::atomic<int> next_event_id_;

  int wire_id_ = 0;
  EventClass event_class_ = EventClass::kInstance;
  int flags_ = 0;
  const char* name_spec_ = nullptr;
  ArgumentZipperCallback argument_zipper_ = nullptr;
};

// Singleton registry of all EventDefinitions.
// The registry is thread safe.
class EventRegistry {
 public:
  // Gets the lone singleton instance.
  static EventRegistry* GetInstance();

  // Adds an event definition.
  // This is a static here because it will be called at every instantiation
  // point and we would like to should code bloat at the expense of a one time
  // function call.
  static void AddEventDefinition(EventDefinition event_definition);

  // Makes a copy of all event definitions. This is potentially expensive but
  // is not deadlock or iteration invalidation prone.
  std::vector<EventDefinition> GetEventDefinitions();

 private:
  platform::mutex mu_;
  std::deque<EventDefinition> event_definitions_;
  EventRegistry();
  EventRegistry(const EventRegistry&) = delete;
  void operator=(const EventRegistry&) = delete;
};

// An Event that can be invoked with arbitrary arguments.
//
// There are a number of constructors for events, but most are only used
// for low-level/system events. The primary user facing constructor simply
// takes a "name spec", which syntactically encodes the event name and
// argument names (argument types are injected based on template args).
//
// Syntax:
//   MyClass#MyMethod: arg1, arg2
//   MyClass#MyMethod
//
// The part before the colon is passed verbatim to WTF. The argument list
// after is merged with the argument types to form a WTF argument signature.
// If any argument names are missing or malformed, they will be emmitted with
// a system generated name.
//
// Validation of the event name or argument names according to WTF rules is
// up to the caller.
template <bool kEnable, typename... ArgTypes>
class EventIf {
 public:
  static constexpr int kArgCount = sizeof...(ArgTypes);

  // Disallow copy and assign.
  EventIf(const EventIf&) = delete;
  void operator=(const EventIf&) = delete;

  // Creates a standard instance event.
  explicit EventIf(const char* name_spec)
      : EventIf(EventClass::kInstance, 0, name_spec) {}

  // Most general Event ctor for defining events of known wire_id. In practice,
  // this is only used for the primordial defineEvent.
  EventIf(int wire_id, EventClass event_class, int flags, const char* name_spec)
      : wire_id_(wire_id) {
    EventRegistry::AddEventDefinition(EventDefinition::Create<ArgTypes...>(
        wire_id, event_class, flags, name_spec));
  }

  // Creates an event with an auto-assigned id.
  EventIf(EventClass event_class, int flags, const char* name_spec)
      : EventIf(EventDefinition::NextEventId(), event_class, flags, name_spec) {
  }

  // Invokes the event with a specific EventBuffer.
  void InvokeSpecific(EventBuffer* event_buffer, ArgTypes... args) {
    event_buffer->AddEntry(wire_id_);
    event_buffer->AddEntry(PlatformGetTimestampMicros32());
    EmitArguments(event_buffer, args...);
  }

  // Invokes the event against the current thread (if it has been enabled).
  void Invoke(ArgTypes... args) {
    EventBuffer* event_buffer = PlatformGetThreadLocalEventBuffer();
    if (event_buffer) {
      InvokeSpecific(event_buffer, args...);
    }
  }

 private:
  // Emitters for a variable list of arguments.
  void EmitArguments(EventBuffer* event_buffer) {}

  template <typename T, typename... RestArgTypes>
  void EmitArguments(EventBuffer* event_buffer, T first, RestArgTypes... rest) {
    ArgTypeDef<T>::Emit(event_buffer, first);
    EmitArguments(event_buffer, rest...);
  }

  int wire_id_;
};

// Explicit specialization for when kEnable == false.
// This must have the same public surface area as the generic version but no-op.
template <typename... ArgTypes>
class EventIf<false, ArgTypes...> {
 public:
  static constexpr int kArgCount = sizeof...(ArgTypes);

  // Disallow copy and assign.
  EventIf(const EventIf&) = delete;
  void operator=(const EventIf&) = delete;

  explicit EventIf(const char*) {}
  EventIf(int wire_id, EventClass event_class, int flags,
          const char* name_spec) {}
  EventIf(EventClass event_class, int flags, const char* name_spec) {}

  void InvokeSpecific(EventBuffer*, ArgTypes...) {}
  void Invoke(ArgTypes...) {}
};

// Default instantiation of EventIf that is enabled if kMasterEnable.
template <typename... ArgTypes>
using Event = EventIf<kMasterEnable, ArgTypes...>;

template <typename... ArgTypes>
using EventEnabled = EventIf<true, ArgTypes...>;

// Container for standard event instantes.
class StandardEvents {
 public:
  // The Scope leave event is special because some code will emit it directly,
  // avoiding the overhead of calling it here. It is arranged to always be
  // registered with a fixed id, but when serializing is must be guaranteed
  // to have been referenced.
  static constexpr int kScopeLeaveEventId = 2;
  static EventEnabled<>& GetScopeLeaveEvent();

  static void DefineEvent(EventBuffer* event_buffer, uint16_t wire_id,
                          uint16_t event_class, uint32_t flags,
                          const char* name, const char* args);
  static void ScopeLeave(EventBuffer* event_buffer);

  // Creates a new zone, returning the zone id.
  static int CreateZone(EventBuffer* event_buffer, const char* name,
                        const char* type, const char* location);

  // Sets a zone.
  static void SetZone(EventBuffer* event_buffer, int zoneId);

  // Notes the start of a frame.
  static void FrameStart(EventBuffer* event_buffer, uint32_t number);

  // Notes the end of a frame.
  static void FrameEnd(EventBuffer* event_buffer, uint32_t number);

 private:
  StandardEvents() = delete;
};

// Raw scope used to track enter and leave of a scope. This does not actually
// do automatic RAII enter/exit, which is done by higher level wrapper types
// and macros.
template <bool kEnable, typename... ArgTypes>
class ScopedEventIf : private EventIf<kEnable, ArgTypes...> {
 public:
  // Disallow copy and assign.
  ScopedEventIf(const ScopedEventIf&) = delete;
  void operator=(const ScopedEventIf&) = delete;

  explicit ScopedEventIf(const char* name_spec)
      : Event<ArgTypes...>(EventClass::kScoped, 0, name_spec) {}

  // Emits an enter event against a specific EventBuffer.
  void EnterSpecific(EventBuffer* event_buffer, ArgTypes... args) {
    Event<ArgTypes...>::InvokeSpecific(event_buffer, args...);
  }

  // Emits a leave event against a specific EventBuffer.
  void LeaveSpecific(EventBuffer* event_buffer) {
    // We directly emit the scope leave event to avoid some overhead.
    event_buffer->AddEntry(StandardEvents::kScopeLeaveEventId);
    event_buffer->AddEntry(PlatformGetTimestampMicros32());
  }

  // Emits an enter event against the current thread's EventBuffer (if enabled).
  // This is here for completeness: The RAII wrappers use
  // EnterSpecific/LeaveSpecific directly for efficiency.
  void Enter(ArgTypes... args) {
    EventBuffer* event_buffer = PlatformGetThreadLocalEventBuffer();
    if (event_buffer) {
      EnterSpecific(event_buffer, args...);
    }
  }

  // Emits a leave event against the current thread's EventBuffer (if enabled).
  // This is here for completeness: The RAII wrappers use
  // EnterSpecific/LeaveSpecific directly for efficiency.
  void Leave() {
    EventBuffer* event_buffer = PlatformGetThreadLocalEventBuffer();
    if (event_buffer) {
      LeaveSpecific(event_buffer);
    }
  }
};

// RAII wrapper around a static ScopedEvent.
template <bool kEnable, typename... ArgTypes>
class AutoScopeIf {
 public:
  using EventType = ScopedEventIf<kEnable, ArgTypes...>;

  // Disallow copy and assign.
  AutoScopeIf(const AutoScopeIf&) = delete;
  void operator=(const AutoScopeIf&) = delete;

  explicit AutoScopeIf(EventType& event)  // NOLINT
      : event_(event),
        event_buffer_(nullptr) {}

  // Even though it makes the API a bit fragile, having a separate Enter()
  // function is more compatible with macro invocation.
  void Enter(ArgTypes... args) {
    event_buffer_ = PlatformGetThreadLocalEventBuffer();
    if (event_buffer_) {
      event_.EnterSpecific(event_buffer_, args...);
    }
  }

  ~AutoScopeIf() {
    if (event_buffer_) {
      event_.LeaveSpecific(event_buffer_);
    }
  }

 private:
  EventType& event_;
  EventBuffer* event_buffer_;
};

// Explicit specialization for when kEnable == false.
// This must have the same public surface area as the generic version but no-op.
template <typename... ArgTypes>
class AutoScopeIf<false, ArgTypes...> {
 public:
  using EventType = ScopedEventIf<false, ArgTypes...>;

  // Disallow copy and assign.
  AutoScopeIf(const AutoScopeIf&) = delete;
  void operator=(const AutoScopeIf&) = delete;

  explicit AutoScopeIf(EventType&) {}  // NOLINT
  void Enter(ArgTypes... args) {}
};

// Default instantiation of AutoScopeIf that is enabled if kMasterEnable.
template <typename... ArgTypes>
using AutoScope = AutoScopeIf<kMasterEnable, ArgTypes...>;

template <typename... ArgTypes>
using AutoScopeEnabled = AutoScopeIf<true, ArgTypes...>;

// Explicit specialization for when kEnable == false.
// This must have the same public surface area as the generic version but no-op.
template <typename... ArgTypes>
class ScopedEventIf<false, ArgTypes...> {
 public:
  // Disallow copy and assign.
  ScopedEventIf(const ScopedEventIf&) = delete;
  void operator=(const ScopedEventIf&) = delete;

  explicit ScopedEventIf(const char*) {}
  void EnterSpecific(EventBuffer*, ArgTypes...) {}
  void LeaveSpecific(EventBuffer*) {}
  void Enter(ArgTypes... args) {}
  void Leave() {}
};

// Default instantiation of ScopedEventIf that is enabled if kMasterEnable.
template <typename... ArgTypes>
using ScopedEvent = ScopedEventIf<kMasterEnable, ArgTypes...>;

template <typename... ArgTypes>
using ScopedEventEnabled = ScopedEventIf<true, ArgTypes...>;

}  // namespace wtf

#endif  // TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_EVENT_H_
