#include "ezinject.h"

#define SOCKNAME "/dev/shm/%08x"
#define SUN_PATH_ABSTRACT(ptr) ((char *)(ptr) + offsetof(struct sockaddr_un, sun_path) + 1)

static char *asun_build_path(key_t key){
	struct sockaddr_un dummy;
	int max_length = sizeof(dummy.sun_path) - 1;
	char *buf = calloc(max_length, 1);
	snprintf(buf, max_length, SOCKNAME, key);
	return buf;
}

static int asun_path_cpy(char *dest, const char *src){
	struct sockaddr_un dummy;
	int in_length = strlen(src) + 1;
	int max_length = sizeof(dummy.sun_path) - 1;

	int length = in_length;
	if(in_length > max_length){
		length = max_length;
	}

	strncpy(dest, src, length);
	return length;
}

struct shmat_payload {
	struct sockaddr_un sock;
	key_t key;

	char nothing;
	struct iovec nothing_ptr;
	struct msghdr msghdr;

	struct {
		struct cmsghdr h;
		int fd;
	} cmsg;
};

uintptr_t prepare_socket_payload(ez_addr payload){
	struct shmat_payload *pl = (struct shmat_payload *)payload.local;
	// prepare message data (dummy, single char)
	pl->nothing_ptr.iov_len = 1;
	pl->nothing_ptr.iov_base = (void *)EZ_REMOTE(payload, &pl->nothing_ptr);
	
	// attach message data to header
	pl->msghdr.msg_name = NULL;
	pl->msghdr.msg_namelen = 0;
    pl->msghdr.msg_iov = (void *)EZ_REMOTE(payload, &pl->nothing_ptr);
    pl->msghdr.msg_iovlen = 1;
    pl->msghdr.msg_flags = 0;

	// prepare control header
	struct cmsghdr *chdr = &pl->cmsg;
    chdr->cmsg_len = pl->msghdr.msg_controllen;
    chdr->cmsg_level = SOL_SOCKET;
    chdr->cmsg_type = SCM_RIGHTS;
	
	// set initial control data
	void *cdata = (void *)(UPTR(chdr) + sizeof(struct cmsghdr));
	*(int *)cdata = -1; // set initial fd value
	
	// attach control data to message
    pl->msghdr.msg_control = (void *)EZ_REMOTE(payload, &pl->cmsg);
    pl->msghdr.msg_controllen = sizeof(pl->cmsg);

	// return remote control data address
	return UPTR(pl->msghdr.msg_control) + sizeof(struct cmsghdr);
}

uintptr_t remote_shmat_android(
	struct ezinj_ctx *ctx,
	int shm_id,
	void *shmaddr,
	int shmflg,
	size_t map_size
){
	uintptr_t result = (uintptr_t)MAP_FAILED;

	key_t shm_key = (key_t)ctx->target;
	uintptr_t remote_shm_ptr = 0;

	struct shmat_payload payload;
	memset(&payload, 0x00, sizeof(payload));
	
	/* setup socket parameters */
	payload.sock.sun_family = AF_UNIX;
	char *socketPath = asun_build_path(shm_key);
	DBG("socket path: %s", socketPath);
	asun_path_cpy(SUN_PATH_ABSTRACT(&payload.sock), socketPath);
	free(socketPath);
	payload.key = shm_key;

	regs_t orig_regs;
	regs_t regs;
	ptrace(PTRACE_GETREGS, ctx->target, 0, &regs);
	memcpy(&orig_regs, &regs, sizeof(regs));

	uintptr_t remote_stack = REG(regs, REG_SP);
	size_t payload_size = (size_t) WORDALIGN(sizeof(payload));
	uint8_t *backup = calloc(payload_size, 1);
	do {
		INFO("backing up stack...");
		if(remote_read(ctx, backup, remote_stack, payload_size) != payload_size){
			ERR("stack backup failed");
			break;
		}

		uintptr_t r_payload = remote_stack - payload_size;
		DBG("remote stack: %p", (void *)remote_stack);
		DBG("remote payload: %p", (void *)r_payload);

		ez_addr payload_addr = {
			.local = &payload,
			.remote = r_payload
		};
		
		// socket message control data
		uintptr_t r_sock_cdata = prepare_socket_payload(payload_addr);
		DBG("remote cdata: %p", (void *)r_sock_cdata);

		remote_write(ctx, r_payload, &payload, payload_size);

		REG(regs, REG_SP) = r_payload;
		ptrace(PTRACE_SETREGS, ctx->target, 0, &regs);

		int remote_sock_fd = (int)RSCALL3(ctx, __NR_socket, AF_UNIX, SOCK_STREAM, 0);
		if(remote_sock_fd < 0){
			ERR("cannot create UNIX socket");
			break;
		}
		DBG("remote socket(): %d", remote_sock_fd);

		do {
			int ret = -1;
			uintptr_t r_sockaddr = r_payload + offsetof(struct shmat_payload, sock);
			ret = (int)RSCALL3(ctx, __NR_connect, remote_sock_fd, r_sockaddr, sizeof(payload.sock));
			DBG("remote connect(): %d", ret);
			if(ret != 0){
				ERR("cannot connect to UNIX socket");
				break;
			}

			uintptr_t r_key = r_payload + offsetof(struct shmat_payload, key);
			ret = (int) RSCALL4(ctx, __NR_send, remote_sock_fd, r_key, sizeof(payload.key), 0);
			DBG("remote send(): %d", ret);
			if(ret != sizeof(payload.key)){
				ERR("send() failed");
				break;
			}

			char cmd[256];
			sprintf(cmd, "ls -als /proc/%u/fd", ctx->target);
			system(cmd);

			uintptr_t r_msghdr = r_payload + offsetof(struct shmat_payload, msghdr);
			ret = (int)RSCALL3(ctx, __NR_recvmsg, remote_sock_fd, r_msghdr, 0);
			DBG("remote recvmsg(): %d", ret);
			if(ret < 0){
				ERR("recvmsg() failed");
				break;
			}

			uintptr_t l_remote_fd;
			// read remote fd
			remote_read(ctx, &l_remote_fd, r_sock_cdata, sizeof(uintptr_t));

			sprintf(cmd, "ls -als /proc/%u/fd", ctx->target);
			system(cmd);

			int remote_fd = (int)l_remote_fd;
			DBG("remote fd: %d", remote_fd);
			if(remote_fd < 0){
				ERR("invalid ashmem fd");
				break;
			}

			uintptr_t r_mem = RSCALL6(ctx, __NR_mmap2,
				0, map_size,
				PROT_READ|PROT_WRITE|PROT_EXEC,
				MAP_SHARED,
				remote_fd, 0
			);
			DBG("remote mmap: %p", (void *)r_mem);
			if(r_mem == (uintptr_t)MAP_FAILED){
				ERR("mmap failed");
				break;
			}

			result = r_mem;
		} while(0);
		RSCALL1(ctx, __NR_close, remote_sock_fd);
	} while(0);

	INFO("restoring stack");
	remote_write(ctx, remote_stack, backup, payload_size);
	ptrace(PTRACE_SETREGS, ctx->target, 0, &orig_regs);
	free(backup);

	return result;
}