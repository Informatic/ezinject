#include <sys/types.h>
#include <linux/limits.h>

#define MAPPINGSIZE 4096
#define INJ_PATH_MAX 128

struct injcode_user {
	// any user data here
};

struct injcode_bearing
{
	void *mapped_mem;
	void *(*libc_dlopen_mode)(const char *name, int mode);
	long (*libc_syscall)(long number, ...);
	struct injcode_user user;
	int argc;
	int dyn_size;
	char *argv[];
};

extern __attribute__((naked, noreturn)) void injected_code();
extern __attribute__((naked)) void injected_code_end(void);