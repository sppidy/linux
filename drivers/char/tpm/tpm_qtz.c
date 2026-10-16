// SPDX-License-Identifier: GPL-2.0
/*
 * TPM 2.0 driver for the Qualcomm "qcom.tz.tpm" QSEE fTPM on Snapdragon X
 * laptops booted with devicetree (ACPI TPM2 start method 9).
 *
 * Protocol, as used by the Windows QcTrEE.sys TPM service:
 *   0x1000  query TA version            -> {status, cmd, version}
 *   0x4012  query CRB physical address  -> {u32, u64 crb_pa}
 *   0x4008  process the command in CRB  -> 0x404-byte response
 * The TPM command and response live in the CRB buffer; the 0x4008 message
 * is only a doorbell.
 */
#include <linux/delay.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/tpm.h>
#include <linux/unaligned.h>

#include "tpm.h"

#define QTZ_CMD_VERSION		0x1000
#define QTZ_CMD_PROCESS		0x4008
#define QTZ_CMD_GET_CRB		0x4012
#define QTZ_TA_VERSION		0x00010000
#define QTZ_CRB_MAP_SIZE	0x3000
#define QTZ_PROCESS_LEN		0x400
#define QTZ_PROCESS_RSP_LEN	0x404
#define QTZ_SHM_BRIDGE_NUM_VM_SHIFT	9

/* CRB control area (TCG PC Client PTP, without the locality registers). */
#define CRB_CTRL_REQ		0x00
#define CRB_CTRL_REQ_CMD_READY	BIT(0)
#define CRB_CTRL_REQ_GO_IDLE	BIT(1)
#define CRB_CTRL_STS		0x04
#define CRB_CTRL_CANCEL		0x08
#define CRB_CTRL_START		0x0c
#define CRB_CTRL_CMD_SIZE	0x18
#define CRB_CTRL_CMD_PA_LO	0x1c
#define CRB_CTRL_CMD_PA_HI	0x20
#define CRB_CTRL_RSP_SIZE	0x24
#define CRB_CTRL_RSP_PA		0x28

struct tpm_qtz {
	struct device *dev;
	struct qcom_tzmem_pool *pool;
	void __iomem *crb;
	phys_addr_t crb_pa;
	u32 app_id;
	void __iomem *cmd;
	void __iomem *rsp;
	u32 cmd_size;
	u32 rsp_size;
	u64 bridge;
};

static void tpm_qtz_bridge_delete(void *data)
{
	struct tpm_qtz *priv = data;

	qcom_scm_shm_bridge_delete(priv->bridge);
}

/*
 * Register the CRB with QTEE as a non-secure shared buffer. The TA's fTPM
 * command (QTZ_CMD_PROCESS) maps the CRB this way before it touches it, and it
 * only accepts the HLOS-vmid bridge form: the self-owner form that
 * qcom_tzmem_shm_bridge_create() derives from the DT "qcom,shm-bridge-vmid"
 * (0xffffffff on this platform) makes the TA reject the buffer and leave the
 * command unprocessed. So build the HLOS form by hand.
 */
static int tpm_qtz_bridge_create(struct tpm_qtz *priv)
{
	u64 size_and_flags = QTZ_CRB_MAP_SIZE | (1ULL << QTZ_SHM_BRIDGE_NUM_VM_SHIFT);

	return qcom_scm_shm_bridge_create(priv->crb_pa | QCOM_SCM_PERM_RW,
					  priv->crb_pa | QCOM_SCM_PERM_RW,
					  size_and_flags, QCOM_SCM_VMID_HLOS,
					  &priv->bridge);
}

static int tpm_qtz_app_send(struct tpm_qtz *priv, const void *req, size_t req_len,
			    void *rsp, size_t rsp_len)
{
	void *treq, *trsp;
	int ret;

	treq = qcom_tzmem_alloc(priv->pool, req_len, GFP_KERNEL);
	trsp = qcom_tzmem_alloc(priv->pool, rsp_len, GFP_KERNEL);
	if (!treq || !trsp) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(treq, req, req_len);
	memset(trsp, 0, rsp_len);
	ret = qcom_scm_qseecom_app_send(priv->app_id, treq, req_len, trsp, rsp_len);
	if (!ret)
		memcpy(rsp, trsp, rsp_len);
out:
	if (trsp)
		qcom_tzmem_free(trsp);
	if (treq)
		qcom_tzmem_free(treq);
	return ret;
}

static int tpm_qtz_query(struct tpm_qtz *priv, u32 cmd, u8 rsp[12])
{
	return tpm_qtz_app_send(priv, &cmd, sizeof(cmd), rsp, 12);
}

/* Ask the TA to act on the CRB control area (QcTrEE.sys TpmServiceInvoke). */
static int tpm_qtz_doorbell(struct tpm_qtz *priv)
{
	struct {
		u32 cmd;
		u32 len;
		u8 pad[QTZ_PROCESS_LEN];
	} *req __free(kfree) = kzalloc(sizeof(*req), GFP_KERNEL);
	u8 *rsp __free(kfree) = kzalloc(QTZ_PROCESS_RSP_LEN, GFP_KERNEL);
	int ret;

	if (!req || !rsp)
		return -ENOMEM;

	wmb();
	req->cmd = QTZ_CMD_PROCESS;
	req->len = QTZ_PROCESS_LEN;
	ret = tpm_qtz_app_send(priv, req, sizeof(*req), rsp, QTZ_PROCESS_RSP_LEN);
	if (ret)
		dev_err(priv->dev, "doorbell failed: %d\n", ret);
	return ret;
}

static void tpm_qtz_dump_ctrl(struct tpm_qtz *priv, const char *what)
{
	u8 rsp[16];

	memcpy_fromio(rsp, priv->rsp, sizeof(rsp));
	dev_info(priv->dev, "%s: req=%x sts=%x cancel=%x start=%x rsp %*ph\n", what,
		 ioread32(priv->crb + CRB_CTRL_REQ), ioread32(priv->crb + CRB_CTRL_STS),
		 ioread32(priv->crb + CRB_CTRL_CANCEL),
		 ioread32(priv->crb + CRB_CTRL_START), 16, rsp);
}

/*
 * The TA never sees CTRL_REQ: QcTrEE.sys (start-method handler 0x27290)
 * clears cmdReady/goIdle itself for an fTPM and only forwards START to the
 * TA as QTZ_CMD_PROCESS.
 */
static int tpm_qtz_req(struct tpm_chip *chip, u32 bit)
{
	struct tpm_qtz *priv = dev_get_drvdata(&chip->dev);

	iowrite32(ioread32(priv->crb + CRB_CTRL_REQ) & ~bit, priv->crb + CRB_CTRL_REQ);
	return 0;
}

static int tpm_qtz_cmd_ready(struct tpm_chip *chip)
{
	return tpm_qtz_req(chip, CRB_CTRL_REQ_CMD_READY);
}

static int tpm_qtz_go_idle(struct tpm_chip *chip)
{
	return tpm_qtz_req(chip, CRB_CTRL_REQ_GO_IDLE);
}

static int tpm_qtz_send(struct tpm_chip *chip, u8 *buf, size_t bufsiz,
			size_t cmd_len)
{
	struct tpm_qtz *priv = dev_get_drvdata(&chip->dev);
	u8 hdr[TPM_HEADER_SIZE];
	size_t len;
	u32 val;
	int ret;

	if (cmd_len > priv->cmd_size)
		return -E2BIG;

	/* tpm.sys refuses to start a command unless START and STS are both 0. */
	if (ioread32(priv->crb + CRB_CTRL_START) || ioread32(priv->crb + CRB_CTRL_STS)) {
		tpm_qtz_dump_ctrl(priv, "busy before start");
		return -EBUSY;
	}

	iowrite32(0, priv->crb + CRB_CTRL_CANCEL);
	memcpy_toio(priv->cmd, buf, cmd_len);
	wmb();
	iowrite32(1, priv->crb + CRB_CTRL_START);
	ret = tpm_qtz_doorbell(priv);
	if (ret)
		return ret;

	ret = readl_poll_timeout(priv->crb + CRB_CTRL_START, val, !(val & 1), 1000,
				 2 * USEC_PER_SEC);
	if (ret) {
		tpm_qtz_dump_ctrl(priv, "TA did not complete command");
		return -ETIME;
	}

	rmb();
	/* The response buffer is only 16-bit aligned at +2: never do unaligned MMIO. */
	memcpy_fromio(hdr, priv->rsp, sizeof(hdr));
	len = get_unaligned_be32(hdr + 2);
	if (len < TPM_HEADER_SIZE || len > bufsiz || len > priv->rsp_size) {
		dev_err(&chip->dev, "bad response length %zu\n", len);
		return -EIO;
	}
	memcpy_fromio(buf, priv->rsp, len);
	return len;
}

static const struct tpm_class_ops tpm_qtz_ops = {
	.flags = TPM_OPS_AUTO_STARTUP,
	.send = tpm_qtz_send,
	.cmd_ready = tpm_qtz_cmd_ready,
	.go_idle = tpm_qtz_go_idle,
};

static int tpm_qtz_probe(struct platform_device *pdev)
{
	struct qcom_tzmem_pool_config cfg = {
		.initial_size = SZ_64K,
		.policy = QCOM_TZMEM_POLICY_STATIC,
	};
	struct device *dev = &pdev->dev;
	struct tpm_qtz *priv;
	struct tpm_chip *chip;
	u64 cmd_pa, rsp_pa;
	u8 rsp[12];
	u32 ver;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	ret = qcom_scm_qseecom_app_get_id("qcom.tz.tpm", &priv->app_id);
	if (ret)
		return dev_err_probe(dev, -ENODEV, "qcom.tz.tpm app not found: %d\n", ret);

	priv->pool = devm_qcom_tzmem_pool_new(dev, &cfg);
	if (IS_ERR(priv->pool))
		return PTR_ERR(priv->pool);

	ret = tpm_qtz_query(priv, QTZ_CMD_VERSION, rsp);
	ver = get_unaligned_le32(rsp + 8);
	if (ret || get_unaligned_le32(rsp) || ver != QTZ_TA_VERSION)
		return dev_err_probe(dev, -ENODEV, "unexpected TA version %*ph (%d)\n",
				     12, rsp, ret);

	ret = tpm_qtz_query(priv, QTZ_CMD_GET_CRB, rsp);
	priv->crb_pa = get_unaligned_le64(rsp + 4);
	if (ret || !priv->crb_pa)
		return dev_err_probe(dev, -ENODEV, "no CRB address %*ph (%d)\n",
				     12, rsp, ret);

	ret = tpm_qtz_bridge_create(priv);
	if (ret)
		return dev_err_probe(dev, ret, "CRB SHM bridge failed\n");
	ret = devm_add_action_or_reset(dev, tpm_qtz_bridge_delete, priv);
	if (ret)
		return ret;

	priv->crb = devm_ioremap(dev, priv->crb_pa, QTZ_CRB_MAP_SIZE);
	if (!priv->crb)
		return -ENOMEM;

	priv->cmd_size = ioread32(priv->crb + CRB_CTRL_CMD_SIZE);
	priv->rsp_size = ioread32(priv->crb + CRB_CTRL_RSP_SIZE);
	cmd_pa = ((u64)ioread32(priv->crb + CRB_CTRL_CMD_PA_HI) << 32) |
		 ioread32(priv->crb + CRB_CTRL_CMD_PA_LO);
	rsp_pa = lo_hi_readq(priv->crb + CRB_CTRL_RSP_PA);
	if (cmd_pa < priv->crb_pa || rsp_pa < priv->crb_pa ||
	    cmd_pa + priv->cmd_size > priv->crb_pa + QTZ_CRB_MAP_SIZE ||
	    rsp_pa + priv->rsp_size > priv->crb_pa + QTZ_CRB_MAP_SIZE ||
	    priv->cmd_size < TPM_HEADER_SIZE || priv->rsp_size < TPM_HEADER_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "CRB buffers out of range: cmd %llx/%x rsp %llx/%x\n",
				     cmd_pa, priv->cmd_size, rsp_pa, priv->rsp_size);
	priv->cmd = priv->crb + (cmd_pa - priv->crb_pa);
	priv->rsp = priv->crb + (rsp_pa - priv->crb_pa);

	dev_info(dev, "qcom.tz.tpm app %u, TA v1.0, CRB %pa (cmd %x rsp %x)\n",
		 priv->app_id, &priv->crb_pa, priv->cmd_size, priv->rsp_size);

	/* Clear a cmdReady/goIdle left pending by firmware or an earlier load. */
	iowrite32(0, priv->crb + CRB_CTRL_REQ);

	chip = tpmm_chip_alloc(dev, &tpm_qtz_ops);
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	chip->flags |= TPM_CHIP_FLAG_TPM2 | TPM_CHIP_FLAG_SYNC;
	dev_set_drvdata(&chip->dev, priv);

	return tpm_chip_register(chip);
}

static void tpm_qtz_remove(struct platform_device *pdev)
{
	struct tpm_chip *chip = platform_get_drvdata(pdev);

	tpm_chip_unregister(chip);
}

static struct platform_driver tpm_qtz_driver = {
	.probe = tpm_qtz_probe,
	.remove = tpm_qtz_remove,
	.driver = {
		.name = "tpm_qtz",
	},
};

static struct platform_device *tpm_qtz_pdev;

static int __init tpm_qtz_init(void)
{
	int ret;

	ret = platform_driver_register(&tpm_qtz_driver);
	if (ret)
		return ret;

	tpm_qtz_pdev = platform_device_register_simple("tpm_qtz", -1, NULL, 0);
	if (IS_ERR(tpm_qtz_pdev)) {
		platform_driver_unregister(&tpm_qtz_driver);
		return PTR_ERR(tpm_qtz_pdev);
	}
	return 0;
}
module_init(tpm_qtz_init);

static void __exit tpm_qtz_exit(void)
{
	platform_device_unregister(tpm_qtz_pdev);
	platform_driver_unregister(&tpm_qtz_driver);
}
module_exit(tpm_qtz_exit);

MODULE_DESCRIPTION("TPM driver for Qualcomm qcom.tz.tpm QSEE fTPM (start method 9)");
MODULE_LICENSE("GPL");
