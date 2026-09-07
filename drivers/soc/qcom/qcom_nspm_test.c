// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/of.h>

#include "qcom_nspm_internal.h"

struct qcom_nspm_transition_case {
	enum qcom_nspm_state from;
	enum qcom_nspm_event event;
	bool terminal_proven;
	int expected;
};

static void qcom_nspm_expect_result(struct kunit *test,
				    enum qcom_nspm_state from,
				    enum qcom_nspm_event event,
				    bool terminal_proven, int expected)
{
	int actual;

	actual = qcom_nspm_next_state(from, event, terminal_proven);
	KUNIT_EXPECT_EQ(test, actual, expected);
}

static void qcom_nspm_valid_transitions_test(struct kunit *test)
{
	static const struct qcom_nspm_transition_case cases[] = {
		{ QCOM_NSPM_FREE, QCOM_NSPM_RESERVE, false,
		  QCOM_NSPM_RESERVED },
		{ QCOM_NSPM_RESERVED, QCOM_NSPM_RESERVATION_ROLLBACK, false,
		  QCOM_NSPM_FREE },
		{ QCOM_NSPM_RESERVED, QCOM_NSPM_CREATE_START, false,
		  QCOM_NSPM_STARTING },
		{ QCOM_NSPM_STARTING, QCOM_NSPM_CREATE_OK, false,
		  QCOM_NSPM_ACTIVE },
		{ QCOM_NSPM_STARTING, QCOM_NSPM_CREATE_LOCAL_FAIL, false,
		  QCOM_NSPM_RESERVED },
		{ QCOM_NSPM_ACTIVE, QCOM_NSPM_RELEASE_START, false,
		  QCOM_NSPM_RELEASING },
		{ QCOM_NSPM_RELEASING, QCOM_NSPM_RELEASE_OK, false,
		  QCOM_NSPM_QUIESCING },
		{ QCOM_NSPM_QUIESCING, QCOM_NSPM_TERMINAL, true,
		  QCOM_NSPM_FREE },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		qcom_nspm_expect_result(test, cases[i].from, cases[i].event,
					cases[i].terminal_proven, cases[i].expected);
}

static void qcom_nspm_exception_transitions_test(struct kunit *test)
{
	static const enum qcom_nspm_state live_states[] = {
		QCOM_NSPM_RESERVED,
		QCOM_NSPM_STARTING,
		QCOM_NSPM_ACTIVE,
		QCOM_NSPM_RELEASING,
		QCOM_NSPM_QUIESCING,
		QCOM_NSPM_QUARANTINED,
	};
	int i;

	qcom_nspm_expect_result(test, QCOM_NSPM_STARTING,
				QCOM_NSPM_CREATE_AMBIGUOUS_FAIL, false,
				QCOM_NSPM_QUARANTINED);
	qcom_nspm_expect_result(test, QCOM_NSPM_RELEASING,
				QCOM_NSPM_RELEASE_FAIL, false,
				QCOM_NSPM_QUARANTINED);
	qcom_nspm_expect_result(test, QCOM_NSPM_QUIESCING,
				QCOM_NSPM_TIMEOUT, false,
				QCOM_NSPM_QUARANTINED);
	qcom_nspm_expect_result(test, QCOM_NSPM_ACTIVE,
				QCOM_NSPM_FIFO_OVERFLOW, false,
				QCOM_NSPM_QUARANTINED);
	qcom_nspm_expect_result(test, QCOM_NSPM_ACTIVE,
				QCOM_NSPM_AMBIGUOUS_NOTIFICATION, false,
				QCOM_NSPM_QUARANTINED);

	for (i = 0; i < ARRAY_SIZE(live_states); i++)
		qcom_nspm_expect_result(test, live_states[i],
					QCOM_NSPM_CHANNEL_LOST, false,
					QCOM_NSPM_DEAD);

	qcom_nspm_expect_result(test, QCOM_NSPM_DEAD,
				QCOM_NSPM_CHANNEL_ONLINE, false,
				QCOM_NSPM_FREE);
}

static void qcom_nspm_rejected_transitions_test(struct kunit *test)
{
	qcom_nspm_expect_result(test, QCOM_NSPM_FREE,
				QCOM_NSPM_CREATE_START, false, -EPROTO);
	qcom_nspm_expect_result(test, QCOM_NSPM_ACTIVE,
				QCOM_NSPM_RESERVE, false, -EPROTO);
	qcom_nspm_expect_result(test, QCOM_NSPM_QUIESCING,
				QCOM_NSPM_TERMINAL, false, -EPROTO);
	qcom_nspm_expect_result(test, QCOM_NSPM_QUARANTINED,
				QCOM_NSPM_CHANNEL_ONLINE, false, -EPROTO);
}

static void qcom_nspm_generation_order_test(struct kunit *test)
{
	enum qcom_nspm_state state = QCOM_NSPM_FREE;
	int ret;

	ret = qcom_nspm_apply_event(&state, 2, 1, QCOM_NSPM_RESERVE, false);
	KUNIT_EXPECT_EQ(test, ret, -ESTALE);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_FREE);

	ret = qcom_nspm_apply_event(&state, 2, 2, QCOM_NSPM_RESERVE, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_RESERVED);

	ret = qcom_nspm_apply_event(&state, 2, 2,
				    QCOM_NSPM_CHANNEL_LOST, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_DEAD);

	ret = qcom_nspm_apply_event(&state, 3, 3,
				    QCOM_NSPM_CHANNEL_ONLINE, false);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_FREE);
}

static void qcom_nspm_notification_order_test(struct kunit *test)
{
	enum qcom_nspm_state state = QCOM_NSPM_ACTIVE;
	int ret;

	ret = qcom_nspm_apply_event(&state, 1, 1, QCOM_NSPM_TERMINAL, true);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_ACTIVE);

	ret = qcom_nspm_apply_event(&state, 1, 1,
				    QCOM_NSPM_RELEASE_START, false);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = qcom_nspm_apply_event(&state, 1, 1,
				    QCOM_NSPM_RELEASE_OK, false);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = qcom_nspm_apply_event(&state, 1, 1, QCOM_NSPM_TERMINAL, false);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_QUIESCING);

	ret = qcom_nspm_apply_event(&state, 1, 1, QCOM_NSPM_TERMINAL, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state, QCOM_NSPM_FREE);

	ret = qcom_nspm_apply_event(&state, 1, 1, QCOM_NSPM_TERMINAL, true);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
}

static void qcom_nspm_state_metadata_test(struct kunit *test)
{
	KUNIT_EXPECT_STREQ(test, qcom_nspm_state_name(QCOM_NSPM_FREE), "free");
	KUNIT_EXPECT_STREQ(test,
			   qcom_nspm_state_name(QCOM_NSPM_QUARANTINED),
			   "quarantined");
	KUNIT_EXPECT_TRUE(test, qcom_nspm_state_holds_vote(QCOM_NSPM_ACTIVE));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_state_holds_vote(QCOM_NSPM_QUARANTINED));
	KUNIT_EXPECT_FALSE(test, qcom_nspm_state_holds_vote(QCOM_NSPM_FREE));
	KUNIT_EXPECT_FALSE(test, qcom_nspm_state_holds_vote(QCOM_NSPM_DEAD));
}

static void qcom_nspm_vote_policy_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, qcom_nspm_mode_owns_votes(false));
	KUNIT_EXPECT_TRUE(test, qcom_nspm_mode_owns_votes(true));
}

static void qcom_nspm_notification_client_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_notification_matches_client(3, 3));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_notification_matches_client(3, 2));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_notification_matches_client(3, 0));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_notification_matches_client(3, -1));
	/* A delayed notification from an older PD lifetime must not match. */
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_notification_matches_client(24, 23));
}

static void qcom_nspm_terminal_notification_test(struct kunit *test)
{
	bool terminal;

	terminal = qcom_nspm_notification_is_terminal(4, 1, QCOM_NSPM_ACTIVE);
	KUNIT_EXPECT_TRUE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 2, QCOM_NSPM_ACTIVE);
	KUNIT_EXPECT_TRUE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 3, QCOM_NSPM_ACTIVE);
	KUNIT_EXPECT_TRUE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 4, QCOM_NSPM_RELEASING);
	KUNIT_EXPECT_TRUE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 4, QCOM_NSPM_QUIESCING);
	KUNIT_EXPECT_TRUE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 4, QCOM_NSPM_ACTIVE);
	KUNIT_EXPECT_FALSE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(3, 1, QCOM_NSPM_RELEASING);
	KUNIT_EXPECT_FALSE(test, terminal);
	terminal = qcom_nspm_notification_is_terminal(4, 0, QCOM_NSPM_RELEASING);
	KUNIT_EXPECT_FALSE(test, terminal);
}

static void qcom_nspm_terminal_latch_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_state_can_latch_terminal(QCOM_NSPM_ACTIVE));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_state_can_latch_terminal(QCOM_NSPM_RELEASING));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_state_can_latch_terminal(QCOM_NSPM_QUIESCING));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_state_can_latch_terminal(QCOM_NSPM_STARTING));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_state_can_latch_terminal(QCOM_NSPM_FREE));
}

static void qcom_nspm_reservation_policy_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_state_can_reserve(QCOM_NSPM_FREE));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_state_can_reserve(QCOM_NSPM_QUIESCING));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_state_can_reserve(QCOM_NSPM_QUARANTINED));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_channel_accepts_reservation(true, true, false));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_channel_accepts_reservation(false, true, false));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_channel_accepts_reservation(true, false, false));
	/* This gate has no mode argument: degraded always rejects reservations. */
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_channel_accepts_reservation(true, true, true));
	KUNIT_EXPECT_EQ(test, qcom_nspm_next_start_index(14, 12), 2U);
	KUNIT_EXPECT_EQ(test, qcom_nspm_next_start_index(3, 0), 0U);
	KUNIT_EXPECT_EQ(test,
			qcom_nspm_session_policy_start_index(false, 14, 12),
			0U);
	KUNIT_EXPECT_EQ(test,
			qcom_nspm_session_policy_start_index(true, 14, 12),
			2U);
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_reservation_allowed(true, true, false,
							 QCOM_NSPM_QUARANTINED));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_reservation_allowed(true, true, false,
							QCOM_NSPM_FREE));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_reservation_allowed(false, true, false,
							 QCOM_NSPM_FREE));
}

static void qcom_nspm_integrity_error_test(struct kunit *test)
{
	bool degrades;

	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTMEMMAPCREATE, true);
	KUNIT_EXPECT_TRUE(test,
			  degrades);
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_should_degrade(false,
						    QCOM_NSPM_AEE_EQURTMEMMAPCREATE,
						    true));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_should_degrade(true,
						   QCOM_NSPM_AEE_EQURTMEMMAPCREATE,
						   true));
	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTINVHANDLE, true);
	KUNIT_EXPECT_TRUE(test,
			  degrades);
	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTBADASID, true);
	KUNIT_EXPECT_TRUE(test,
			  degrades);
	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTMEMMAPCREATE, false);
	KUNIT_EXPECT_FALSE(test,
			   degrades);
	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTINVHANDLE, false);
	KUNIT_EXPECT_FALSE(test,
			   degrades);
	degrades = qcom_nspm_create_error_degrades(QCOM_NSPM_AEE_EQURTBADASID, false);
	KUNIT_EXPECT_FALSE(test,
			   degrades);
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_create_error_degrades(0, true));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_create_error_degrades(-EIO, true));
}

static void qcom_nspm_notification_lifetime_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_notification_is_current(ms_to_ktime(10),
							     ms_to_ktime(20)));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_notification_is_current(ms_to_ktime(20),
							    ms_to_ktime(20)));
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_notification_is_current(ms_to_ktime(30),
							    ms_to_ktime(20)));
}

static void qcom_nspm_release_completion_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  qcom_nspm_release_is_complete(QCOM_NSPM_QUIESCING,
							true, 0, 0));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_release_is_complete(QCOM_NSPM_QUIESCING,
							 false, 0, 0));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_release_is_complete(QCOM_NSPM_QUIESCING,
							 true, 1, 0));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_release_is_complete(QCOM_NSPM_QUIESCING,
							 true, 0, 1));
	KUNIT_EXPECT_FALSE(test,
			   qcom_nspm_release_is_complete(QCOM_NSPM_RELEASING,
							 true, 0, 0));
}

static void qcom_nspm_iommu_sid_test(struct kunit *test)
{
	struct of_phandle_args iommu = {
		.args_count = 2,
		.args = { 0x0c0c, 0x20 },
	};
	u32 sid = 0;
	int ret;

	ret = qcom_nspm_iommu_sid(&iommu, &sid);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, sid, 0x0c0c);

	iommu.args_count = 0;
	KUNIT_EXPECT_EQ(test, qcom_nspm_iommu_sid(&iommu, &sid), -EINVAL);
}

static void qcom_nspm_mapping_identity_test(struct kunit *test)
{
	struct qcom_nspm_mapping_identity identity;
	u64 iova = 0x12345000;
	u64 dsp_address = (0x0c0cULL << 32) | iova;
	int ret;

	ret = qcom_nspm_decode_mapping_identity(dsp_address, iova, 0x0c0c,
						 32, &identity);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, identity.encoded_sid, 0x0c0cU);
	KUNIT_EXPECT_EQ(test, identity.encoded_iova, iova);
	KUNIT_EXPECT_TRUE(test, identity.sid_matches);
	KUNIT_EXPECT_TRUE(test, identity.iova_matches);

	ret = qcom_nspm_decode_mapping_identity(dsp_address, iova + PAGE_SIZE,
						 0x0c08, 32, &identity);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, identity.sid_matches);
	KUNIT_EXPECT_FALSE(test, identity.iova_matches);
}

static void qcom_nspm_mapping_identity_invalid_shift_test(struct kunit *test)
{
	struct qcom_nspm_mapping_identity identity;

	KUNIT_EXPECT_EQ(test,
			qcom_nspm_decode_mapping_identity(0, 0, 0, 0,
							  &identity),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			qcom_nspm_decode_mapping_identity(0, 0, 0, 64,
							  &identity),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			qcom_nspm_decode_mapping_identity(0, 0, 0, 32, NULL),
			-EINVAL);
}

static struct kunit_case qcom_nspm_test_cases[] = {
	KUNIT_CASE(qcom_nspm_valid_transitions_test),
	KUNIT_CASE(qcom_nspm_exception_transitions_test),
	KUNIT_CASE(qcom_nspm_rejected_transitions_test),
	KUNIT_CASE(qcom_nspm_generation_order_test),
	KUNIT_CASE(qcom_nspm_notification_order_test),
	KUNIT_CASE(qcom_nspm_state_metadata_test),
	KUNIT_CASE(qcom_nspm_vote_policy_test),
	KUNIT_CASE(qcom_nspm_notification_client_test),
	KUNIT_CASE(qcom_nspm_terminal_notification_test),
	KUNIT_CASE(qcom_nspm_terminal_latch_test),
	KUNIT_CASE(qcom_nspm_reservation_policy_test),
	KUNIT_CASE(qcom_nspm_integrity_error_test),
	KUNIT_CASE(qcom_nspm_notification_lifetime_test),
	KUNIT_CASE(qcom_nspm_release_completion_test),
	KUNIT_CASE(qcom_nspm_iommu_sid_test),
	KUNIT_CASE(qcom_nspm_mapping_identity_test),
	KUNIT_CASE(qcom_nspm_mapping_identity_invalid_shift_test),
	{ }
};

static struct kunit_suite qcom_nspm_test_suite = {
	.name = "qcom-nspm-state",
	.test_cases = qcom_nspm_test_cases,
};

kunit_test_suite(qcom_nspm_test_suite);

MODULE_DESCRIPTION("Qualcomm NSP manager lifecycle KUnit tests");
MODULE_LICENSE("GPL");
