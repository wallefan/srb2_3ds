#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <3ds.h>
#include <citro3d.h>

#define __BYTEBOOL__
#define boolean bool

#include "../doomtype.h"
#include "r_queue.h"
#include "nds_utils.h"

typedef struct {
	queuePacket *mem;
	size_t num;
	volatile size_t offw;
	volatile size_t offr;
} RingBuffer;

const size_t ringBufferSize = 32 * 1024 * sizeof(queuePacket);

static volatile RingBuffer ringBuffer;
static Handle eventNotEmpty;
static Handle eventEmpty;
static Handle eventAlmostEmpty;
static Handle eventAllocOk;
static bool want_eventNotEmpty, want_eventEmpty,
			want_eventAlmostEmpty, want_eventAllocOk;
static size_t allocCount;

void queueDump();

static bool initRingBuffer()
{
	ringBuffer.mem = (queuePacket *) malloc(ringBufferSize);
	ringBuffer.offw = 0;
	ringBuffer.offr = 0;
	ringBuffer.num = ringBufferSize / sizeof(queuePacket);

	__dmb();

	return ringBuffer.mem != NULL;
}

bool queueInit()
{
	if (svcCreateEvent(&eventEmpty, RESET_ONESHOT) != 0)
		goto fail;
	if (svcCreateEvent(&eventNotEmpty, RESET_ONESHOT) != 0)
		goto fail;
	if (svcCreateEvent(&eventAlmostEmpty, RESET_ONESHOT) != 0)
		goto fail;
	if (svcCreateEvent(&eventAllocOk, RESET_ONESHOT) != 0)
		goto fail;

	return initRingBuffer();

fail:

	NDS3D_driverPanic("queueInit failed!");
	return false;
}

static size_t queueGetFreeSpace()
{
	size_t diff;
	size_t offw = ringBuffer.offw;
	size_t offr = ringBuffer.offr;

	if (offw < offr)
	{
		diff = offr - offw;
	}
	else
	{
		diff = ringBuffer.num - offw;
		diff += offr;
	}

	return diff;
}

bool queueCheckDiff(size_t maxDiff)
{
	size_t diff;
	size_t offw = ringBuffer.offw;
	size_t offr = ringBuffer.offr;

	if (offw < offr)
	{
		diff = ringBuffer.num - offr;
		diff += offw;
	}
	else
	{
		diff = offw - offr;
	}

	return maxDiff >= diff;
}

static void queueWakeupSleepers()
{
	if (want_eventNotEmpty)
	{
		if (ringBuffer.offw != ringBuffer.offr)
		{
			want_eventNotEmpty = false;
			svcSignalEvent(eventNotEmpty);
		}
	}

	if (want_eventEmpty)
	{
		if (ringBuffer.offw == ringBuffer.offr)
		{
			want_eventEmpty = false;
			svcSignalEvent(eventEmpty);
		}
	}

	if (want_eventAlmostEmpty)
	{
		if (queueCheckDiff(10))
		{
			want_eventAlmostEmpty = false;
			svcSignalEvent(eventAlmostEmpty);
		}
	}

	if (want_eventAllocOk)
	{
		if (queueGetFreeSpace() >= 100u)
		{
			want_eventAllocOk = false;
			svcSignalEvent(eventAllocOk);
		}
	}
}

bool queueWaitForEvent(int mode, s64 nanoseconds)
{
	Result res;
	Handle *event;

	switch (mode)
	{
		case QUEUE_EVENT_NOT_EMPTY:
			want_eventNotEmpty = true;
			event = &eventNotEmpty;
			break;
		case QUEUE_EVENT_EMPTY:
			want_eventEmpty = true;
			event = &eventEmpty;
			break;
		case QUEUE_EVENT_ALMOST_EMPTY:
			want_eventAlmostEmpty = true;
			event = &eventAlmostEmpty;
			break;
		case QUEUE_EVENT_ALLOC_OK:
			want_eventAllocOk = true,
			event = &eventAllocOk;
			break;
		default:
			event = NULL;
	}

	/* First, wake up any other sleeping thread */
	queueWakeupSleepers();

	/* Before we start waiting, make sure the flag above is set */
	__dmb();

	res = svcWaitSynchronization(*event, nanoseconds);

	return res == 0;
}

queuePacket *queueAllocPacket()
{
	size_t curOffsetW = ringBuffer.offw;
	size_t curOffsetR = ringBuffer.offr;

	if (curOffsetW < curOffsetR && curOffsetW + 1 >= curOffsetR)
	{
		return NULL;
/*
#ifdef DIAGNOSTIC
		if (curOffsetW < curOffsetR && curOffsetW + 10 >= curOffsetR)
		{
			queueDump();
			NDS3D_driverPanic("Queue overflow! offw 0x%X, offr 0x%X",
				curOffsetW, curOffsetR);
		}
#endif
*/
	}

	allocCount++;

	return ringBuffer.mem + curOffsetW;
}

void queueEnqueuePacket(queuePacket *packet)
{
	size_t curOffset = ringBuffer.offw;
	size_t packet_ = (size_t) packet;

#ifdef DIAGNOSTIC
	if (packet_ != (size_t) ringBuffer.mem + curOffset * sizeof(queuePacket))
		NDS3D_driverPanic("Bad packet ptr!");
#endif
	//N3DS_Print("sent packet %i\n", packet->type);

	__dmb();
	
	ringBuffer.offw = (curOffset + 1) % ringBuffer.num;

	__dmb();

	if (want_eventNotEmpty)
	{
		//printf(" sigNE ");
		want_eventNotEmpty = false;
		svcSignalEvent(eventNotEmpty);
	}

	//N3DS_Print("enqueued w %x, r %x\n", ringBuffer.offw, ringBuffer.offr);
}

bool queuePollForDequeue()
{
	return ringBuffer.offw != ringBuffer.offr;
}

queuePacket *queueDequeuePacket()
{
	queuePacket *packet;
	size_t curOffset = ringBuffer.offr;

#ifdef DIAGNOSTIC
	if (curOffset == ringBuffer.offw)
		NDS3D_driverPanic("queueDequeuePacket: Queue empty!");
#endif

	packet = ringBuffer.mem + curOffset;
	
	ringBuffer.offr = (curOffset + 1) % ringBuffer.num;

	__dmb();

	//N3DS_Print("dequeued w %x, r %x\n", ringBuffer.offw, ringBuffer.offr);

	if (want_eventEmpty)
	{
		if (ringBuffer.offw == ringBuffer.offr)
		{
			want_eventEmpty = false;
			svcSignalEvent(eventEmpty);
		}
	}
	else if (want_eventAlmostEmpty)
	{
		if (queueCheckDiff(10))
		{
			want_eventAlmostEmpty = false;
			svcSignalEvent(eventAlmostEmpty);
		}
	}
	else if (want_eventAllocOk)
	{
		if (queueGetFreeSpace() >= 100u)
		{
			want_eventAllocOk = false;
			svcSignalEvent(eventAllocOk);
		}
	}

	return packet;
}

float queueGetUsage()
{
	size_t diff;
	size_t offw = ringBuffer.offw;
	size_t offr = ringBuffer.offr;

	if (offw < offr)
	{
		diff = ringBuffer.num - offr;
		diff += offw;
	}
	else
	{
		diff = offw - offr;
	}

	return (float) diff / (float) ringBuffer.num;
}

size_t queueGetAllocCount()
{
	return allocCount;
}

void queueResetAllocCount()
{
	allocCount = 0;
}

void queueDump()
{
	size_t offw = ringBuffer.offw;
	size_t offr = ringBuffer.offr;

	N3DS_Print("queue w %x, r %x\n", offw, offr);
	N3DS_Print("data [w] %x, [r] %x\n", ringBuffer.mem[offw], ringBuffer.mem[offr]);
	NDS3D_driverMemDump(ringBuffer.mem, ringBufferSize);

	N3DS_Print("queue dump done.\n");
}
