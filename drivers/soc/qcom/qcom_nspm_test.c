// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

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

static struct kunit_case qcom_nspm_test_cases[] = {
	KUNIT_CASE(qcom_nspm_valid_transitions_test),
	KUNIT_CASE(qcom_nspm_exception_transitions_test),
	KUNIT_CASE(qcom_nspm_rejected_transitions_test),
	{ }
};

static struct kunit_suite qcom_nspm_test_suite = {
	.name = "qcom-nspm-state",
	.test_cases = qcom_nspm_test_cases,
};

kunit_test_suite(qcom_nspm_test_suite);

MODULE_DESCRIPTION("Qualcomm NSP manager lifecycle KUnit tests");
MODULE_LICENSE("GPL");
