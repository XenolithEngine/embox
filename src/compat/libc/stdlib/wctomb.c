/**
 * @file
 * @brief wctomb function.
 *
 * @see stdlib.h
 *
 * @date 07.12.18
 * @author Chubukov Filipp
 */

#include <stdlib.h>

int wctomb(char *out, wchar_t in) {
	if (out == NULL) {
		return 0;
	}

	*out = (char)in;

	return sizeof(char);
}
