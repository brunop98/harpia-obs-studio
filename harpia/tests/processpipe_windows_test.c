#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include "../../libobs/util/pipe-windows.c"
DWORD fake_available, fake_chunk = MAXDWORD, fake_error = 109, fake_first_request;
int fake_reads, fake_writes, fake_failure_at, fake_zero_at, fake_logs;
void *bmalloc(size_t n) { return malloc(n); }
void bfree(void *p) { free(p); }
void blog(int level, const char *format, ...) { (void)level;(void)format;++fake_logs; }
static int failures;
static void ok(int pass, const char *why) { printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
int main(void)
{
	os_process_pipe_t pp = {0}; pp.handle = pp.handle_err = (HANDLE)1;
	uint8_t *data = malloc(32);
	ok(os_process_pipe_read_err(&pp, data, 32) == 0 && fake_reads == 0, "Windows never calls ReadFile on empty stderr");
	fake_available = 5;
	ok(os_process_pipe_read_err(&pp, data, 32) == 5 && fake_reads == 1, "Windows caps stderr read at available bytes");
	ok(os_process_pipe_read_err(&pp, data, 32) == 0 && fake_reads == 1, "Windows drained stderr remains nonblocking");
	fake_chunk = 3;
	ok(os_process_pipe_write(&pp, data, 11) == 11 && fake_writes == 4, "Windows completes partial writes");
	fake_writes = 0; fake_failure_at = 3;
	ok(os_process_pipe_write(&pp, data, 11) == 6 && fake_logs == 1, "Windows reports partial progress and logs GetLastError on failure");
	fake_writes = 0; fake_failure_at = 0; fake_zero_at = 2;
	ok(os_process_pipe_write(&pp, data, 11) == 3 && fake_writes == 2, "Windows exits on zero progress");
	fake_writes = 0; fake_zero_at = 1;
	ok(os_process_pipe_write(&pp, data, (size_t)MAXDWORD + 10) == 0 && fake_first_request == MAXDWORD,
	   "requests larger than DWORD are capped before conversion");
	pp.read_pipe = true; fake_writes = 0;
	ok(os_process_pipe_write(&pp, data, 5) == 0 && fake_writes == 0, "read-only pipes cannot be written");
	free(data);
	return failures ? 1 : 0;
}
