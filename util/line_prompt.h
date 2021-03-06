#ifndef IBCHAT_CLIENT_LINE_PROMPT_H
#define IBCHAT_CLIENT_LINE_PROMPT_H

#include <stdint.h>

/* prompts an input line from the console */
char* line_prompt(const char* prompt, const char* confprompt, int hide);

/* returns ULLONG_MAX if error occurred */
uint64_t num_prompt(char *prompt, uint64_t min, uint64_t max);
/* acts same as above, but does not attempt to try again if invalid
 * returns ULLONG_MAX - 1 if invalid but non-crashing */
uint64_t num_prompt_no_retry(char *prompt, uint64_t min, uint64_t max);

/* returns -1 on error, 0 on no, 1 on yes */
int yn_prompt();

#endif

