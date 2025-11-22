#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../src/string_helpers.h"

int main(void)
{
    char buf[8];
    char dst[32];
    size_t pos = 0;
    int r;

    /* Test itoa_new normal conversion */
    itoa_new(0, buf, sizeof(buf));
    assert(strcmp(buf, "0") == 0);
    itoa_new(12345, buf, sizeof(buf));
    assert(strcmp(buf, "12345") == 0);

    /* Test append_fmt basic */
    dst[0] = '\0'; pos = 0;
    r = append_fmt(dst, sizeof(dst), &pos, "Hello %s", "X");
    assert(r > 0);
    assert(strcmp(dst, "Hello X") == 0);

    /* Test truncation behavior */
    dst[0] = '\0'; pos = 0;
    r = append_fmt(dst, 6, &pos, "1234567890"); /* will be truncated */
    assert(r == -1);
    /* Ensure dst is NUL-terminated (vsnprintf writes up to size-1) */
    assert(dst[5] == '\0');

    printf("string_helpers tests passed\n");
    return 0;
}
