#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include "util/pipe.h"

void *bmalloc(size_t n) { return malloc(n); }
void bfree(void *p) { free(p); }
char **os_process_args_get_argv(const os_process_args_t *args) { (void)args; return NULL; }
static int failures;
static void ok(int pass, const char *why) { printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void)
{
	alarm(5); // a blocking stderr read fails deterministically rather than hanging CI
	uint8_t data[128] = {0};
	os_process_pipe_t *pp = os_process_pipe_create("sleep 0.2; printf diagnostic >&2; sleep 0.2", "r");
	ok(pp != NULL, "helper process created");
	double start = seconds();
	ok(os_process_pipe_read_err(pp, data, sizeof(data)) == 0 && seconds() - start < 0.1,
	   "empty stderr returns immediately while helper is alive");
	struct timespec delay = {0, 250000000}; nanosleep(&delay, NULL);
	start = seconds();
	size_t n = os_process_pipe_read_err(pp, data, 4);
	ok(n == 4 && memcmp(data, "diag", 4) == 0 && seconds() - start < 0.1, "short stderr reads available bytes without waiting for requested capacity");
	n = os_process_pipe_read_err(pp, data, sizeof(data));
	ok(n == 6 && memcmp(data, "nostic", 6) == 0, "descriptor reads preserve remaining diagnostic bytes");
	ok(os_process_pipe_read_err(pp, data, sizeof(data)) == 0, "drained stderr returns immediately");
	ok(os_process_pipe_read_err(pp, data, 0) == 0, "zero length read is harmless");
	ok(os_process_pipe_destroy(pp) == 0, "helper finishes cleanly");
	ok(os_process_pipe_read_err(NULL, data, sizeof(data)) == 0, "null pipe is harmless");
	return failures ? 1 : 0;
}
