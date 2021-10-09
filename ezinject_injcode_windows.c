/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
INLINE void inj_thread_stop(struct injcode_ctx *ctx, int signal){
	UNUSED(ctx);
	UNUSED(signal);
	asm volatile("int $3\n");
	while(1);
}

INLINE void *inj_dlopen(struct injcode_ctx *ctx, const char *filename, unsigned flags){
	UNUSED(flags);
	return ctx->libdl.dlopen(filename);
}

INLINE intptr_t inj_thread_wait(
	struct injcode_ctx *ctx,
	intptr_t *pExitStatus
){
	struct injcode_bearing *br = ctx->br;
	struct thread_api *api = &ctx->libthread;

	if(br->hEvent == INVALID_HANDLE_VALUE){
		return -1;
	}

	DWORD result = api->WaitForSingleObject(br->hEvent, INFINITE);
	api->CloseHandle(br->hEvent);
	
	if(result != WAIT_OBJECT_0){
		return -1;
	}

	result = api->WaitForSingleObject(br->hThread, INFINITE);
	if(result != WAIT_OBJECT_0){
		return -1;
	}

	DWORD exitStatus;
	if(api->GetExitCodeThread(br->hThread, &exitStatus) == FALSE){
		return -1;
	}
	
	*pExitStatus = exitStatus;
	return 0;
}

INLINE void *_inj_get_kernel32(struct injcode_bearing *br){
	// kernel32.dll
	char *libdl_name = STR_DATA(BR_STRTBL(br));

	// kernel32.dll length in utf16
	const int NAME_LENGTH = 24;
	
	/** poor man's UTF16 conversion of "kernel32.dll" **/
	char buf[NAME_LENGTH];
	for(int i=0; i<NAME_LENGTH; i+=2){
		buf[i+0] = libdl_name[i/2];
		buf[i+1] = '\0';
	}

	UNICODE_STRING kernel32Name = {
		.Length = NAME_LENGTH,
		.MaximumLength = NAME_LENGTH,
		.Buffer = (PWSTR)&buf[0]
	};
	PVOID baseAddr = NULL;
	br->libc_dlopen(
		NULL, // SearchPath
		NULL, // DllCharacteristics
		&kernel32Name,
		&baseAddr
	);

	return baseAddr;
}

INLINE intptr_t inj_api_init(struct injcode_ctx *ctx){
	intptr_t result = 0;
	result += fetch_sym(ctx, ctx->h_libthread, (void **)&ctx->libthread.CreateEventA);
	result += fetch_sym(ctx, ctx->h_libthread, (void **)&ctx->libthread.CreateThread);
	result += fetch_sym(ctx, ctx->h_libthread, (void **)&ctx->libthread.CloseHandle);
	result += fetch_sym(ctx, ctx->h_libthread, (void **)&ctx->libthread.WaitForSingleObject);
	result += fetch_sym(ctx, ctx->h_libthread, (void **)&ctx->libthread.GetExitCodeThread);
	if(result != 0){
		return -1;
	}
	return 0;
}

INLINE void *inj_get_libdl(struct injcode_ctx *ctx){
	return _inj_get_kernel32(ctx->br);
}

INLINE intptr_t inj_load_prepare(struct injcode_ctx *ctx){
	struct injcode_bearing *br = ctx->br;

	br->hEvent = ctx->libthread.CreateEventA(NULL, TRUE, FALSE, NULL);
	if(br->hEvent == INVALID_HANDLE_VALUE){
		return -1;
	}
	return 0;
}
