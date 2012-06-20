#ifndef __MURPHY_DEBUG_H__
#define __MURPHY_DEBUG_H__

#include <stdio.h>

#include <murphy/common/macros.h>

MRP_CDECL_BEGIN

/** Macro to generate a debug site string. */
#define MRP_DEBUG_SITE(file, line, func)	\
    "__DEBUG_SITE_"file":"MRP_STRINGIFY(line)

/** Log a debug message if the invoking debug site is enabled. */
#define mrp_debug(fmt, args...)	do {					\
	static const char *__site =					\
	    MRP_DEBUG_SITE(__FILE__, __LINE__, __FUNCTION__);		\
	static int __site_stamp = -1;					\
	static int __site_enabled;					\
									\
	if (MRP_UNLIKELY(__site_stamp != mrp_debug_stamp)) {		\
	    __site_enabled = mrp_debug_check(__FUNCTION__,		\
					     __FILE__, __LINE__);	\
	    __site_stamp   = mrp_debug_stamp;				\
	}								\
    									\
	if (MRP_UNLIKELY(__site_enabled))				\
	    mrp_debug_msg(__site, __LOC__, fmt, ## args);		\
    } while (0)

/** Global debug configuration stamp, exported for minimum-overhead checking. */
extern int mrp_debug_stamp;

/** Enable/disable debug messages globally. */
int mrp_debug_enable(int enabled);

/** Reset all debug configuration to the defaults. */
void mrp_debug_reset(void);

/** Apply the debug configuration settings given in cmd. */
int mrp_debug_set_config(const char *cmd);

/** Dump the active debug configuration. */
int mrp_debug_dump_config(FILE *fp);

/** Low-level log wrapper for debug messages. */
void mrp_debug_msg(const char *site, const char *file, int line,
		   const char *func, const char *format, ...);

/** Check if the given debug site is enabled. */
int mrp_debug_check(const char *func, const char *file, int line);

MRP_CDECL_END

#endif /* __MURPHY_DEBUG_H__ */