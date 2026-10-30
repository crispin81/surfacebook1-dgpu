// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Discrete GPU power switch for the Microsoft Surface Book 1.
 *
 * Based on the linux-surface surfacebook1_dgpu_switch driver by
 * Maximilian Luz. Additions for running on kernels without the
 * linux-surface patches, and for powering the dGPU back on after it has
 * been switched off, by Chris Cork.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/delay.h>

/* MSHW0040/VGBI DSM UUID: 6fd05c69-cde3-49f4-95ed-ab1665498035 */
static const guid_t dgpu_sw_guid =
	GUID_INIT(0x6fd05c69, 0xcde3, 0x49f4,
		  0x95, 0xed, 0xab, 0x16, 0x65, 0x49, 0x80, 0x35);

#define DGPUSW_ACPI_PATH_DSM	"\\_SB_.PCI0.LPCB.EC0_.VGBI"
#define DGPUSW_ACPI_PATH_HGON	"\\_SB_.PCI0.RP05.HGON"
#define DGPUSW_ACPI_PATH_HGOF	"\\_SB_.PCI0.RP05.HGOF"
#define DGPUSW_ACPI_PATH_RP05	"\\_SB_.PCI0.RP05"
#define DGPUSW_ACPI_PATH_GLNK	"\\_SB_.PCI0.RP05.GLNK"
#define DGPUSW_ACPI_PATH_REGR	"\\_SB_.PCI0.RP05.REGR"

/* RP05 config space fields used by HGON (see DSDT RP05.PXCS) */
#define RP_LNKSTA		0x52
#define RP_LNKSTA_LASX		BIT(13)
#define RP_324		0x324
#define RP_324_LEDM		BIT(3)
#define RP_420		0x420
#define RP_420_DPGE	BIT(30)

/*
 * RawBytes GenericSerialBus support for the SAM I2C device.
 *
 * Note: i2c_acpi_space_handler is static, so its address is looked up via a
 * kprobe (CONFIG_KPROBES is required). On linux-surface kernels the I2C core
 * already handles RawBytes; the wrapper is harmless there.
 *
 * HGON/HGOF talk to \_SB.PCI0.I2C0.SAM via RawBytes accesses, which mainline
 * i2c-core-acpi rejects. linux-surface patches the I2C core for this
 * ("i2c: acpi: Implement RawBytes read access"); since the I2C core is
 * built in, we instead wrap its operation region handler on I2C0 and handle
 * RawBytes writes here, passing everything else through.
 */
#define SB1_I2C_ACPI_PATH	"\\_SB_.PCI0.I2C0"

/* Mirrors struct i2c_acpi_handler_data in drivers/i2c/i2c-core-acpi.c */
struct sb1_i2c_handler_data {
	struct acpi_connection_info info;
	struct i2c_adapter *adapter;
};

/* Mirrors struct gsb_buffer in drivers/i2c/i2c-core-acpi.c */
struct sb1_gsb_buffer {
	u8	status;
	u8	len;
	union {
		u16	wdata;
		u8	bdata;
		DECLARE_FLEX_ARRAY(u8, data);
	};
} __packed;

static acpi_adr_space_handler sb1_orig_handler;
static void *sb1_handler_ctx;
static acpi_handle sb1_i2c_handle;

static int sb1_write_raw_bytes(struct i2c_client *client, u8 *data, u8 data_len)
{
	struct i2c_msg msgs[1];
	int ret;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = data_len + 1;
	msgs[0].buf = data;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		dev_err(&client->adapter->dev, "i2c write failed: %d\n", ret);
		return ret;
	}

	return (ret == 1) ? 0 : -EIO;
}

static acpi_status sb1_space_handler(u32 function, acpi_physical_address command,
				     u32 bits, u64 *value64,
				     void *handler_context, void *region_context)
{
	struct sb1_gsb_buffer *gsb = (struct sb1_gsb_buffer *)value64;
	struct sb1_i2c_handler_data *data = handler_context;
	struct acpi_connection_info *info = &data->info;
	struct acpi_resource_i2c_serialbus *sb;
	struct i2c_client client = { .adapter = data->adapter };
	struct acpi_resource *ares;
	acpi_status ret;

	if ((function >> 16) != ACPI_GSB_ACCESS_ATTRIB_RAW_BYTES ||
	    (function & ACPI_IO_MASK) == ACPI_READ)
		return sb1_orig_handler(function, command, bits, value64,
					handler_context, region_context);

	ret = acpi_buffer_to_resource(info->connection, info->length, &ares);
	if (ACPI_FAILURE(ret))
		return ret;

	if (!value64 || !i2c_acpi_get_i2c_resource(ares, &sb)) {
		ACPI_FREE(ares);
		return AE_BAD_PARAMETER;
	}

	client.addr = sb->slave_address;
	if (sb->access_mode == ACPI_I2C_10BIT_MODE)
		client.flags |= I2C_CLIENT_TEN;

	gsb->status = sb1_write_raw_bytes(&client, gsb->data, info->access_length);

	ACPI_FREE(ares);
	return AE_OK;
}

static int sb1_lookup_orig_handler(void)
{
	struct kprobe kp = { .symbol_name = "i2c_acpi_space_handler" };
	u8 *addr;
	u32 insn;
	int ret;

	ret = register_kprobe(&kp);
	if (ret)
		return ret;
	addr = (u8 *)kp.addr;
	unregister_kprobe(&kp);

	/* With IBT, kprobes land after the ENDBR64 at the function entry. */
	if (!copy_from_kernel_nofault(&insn, addr - 4, sizeof(insn)) &&
	    insn == 0xfa1e0ff3)
		addr -= 4;

	sb1_orig_handler = (acpi_adr_space_handler)addr;
	return 0;
}

static int sb1_space_handler_install(void)
{
	acpi_status status;
	int ret;

	status = acpi_get_handle(NULL, SB1_I2C_ACPI_PATH, &sb1_i2c_handle);
	if (ACPI_FAILURE(status)) {
		pr_err("no %s: %d\n", SB1_I2C_ACPI_PATH, status);
		return -ENODEV;
	}

	ret = sb1_lookup_orig_handler();
	if (ret) {
		pr_err("can't find i2c_acpi_space_handler: %d\n", ret);
		return ret;
	}

	status = acpi_bus_get_private_data(sb1_i2c_handle, &sb1_handler_ctx);
	if (ACPI_FAILURE(status) || !sb1_handler_ctx) {
		pr_err("no I2C0 handler data: %d\n", status);
		return -ENODEV;
	}

	status = acpi_remove_address_space_handler(sb1_i2c_handle, ACPI_ADR_SPACE_GSBUS,
						   sb1_orig_handler);
	if (ACPI_FAILURE(status)) {
		pr_err("can't remove I2C0 handler: %d\n", status);
		return -ENODEV;
	}

	status = acpi_install_address_space_handler(sb1_i2c_handle, ACPI_ADR_SPACE_GSBUS,
						    sb1_space_handler, NULL, sb1_handler_ctx);
	if (ACPI_FAILURE(status)) {
		acpi_install_address_space_handler(sb1_i2c_handle, ACPI_ADR_SPACE_GSBUS,
						   sb1_orig_handler, NULL, sb1_handler_ctx);
		pr_err("can't install I2C0 handler: %d\n", status);
		return -ENODEV;
	}

	pr_info("RawBytes handler installed on I2C0\n");
	return 0;
}

static void sb1_space_handler_remove(void)
{
	acpi_remove_address_space_handler(sb1_i2c_handle, ACPI_ADR_SPACE_GSBUS,
					  sb1_space_handler);
	acpi_install_address_space_handler(sb1_i2c_handle, ACPI_ADR_SPACE_GSBUS,
					   sb1_orig_handler, NULL, sb1_handler_ctx);
}

static int sb1_dgpu_sw_dsmcall(void)
{
	union acpi_object *obj;
	acpi_handle handle;
	acpi_status status;

	status = acpi_get_handle(NULL, DGPUSW_ACPI_PATH_DSM, &handle);
	if (status)
		return -EINVAL;

	obj = acpi_evaluate_dsm_typed(handle, &dgpu_sw_guid, 1, 1, NULL, ACPI_TYPE_BUFFER);
	if (!obj)
		return -EINVAL;

	ACPI_FREE(obj);
	return 0;
}

static int sb1_dgpu_sw_glnk(void)
{
	unsigned long long glnk;

	if (ACPI_FAILURE(acpi_evaluate_integer(NULL, DGPUSW_ACPI_PATH_GLNK, NULL, &glnk)))
		return -EINVAL;
	return glnk;
}

/*
 * After HGOF has put the link into L2/L3 Ready, HGON requests the L23 exit
 * (L23R) and waits 128ms for it to clear before re-enabling dynamic power
 * gating (DPGE) and clearing LEDM. But on this hardware L23R only clears
 * after those later steps, so HGON gives up midway and leaves the link down.
 * Finish the rest of HGON here: DPGE=1, wait for LASX, LEDM=0, REGR.
 */
static int sb1_dgpu_sw_finish_link(struct device *dev)
{
	struct pci_dev *rp;
	acpi_handle handle;
	u16 lnksta = 0;
	u32 val;
	int i;

	if (ACPI_FAILURE(acpi_get_handle(NULL, DGPUSW_ACPI_PATH_RP05, &handle)))
		return -ENODEV;

	rp = acpi_get_pci_dev(handle);
	if (!rp)
		return -ENODEV;

	pci_read_config_dword(rp, RP_420, &val);
	pci_write_config_dword(rp, RP_420, val | RP_420_DPGE);

	for (i = 0; i < 50; i++) {
		pci_read_config_word(rp, RP_LNKSTA, &lnksta);
		if (lnksta & RP_LNKSTA_LASX)
			break;
		msleep(10);
	}

	pci_read_config_dword(rp, RP_324, &val);
	pci_write_config_dword(rp, RP_324, val & ~RP_324_LEDM);

	pci_dev_put(rp);

	acpi_evaluate_object(NULL, DGPUSW_ACPI_PATH_REGR, NULL, NULL);

	if (!(lnksta & RP_LNKSTA_LASX)) {
		dev_err(dev, "dGPU link did not come up after HGON\n");
		return -EIO;
	}

	dev_info(dev, "completed HGON link bring-up\n");
	return 0;
}

static int sb1_dgpu_sw_hgon(struct device *dev)
{
	struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
	acpi_status status;
	int ret;

	status = acpi_evaluate_object(NULL, DGPUSW_ACPI_PATH_HGON, NULL, &buf);
	if (status) {
		dev_err(dev, "failed to run HGON: %d\n", status);
		return -EINVAL;
	}

	ACPI_FREE(buf.pointer);

	/* HGON sets GLNK only once the link is up; finish the job if it gave up. */
	if (sb1_dgpu_sw_glnk() == 0) {
		ret = sb1_dgpu_sw_finish_link(dev);
		if (ret)
			return ret;
	}

	dev_info(dev, "turned-on dGPU via HGON\n");
	return 0;
}

static int sb1_dgpu_sw_hgof(struct device *dev)
{
	struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
	acpi_status status;

	status = acpi_evaluate_object(NULL, DGPUSW_ACPI_PATH_HGOF, NULL, &buf);
	if (status) {
		dev_err(dev, "failed to run HGOF: %d\n", status);
		return -EINVAL;
	}

	ACPI_FREE(buf.pointer);

	dev_info(dev, "turned-off dGPU via HGOF\n");
	return 0;
}

static ssize_t dgpu_dsmcall_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t len)
{
	bool value;
	int status;

	status = kstrtobool(buf, &value);
	if (status < 0)
		return status;

	if (!value)
		return 0;

	status = sb1_dgpu_sw_dsmcall();

	return status < 0 ? status : len;
}
static DEVICE_ATTR_WO(dgpu_dsmcall);

static ssize_t dgpu_power_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t len)
{
	bool power;
	int status;

	status = kstrtobool(buf, &power);
	if (status < 0)
		return status;

	if (power)
		status = sb1_dgpu_sw_hgon(dev);
	else
		status = sb1_dgpu_sw_hgof(dev);

	return status < 0 ? status : len;
}
static DEVICE_ATTR_WO(dgpu_power);

static struct attribute *sb1_dgpu_sw_attrs[] = {
	&dev_attr_dgpu_dsmcall.attr,
	&dev_attr_dgpu_power.attr,
	NULL
};
ATTRIBUTE_GROUPS(sb1_dgpu_sw);

/*
 * The dGPU power seems to be actually handled by MSHW0040. However, that is
 * also the power-/volume-button device with a mainline driver. So let's use
 * MSHW0041 instead for now, which seems to be the LTCH (latch/DTX) device.
 */
static const struct acpi_device_id sb1_dgpu_sw_match[] = {
	{ "MSHW0041", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sb1_dgpu_sw_match);

static struct platform_driver sb1_dgpu_sw = {
	.driver = {
		.name = "surfacebook1_dgpu_switch",
		.acpi_match_table = sb1_dgpu_sw_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.dev_groups = sb1_dgpu_sw_groups,
	},
};

static int __init sb1_dgpu_sw_init(void)
{
	int ret;

	ret = sb1_space_handler_install();
	if (ret)
		return ret;

	ret = platform_driver_register(&sb1_dgpu_sw);
	if (ret)
		sb1_space_handler_remove();
	return ret;
}
module_init(sb1_dgpu_sw_init);

static void __exit sb1_dgpu_sw_exit(void)
{
	platform_driver_unregister(&sb1_dgpu_sw);
	sb1_space_handler_remove();
}
module_exit(sb1_dgpu_sw_exit);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_AUTHOR("Chris Cork");
MODULE_DESCRIPTION("Discrete GPU Power-Switch for Surface Book 1");
MODULE_LICENSE("GPL");
