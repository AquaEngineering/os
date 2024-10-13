/**
 * @file os_timer.c
 */

/*********************
 *      INCLUDES
 *********************/
#include "os_timer.h"
#include "os_ll.h"
#include "os_mem.h"
#include "os_hal_tick.h"
/*********************
 *      DEFINES
 *********************/
#define IDLE_MEAS_PERIOD 500 /*[ms]*/
#define DEF_PERIOD 500

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool os_timer_exec(os_timer_t * timer);
static uint32_t os_timer_time_remaining(os_timer_t * timer);

/**********************
 *  STATIC VARIABLES
 **********************/
static bool os_timer_run = false;
static uint8_t idle_last = 0;
static bool timer_deleted;
static bool timer_created;

os_ll_t _os_timer_ll;
os_timer_t* _os_timer_act;
/**********************
 *      MACROS
 **********************/


/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Init the os_timer module
 */
void _os_timer_core_init(void)
{
    _os_ll_init(&_os_timer_ll, sizeof(os_timer_t));

    /*Initially enable the os_timer handling*/
    os_timer_enable(true);
}

/**
 * Call it periodically to handle os_timers.
 * @return the time after which it must be called again
 */
uint32_t os_timer_handler(void)
{
    /*Avoid concurrent running of the timer handler*/
    static bool already_running = false;
    if(already_running) {
        return 1;
    }
    already_running = true;

    if(os_timer_run == false) {
        already_running = false; /*Release mutex*/
        return 1;
    }

    static uint32_t idle_period_start = 0;
    static uint32_t busy_time         = 0;

    uint32_t handler_start = os_tick_get();

    if(handler_start == 0) {
        static uint32_t run_cnt = 0;
        run_cnt++;
        if(run_cnt > 100) {
            run_cnt = 0;
        }
    }

    /*Run all timer from the list*/
    os_timer_t * next;
    do {
        timer_deleted             = false;
        timer_created             = false;
        _os_timer_act = _os_ll_get_head(&_os_timer_ll);
        while(_os_timer_act) {
            /*The timer might be deleted if it runs only once ('repeat_count = 1')
             *So get next element until the current is surely valid*/
            next = _os_ll_get_next(&_os_timer_ll, _os_timer_act);

            if(os_timer_exec(_os_timer_act)) {
                /*If a timer was created or deleted then this or the next item might be corrupted*/
                if(timer_created || timer_deleted) {
                    break;
                }
            }

            _os_timer_act = next; /*Load the next timer*/
        }
    } while(_os_timer_act);

    uint32_t time_till_next = OS_NO_TIMER_READY;
    next = _os_ll_get_head(&_os_timer_ll);
    while(next) {
        if(!next->paused) {
            uint32_t delay = os_timer_time_remaining(next);
            if(delay < time_till_next)
                time_till_next = delay;
        }

        next = _os_ll_get_next(&_os_timer_ll, next); /*Find the next timer*/
    }

    busy_time += os_tick_elaps(handler_start);
    uint32_t idle_period_time = os_tick_elaps(idle_period_start);
    if(idle_period_time >= IDLE_MEAS_PERIOD) {
        idle_last         = (busy_time * 100) / idle_period_time;  /*Calculate the busy percentage*/
        idle_last         = idle_last > 100 ? 0 : 100 - idle_last; /*But we need idle time*/
        busy_time         = 0;
        idle_period_start = os_tick_get();
    }

    already_running = false; /*Release the mutex*/
    return time_till_next;
}

/**
 * Create an "empty" timer. It needs to initialized with at least
 * `os_timer_set_cb` and `os_timer_set_period`
 * @return pointer to the created timer
 */
os_timer_t * os_timer_create_basic(void)
{
    return os_timer_create(NULL, DEF_PERIOD, NULL);
}

/**
 * Create a new os_timer
 * @param timer_xcb a callback which is the timer itself. It will be called periodically.
 *                 (the 'x' in the argument name indicates that it's not a fully generic function because it not follows
 *                  the `func_name(object, callback, ...)` convention)
 * @param period call period in ms unit
 * @param user_data custom parameter
 * @return pointer to the new timer
 */
os_timer_t * os_timer_create(os_timer_cb_t timer_xcb, uint32_t period, void * user_data)
{
	os_timer_t * new_timer = NULL;

    new_timer = _os_ll_ins_head(&_os_timer_ll);
    if(new_timer == NULL) return NULL;

    new_timer->period = period;
    new_timer->timer_cb = timer_xcb;
    new_timer->repeat_count = -1;
    new_timer->paused = 0;
    new_timer->last_run = os_tick_get();
    new_timer->user_data = user_data;

    timer_created = true;

    return new_timer;
}

/**
 * Set the callback the timer (the function to call periodically)
 * @param timer pointer to a timer
 * @param timer_cb the function to call periodically
 */
void os_timer_set_cb(os_timer_t * timer, os_timer_cb_t timer_cb)
{
    timer->timer_cb = timer_cb;
}

/**
 * Delete a os_timer
 * @param timer pointer to timer created by timer
 */
void os_timer_del(os_timer_t * timer)
{
    _os_ll_remove(&_os_timer_ll, timer);
    timer_deleted = true;

    os_mem_free(timer);
}

/**
 * Pause/resume a timer.
 * @param timer pointer to an os_timer
 */
void os_timer_pause(os_timer_t * timer)
{
    timer->paused = true;
}

void os_timer_resume(os_timer_t * timer)
{
    timer->paused = false;
}

/**
 * Set new period for a os_timer
 * @param timer pointer to a os_timer
 * @param period the new period
 */
void os_timer_set_period(os_timer_t * timer, uint32_t period)
{
    timer->period = period;
}

/**
 * Make a os_timer ready. It will not wait its period.
 * @param timer pointer to a os_timer.
 */
void os_timer_ready(os_timer_t * timer)
{
    timer->last_run = os_tick_get() - timer->period - 1;
}

/**
 * Set the number of times a timer will repeat.
 * @param timer pointer to a os_timer.
 * @param repeat_count -1 : infinity;  0 : stop ;  n >0: residual times
 */
void os_timer_set_repeat_count(os_timer_t * timer, int32_t repeat_count)
{
    timer->repeat_count = repeat_count;
}

/**
 * Reset a os_timer.
 * It will be called the previously set period milliseconds later.
 * @param timer pointer to a os_timer.
 */
void os_timer_reset(os_timer_t * timer)
{
    timer->last_run = os_tick_get();
}

/**
 * Enable or disable the whole os_timer handling
 * @param en true: os_timer handling is running, false: os_timer handling is suspended
 */
void os_timer_enable(bool en)
{
	os_timer_run = en;
}

/**
 * Get idle percentage
 * @return the os_timer idle in percentage
 */
uint8_t os_timer_get_idle(void)
{
    return idle_last;
}

/**
 * Iterate through the timers
 * @param timer NULL to start iteration or the previous return value to get the next timer
 * @return the next timer or NULL if there is no more timer
 */
os_timer_t * os_timer_get_next(os_timer_t * timer)
{
    if(timer == NULL) return _os_ll_get_head(&_os_timer_ll);
    else return _os_ll_get_next(&_os_timer_ll, timer);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Execute timer if its remaining time is zero
 * @param timer pointer to os_timer
 * @return true: execute, false: not executed
 */
static bool os_timer_exec(os_timer_t * timer)
{
    if(timer->paused) return false;

    bool exec = false;
    if(os_timer_time_remaining(timer) == 0) {
        /* Decrement the repeat count before executing the timer_cb.
         * If any timer is deleted `if(timer->repeat_count == 0)` is not executed below
         * but at least the repeat count is zero and the timer can be deleted in the next round*/
        int32_t original_repeat_count = timer->repeat_count;
        if(timer->repeat_count > 0) timer->repeat_count--;
        timer->last_run = os_tick_get();
        if(timer->timer_cb && original_repeat_count != 0) timer->timer_cb(timer);
        exec = true;
    }

    if(timer_deleted == false) { /*The timer might be deleted by itself as well*/
        if(timer->repeat_count == 0) { /*The repeat count is over, delete the timer*/
        	os_timer_del(timer);
        }
    }

    return exec;
}

/**
 * Find out how much time remains before a timer must be run.
 * @param timer pointer to os_timer
 * @return the time remaining, or 0 if it needs to be run again
 */
static uint32_t os_timer_time_remaining(os_timer_t * timer)
{
    /*Check if at least 'period' time elapsed*/
    uint32_t elp = os_tick_elaps(timer->last_run);
    if(elp >= timer->period)
        return 0;
    return timer->period - elp;
}
