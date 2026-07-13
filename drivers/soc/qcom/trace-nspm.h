/* SPDX-License-Identifier: GPL-2.0-only */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_nspm

#if !defined(_TRACE_QCOM_NSPM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QCOM_NSPM_H

#include <linux/tracepoint.h>

TRACE_EVENT(nspm_reservation,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, s32 tgid, int ret),
	TP_ARGS(generation, sid, client_id, tgid, ret),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(s32, tgid)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->tgid = tgid;
		__entry->ret = ret;
	),
	TP_printk("generation=%u sid=%#x client=%u tgid=%d ret=%d",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->tgid, __entry->ret)
);

TRACE_EVENT(nspm_transition,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, u32 from, u32 to,
		 u32 event, int ret),
	TP_ARGS(generation, sid, client_id, from, to, event, ret),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(u32, from)
		__field(u32, to)
		__field(u32, event)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->from = from;
		__entry->to = to;
		__entry->event = event;
		__entry->ret = ret;
	),
	TP_printk("generation=%u sid=%#x client=%u state=%u->%u event=%u ret=%d",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->from, __entry->to, __entry->event, __entry->ret)
);

TRACE_EVENT(nspm_notification,
	TP_PROTO(u32 generation, s32 pid, u32 type, u32 status, bool matched),
	TP_ARGS(generation, pid, type, status, matched),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(s32, pid)
		__field(u32, type)
		__field(u32, status)
		__field(bool, matched)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->pid = pid;
		__entry->type = type;
		__entry->status = status;
		__entry->matched = matched;
	),
	TP_printk("generation=%u pid=%d type=%u status=%u matched=%d",
		  __entry->generation, __entry->pid, __entry->type,
		  __entry->status, __entry->matched)
);

TRACE_EVENT(nspm_vote,
	TP_PROTO(bool enabled, int ret),
	TP_ARGS(enabled, ret),
	TP_STRUCT__entry(
		__field(bool, enabled)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->enabled = enabled;
		__entry->ret = ret;
	),
	TP_printk("enabled=%d ret=%d", __entry->enabled, __entry->ret)
);

TRACE_EVENT(nspm_quarantine,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, u32 reason),
	TP_ARGS(generation, sid, client_id, reason),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(u32, reason)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->reason = reason;
	),
	TP_printk("generation=%u sid=%#x client=%u reason=%u",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->reason)
);

TRACE_EVENT(nspm_channel,
	TP_PROTO(u32 generation, bool online),
	TP_ARGS(generation, online),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(bool, online)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->online = online;
	),
	TP_printk("generation=%u online=%d", __entry->generation,
		  __entry->online)
);

TRACE_EVENT(nspm_bad_asid,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, int ret),
	TP_ARGS(generation, sid, client_id, ret),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->ret = ret;
	),
	TP_printk("generation=%u sid=%#x client=%u ret=%d",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->ret)
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace-nspm

#include <trace/define_trace.h>
