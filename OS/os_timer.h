/**
 * @file os_timer.h
 */

#ifndef OS_TIMER_H
#define OS_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include <stdint.h>
#include <stdbool.h>
#include "os_hal_tick.h"
#include "os_ll.h"
/*********************
 *      DEFINES
 *********************/
#define OS_NO_TIMER_READY 0xFFFFFFFF

/**********************
 *      TYPEDEFS
 **********************/
struct _os_timer_t;

/**
 * Timers execute this type of functions.
 */
typedef void (*os_timer_cb_t)(struct _os_timer_t *);

/**
 * Descriptor of a os_timer
 */
typedef struct _os_timer_t {
    uint32_t period; /**< How often the timer should run*/
    uint32_t last_run; /**< Last time the timer ran*/
    os_timer_cb_t timer_cb; /**< Timer function*/
    void * user_data; /**< Custom user data*/
    int32_t repeat_count; /**< 1: One time;  -1 : infinity;  n>0: residual times*/
    uint32_t paused : 1;
} os_timer_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Init the os_timer module
 */
void _os_timer_core_init(void);

//! @cond Doxygen_Suppress

/**
 * Call it periodically to handle os_timers.
 * @return time till it needs to be run next (in ms)
 */
uint32_t  os_timer_handler(void);

//! @endcond

/**
 * Call it in the super-loop of main() or threads. It will run lv_timer_handler()
 * with a given period in ms. You can use it with sleep or delay in OS environment.
 * This function is used to simplify the porting.
 * @param __ms the period for running lv_timer_handler()
 */
static inline uint32_t os_timer_handler_run_in_period(uint32_t ms)
{
    static uint32_t last_tick = 0;
    uint32_t curr_tick = os_tick_get();

    if((curr_tick - last_tick) >= (ms)) {
        last_tick = curr_tick;
        return os_timer_handler();
    }
    return 1;
}

/**
 * Create an "empty" timer. It needs to initialized with at least
 * `os_timer_set_cb` and `os_timer_set_period`
 * @return pointer to the created timer
 */
os_timer_t * os_timer_create_basic(void);

/**
 * Create a new os_timer
 * @param timer_xcb a callback to call periodically.
 *                 (the 'x' in the argument name indicates that it's not a fully generic function because it not follows
 *                  the `func_name(object, callback, ...)` convention)
 * @param period call period in ms unit
 * @param user_data custom parameter
 * @return pointer to the new timer
 */
os_timer_t * os_timer_create(os_timer_cb_t timer_xcb, uint32_t period, void * user_data);

/**
 * Delete a os_timer
 * @param timer pointer to an os_timer
 */
void os_timer_del(os_timer_t * timer);

/**
 * Pause/resume a timer.
 * @param timer pointer to an os_timer
 */
void os_timer_pause(os_timer_t * timer);

void os_timer_resume(os_timer_t * timer);

/**
 * Set the callback the timer (the function to call periodically)
 * @param timer pointer to a timer
 * @param timer_cb the function to call periodically
 */
void os_timer_set_cb(os_timer_t * timer, os_timer_cb_t timer_cb);

/**
 * Set new period for a os_timer
 * @param timer pointer to a os_timer
 * @param period the new period
 */
void os_timer_set_period(os_timer_t * timer, uint32_t period);

/**
 * Make a os_timer ready. It will not wait its period.
 * @param timer pointer to a os_timer.
 */
void os_timer_ready(os_timer_t * timer);

/**
 * Set the number of times a timer will repeat.
 * @param timer pointer to a os_timer.
 * @param repeat_count -1 : infinity;  0 : stop ;  n>0: residual times
 */
void os_timer_set_repeat_count(os_timer_t * timer, int32_t repeat_count);

/**
 * Reset a os_timer.
 * It will be called the previously set period milliseconds later.
 * @param timer pointer to a os_timer.
 */
void os_timer_reset(os_timer_t * timer);

/**
 * Enable or disable the whole os_timer handling
 * @param en true: os_timer handling is running, false: os_timer handling is suspended
 */
void os_timer_enable(bool en);

/**
 * Get idle percentage
 * @return the os_timer idle in percentage
 */
uint8_t os_timer_get_idle(void);

/**
 * Iterate through the timers
 * @param timer NULL to start iteration or the previous return value to get the next timer
 * @return the next timer or NULL if there is no more timer
 */
os_timer_t * os_timer_get_next(os_timer_t * timer);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
