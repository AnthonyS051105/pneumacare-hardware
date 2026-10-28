#pragma once

// Critical section lintas-core untuk data yang dibagi antara task acquisition
// (Core 0) dan task network (Core 1). Di build native (unit test PC) tidak ada
// FreeRTOS dan test berjalan single-thread, jadi lock menjadi no-op.

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>

typedef portMUX_TYPE platform_lock_t;
#define PLATFORM_LOCK_INITIALIZER portMUX_INITIALIZER_UNLOCKED
#define platform_lock_enter(lock) portENTER_CRITICAL(lock)
#define platform_lock_exit(lock) portEXIT_CRITICAL(lock)

#else

typedef int platform_lock_t;
#define PLATFORM_LOCK_INITIALIZER 0
#define platform_lock_enter(lock) ((void)(lock))
#define platform_lock_exit(lock) ((void)(lock))

#endif
