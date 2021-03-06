// Provides the PlatformGetThreadLocalEventBuffer() function by using
// pthread keys. This should work for all POSIX platforms, but will need
// to be different for Windows.
#ifndef TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_PLATFORM_AUX_STD_THREADED_INL_H_
#define TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_PLATFORM_AUX_STD_THREADED_INL_H_

#include <atomic>
#include <mutex>

// TODO(wcraddock, laurenzo): Begin using the C++11 thread interface.
#include <pthread.h>

namespace wtf {

// On this platform, use the standard-library versions of atomics and mutexes.
namespace platform {
using mutex = std::mutex;

template <typename T>
using lock_guard = std::lock_guard<T>;

template <typename T>
using atomic = std::atomic<T>;
}  // namespace platform

namespace internal {
extern pthread_key_t event_buffer_key;
extern pthread_once_t initialize_threading_once;

void InitializeThreadingOnce();

}  // namespace internal

inline EventBuffer* PlatformGetThreadLocalEventBuffer() {
  pthread_once(&internal::initialize_threading_once,
               internal::InitializeThreadingOnce);
  return static_cast<EventBuffer*>(
      pthread_getspecific(internal::event_buffer_key));
}

}  // namespace wtf

#endif  // TRACING_FRAMEWORK_BINDINGS_CPP_INCLUDE_WTF_PLATFORM_AUX_STD_THREADED_INL_H_
