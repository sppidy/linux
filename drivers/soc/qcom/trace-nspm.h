/* SPDX-License-Identifier: GPL-2.0-only */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qcom_nspm

#if !defined(_TRACE_QCOM_NSPM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_QCOM_NSPM_H

#include <linux/tracepoint.h>

TRACE_EVENT(nspm_reservation,
	TP_PROTO(u32 generation, u32 sid, u32 cb_index, u32 arid_base,
		 u32 mcdm_client_arid_base, u32 client_id, s32 tgid, int ret),
	TP_ARGS(generation, sid, cb_index, arid_base, mcdm_client_arid_base,
		client_id, tgid, ret),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, cb_index)
		__field(u32, arid_base)
		__field(u32, mcdm_client_arid_base)
		__field(u32, client_id)
		__field(s32, tgid)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->cb_index = cb_index;
		__entry->arid_base = arid_base;
		__entry->mcdm_client_arid_base = mcdm_client_arid_base;
		__entry->client_id = client_id;
		__entry->tgid = tgid;
		__entry->ret = ret;
	),
	TP_printk("generation=%u sid=%#x cb=%#x arid=%#x mcdm_arid=%#x client=%u tgid=%d ret=%d",
		  __entry->generation, __entry->sid, __entry->cb_index,
		  __entry->arid_base, __entry->mcdm_client_arid_base,
		  __entry->client_id, __entry->tgid, __entry->ret)
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
	TP_PROTO(u32 generation, s32 client_id, u32 type, u32 status,
		 bool matched),
	TP_ARGS(generation, client_id, type, status, matched),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(s32, client_id)
		__field(u32, type)
		__field(u32, status)
		__field(bool, matched)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->client_id = client_id;
		__entry->type = type;
		__entry->status = status;
		__entry->matched = matched;
	),
	TP_printk("generation=%u client=%d type=%u status=%u matched=%d",
		  __entry->generation, __entry->client_id, __entry->type,
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

TRACE_EVENT(nspm_mapping,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, s32 tgid,
		 u32 operation, u64 address, u64 size, int ret, bool sent_to_dsp),
	TP_ARGS(generation, sid, client_id, tgid, operation, address, size, ret,
		sent_to_dsp),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(s32, tgid)
		__field(u32, operation)
		__field(u64, address)
		__field(u64, size)
		__field(int, ret)
		__field(bool, sent_to_dsp)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->tgid = tgid;
		__entry->operation = operation;
		__entry->address = address;
		__entry->size = size;
		__entry->ret = ret;
		__entry->sent_to_dsp = sent_to_dsp;
	),
	TP_printk("generation=%u sid=%#x client=%u tgid=%d operation=%u address=%#llx size=%#llx ret=%d sent_to_dsp=%d",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->tgid, __entry->operation, __entry->address,
		  __entry->size, __entry->ret, __entry->sent_to_dsp)
);

TRACE_EVENT(nspm_iommu_mapping,
	TP_PROTO(u32 generation, u32 sid, u32 client_id, int group_id,
		 u32 domain_type, u64 dsp_address, u64 dma_address,
		 u64 physical_address, u64 size, u32 encoded_sid,
		 u64 encoded_iova, u32 flags),
	TP_ARGS(generation, sid, client_id, group_id, domain_type, dsp_address,
		dma_address, physical_address, size, encoded_sid, encoded_iova,
		flags),
	TP_STRUCT__entry(
		__field(u32, generation)
		__field(u32, sid)
		__field(u32, client_id)
		__field(int, group_id)
		__field(u32, domain_type)
		__field(u64, dsp_address)
		__field(u64, dma_address)
		__field(u64, physical_address)
		__field(u64, size)
		__field(u32, encoded_sid)
		__field(u64, encoded_iova)
		__field(u32, flags)
	),
	TP_fast_assign(
		__entry->generation = generation;
		__entry->sid = sid;
		__entry->client_id = client_id;
		__entry->group_id = group_id;
		__entry->domain_type = domain_type;
		__entry->dsp_address = dsp_address;
		__entry->dma_address = dma_address;
		__entry->physical_address = physical_address;
		__entry->size = size;
		__entry->encoded_sid = encoded_sid;
		__entry->encoded_iova = encoded_iova;
		__entry->flags = flags;
	),
	TP_printk("generation=%u sid=%#x client=%u group=%d domain_type=%u dsp=%#llx dma=%#llx phys=%#llx size=%#llx encoded_sid=%#x encoded_iova=%#llx domain=%d translated=%d sid_match=%d iova_match=%d",
		  __entry->generation, __entry->sid, __entry->client_id,
		  __entry->group_id, __entry->domain_type, __entry->dsp_address,
		  __entry->dma_address,
		  __entry->physical_address, __entry->size,
		  __entry->encoded_sid, __entry->encoded_iova,
		  !!(__entry->flags & BIT(0)), !!(__entry->flags & BIT(1)),
		  !!(__entry->flags & BIT(2)), !!(__entry->flags & BIT(3)))
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace-nspm

#include <trace/define_trace.h>
