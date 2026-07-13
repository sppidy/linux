/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_QCOM_NSPM_H__
#define __LINUX_SOC_QCOM_NSPM_H__

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct qcom_nspm;

struct qcom_nspm_notification {
	u64 ctx;
	u32 type;
	s32 pid;
	u32 status;
};

#if IS_ENABLED(CONFIG_QCOM_NSPM)

struct qcom_nspm *qcom_nspm_fastrpc_register(struct device *dev);
void qcom_nspm_fastrpc_unregister(struct qcom_nspm *nspm);
unsigned int qcom_nspm_session_start_index(struct qcom_nspm *nspm,
					   unsigned int count);
int qcom_nspm_session_reserve(struct qcom_nspm *nspm, struct device *session_dev,
			      u32 client_id, pid_t tgid, u32 *generation);
void qcom_nspm_session_rollback(struct qcom_nspm *nspm, u32 generation,
				u32 client_id);
void qcom_nspm_create_start(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id);
void qcom_nspm_create_done(struct qcom_nspm *nspm, u32 generation,
			   u32 client_id, int create_ret, bool sent_to_dsp);
void qcom_nspm_release_start(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id, u32 pending, u32 mappings);
void qcom_nspm_release_done(struct qcom_nspm *nspm, u32 generation,
			   u32 client_id, int release_ret);
void qcom_nspm_session_close(struct qcom_nspm *nspm, u32 generation,
			    u32 client_id, bool dsp_process_init, u32 pending,
			    u32 mappings);
bool qcom_nspm_queue_notification(struct qcom_nspm *nspm,
				  const struct qcom_nspm_notification *notif);
void qcom_nspm_channel_lost(struct qcom_nspm *nspm);

#else

static inline struct qcom_nspm *
qcom_nspm_fastrpc_register(struct device *dev)
{
	return NULL;
}

static inline void qcom_nspm_fastrpc_unregister(struct qcom_nspm *nspm)
{
}

static inline unsigned int
qcom_nspm_session_start_index(struct qcom_nspm *nspm, unsigned int count)
{
	return 0;
}

static inline int qcom_nspm_session_reserve(struct qcom_nspm *nspm,
					    struct device *session_dev,
					    u32 client_id, pid_t tgid,
					    u32 *generation)
{
	if (generation)
		*generation = 0;
	return 0;
}

static inline void qcom_nspm_session_rollback(struct qcom_nspm *nspm,
					      u32 generation, u32 client_id)
{
}

static inline void qcom_nspm_create_start(struct qcom_nspm *nspm,
					  u32 generation, u32 client_id)
{
}

static inline void qcom_nspm_create_done(struct qcom_nspm *nspm,
					 u32 generation, u32 client_id,
					 int create_ret, bool sent_to_dsp)
{
}

static inline void qcom_nspm_release_start(struct qcom_nspm *nspm,
					   u32 generation, u32 client_id,
					   u32 pending, u32 mappings)
{
}

static inline void qcom_nspm_release_done(struct qcom_nspm *nspm,
					  u32 generation, u32 client_id,
					  int release_ret)
{
}

static inline void qcom_nspm_session_close(struct qcom_nspm *nspm,
					   u32 generation, u32 client_id,
					   bool dsp_process_init,
					   u32 pending, u32 mappings)
{
}

static inline bool
qcom_nspm_queue_notification(struct qcom_nspm *nspm,
			     const struct qcom_nspm_notification *notif)
{
	return false;
}

static inline void qcom_nspm_channel_lost(struct qcom_nspm *nspm)
{
}

#endif

#endif
