/**
 * @file os_hal_tick.h
 * Provide access to the system tick with 1 millisecond resolution
 */

#ifndef OS_HAL_TICK_H
#define OS_HAL_TICK_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>
#include <stdbool.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * You have to call this function periodically
 * @param tick_period the call period of this function in milliseconds
 */
void  os_tick_inc(uint32_t tick_period);


/**
 * Get the elapsed milliseconds since start up
 * @return the elapsed milliseconds
 */
uint32_t os_tick_get(void);

/**
 * Get the elapsed milliseconds since a previous time stamp
 * @param prev_tick a previous time stamp (return value of lv_tick_get() )
 * @return the elapsed milliseconds since 'prev_tick'
 */
uint32_t os_tick_elaps(uint32_t prev_tick);

/**********************
 *      MACROS
 **********************/


#endif /*LV_HAL_TICK_H*/
