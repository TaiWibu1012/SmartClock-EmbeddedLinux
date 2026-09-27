/**
 * @file clock_screen.h
 * @brief Clock Screen rendering thread using timerfd and NTP jump handling
 * @author PHUC TAI
 */

#ifndef _CLOCK_SCREEN_H_
#define _CLOCK_SCREEN_H_

#include "system_state.h"

/**
 * @brief Main worker thread function for Clock Screen
 * @param arg Pointer to user arguments (typically NULL or shared context)
 * @return NULL on thread termination
 */
void *clock_thread_func(void *arg);

#endif /* _CLOCK_SCREEN_H_ */