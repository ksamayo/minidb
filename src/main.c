#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *trim(char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length - 1])) {
        line[--length] = '\0';
    }
    return line;
}

int main(void) {
    char *line = NULL;
    size_t line_capacity = 0;
    int interactive = isatty(STDIN_FILENO);
    for (;;) {
        if (interactive) {
            fputs("minidb> ", stdout);
            fflush(stdout);
        }
        if (getline(&line, &line_capacity, stdin) < 0) {
            break;
        }
        char *command = trim(line);
        if (!*command) {
            continue;
        }
        if (!strcmp(command, ".exit")) {
            break;
        }
        fprintf(stderr, "Error: unknown command '%s'\n", command);
    }
    free(line);
    if (ferror(stdin)) {
        perror("stdin");
        return 1;
    }
    return 0;
}
