// SPDX-License-Identifier: GPL-2.0-only

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/interconnect.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/qcom/nspm.h>
#include <linux/workqueue.h>

#include "qcom_nspm_internal.h"

#define CREATE_TRACE_POINTS
#include "trace-nspm.h"

#define QCOM_NSPM_BANK_COUNT		12
#define QCOM_NSPM_FIFO_DEPTH		64
#define QCOM_NSPM_EVENT_DEPTH		64
#define QCOM_NSPM_RELEASE_TIMEOUT_MS	10000
#define QCOM_NSPM_BAD_ASID		((s32)0x8000054f)

struct qcom_nspm_fifo_event {
	struct qcom_nspm_notification notification;
	u32 generation;
	ktime_t timestamp;
};

struct qcom_nspm_bank {
	u32 sid;
	u32 client_id;
	pid_t tgid;
	enum qcom_nspm_state state;
	ktime_t transition_time;
	int create_ret;
	int release_ret;
	u32 pending;
	u32 mappings;
	bool terminal_seen;
	struct qcom_nspm_notification last_notification;
};

struct qcom_nspm_event_log {
	ktime_t timestamp;
	u32 generation;
	u32 sid;
	u32 client_id;
	enum qcom_nspm_state from;
	enum qcom_nspm_state to;
	enum qcom_nspm_event event;
	int ret;
};

struct qcom_nspm_counters {
	u64 reservations;
	u64 reservation_failures;
	u64 creates;
	u64 releases;
	u64 notifications;
	u64 notification_misses;
	u64 timeouts;
	u64 quarantines;
	u64 bad_asid;
	u64 fifo_overflows;
	u64 stale_events;
	u64 invalid_transitions;
};

struct qcom_nspm;

struct qcom_nspm_hw_ops {
	int (*acquire_votes)(struct qcom_nspm *nspm);
	void (*release_votes)(struct qcom_nspm *nspm);
};

struct qcom_nspm {
	struct device *dev;
	struct icc_path *path;
	const struct qcom_nspm_hw_ops *hw_ops;
	/* Serializes bank state, lifecycle calls, votes, and diagnostics. */
	struct mutex lock;
	/* Protects the notification FIFO used from the RPMsg callback. */
	spinlock_t fifo_lock;
	DECLARE_KFIFO(fifo, struct qcom_nspm_fifo_event,
		      QCOM_NSPM_FIFO_DEPTH);
	struct workqueue_struct *workqueue;
	struct work_struct notification_work;
	struct delayed_work timeout_work;
	struct qcom_nspm_bank banks[QCOM_NSPM_BANK_COUNT];
	struct qcom_nspm_event_log events[QCOM_NSPM_EVENT_DEPTH];
	struct qcom_nspm_counters counters;
	struct dentry *debugfs_root;
	struct device *consumer;
	u32 generation;
	u32 next_bank;
	u32 event_head;
	u32 event_count;
	s32 terminal_status;
	bool terminal_status_valid;
	bool enforcement;
	bool online;
	bool accepting;
	bool voted;
	bool degraded;
	bool overflow_reported;
};

struct qcom_nspm_snapshot {
	struct qcom_nspm_bank banks[QCOM_NSPM_BANK_COUNT];
	struct qcom_nspm_event_log events[QCOM_NSPM_EVENT_DEPTH];
	struct qcom_nspm_counters counters;
	u32 generation;
	u32 event_head;
	u32 event_count;
	s32 terminal_status;
	bool terminal_status_valid;
	bool enforcement;
	bool online;
	bool accepting;
	bool voted;
	bool degraded;
};

static struct qcom_nspm_bank *
qcom_nspm_find_sid_locked(struct qcom_nspm *nspm, u32 sid)
{
	int i;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++)
		if (nspm->banks[i].sid == sid)
			return &nspm->banks[i];

	return NULL;
}

static struct qcom_nspm_bank *
qcom_nspm_find_client_locked(struct qcom_nspm *nspm, u32 client_id)
{
	int i;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		if (nspm->banks[i].state == QCOM_NSPM_FREE)
			continue;
		if (nspm->banks[i].client_id == client_id)
			return &nspm->banks[i];
	}

	return NULL;
}

static struct qcom_nspm_bank *
qcom_nspm_find_tgid_locked(struct qcom_nspm *nspm, pid_t tgid)
{
	int i;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		if (nspm->banks[i].state == QCOM_NSPM_FREE ||
		    nspm->banks[i].state == QCOM_NSPM_DEAD)
			continue;
		if (nspm->banks[i].tgid == tgid)
			return &nspm->banks[i];
	}

	return NULL;
}

static bool qcom_nspm_any_vote_user_locked(struct qcom_nspm *nspm)
{
	int i;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++)
		if (qcom_nspm_state_holds_vote(nspm->banks[i].state))
			return true;

	return false;
}

static bool qcom_nspm_generation_valid_locked(struct qcom_nspm *nspm,
					      u32 generation)
{
	if (generation == nspm->generation)
		return true;

	nspm->counters.stale_events++;

	return false;
}

static void qcom_nspm_log_event_locked(struct qcom_nspm *nspm,
				       struct qcom_nspm_bank *bank,
				       enum qcom_nspm_state from,
				       enum qcom_nspm_event event, int ret)
{
	struct qcom_nspm_event_log *entry;

	entry = &nspm->events[nspm->event_head];
	entry->timestamp = ktime_get_boottime();
	entry->generation = nspm->generation;
	entry->sid = bank->sid;
	entry->client_id = bank->client_id;
	entry->from = from;
	entry->to = bank->state;
	entry->event = event;
	entry->ret = ret;

	nspm->event_head = (nspm->event_head + 1) % QCOM_NSPM_EVENT_DEPTH;
	if (nspm->event_count < QCOM_NSPM_EVENT_DEPTH)
		nspm->event_count++;
}

static int qcom_nspm_generic_acquire_votes(struct qcom_nspm *nspm)
{
	int ret;

	dev_pm_genpd_set_performance_state(nspm->dev, INT_MAX);
	ret = pm_runtime_resume_and_get(nspm->dev);
	if (ret < 0) {
		dev_pm_genpd_set_performance_state(nspm->dev, 0);
		return ret;
	}

	ret = icc_set_bw(nspm->path, 0, UINT_MAX);
	if (ret) {
		dev_pm_genpd_set_performance_state(nspm->dev, 0);
		pm_runtime_put_sync(nspm->dev);
	}

	return ret;
}

static void qcom_nspm_generic_release_votes(struct qcom_nspm *nspm)
{
	icc_set_bw(nspm->path, 0, 0);
	dev_pm_genpd_set_performance_state(nspm->dev, 0);
	pm_runtime_put_sync(nspm->dev);
}

static const struct qcom_nspm_hw_ops qcom_nspm_generic_hw_ops = {
	.acquire_votes = qcom_nspm_generic_acquire_votes,
	.release_votes = qcom_nspm_generic_release_votes,
};

static int qcom_nspm_acquire_votes_locked(struct qcom_nspm *nspm)
{
	int ret;

	if (nspm->voted)
		return 0;

	ret = nspm->hw_ops->acquire_votes(nspm);
	if (!ret)
		nspm->voted = true;

	trace_nspm_vote(!ret, ret);

	return ret;
}

static void qcom_nspm_release_votes_locked(struct qcom_nspm *nspm,
					   bool force)
{
	if (!nspm->voted)
		return;

	if (!force && qcom_nspm_any_vote_user_locked(nspm))
		return;

	nspm->hw_ops->release_votes(nspm);
	nspm->voted = false;
	trace_nspm_vote(false, 0);
}

static int qcom_nspm_transition_locked(struct qcom_nspm *nspm,
				       struct qcom_nspm_bank *bank,
				       enum qcom_nspm_event event,
				       bool terminal_proven)
{
	enum qcom_nspm_state from = bank->state;
	int ret;

	ret = qcom_nspm_apply_event(&bank->state, nspm->generation,
				    nspm->generation, event, terminal_proven);
	if (ret) {
		nspm->counters.invalid_transitions++;
		qcom_nspm_log_event_locked(nspm, bank, from, event, ret);
		trace_nspm_transition(nspm->generation, bank->sid,
				       bank->client_id, from, from, event, ret);
		return ret;
	}

	bank->transition_time = ktime_get_boottime();
	qcom_nspm_log_event_locked(nspm, bank, from, event, 0);
	trace_nspm_transition(nspm->generation, bank->sid, bank->client_id,
			       from, bank->state, event, 0);

	if (bank->state == QCOM_NSPM_QUARANTINED &&
	    from != QCOM_NSPM_QUARANTINED) {
		nspm->counters.quarantines++;
		trace_nspm_quarantine(nspm->generation, bank->sid,
				       bank->client_id, event);
	}

	if (bank->state == QCOM_NSPM_FREE)
		qcom_nspm_release_votes_locked(nspm, false);

	return 0;
}

static void qcom_nspm_schedule_timeout_locked(struct qcom_nspm *nspm)
{
	mod_delayed_work(nspm->workqueue, &nspm->timeout_work, HZ);
}

static void qcom_nspm_handle_overflow_locked(struct qcom_nspm *nspm)
{
	int i;

	if (!READ_ONCE(nspm->degraded) || nspm->overflow_reported)
		return;

	nspm->overflow_reported = true;
	nspm->counters.fifo_overflows++;
	if (!nspm->enforcement)
		return;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		if (!qcom_nspm_state_holds_vote(nspm->banks[i].state))
			continue;
		qcom_nspm_transition_locked(nspm, &nspm->banks[i],
					    QCOM_NSPM_FIFO_OVERFLOW, false);
	}
}

static void qcom_nspm_process_notification_locked(
		struct qcom_nspm *nspm, const struct qcom_nspm_fifo_event *event)
{
	const struct qcom_nspm_notification *notification;
	struct qcom_nspm_bank *bank;
	bool terminal;

	if (!nspm->online || event->generation != nspm->generation) {
		nspm->counters.stale_events++;
		return;
	}

	notification = &event->notification;
	nspm->counters.notifications++;
	bank = qcom_nspm_find_tgid_locked(nspm, notification->pid);
	trace_nspm_notification(event->generation, notification->pid,
				 notification->type, notification->status, !!bank);
	if (!bank) {
		nspm->counters.notification_misses++;
		return;
	}

	bank->last_notification = *notification;
	terminal = nspm->terminal_status_valid &&
		   notification->status == (u32)nspm->terminal_status;
	if (!terminal)
		return;

	if (bank->state == QCOM_NSPM_RELEASING) {
		bank->terminal_seen = true;
		return;
	}

	if (bank->state == QCOM_NSPM_QUIESCING) {
		bank->terminal_seen = true;
		if (!bank->pending && !bank->mappings)
			qcom_nspm_transition_locked(nspm, bank,
						    QCOM_NSPM_TERMINAL, true);
		return;
	}

	nspm->counters.invalid_transitions++;
	qcom_nspm_log_event_locked(nspm, bank, bank->state,
				   QCOM_NSPM_TERMINAL, -EPROTO);
	if (nspm->enforcement)
		qcom_nspm_transition_locked(nspm, bank,
					    QCOM_NSPM_AMBIGUOUS_NOTIFICATION,
					    false);
}

static void qcom_nspm_notification_work(struct work_struct *work)
{
	struct qcom_nspm *nspm;
	struct qcom_nspm_fifo_event event;
	unsigned long flags;
	bool present;

	nspm = container_of(work, struct qcom_nspm, notification_work);

	for (;;) {
		spin_lock_irqsave(&nspm->fifo_lock, flags);
		present = kfifo_get(&nspm->fifo, &event);
		spin_unlock_irqrestore(&nspm->fifo_lock, flags);
		if (!present)
			break;

		mutex_lock(&nspm->lock);
		qcom_nspm_handle_overflow_locked(nspm);
		qcom_nspm_process_notification_locked(nspm, &event);
		mutex_unlock(&nspm->lock);
	}

	mutex_lock(&nspm->lock);
	qcom_nspm_handle_overflow_locked(nspm);
	mutex_unlock(&nspm->lock);
}

static void qcom_nspm_timeout_work(struct work_struct *work)
{
	struct qcom_nspm *nspm;
	ktime_t now = ktime_get_boottime();
	bool reschedule = false;
	int i;

	nspm = container_of(to_delayed_work(work), struct qcom_nspm,
			    timeout_work);

	mutex_lock(&nspm->lock);
	if (!nspm->online)
		goto out_unlock;

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		struct qcom_nspm_bank *bank = &nspm->banks[i];

		if (bank->state != QCOM_NSPM_RELEASING &&
		    bank->state != QCOM_NSPM_QUIESCING)
			continue;

		if (ktime_ms_delta(now, bank->transition_time) <
		    QCOM_NSPM_RELEASE_TIMEOUT_MS) {
			reschedule = true;
			continue;
		}

		nspm->counters.timeouts++;
		qcom_nspm_transition_locked(nspm, bank, QCOM_NSPM_TIMEOUT,
					    false);
	}

	if (reschedule)
		qcom_nspm_schedule_timeout_locked(nspm);

out_unlock:
	mutex_unlock(&nspm->lock);
}

static int qcom_nspm_state_show(struct seq_file *seq, void *unused)
{
	struct qcom_nspm *nspm = seq->private;
	struct qcom_nspm_snapshot *snapshot;
	u32 oldest;
	int i;

	snapshot = kzalloc_obj(*snapshot);
	if (!snapshot)
		return -ENOMEM;

	mutex_lock(&nspm->lock);
	memcpy(snapshot->banks, nspm->banks, sizeof(snapshot->banks));
	memcpy(snapshot->events, nspm->events, sizeof(snapshot->events));
	snapshot->counters = nspm->counters;
	snapshot->generation = nspm->generation;
	snapshot->event_head = nspm->event_head;
	snapshot->event_count = nspm->event_count;
	snapshot->terminal_status = nspm->terminal_status;
	snapshot->terminal_status_valid = nspm->terminal_status_valid;
	snapshot->enforcement = nspm->enforcement;
	snapshot->online = nspm->online;
	snapshot->accepting = nspm->accepting;
	snapshot->voted = nspm->voted;
	snapshot->degraded = nspm->degraded;
	mutex_unlock(&nspm->lock);

	seq_printf(seq, "mode: %s\n",
		   snapshot->enforcement ? "enforcement" : "observation");
	seq_printf(seq, "generation: %u\n", snapshot->generation);
	seq_printf(seq, "online: %u\naccepting: %u\nvoted: %u\ndegraded: %u\n",
		   snapshot->online, snapshot->accepting, snapshot->voted,
		   snapshot->degraded);
	if (snapshot->terminal_status_valid)
		seq_printf(seq, "terminal_status: %d\n",
			   snapshot->terminal_status);
	else
		seq_puts(seq, "terminal_status: unproven\n");

	seq_printf(seq,
		   "counters: reserve=%llu reserve_fail=%llu create=%llu release=%llu notification=%llu miss=%llu timeout=%llu quarantine=%llu bad_asid=%llu overflow=%llu stale=%llu invalid=%llu\n",
		   snapshot->counters.reservations,
		   snapshot->counters.reservation_failures,
		   snapshot->counters.creates, snapshot->counters.releases,
		   snapshot->counters.notifications,
		   snapshot->counters.notification_misses,
		   snapshot->counters.timeouts,
		   snapshot->counters.quarantines,
		   snapshot->counters.bad_asid,
		   snapshot->counters.fifo_overflows,
		   snapshot->counters.stale_events,
		   snapshot->counters.invalid_transitions);

	seq_puts(seq, "banks:\n");
	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		struct qcom_nspm_bank *bank = &snapshot->banks[i];

		seq_printf(seq,
			   "  %02d sid=%#06x client=%u tgid=%d state=%s age_ms=%lld create=%d release=%d pending=%u mappings=%u notif={type=%u pid=%d status=%u}\n",
			   i, bank->sid, bank->client_id, bank->tgid,
			   qcom_nspm_state_name(bank->state),
			   ktime_ms_delta(ktime_get_boottime(),
					  bank->transition_time),
			   bank->create_ret, bank->release_ret, bank->pending,
			   bank->mappings, bank->last_notification.type,
			   bank->last_notification.pid,
			   bank->last_notification.status);
	}

	seq_puts(seq, "events:\n");
	oldest = (snapshot->event_head + QCOM_NSPM_EVENT_DEPTH -
		  snapshot->event_count) % QCOM_NSPM_EVENT_DEPTH;
	for (i = 0; i < snapshot->event_count; i++) {
		struct qcom_nspm_event_log *event;
		u32 index = (oldest + i) % QCOM_NSPM_EVENT_DEPTH;

		event = &snapshot->events[index];
		seq_printf(seq,
			   "  age_ms=%lld generation=%u sid=%#x client=%u state=%s->%s event=%u ret=%d\n",
			   ktime_ms_delta(ktime_get_boottime(), event->timestamp),
			   event->generation, event->sid, event->client_id,
			   qcom_nspm_state_name(event->from),
			   qcom_nspm_state_name(event->to), event->event,
			   event->ret);
	}

	kfree(snapshot);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(qcom_nspm_state);

struct qcom_nspm *qcom_nspm_fastrpc_register(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *node;
	struct qcom_nspm *nspm;
	unsigned long flags;
	int i;

	if (!dev || !dev->of_node)
		return NULL;

	node = of_parse_phandle(dev->of_node, "qcom,nspm", 0);
	if (!node)
		return NULL;

	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return NULL;

	nspm = platform_get_drvdata(pdev);
	if (!nspm) {
		put_device(&pdev->dev);
		dev_dbg(dev, "NSPM provider is not available\n");
		return NULL;
	}

	mutex_lock(&nspm->lock);
	if (nspm->consumer) {
		mutex_unlock(&nspm->lock);
		put_device(&pdev->dev);
		dev_warn(dev, "NSPM already has a FastRPC consumer\n");
		return NULL;
	}

	nspm->consumer = dev;
	nspm->generation++;
	if (!nspm->generation)
		nspm->generation++;
	nspm->online = true;
	nspm->accepting = true;
	nspm->degraded = false;
	nspm->overflow_reported = false;

	spin_lock_irqsave(&nspm->fifo_lock, flags);
	kfifo_reset(&nspm->fifo);
	spin_unlock_irqrestore(&nspm->fifo_lock, flags);

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		if (nspm->banks[i].state == QCOM_NSPM_DEAD)
			qcom_nspm_transition_locked(nspm, &nspm->banks[i],
						    QCOM_NSPM_CHANNEL_ONLINE,
						    false);
	}
	trace_nspm_channel(nspm->generation, true);
	mutex_unlock(&nspm->lock);

	device_link_add(dev, nspm->dev, DL_FLAG_AUTOREMOVE_CONSUMER);

	return nspm;
}
EXPORT_SYMBOL_GPL(qcom_nspm_fastrpc_register);

void qcom_nspm_fastrpc_unregister(struct qcom_nspm *nspm)
{
	bool registered;

	if (!nspm)
		return;

	qcom_nspm_channel_lost(nspm);

	mutex_lock(&nspm->lock);
	registered = !!nspm->consumer;
	nspm->consumer = NULL;
	mutex_unlock(&nspm->lock);

	if (registered)
		put_device(nspm->dev);
}
EXPORT_SYMBOL_GPL(qcom_nspm_fastrpc_unregister);

unsigned int qcom_nspm_session_start_index(struct qcom_nspm *nspm,
					   unsigned int count)
{
	unsigned int start = 0;

	if (!nspm || !count)
		return 0;

	mutex_lock(&nspm->lock);
	if (nspm->enforcement)
		start = nspm->next_bank % count;
	mutex_unlock(&nspm->lock);

	return start;
}
EXPORT_SYMBOL_GPL(qcom_nspm_session_start_index);

int qcom_nspm_session_reserve(struct qcom_nspm *nspm, u32 sid,
			      u32 client_id, pid_t tgid, u32 *generation)
{
	struct qcom_nspm_bank *bank;
	enum qcom_nspm_state from;
	int bank_index;
	int ret;

	if (!nspm)
		return 0;

	mutex_lock(&nspm->lock);
	bank = qcom_nspm_find_sid_locked(nspm, sid);
	if (!bank) {
		ret = -EINVAL;
		goto out_failure;
	}

	if (!nspm->online || !nspm->accepting) {
		ret = -EPIPE;
		goto out_failure;
	}

	if (nspm->enforcement && nspm->degraded) {
		ret = -EOVERFLOW;
		goto out_failure;
	}

	if (nspm->enforcement && bank->state != QCOM_NSPM_FREE) {
		ret = -EBUSY;
		goto out_failure;
	}

	ret = qcom_nspm_acquire_votes_locked(nspm);
	if (ret && nspm->enforcement)
		goto out_failure;

	from = bank->state;
	bank->client_id = client_id;
	bank->tgid = tgid;
	bank->create_ret = 0;
	bank->release_ret = 0;
	bank->pending = 0;
	bank->mappings = 0;
	bank->terminal_seen = false;
	memset(&bank->last_notification, 0,
	       sizeof(bank->last_notification));

	if (from == QCOM_NSPM_FREE) {
		qcom_nspm_transition_locked(nspm, bank, QCOM_NSPM_RESERVE,
					    false);
	} else {
		nspm->counters.invalid_transitions++;
		bank->state = QCOM_NSPM_RESERVED;
		bank->transition_time = ktime_get_boottime();
		qcom_nspm_log_event_locked(nspm, bank, from,
					   QCOM_NSPM_RESERVE, -EALREADY);
		trace_nspm_transition(nspm->generation, bank->sid,
				       bank->client_id, from, bank->state,
				       QCOM_NSPM_RESERVE, -EALREADY);
	}

	bank_index = bank - nspm->banks;
	nspm->next_bank = (bank_index + 1) % QCOM_NSPM_BANK_COUNT;
	nspm->counters.reservations++;
	if (generation)
		*generation = nspm->generation;
	trace_nspm_reservation(nspm->generation, sid, client_id, tgid, 0);
	mutex_unlock(&nspm->lock);

	return 0;

out_failure:
	nspm->counters.reservation_failures++;
	trace_nspm_reservation(nspm->generation, sid, client_id, tgid, ret);
	mutex_unlock(&nspm->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_nspm_session_reserve);

void qcom_nspm_session_rollback(struct qcom_nspm *nspm, u32 generation,
				u32 client_id)
{
	struct qcom_nspm_bank *bank;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (bank)
		qcom_nspm_transition_locked(nspm, bank,
					    QCOM_NSPM_RESERVATION_ROLLBACK,
					    false);
out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_session_rollback);

void qcom_nspm_create_start(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id)
{
	struct qcom_nspm_bank *bank;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (bank)
		qcom_nspm_transition_locked(nspm, bank,
					    QCOM_NSPM_CREATE_START, false);
out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_create_start);

void qcom_nspm_create_done(struct qcom_nspm *nspm, u32 generation,
			   u32 client_id, int create_ret, bool sent_to_dsp)
{
	struct qcom_nspm_bank *bank;
	enum qcom_nspm_event event;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (!bank)
		goto out_unlock;

	nspm->counters.creates++;
	bank->create_ret = create_ret;
	if (!create_ret)
		event = QCOM_NSPM_CREATE_OK;
	else if (!sent_to_dsp)
		event = QCOM_NSPM_CREATE_LOCAL_FAIL;
	else
		event = QCOM_NSPM_CREATE_AMBIGUOUS_FAIL;

	qcom_nspm_transition_locked(nspm, bank, event, false);
	if (create_ret == QCOM_NSPM_BAD_ASID) {
		nspm->counters.bad_asid++;
		trace_nspm_bad_asid(nspm->generation, bank->sid,
				     bank->client_id, create_ret);
	}

out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_create_done);

void qcom_nspm_release_start(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id, u32 pending, u32 mappings)
{
	struct qcom_nspm_bank *bank;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (bank) {
		bank->pending = pending;
		bank->mappings = mappings;
		qcom_nspm_transition_locked(nspm, bank,
					    QCOM_NSPM_RELEASE_START, false);
		qcom_nspm_schedule_timeout_locked(nspm);
	}
out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_release_start);

void qcom_nspm_release_done(struct qcom_nspm *nspm, u32 generation,
			   u32 client_id, int release_ret)
{
	struct qcom_nspm_bank *bank;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (!bank)
		goto out_unlock;

	nspm->counters.releases++;
	bank->release_ret = release_ret;
	qcom_nspm_transition_locked(nspm, bank,
				    release_ret ? QCOM_NSPM_RELEASE_FAIL :
				    QCOM_NSPM_RELEASE_OK, false);
	if (!release_ret && bank->terminal_seen && !bank->pending &&
	    !bank->mappings)
		qcom_nspm_transition_locked(nspm, bank, QCOM_NSPM_TERMINAL,
					    true);

out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_release_done);

void qcom_nspm_session_close(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id, bool dsp_process_init, u32 pending,
			    u32 mappings)
{
	struct qcom_nspm_bank *bank;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	if (!qcom_nspm_generation_valid_locked(nspm, generation))
		goto out_unlock;
	bank = qcom_nspm_find_client_locked(nspm, client_id);
	if (!bank)
		goto out_unlock;

	bank->pending = pending;
	bank->mappings = mappings;
	if (!dsp_process_init && bank->state == QCOM_NSPM_RESERVED) {
		qcom_nspm_transition_locked(nspm, bank,
					    QCOM_NSPM_RESERVATION_ROLLBACK,
					    false);
	} else if (dsp_process_init && bank->state == QCOM_NSPM_QUIESCING &&
		   bank->terminal_seen && !pending && !mappings) {
		qcom_nspm_transition_locked(nspm, bank, QCOM_NSPM_TERMINAL,
					    true);
	} else if (bank->state == QCOM_NSPM_RELEASING ||
		   bank->state == QCOM_NSPM_QUIESCING) {
		qcom_nspm_schedule_timeout_locked(nspm);
	}

out_unlock:
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_session_close);

bool qcom_nspm_queue_notification(
		struct qcom_nspm *nspm,
		const struct qcom_nspm_notification *notification)
{
	struct qcom_nspm_fifo_event event;
	unsigned long flags;
	bool queued;

	if (!nspm || !notification || !READ_ONCE(nspm->online))
		return false;

	event.notification = *notification;
	event.generation = READ_ONCE(nspm->generation);
	event.timestamp = ktime_get_boottime();

	spin_lock_irqsave(&nspm->fifo_lock, flags);
	queued = kfifo_put(&nspm->fifo, event);
	if (!queued)
		WRITE_ONCE(nspm->degraded, true);
	spin_unlock_irqrestore(&nspm->fifo_lock, flags);

	queue_work(nspm->workqueue, &nspm->notification_work);

	return queued;
}
EXPORT_SYMBOL_GPL(qcom_nspm_queue_notification);

void qcom_nspm_channel_lost(struct qcom_nspm *nspm)
{
	unsigned long flags;
	bool was_online;
	int i;

	if (!nspm)
		return;

	mutex_lock(&nspm->lock);
	was_online = nspm->online;
	nspm->online = false;
	nspm->accepting = false;
	if (was_online) {
		for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
			if (nspm->banks[i].state != QCOM_NSPM_FREE &&
			    nspm->banks[i].state != QCOM_NSPM_DEAD)
				qcom_nspm_transition_locked(
					nspm, &nspm->banks[i],
					QCOM_NSPM_CHANNEL_LOST, false);
		}
		trace_nspm_channel(nspm->generation, false);
	}
	mutex_unlock(&nspm->lock);

	cancel_delayed_work_sync(&nspm->timeout_work);
	flush_workqueue(nspm->workqueue);

	spin_lock_irqsave(&nspm->fifo_lock, flags);
	kfifo_reset(&nspm->fifo);
	spin_unlock_irqrestore(&nspm->fifo_lock, flags);

	mutex_lock(&nspm->lock);
	qcom_nspm_release_votes_locked(nspm, true);
	mutex_unlock(&nspm->lock);
}
EXPORT_SYMBOL_GPL(qcom_nspm_channel_lost);

static int qcom_nspm_probe(struct platform_device *pdev)
{
	struct qcom_nspm *nspm;
	struct device *dev = &pdev->dev;
	u32 terminal_status;
	u32 sids[QCOM_NSPM_BANK_COUNT];
	int count;
	int i;
	int j;
	int ret;

	count = device_property_count_u32(dev, "qcom,session-sids");
	if (count != QCOM_NSPM_BANK_COUNT)
		return dev_err_probe(dev, -EINVAL,
				     "expected %u session SIDs, got %d\n",
				     QCOM_NSPM_BANK_COUNT, count);

	ret = device_property_read_u32_array(dev, "qcom,session-sids", sids,
					     ARRAY_SIZE(sids));
	if (ret)
		return dev_err_probe(dev, ret, "failed to read session SIDs\n");

	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++)
		for (j = i + 1; j < QCOM_NSPM_BANK_COUNT; j++)
			if (sids[i] == sids[j])
				return dev_err_probe(dev, -EINVAL,
						     "duplicate SID %#x\n", sids[i]);

	nspm = devm_kzalloc(dev, sizeof(*nspm), GFP_KERNEL);
	if (!nspm)
		return -ENOMEM;

	nspm->path = devm_of_icc_get(dev, NULL);
	if (IS_ERR(nspm->path))
		return dev_err_probe(dev, PTR_ERR(nspm->path),
				     "failed to acquire NSP interconnect\n");

	nspm->dev = dev;
	nspm->hw_ops = &qcom_nspm_generic_hw_ops;
	nspm->enforcement = device_property_read_bool(
		dev, "qcom,enforce-session-lifecycle");
	ret = device_property_read_u32(dev, "qcom,terminal-status",
				       &terminal_status);
	if (!ret) {
		nspm->terminal_status = (s32)terminal_status;
		nspm->terminal_status_valid = true;
	}
	if (nspm->enforcement && !nspm->terminal_status_valid)
		return dev_err_probe(dev, -EINVAL,
				     "enforcement requires a proven terminal status\n");

	mutex_init(&nspm->lock);
	spin_lock_init(&nspm->fifo_lock);
	INIT_KFIFO(nspm->fifo);
	INIT_WORK(&nspm->notification_work, qcom_nspm_notification_work);
	INIT_DELAYED_WORK(&nspm->timeout_work, qcom_nspm_timeout_work);
	for (i = 0; i < QCOM_NSPM_BANK_COUNT; i++) {
		nspm->banks[i].sid = sids[i];
		nspm->banks[i].state = QCOM_NSPM_FREE;
		nspm->banks[i].transition_time = ktime_get_boottime();
	}

	nspm->workqueue = alloc_ordered_workqueue("qcom_nspm",
						 WQ_MEM_RECLAIM);
	if (!nspm->workqueue)
		return -ENOMEM;

	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, nspm);
	nspm->debugfs_root = debugfs_create_dir("qcom_nspm", NULL);
	debugfs_create_file("state", 0444, nspm->debugfs_root, nspm,
			    &qcom_nspm_state_fops);

	dev_info(dev, "registered in %s mode\n",
		 nspm->enforcement ? "enforcement" : "observation");

	return 0;
}

static void qcom_nspm_remove(struct platform_device *pdev)
{
	struct qcom_nspm *nspm = platform_get_drvdata(pdev);

	qcom_nspm_channel_lost(nspm);
	debugfs_remove_recursive(nspm->debugfs_root);
	destroy_workqueue(nspm->workqueue);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id qcom_nspm_of_match[] = {
	{ .compatible = "qcom,x1p42100-nspm" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_nspm_of_match);

static struct platform_driver qcom_nspm_driver = {
	.probe = qcom_nspm_probe,
	.remove = qcom_nspm_remove,
	.driver = {
		.name = "qcom-nspm",
		.of_match_table = qcom_nspm_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(qcom_nspm_driver);

MODULE_DESCRIPTION("Qualcomm X1P NSP session lifecycle manager");
MODULE_LICENSE("GPL");
