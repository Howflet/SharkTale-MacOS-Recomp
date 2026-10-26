/*=============================================================================
	sampler.c: Debug watchdog. SHARK_SAMPLE=<seconds> prints the main
	thread's last dispatched guest address and the code addresses found on
	its guest stack (pipe through tools/symbolize.py).
=============================================================================*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "runtime.h"

static Ctx* Watched;

static void* sampler(void* arg)
{
	int secs = (int)(intptr_t)arg;
	for (;;)
	{
		sleep((unsigned)secs);
		Ctx* c = Watched;
		u32 esp = R4(REG_ESP);
		fprintf(stderr, "[sample] eip=%08x esp=%08x ebp=%08x stack:", R4(REG_EIP), esp, R4(REG_EBP));
		int shown = 0;
		for (u32 i = 0; i < 4096 && shown < 24; i++)
		{
			u32 v = LD4(esp + 4 * i);
			if (v >= 0x10100000u && v < 0x12000000u && rt_module_at(v))
			{
				fprintf(stderr, " %08x", v);
				shown++;
			}
		}
		fprintf(stderr, "\n");
	}
	return NULL;
}

void rt_start_sampler(Ctx* main_ctx)
{
	const char* s = getenv("SHARK_SAMPLE");
	if (!s)
		return;
	Watched = main_ctx;
	pthread_t t;
	pthread_create(&t, NULL, sampler, (void*)(intptr_t)atoi(s));
	pthread_detach(t);
}
