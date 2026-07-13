/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_NSPM_INTERNAL_H__
#define __QCOM_NSPM_INTERNAL_H__

#include <linux/errno.h>
#include <linux/types.h>

enum qcom_nspm_state {
	QCOM_NSPM_FREE,
	QCOM_NSPM_RESERVED,
	QCOM_NSPM_STARTING,
	QCOM_NSPM_ACTIVE,
	QCOM_NSPM_RELEASING,
	QCOM_NSPM_QUIESCING,
	QCOM_NSPM_QUARANTINED,
	QCOM_NSPM_DEAD,
};

enum qcom_nspm_event {
	QCOM_NSPM_RESERVE,
	QCOM_NSPM_RESERVATION_ROLLBACK,
	QCOM_NSPM_CREATE_START,
	QCOM_NSPM_CREATE_OK,
	QCOM_NSPM_CREATE_LOCAL_FAIL,
	QCOM_NSPM_CREATE_AMBIGUOUS_FAIL,
	QCOM_NSPM_RELEASE_START,
	QCOM_NSPM_RELEASE_OK,
	QCOM_NSPM_RELEASE_FAIL,
	QCOM_NSPM_TERMINAL,
	QCOM_NSPM_TIMEOUT,
	QCOM_NSPM_FIFO_OVERFLOW,
	QCOM_NSPM_AMBIGUOUS_NOTIFICATION,
	QCOM_NSPM_CHANNEL_LOST,
	QCOM_NSPM_CHANNEL_ONLINE,
};

static inline int qcom_nspm_next_state(enum qcom_nspm_state state,
				       enum qcom_nspm_event event,
				       bool terminal_proven)
{
	if (event == QCOM_NSPM_CHANNEL_LOST && state != QCOM_NSPM_FREE &&
	    state != QCOM_NSPM_DEAD)
		return QCOM_NSPM_DEAD;

	if (event == QCOM_NSPM_CHANNEL_ONLINE && state == QCOM_NSPM_DEAD)
		return QCOM_NSPM_FREE;

	if (event == QCOM_NSPM_FIFO_OVERFLOW && state != QCOM_NSPM_FREE &&
	    state != QCOM_NSPM_DEAD)
		return QCOM_NSPM_QUARANTINED;

	if (event == QCOM_NSPM_AMBIGUOUS_NOTIFICATION &&
	    state != QCOM_NSPM_FREE && state != QCOM_NSPM_DEAD)
		return QCOM_NSPM_QUARANTINED;

	if (event == QCOM_NSPM_CREATE_AMBIGUOUS_FAIL &&
	    state == QCOM_NSPM_STARTING)
		return QCOM_NSPM_QUARANTINED;

	if (event == QCOM_NSPM_RELEASE_FAIL &&
	    (state == QCOM_NSPM_RELEASING || state == QCOM_NSPM_QUIESCING))
		return QCOM_NSPM_QUARANTINED;

	if (event == QCOM_NSPM_TIMEOUT &&
	    (state == QCOM_NSPM_RELEASING || state == QCOM_NSPM_QUIESCING))
		return QCOM_NSPM_QUARANTINED;

	switch (state) {
	case QCOM_NSPM_FREE:
		if (event == QCOM_NSPM_RESERVE)
			return QCOM_NSPM_RESERVED;
		break;
	case QCOM_NSPM_RESERVED:
		if (event == QCOM_NSPM_RESERVATION_ROLLBACK)
			return QCOM_NSPM_FREE;
		if (event == QCOM_NSPM_CREATE_START)
			return QCOM_NSPM_STARTING;
		break;
	case QCOM_NSPM_STARTING:
		if (event == QCOM_NSPM_CREATE_OK)
			return QCOM_NSPM_ACTIVE;
		if (event == QCOM_NSPM_CREATE_LOCAL_FAIL)
			return QCOM_NSPM_RESERVED;
		break;
	case QCOM_NSPM_ACTIVE:
		if (event == QCOM_NSPM_RELEASE_START)
			return QCOM_NSPM_RELEASING;
		break;
	case QCOM_NSPM_RELEASING:
		if (event == QCOM_NSPM_RELEASE_OK)
			return QCOM_NSPM_QUIESCING;
		break;
	case QCOM_NSPM_QUIESCING:
		if (event == QCOM_NSPM_TERMINAL && terminal_proven)
			return QCOM_NSPM_FREE;
		break;
	case QCOM_NSPM_QUARANTINED:
	case QCOM_NSPM_DEAD:
		break;
	}

	return -EPROTO;
}

static inline int qcom_nspm_apply_event(enum qcom_nspm_state *state,
					u32 generation, u32 event_generation,
					enum qcom_nspm_event event,
					bool terminal_proven)
{
	int next;

	if (generation != event_generation)
		return -ESTALE;

	next = qcom_nspm_next_state(*state, event, terminal_proven);
	if (next < 0)
		return next;

	*state = next;

	return 0;
}

static inline bool qcom_nspm_state_holds_vote(enum qcom_nspm_state state)
{
	return state != QCOM_NSPM_FREE && state != QCOM_NSPM_DEAD;
}

static inline const char *qcom_nspm_state_name(enum qcom_nspm_state state)
{
	switch (state) {
	case QCOM_NSPM_FREE:
		return "free";
	case QCOM_NSPM_RESERVED:
		return "reserved";
	case QCOM_NSPM_STARTING:
		return "starting";
	case QCOM_NSPM_ACTIVE:
		return "active";
	case QCOM_NSPM_RELEASING:
		return "releasing";
	case QCOM_NSPM_QUIESCING:
		return "quiescing";
	case QCOM_NSPM_QUARANTINED:
		return "quarantined";
	case QCOM_NSPM_DEAD:
		return "dead";
	}

	return "unknown";
}

#endif
