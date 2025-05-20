// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 VEST
 */

#include <config.h>
#include <efi_loader.h>
#include <env.h>
#include <errno.h>
#include <init.h>
#include <miiphy.h>
#include <netdev.h>
#include <linux/delay.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm-generic/gpio.h>
#include <asm/arch/imx8mp_pins.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/gpio.h>
#include <asm/mach-imx/mxc_i2c.h>
#include <spl.h>
#include <asm/mach-imx/dma.h>
#include <power/pmic.h>
#include <usb.h>
#include <dwc3-uboot.h>
#include <mmc.h>

DECLARE_GLOBAL_DATA_PTR;

#define USB1_EN 	IMX_GPIO_NR(1, 12)
#define USB2_EN 	IMX_GPIO_NR(1, 14)

#define UART_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL1)

/* VEST-i.MX8M-PLUS-SOM-USG-001 UART_2, Console/Debug UART */
static iomux_v3_cfg_t const uart_pads[] = {
	MX8MP_PAD_UART2_RXD__UART2_DCE_RX | MUX_PAD_CTRL(UART_PAD_CTRL),
	MX8MP_PAD_UART2_TXD__UART2_DCE_TX | MUX_PAD_CTRL(UART_PAD_CTRL),
};

#if CONFIG_IS_ENABLED(EFI_HAVE_CAPSULE_SUPPORT)
struct efi_fw_image fw_images[] = {
	{
		.image_type_id = IMX_BOOT_IMAGE_GUID,
		.fw_name = u"IMX8MP-E2I-PLUS-RAW",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "mmc 2=flash-bin raw 0 0x2000 mmcpart 1",
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};

#endif /* EFI_HAVE_CAPSULE_SUPPORT */

int board_early_init_f(void)
{
	imx_iomux_v3_setup_multiple_pads(uart_pads, ARRAY_SIZE(uart_pads));
	init_uart_clk(0);

	return 0;
}

#if IS_ENABLED(CONFIG_NET)
int board_phy_config(struct phy_device *phydev)
{
	if (phydev->drv->config)
		phydev->drv->config(phydev);
	return 0;
}
#endif

static void setup_usb(void)
{
	iomux_v3_cfg_t const usb_pads[] = {
		MX8MP_PAD_GPIO1_IO10__USB1_OTG_ID | MUX_PAD_CTRL(NO_PAD_CTRL),	// USB1_ID
		MX8MP_PAD_GPIO1_IO12__GPIO1_IO12  | MUX_PAD_CTRL(NO_PAD_CTRL),	// USB1_EN
		//MX8MP_PAD_GPIO1_IO13__USB1_OTG_OC | MUX_PAD_CTRL(NO_PAD_CTRL),// USB1_OC

		MX8MP_PAD_GPIO1_IO11__USB2_OTG_ID | MUX_PAD_CTRL(NO_PAD_CTRL),	// USB2_ID
		MX8MP_PAD_GPIO1_IO14__GPIO1_IO14  | MUX_PAD_CTRL(NO_PAD_CTRL),	// USB2_EN
	};

	imx_iomux_v3_setup_multiple_pads(usb_pads, ARRAY_SIZE(usb_pads));
	gpio_request(USB1_EN, "usb1_en");
	gpio_request(USB2_EN, "usb2_en");
}

#ifdef CONFIG_USB_DWC3

#define USB_PHY_CTRL0			0xF0040
#define USB_PHY_CTRL0_REF_SSP_EN	BIT(2)

#define USB_PHY_CTRL1			0xF0044
#define USB_PHY_CTRL1_RESET		BIT(0)
#define USB_PHY_CTRL1_COMMONONN		BIT(1)
#define USB_PHY_CTRL1_ATERESET		BIT(3)
#define USB_PHY_CTRL1_VDATSRCENB0	BIT(19)
#define USB_PHY_CTRL1_VDATDETENB0	BIT(20)

#define USB_PHY_CTRL2			0xF0048
#define USB_PHY_CTRL2_TXENABLEN0	BIT(8)

#define USB_PHY_CTRL6			0xF0058

#define HSIO_GPR_BASE                               (0x32F10000U)
#define HSIO_GPR_REG_0                              (HSIO_GPR_BASE)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT    (1)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN          (0x1U << HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT)


static struct dwc3_device dwc3_device_data = {
#ifdef CONFIG_SPL_BUILD
	.maximum_speed = USB_SPEED_HIGH,
#else
	.maximum_speed = USB_SPEED_SUPER,
#endif
	.base = USB1_BASE_ADDR,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.power_down_scale = 2,
};

int dm_usb_gadget_handle_interrupts(struct udevice *dev)
{
	dwc3_uboot_handle_interrupt(dev);
	return 0;
}

static void dwc3_nxp_usb_phy_init(struct dwc3_device *dwc3)
{
	u32 RegData;

	/* enable usb clock via hsio gpr */
	RegData = readl(HSIO_GPR_REG_0);
	RegData |= HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN;
	writel(RegData, HSIO_GPR_REG_0);

	/* USB3.0 PHY signal fsel for 100M ref */
	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData = (RegData & 0xfffff81f) | (0x2a<<5);
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL6);
	RegData &=~0x1;
	writel(RegData, dwc3->base + USB_PHY_CTRL6);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_VDATSRCENB0 | USB_PHY_CTRL1_VDATDETENB0 |
			USB_PHY_CTRL1_COMMONONN);
	RegData |= USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET;
	writel(RegData, dwc3->base + USB_PHY_CTRL1);

	RegData = readl(dwc3->base + USB_PHY_CTRL0);
	RegData |= USB_PHY_CTRL0_REF_SSP_EN;
	writel(RegData, dwc3->base + USB_PHY_CTRL0);

	RegData = readl(dwc3->base + USB_PHY_CTRL2);
	RegData |= USB_PHY_CTRL2_TXENABLEN0;
	writel(RegData, dwc3->base + USB_PHY_CTRL2);

	RegData = readl(dwc3->base + USB_PHY_CTRL1);
	RegData &= ~(USB_PHY_CTRL1_RESET | USB_PHY_CTRL1_ATERESET);
	writel(RegData, dwc3->base + USB_PHY_CTRL1);
}
#endif	/* CONFIG_USB_DWC3 */

int board_usb_init(int index, enum usb_init_type init)
{
	switch(init) {
	case USB_INIT_HOST:
		imx8m_usb_power(index, true);					// turn on imx8mp USB power
		gpio_direction_output(index == 0 ? USB1_EN : USB2_EN, 1);	// turn on board  USB power
		return 0;

	case USB_INIT_DEVICE:
		if (0 != index)
			return 0;
		dwc3_nxp_usb_phy_init(&dwc3_device_data);
		return dwc3_uboot_init(&dwc3_device_data);

	case USB_INIT_UNKNOWN:
		return 0;
	}

	return 0;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	switch(init) {
	case USB_INIT_HOST:
		gpio_direction_output(index == 0 ? USB1_EN : USB2_EN, 0);	// turn off board  USB power
		imx8m_usb_power(index, false);					// turn off imx8mp USB power
		return 0;

	case USB_INIT_DEVICE:
		if (0 == index)
			dwc3_uboot_exit(index);
		return 0;

	case USB_INIT_UNKNOWN:
		return 0;
	}

	return 0;
}

int board_init(void)
{
#if defined(CONFIG_USB_DWC3) || defined(CONFIG_USB_XHCI_IMX8M)
	init_usb_clk();
#endif
	setup_usb();

	return 0;
}

int board_late_init(void)
{
#ifdef CONFIG_ENV_IS_IN_MMC
	board_late_mmc_env_init();
#endif
#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	env_set("board_name", "E2I Plus");
	env_set("board_rev", "iMX8MP");
#endif

	return 0;
}

void board_setup_video_link(void)
{
	/* NOP atm. */
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
//	setup_lvds_touch(blob);
//	turn_off_lvds();

	return 0;
}
