/*
 * Copyright (c) 2026 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_xec_cct_ktimer

#include <soc.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/common/sys_io.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/drivers/timer/system_timer_lpm.h>

/* Capture Compare Timer (CCT) register offsets */
#define XEC_CCT_CR_OFS       0
#define XEC_CCT_CAP_CR0_OFS  0x04U
#define XEC_CCT_CAP_CR1_OFS  0x08U
#define XEC_CCT_FRT_CNT_OFS  0x0CU /* 32-bit r/w */
#define XEC_CCT_CAP0_OFS     0x10U /* all capture regs are 32-bit read-only */
#define XEC_CCT_CAP1_OFS     0x14U
#define XEC_CCT_CAP2_OFS     0x18U
#define XEC_CCT_CAP3_OFS     0x1CU
#define XEC_CCT_CAP4_OFS     0x20U
#define XEC_CCT_CAP5_OFS     0x24U
#define XEC_CCT_COMP0_OFS    0x28U /* 32-bit r/w */
#define XEC_CCT_COMP1_OFS    0x2CU /* 32-bit r/w */
#define XEC_CCT_INMUX_OFS    0x30U

/* Control TCLK and FCLK field values */
#define XEC_CCT_TFCLK_VAL_DIV1   0
#define XEC_CCT_TFCLK_VAL_DIV2   1U
#define XEC_CCT_TFCLK_VAL_DIV4   2U
#define XEC_CCT_TFCLK_VAL_DIV8   3U
#define XEC_CCT_TFCLK_VAL_DIV16  4U
#define XEC_CCT_TFCLK_VAL_DIV32  5U
#define XEC_CCT_TFCLK_VAL_DIV64  6U
#define XEC_CCT_TFCLK_VAL_DIV128 7U

/* Capture Control Edge field values */
#define XEC_CCT_CAP_CR_FE  0
#define XEC_CCT_CAP_CR_RE  1U
#define XEC_CCT_CAP_CR_BE  2U
#define XEC_CCT_CAP_CR_DIS 3U

/* Control fields: all fields r/w unless otherwise noted */
#define XEC_CCT_CR_ACTV_POS      0
#define XEC_CCT_CR_FRT_EN_POS    1
#define XEC_CCT_CR_FRT_RST_POS   2 /* self-clearing */
#define XEC_CCT_CR_TCLK_POS      4
#define XEC_CCT_CR_TCLK_MSK      GENMASK(6, 4)
#define XEC_CCT_CR_TCLK_SET(v)   FIELD_PREP(XEC_CCT_CR_TCLK_MSK, (v))
#define XEC_CCT_CR_TCLK_GET(r)   FIELD_GET(XEC_CCT_CR_TCLK_MSK, (r))
#define XEC_CCT_CR_COMP0_EN_POS  8
#define XEC_CCT_CR_COMP1_EN_POS  9
#define XEC_CCT_CR_COMP1_SET_POS 16 /* r/w-1-to-set */
#define XEC_CCT_CR_COMP0_SET_POS 17 /* r/w-1-to-set */
#define XEC_CCT_CR_COMP1_CLR_POS 24 /* r/w-1-to-clear */
#define XEC_CCT_CR_COMP0_CLR_POS 25 /* r/w-1-to-clear */

#define XEC_CCT_COMP0_GIRQ_ENC DT_INST_PROP_BY_NAME(0, girq, comp0)
#define XEC_CCT_COMP1_GIRQ_ENC DT_INST_PROP_BY_NAME(0, girq, comp1)

#define XEC_CCT_BASE           (mm_reg_t)DT_INST_REG_ADDR(0)
#define XEC_CCT_PCR_SCR_VAL    DT_INST_PROP(0, pcr_scr)
#define XEC_CCT_COMP0_GIRQ     MCHP_XEC_ECIA_GIRQ(XEC_CCT_COMP0_GIRQ_ENC)
#define XEC_CCT_COMP0_GIRQ_POS MCHP_XEC_ECIA_GIRQ_POS(XEC_CCT_COMP0_GIRQ_ENC)
#define XEC_CCT_COMP1_GIRQ     MCHP_XEC_ECIA_GIRQ(XEC_CCT_COMP1_GIRQ_ENC)
#define XEC_CCT_COMP1_GIRQ_POS MCHP_XEC_ECIA_GIRQ_POS(XEC_CCT_COMP1_GIRQ_ENC)


#define XEC_CCT_DIVIDER XEC_CCT_TFCLK_VAL_DIV4

static int sys_clock_driver_init(void)
{
	mm_reg_t regbase = XEC_CCT_BASE;
	uint32_t v = 0;

	soc_ecia_girq_ctrl(XEC_CCT_COMP0_GIRQ, XEC_CCT_COMP0_GIRQ_POS, 0);
	soc_ecia_girq_ctrl(XEC_CCT_COMP0_GIRQ, XEC_CCT_COMP1_GIRQ_POS, 0);
	soc_xec_pcr_sleep_en_clear((uint8_t)(XEC_CCT_PCR_SCR_VAL));

	v = (BIT(XEC_CCT_CR_ACTV_POS) | BIT(XEC_CCT_CR_FRT_RST_POS) |
	     BIT(XEC_CCT_CR_COMP1_CLR_POS) | BIT(XEC_CCT_CR_COMP0_CLR_POS) |
	     XEC_CCT_CR_TCLK_SET(XEC_CCT_DIVIDER));

	sys_write32(v, regbase + XEC_CCT_CR_OFS);

	soc_ecia_girq_status_clear(XEC_CCT_COMP0_GIRQ, XEC_CCT_COMP0_GIRQ_POS);
	soc_ecia_girq_status_clear(XEC_CCT_COMP1_GIRQ, XEC_CCT_COMP1_GIRQ_POS);


	/* TODO program compare register */


	/* compare 0 and 1 interrupt enables */
	soc_ecia_girq_ctrl(XEC_CCT_COMP0_GIRQ, XEC_CCT_COMP0_GIRQ_POS, 1U);
	soc_ecia_girq_ctrl(XEC_CCT_COMP0_GIRQ, XEC_CCT_COMP1_GIRQ_POS, 1U);

	/* start free runnint timer */
	sys_set_bit(regbase + XEC_CCT_CR_OFS, XEC_CCT_CR_FRT_EN_POS);

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
