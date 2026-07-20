/* chopin - from-scratch C11 reimplementation of GNU cp(1).
 * Sprint 00 skeleton: version/usage only. The option surface lands in
 * sprint 01, copying in sprint 02. cpn is the same binary; argv[0]
 * changes nothing but the diagnostic program name. */

#include <stdio.h>
#include <string.h>

#include "config.h"

static const char *program_name = "chopin";

static void set_program_name(const char *argv0);
static void set_program_name(const char *argv0)
{
    const char *slash;

    if (argv0 == NULL || argv0[0] == '\0')
        return;
    slash = strrchr(argv0, '/');
    program_name = slash != NULL ? slash + 1 : argv0;
}

int main(int argc, char **argv)
{
    set_program_name(argc > 0 ? argv[0] : NULL);

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", program_name, CHOPIN_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [OPTION]... SOURCE... DEST\n", program_name);
        printf("Copy SOURCE to DEST. Option surface arrives in sprint 01.\n");
        return 0;
    }

    fprintf(stderr, "%s: copying is not implemented yet (sprint 00 skeleton)\n",
            program_name);
    return 1;
}
