#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

static struct list sleep_list;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static list_less_func sleep_less;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	list_init(&sleep_list);
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}


/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	//thread_block
	intr_set_level (old_level);
	
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t timer_elapsed (int64_t then) { // then 시점 이후로 얼마나 흘렀는지 
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
/**
 * 이 함수를 호출한 스레드의 실행을 최소한 ticks만큼의 타이머 틱이 지날 때까지 중단시킨다.
 * TIMER_FREQ 기본값 = 100 (변경은 비추)
 * 목표 : Busy Wait 없애기 
 */ 

// static struct list sleep_list;
// static struct list ready_list;
// todo 
void timer_sleep (int64_t ticks) { 
	// 인터럽트 방해 막기 
	struct thread *cur_thread = thread_current();
	cur_thread->wake_up_time = timer_ticks () + ticks;
	
	enum intr_level old_level = intr_disable();
	// void list_push_back (struct list *, struct list_elem *);
	list_insert_ordered(&sleep_list, &cur_thread->sleep_elem, sleep_less, NULL);

	// BLOCK상태로 만들기 (READY 상태 X)
	thread_block();
	intr_set_level(old_level);	
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
// todo : 이거 고치기 
static void timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick (); // 실행 통계 누적(틱증가) + 타임슬라이스 초과 감지

	// list_pop_front
	// 잠든 스레드 깨우고 READY 큐 복귀 시키기 
	if (list_empty(&sleep_list)) {
		return;
	}

	bool need_reschedule = false;
	while (list_empty(&sleep_list) == false)
	{
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, sleep_elem);
		// 
		if (t->wake_up_time > ticks){
			break;
		}

		list_pop_front(&sleep_list);
		thread_unblock(t);		
		need_reschedule = true;
	}
	
	if (need_reschedule){
		intr_yield_on_return();
	}
}

/* sleep 큐 정렬 비교자*/
static bool sleep_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
	const struct thread *thread_a = list_entry(a, struct thread, sleep_elem);
	const struct thread *thread_b = list_entry(b, struct thread, sleep_elem);

	if (thread_a->wake_up_time != thread_b->wake_up_time){
		return thread_a->wake_up_time < thread_b->wake_up_time;
	}
	
	return false;
}



/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
