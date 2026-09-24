/**
 * @file
 * @brief MB_CUR_MAX for the native multibyte functions: a byte per character.
 */

#include <stdlib.h>

size_t __ctype_get_mb_cur_max(void) {
	return 1;
}
