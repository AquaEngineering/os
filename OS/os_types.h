/**
 * @file os_types.h
 *
 */

#ifndef OS_TYPES_H
#define OS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * LVGL error codes.
 */
enum {
    OS_RES_INV = 0, /*Typically indicates that the object is deleted (become invalid) in the action
                      function or an operation was failed*/
    OS_RES_OK,      /*The object is valid (no deleted) after the action*/
};
typedef uint8_t os_res_t;
typedef uintptr_t os_uintptr_t;


/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/
#define OS_UNUSED(x) ((void)x)


#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*OS_TYPES_H*/
