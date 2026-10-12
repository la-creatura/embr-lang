// ffi_fixture.cpp
// a tiny C library for the ffi tests: functions that call a callback, for the cases libc has nothing to test with
// (a callback that returns a pointer, or an 8 or 16 bit integer).
//
// good to know
//   - built next to the test programs (bin/<platform>/tests/), see tests/CMakeLists.txt. ffi_test.cpp opens it

#include <cstdint>

#if defined(_WIN32)
#  define FIXTURE_API extern "C" __declspec(dllexport)
#else
#  define FIXTURE_API extern "C" __attribute__((visibility("default")))
#endif

FIXTURE_API void*    fixture_call_ptr(void* (*cb)(void*), void* arg)              { return cb(arg); }
FIXTURE_API int8_t   fixture_call_i8(int8_t (*cb)(int8_t), int8_t x)             { return cb(x); }
FIXTURE_API uint16_t fixture_call_u16(uint16_t (*cb)(uint16_t), uint16_t x)      { return cb(x); }
