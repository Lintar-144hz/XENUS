#include <stdio.h>
#include <string.h>
#include <xenus/ucore.h>

int main(void)
{
    char key[128] = {0};

    if (xenus_auth_init() != 0) {
        puts("ERROR: XENUS auth initialization failed.");
        return 1;
    }

    if (!fgets(key, sizeof(key), stdin)) {
        puts("ERROR: No recovery key received.");
        return 1;
    }

    key[strcspn(key, "\r\n")] = '\0';

    int result = xenus_auth_recover(key);

    volatile char *p = key;
    for (size_t i = 0; i < sizeof(key); i++)
        p[i] = 0;

    if (result != 0) {
        puts("ERROR: Recovery key invalid or recovery failed.");
        return 1;
    }

    puts("SUCCESS: Password protection removed.");
    puts("Start XENUS and configure a new password.");
    return 0;
}
