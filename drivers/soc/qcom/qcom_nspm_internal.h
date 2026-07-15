/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_NSPM_INTERNAL_H__
#define __QCOM_NSPM_INTERNAL_H__

#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/types.h>

#define QCOM_NSPM_AEE_EQURTMEMMAPCREATE	((s32)0x8000054d)
#define QCOM_NSPM_AEE_EQURTINVHANDLE		((s32)0x8000054e)
#define QCOM_NSPM_AEE_EQURTBADASID		((s32)0x8000054f)

static inline int qcom_nspm_iommu_sid(const struct of_phandle_args *iommu,
				      u32 *sid)
{
	if (!iommu || !sid || !iommu->args_count)
		return -EINVAL;

	*sid = iommu->args[0];

	return 0;
}

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

#define QCOM_NSPM_NOTIF_STATUS_RESPONSE	4

enum qcom_nspm_pd_status {
	QCOM_NSPM_USER_PD_UP,
	QCOM_NSPM_USER_PD_EXIT,
	QCOM_NSPM_USER_PD_FORCE_KILL,
	QCOM_NSPM_USER_PD_EXCEPTION,
	QCOM_NSPM_DSP_SSR,
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

static inline bool qcom_nspm_mode_owns_votes(bool enforcement)
{
	return enforcement;
}

static inline bool
qcom_nspm_notification_matches_client(u32 client_id,
				      s32 notification_client_id)
{
	return notification_client_id > 0 &&
	       client_id == (u32)notification_client_id;
}

static inline bool
qcom_nspm_notification_is_terminal(u32 type, u32 status,
				   enum qcom_nspm_state state)
{
	if (type != QCOM_NSPM_NOTIF_STATUS_RESPONSE)
		return false;

	if (status >= QCOM_NSPM_USER_PD_EXIT &&
	    status <= QCOM_NSPM_USER_PD_EXCEPTION)
		return true;

	/*
	 * This firmware reports status 4 after accepting INIT_RELEASE. Keep
	 * its generic DSP-SSR meaning everywhere except an in-flight release,
	 * where it is the final asynchronous evidence that the ASID is idle.
	 */
	return status == QCOM_NSPM_DSP_SSR &&
	       (state == QCOM_NSPM_RELEASING ||
		state == QCOM_NSPM_QUIESCING);
}

static inline bool
qcom_nspm_state_can_latch_terminal(enum qcom_nspm_state state)
{
	return state == QCOM_NSPM_ACTIVE ||
	       state == QCOM_NSPM_RELEASING ||
	       state == QCOM_NSPM_QUIESCING;
}

static inline bool qcom_nspm_state_can_reserve(enum qcom_nspm_state state)
{
	return state == QCOM_NSPM_FREE;
}

static inline bool qcom_nspm_create_error_degrades(int ret, bool sent_to_dsp)
{
	return sent_to_dsp &&
		(ret == QCOM_NSPM_AEE_EQURTMEMMAPCREATE ||
		 ret == QCOM_NSPM_AEE_EQURTINVHANDLE ||
		 ret == QCOM_NSPM_AEE_EQURTBADASID);
}

static inline bool qcom_nspm_should_degrade(bool enforcement, int ret,
					    bool sent_to_dsp)
{
	return enforcement && qcom_nspm_create_error_degrades(ret, sent_to_dsp);
}

static inline bool qcom_nspm_channel_accepts_reservation(bool online, bool accepting, bool degraded)
{
	return online && accepting && !degraded;
}

static inline unsigned int
qcom_nspm_next_start_index(unsigned int next, unsigned int count)
{
	return count ? next % count : 0;
}

static inline bool
qcom_nspm_reservation_allowed(bool online, bool accepting, bool degraded,
			      enum qcom_nspm_state state)
{
	return qcom_nspm_channel_accepts_reservation(online, accepting,
						     degraded) &&
	       qcom_nspm_state_can_reserve(state);
}

static inline bool qcom_nspm_notification_is_current(ktime_t queued,
						     ktime_t reserved)
{
	return !ktime_before(queued, reserved);
}

static inline bool
qcom_nspm_release_is_complete(enum qcom_nspm_state state,
			      bool terminal_seen, u32 pending, u32 mappings)
{
	return state == QCOM_NSPM_QUIESCING && terminal_seen &&
	       !pending && !mappings;
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
