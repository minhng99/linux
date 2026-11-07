// SPDX-License-Identifier: GPL-2.0-only
/* Realtek RTD1296 CRT and ISO clocks. */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/realtek,rtd1296-clk.h>

/* CRT registers */
#define CRT_CLK_EN1			0x00c
#define CRT_CLK_EN2			0x010
#define CRT_SB2_CLKSEL			0x018
#define CRT_SYS_CLKSEL			0x030
#define  CRT_SYS_CLKSEL_SYS		BIT(0)
#define  CRT_SYS_CLKSEL_SCPU		GENMASK(8, 7)
#define CRT_PLL_VE1			0x114
#define CRT_PLL_VE2			0x1d0
#define CRT_PLL_VODMA			0x260
#define CRT_PLL_VODMA_POWER		0x264
#define CRT_CLK_EN3			0x450
#define CRT_AIO_INPUT_CLOCK		0x080
#define CRT_PLL_SCPU			0x504
#define CRT_SCPU_SSC			0x500
#define CRT_SCPU_STATUS			0x51c
#define SCPU_SSC_MODE			GENMASK(2, 0)
#define SCPU_SSC_READY			BIT(20)
#define CRT_PLL_BUS			0x524
#define CRT_PLL_BUS_H			0x544
#define CRT_PLL_DDSA			0x564
#define CRT_PLL_DDSB			0x584
#define CRT_PLL_GPU			0x5a4
#define CRT_GPU_SSC			0x5a0
#define CRT_GPU_STATUS			0x5bc
#define CRT_GPU_POWER			0x1c4
#define GPU_POWER_ENABLE		GENMASK(1, 0)
#define GPU_OUTPUT_DISABLE		BIT(2)
#define GPU_SSC_MODE			GENMASK(2, 0)
#define GPU_SSC_READY			BIT(20)
#define GPU_PLL_SETTLE_US		200
#define CRT_PLL_ACPU			0x5c4
#define CRT_PLL_EMMC_PHASE		0x1f0
#define CRT_PLL_EMMC_N			0x1f8
#define CRT_PLL_SD_N			0x1e8
#define EMMC_PHASE_RESET_N		BIT(1)
#define EMMC_PHASE_BIAS			BIT(0)
#define MMC_PLL_N			GENMASK(25, 16)
#define MMC_PLL_F_SELECT			BIT(17)
#define MMC_PLL_8X_ENABLE			BIT(16)
#define MMC_PLL_DIVIDER		24

/* ISO registers */
#define ISO_CLK_EN			0x08c

/* N.F type PLL: Fout = Fin * (N + 3 + F / 2048) */
#define PLL_NF_N			GENMASK(18, 11)
#define PLL_NF_F			GENMASK(10, 0)
#define PLL_NF_F_DEN			2048

/* M/N/O type PLL: Fout = Fin * (M + 2) / (N + 1) / (O + 1) */
#define PLL_MNO_M			GENMASK(11, 4)
#define PLL_MNO_N			GENMASK(13, 12)
#define PLL_MNO_O			GENMASK(18, 17)

/* Parent index of clocks fed by the 27 MHz oscillator */
#define RTD1296_OSC			(-1)
#define RTD1296_NO_PARENT		(-2)

struct rtd1296_clk {
	struct clk_hw hw;
	struct regmap *regmap;
	unsigned int reg;
	u32 mask;
};

struct rtd1296_pll_desc {
	unsigned int id;
	const char *name;
	unsigned int reg;
	const struct clk_ops *ops;
};

struct rtd1296_gate_desc {
	unsigned int id;
	const char *name;
	unsigned int reg;
	u8 bit;
	int parent;
	unsigned long flags;
	const struct clk_ops *ops;
};

struct rtd1296_clk_desc {
	const struct rtd1296_pll_desc *plls;
	unsigned int num_plls;
	const struct rtd1296_gate_desc *gates;
	unsigned int num_gates;
	bool has_sys_clks;
	unsigned int num_clks;
};

static inline struct rtd1296_clk *to_rtd1296_clk(struct clk_hw *hw)
{
	return container_of(hw, struct rtd1296_clk, hw);
}

static unsigned long rtd1296_pll_nf_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int val, n, f;

	if (regmap_read(clk->regmap, clk->reg, &val))
		return 0;

	n = FIELD_GET(PLL_NF_N, val);
	f = FIELD_GET(PLL_NF_F, val);

	return div_u64((u64)parent_rate * ((n + 3) * PLL_NF_F_DEN + f),
		       PLL_NF_F_DEN);
}

static const struct clk_ops rtd1296_pll_nf_ops = {
	.recalc_rate = rtd1296_pll_nf_recalc_rate,
};

/* Consumers may retain clock preparation while the GPU is idle. */
static int rtd1296_gpu_pll_enable(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	u32 power, mode, status;
	int ret;

	ret = regmap_read(clk->regmap, CRT_GPU_POWER, &power);
	if (ret)
		return ret;
	ret = regmap_read(clk->regmap, CRT_GPU_SSC, &mode);
	if (ret)
		return ret;
	ret = regmap_update_bits(clk->regmap, CRT_GPU_POWER,
				 GPU_POWER_ENABLE | GPU_OUTPUT_DISABLE,
				 GPU_POWER_ENABLE | GPU_OUTPUT_DISABLE);
	if (ret)
		return ret;
	ret = regmap_update_bits(clk->regmap, CRT_GPU_SSC, GPU_SSC_MODE, 4);
	if (ret)
		goto restore;
	udelay(GPU_PLL_SETTLE_US);
	ret = regmap_update_bits(clk->regmap, CRT_GPU_SSC, GPU_SSC_MODE, 5);
	if (ret)
		goto restore;
	ret = regmap_read_poll_timeout_atomic(clk->regmap, CRT_GPU_STATUS, status,
					      status & GPU_SSC_READY, 10, 10000);
	if (ret)
		goto restore;
	ret = regmap_clear_bits(clk->regmap, CRT_GPU_POWER, GPU_OUTPUT_DISABLE);
	if (!ret)
		return 0;

restore:
	regmap_write(clk->regmap, CRT_GPU_SSC, mode);
	regmap_write(clk->regmap, CRT_GPU_POWER, power);
	return ret;
}

static void rtd1296_gpu_pll_disable(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	regmap_update_bits(clk->regmap, CRT_GPU_POWER,
			   GPU_POWER_ENABLE | GPU_OUTPUT_DISABLE, GPU_OUTPUT_DISABLE);
}

static int rtd1296_gpu_pll_is_enabled(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	u32 value;
	int ret;

	ret = regmap_read(clk->regmap, CRT_GPU_POWER, &value);
	if (ret)
		return ret;
	return (value & (GPU_POWER_ENABLE | GPU_OUTPUT_DISABLE)) == GPU_POWER_ENABLE;
}

static const struct clk_ops rtd1296_gpu_pll_ops = {
	.enable = rtd1296_gpu_pll_enable,
	.disable = rtd1296_gpu_pll_disable,
	.is_enabled = rtd1296_gpu_pll_is_enabled,
	.recalc_rate = rtd1296_pll_nf_recalc_rate,
};

static unsigned long rtd1296_mmc_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int n, config;

	if (regmap_read(clk->regmap, clk->reg - 4, &config) ||
	    regmap_read(clk->regmap, clk->reg, &n) ||
	    ((config & (MMC_PLL_F_SELECT | MMC_PLL_8X_ENABLE)) != MMC_PLL_8X_ENABLE))
		return 0;

	return div_u64((u64)parent_rate * (FIELD_GET(MMC_PLL_N, n) + 2),
		       MMC_PLL_DIVIDER);
}

static const struct clk_ops rtd1296_mmc_ops = {
	.recalc_rate = rtd1296_mmc_recalc_rate,
};

static int rtd1296_emmc_get_phase(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int value;
	int ret;

	ret = regmap_read(clk->regmap, clk->reg, &value);
	if (ret)
		return ret;
	value = (value & clk->mask) >> __ffs(clk->mask);
	return DIV_ROUND_CLOSEST(value * 360, 32);
}

static int rtd1296_emmc_set_phase(struct clk_hw *hw, int degrees)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int value;
	int ret, restore;

	if (degrees < 0 || degrees >= 360)
		return -EINVAL;
	value = DIV_ROUND_CLOSEST(degrees * 32, 360) & 31;
	ret = regmap_clear_bits(clk->regmap, clk->reg, EMMC_PHASE_RESET_N);
	if (ret)
		return ret;
	ret = regmap_update_bits(clk->regmap, clk->reg, clk->mask,
				 value << __ffs(clk->mask));
	restore = regmap_set_bits(clk->regmap, clk->reg,
				  EMMC_PHASE_RESET_N | EMMC_PHASE_BIAS);
	usleep_range(200, 300);
	return ret ?: restore;
}

static const struct clk_ops rtd1296_emmc_phase_ops = {
	.get_phase = rtd1296_emmc_get_phase,
	.set_phase = rtd1296_emmc_set_phase,
};

static unsigned long rtd1296_pll_mno_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int val, m, n, o;

	if (regmap_read(clk->regmap, clk->reg, &val))
		return 0;

	m = FIELD_GET(PLL_MNO_M, val);
	n = FIELD_GET(PLL_MNO_N, val);
	o = FIELD_GET(PLL_MNO_O, val);

	return div_u64((u64)parent_rate * (m + 2), (n + 1) * (o + 1));
}

static const struct clk_ops rtd1296_pll_mno_ops = {
	.recalc_rate = rtd1296_pll_mno_recalc_rate,
};

static int rtd1296_vodma_prepare(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	int ret;

	ret = regmap_update_bits(clk->regmap, CRT_PLL_VODMA_POWER, 7, 7);
	if (ret)
		return ret;
	usleep_range(200, 300);
	return regmap_update_bits(clk->regmap, CRT_PLL_VODMA_POWER, 7, 3);
}

static void rtd1296_vodma_unprepare(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	regmap_update_bits(clk->regmap, CRT_PLL_VODMA_POWER, 7, 4);
}

static const struct clk_ops rtd1296_vodma_ops = {
	.prepare = rtd1296_vodma_prepare,
	.unprepare = rtd1296_vodma_unprepare,
	.recalc_rate = rtd1296_pll_mno_recalc_rate,
};

static u8 rtd1296_sys_get_parent(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int val;

	if (regmap_read(clk->regmap, clk->reg, &val))
		return 0;

	return !!(val & clk->mask);
}

static const struct clk_ops rtd1296_sys_ops = {
	.get_parent = rtd1296_sys_get_parent,
};

static unsigned long rtd1296_scpu_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	/* Both 0 and 1 select the undivided SCPU PLL output */
	static const u8 rtd1296_scpu_div[] = { 1, 1, 2, 4 };
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int val, nf;

	if (regmap_read(clk->regmap, CRT_SYS_CLKSEL, &val))
		return 0;
	if (regmap_read(clk->regmap, CRT_PLL_SCPU, &nf))
		return 0;
	parent_rate = div_u64((u64)parent_rate *
			     (3 * PLL_NF_F_DEN + (nf & (PLL_NF_N | PLL_NF_F))),
			     PLL_NF_F_DEN);

	return parent_rate / rtd1296_scpu_div[FIELD_GET(CRT_SYS_CLKSEL_SCPU, val)];
}

static int rtd1296_scpu_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *request)
{
	unsigned long minimum = max(request->min_rate, 300000000UL);
	unsigned long maximum = min(request->max_rate, 1800000000UL);

	if (minimum > maximum)
		return -EINVAL;
	minimum = roundup(minimum, 100000000UL);
	maximum = rounddown(maximum, 100000000UL);
	if (minimum > maximum)
		return -EINVAL;
	request->rate = clamp(rounddown(request->rate, 100000000UL), minimum, maximum);
	return 0;
}

static u32 rtd1296_scpu_nf(unsigned long rate, unsigned long oscillator)
{
	return DIV_ROUND_CLOSEST_ULL((u64)rate * PLL_NF_F_DEN, oscillator) -
		3 * PLL_NF_F_DEN;
}

static int rtd1296_scpu_program(struct rtd1296_clk *clk, u32 nf)
{
	unsigned int status;
	int ret;

	ret = regmap_update_bits(clk->regmap, CRT_SCPU_SSC, SCPU_SSC_MODE, 4);
	if (ret)
		return ret;
	ret = regmap_update_bits(clk->regmap, CRT_PLL_SCPU, PLL_NF_N | PLL_NF_F, nf);
	if (ret)
		return ret;
	ret = regmap_update_bits(clk->regmap, CRT_SCPU_SSC, SCPU_SSC_MODE, 5);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(clk->regmap, CRT_SCPU_STATUS, status,
		(status & SCPU_SSC_READY) &&
		(status & (PLL_NF_N | PLL_NF_F)) == nf, 10, 20000);
}

static int rtd1296_scpu_set_div(struct rtd1296_clk *clk, unsigned int code)
{
	return regmap_update_bits(clk->regmap, CRT_SYS_CLKSEL,
				  CRT_SYS_CLKSEL_SCPU,
				  FIELD_PREP(CRT_SYS_CLKSEL_SCPU, code));
}

static int rtd1296_scpu_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int old_div, old_nf, val, code, divisor;
	int ret;

	if (!parent_rate || rate < 300000000 || rate > 1800000000 ||
	    rate % 100000000)
		return -EINVAL;
	code = rate < 500000000 ? 3 : rate < 1000000000 ? 2 : 1;
	divisor = code == 3 ? 4 : code;
	ret = regmap_read(clk->regmap, CRT_SYS_CLKSEL, &val);
	if (ret)
		return ret;
	old_div = FIELD_GET(CRT_SYS_CLKSEL_SCPU, val);
	ret = regmap_read(clk->regmap, CRT_PLL_SCPU, &old_nf);
	if (ret)
		return ret;
	old_nf &= PLL_NF_N | PLL_NF_F;

	if (code != old_div) {
		/* Divider switching requires the PLL to settle at 1 GHz first. */
		ret = rtd1296_scpu_program(clk, rtd1296_scpu_nf(1000000000, parent_rate));
		if (ret)
			goto restore;
		ret = rtd1296_scpu_set_div(clk, code);
		if (ret)
			goto restore;
	}
	ret = rtd1296_scpu_program(clk, rtd1296_scpu_nf(rate * divisor, parent_rate));
	if (!ret)
		return 0;

restore:
	if (rtd1296_scpu_program(clk, rtd1296_scpu_nf(1000000000, parent_rate)) ||
	    rtd1296_scpu_set_div(clk, old_div) || rtd1296_scpu_program(clk, old_nf)) {
		rtd1296_scpu_set_div(clk, 3);
		pr_err("scpu: clock recovery failed\n");
	}
	return ret;
}

static const struct clk_ops rtd1296_scpu_ops = {
	.recalc_rate = rtd1296_scpu_recalc_rate,
	.determine_rate = rtd1296_scpu_determine_rate,
	.set_rate = rtd1296_scpu_set_rate,
};

static int rtd1296_gate_enable(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	return regmap_set_bits(clk->regmap, clk->reg, clk->mask);
}

static void rtd1296_gate_disable(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	regmap_clear_bits(clk->regmap, clk->reg, clk->mask);
}

static int rtd1296_gate_is_enabled(struct clk_hw *hw)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	return regmap_test_bits(clk->regmap, clk->reg, clk->mask) > 0;
}

static const struct clk_ops rtd1296_gate_ops = {
	.enable = rtd1296_gate_enable,
	.disable = rtd1296_gate_disable,
	.is_enabled = rtd1296_gate_is_enabled,
};

static int rtd1296_aio_pcm_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);

	if (rate != 48000)
		return -EINVAL;
	return regmap_update_bits(clk->regmap, clk->reg, GENMASK(7, 1), 0);
}

static int rtd1296_aio_pcm_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *request)
{
	if (request->min_rate > 48000 || request->max_rate < 48000)
		return -EINVAL;
	request->rate = 48000;
	return 0;
}

static unsigned long rtd1296_aio_pcm_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct rtd1296_clk *clk = to_rtd1296_clk(hw);
	unsigned int value;

	if (regmap_read(clk->regmap, clk->reg, &value) || (value & GENMASK(7, 1)))
		return 0;
	return 48000;
}

static const struct clk_ops rtd1296_aio_pcm_ops = {
	.enable = rtd1296_gate_enable,
	.disable = rtd1296_gate_disable,
	.is_enabled = rtd1296_gate_is_enabled,
	.recalc_rate = rtd1296_aio_pcm_recalc_rate,
	.determine_rate = rtd1296_aio_pcm_determine_rate,
	.set_rate = rtd1296_aio_pcm_set_rate,
};

#define PLL(_id, _name, _reg, _ops)					\
	{ .id = _id, .name = _name, .reg = _reg, .ops = _ops }

#define GATE(_id, _name, _reg, _bit, _parent, _flags)			\
	{								\
		.id = _id, .name = _name, .reg = _reg, .bit = _bit,	\
		.parent = _parent, .flags = _flags,			\
	}

static const struct rtd1296_pll_desc rtd1296_crt_plls[] = {
	PLL(RTD1296_CRT_PLL_SCPU, "pll_scpu", CRT_PLL_SCPU, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_BUS, "pll_bus", CRT_PLL_BUS, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_BUS_H, "pll_bus_h", CRT_PLL_BUS_H, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_DDSA, "pll_ddsa", CRT_PLL_DDSA, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_DDSB, "pll_ddsb", CRT_PLL_DDSB, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_GPU, "pll_gpu", CRT_PLL_GPU, &rtd1296_gpu_pll_ops),
	PLL(RTD1296_CRT_PLL_VE1, "pll_ve1", CRT_PLL_VE1, &rtd1296_pll_mno_ops),
	PLL(RTD1296_CRT_PLL_VE2, "pll_ve2", CRT_PLL_VE2, &rtd1296_pll_mno_ops),
	PLL(RTD1296_CRT_PLL_VODMA, "pll_vodma", CRT_PLL_VODMA, &rtd1296_vodma_ops),
	PLL(RTD1296_CRT_PLL_ACPU, "pll_acpu", CRT_PLL_ACPU, &rtd1296_pll_nf_ops),
	PLL(RTD1296_CRT_PLL_EMMC, "pll_emmc", CRT_PLL_EMMC_N, &rtd1296_mmc_ops),
	PLL(RTD1296_CRT_PLL_SD, "pll_sd", CRT_PLL_SD_N, &rtd1296_mmc_ops),
};

static const struct rtd1296_gate_desc rtd1296_crt_gates[] = {
	GATE(RTD1296_CRT_CLK_EN_VO, "vo", CRT_CLK_EN1, 15,
	     RTD1296_CRT_PLL_VODMA, 0),
	GATE(RTD1296_CRT_CLK_EN_TVE, "tve", CRT_CLK_EN1, 14,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO, "aio", CRT_CLK_EN1, 10,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO_CODEC, "aio_codec", CRT_CLK_EN2, 17,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO_MOD, "aio_mod", CRT_CLK_EN2, 18,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO_DA, "aio_da", CRT_CLK_EN2, 19,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO_I2S, "aio_i2s", CRT_CLK_EN2, 22,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_CRT_CLK_EN_AIO_MCLK, "aio_mclk", CRT_CLK_EN2, 23,
	     RTD1296_NO_PARENT, 0),
	{
		.id = RTD1296_CRT_CLK_EN_AIO_PCM,
		.name = "aio_pcm",
		.reg = CRT_AIO_INPUT_CLOCK,
		.parent = RTD1296_NO_PARENT,
		.ops = &rtd1296_aio_pcm_ops,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE,
	},
	/* Syscon users access interrupt muxes, GPIOs and timers without clock references. */
	GATE(RTD1296_CRT_CLK_EN_MISC, "misc", CRT_CLK_EN1, 0,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_PCIE0, "pcie0", CRT_CLK_EN1, 1,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SATA0, "sata0", CRT_CLK_EN1, 2,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_GSPI, "gspi", CRT_CLK_EN1, 3,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_USB, "usb", CRT_CLK_EN1, 4,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_ISO_MISC, "iso_misc", CRT_CLK_EN1, 6,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_SATA_ALIVE0, "sata_alive0", CRT_CLK_EN1, 7,
	     RTD1296_CRT_CLK_SYS, 0),
	/* Retain the transmitter clock until the display driver takes ownership. */
	GATE(RTD1296_CRT_CLK_EN_HDMI, "hdmi", CRT_CLK_EN1, 8,
	     RTD1296_CRT_CLK_SYS, CLK_IGNORE_UNUSED),
	GATE(RTD1296_CRT_CLK_EN_ETN, "etn", CRT_CLK_EN1, 9,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_GPU, "gpu", CRT_CLK_EN1, 11,
	     RTD1296_CRT_PLL_GPU, 0),
	GATE(RTD1296_CRT_CLK_EN_LVDS, "lvds", CRT_CLK_EN1, 16,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SE, "se", CRT_CLK_EN1, 17,
	     RTD1296_CRT_CLK_SYS, 0),
	/*
	 * The CP (crypto), MD (memory DMA) and TP (transport stream) engines
	 * are used by the secure firmware, which Linux knows nothing about.
	 */
	GATE(RTD1296_CRT_CLK_EN_CP, "cp", CRT_CLK_EN1, 19,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_MD, "md", CRT_CLK_EN1, 20,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_TP, "tp", CRT_CLK_EN1, 21,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_NF, "nf", CRT_CLK_EN1, 23,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_EMMC, "emmc", CRT_CLK_EN1, 24,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_CR, "cr", CRT_CLK_EN1, 25,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SDIO_IP, "sdio_ip", CRT_CLK_EN1, 26,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_MIPI, "mipi", CRT_CLK_EN1, 27,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_EMMC_IP, "emmc_ip", CRT_CLK_EN1, 28,
	     RTD1296_CRT_PLL_EMMC, 0),
	GATE(RTD1296_CRT_CLK_EN_SDIO, "sdio", CRT_CLK_EN1, 30,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SD_IP, "sd_ip", CRT_CLK_EN1, 31,
	     RTD1296_CRT_PLL_SD, 0),
	GATE(RTD1296_CRT_CLK_EN_NAT, "nat", CRT_CLK_EN2, 0,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_I2C5, "i2c5", CRT_CLK_EN2, 1,
	     RTD1296_OSC, 0),
	GATE(RTD1296_CRT_CLK_EN_JPEG, "jpeg", CRT_CLK_EN2, 3,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_PCIE1, "pcie1", CRT_CLK_EN2, 5,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SC, "sc", CRT_CLK_EN2, 6,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_CBUS_TX, "cbus_tx", CRT_CLK_EN2, 7,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_RTC, "rtc", CRT_CLK_EN2, 10,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_I2C4, "i2c4", CRT_CLK_EN2, 13,
	     RTD1296_OSC, 0),
	GATE(RTD1296_CRT_CLK_EN_I2C3, "i2c3", CRT_CLK_EN2, 14,
	     RTD1296_OSC, 0),
	GATE(RTD1296_CRT_CLK_EN_I2C2, "i2c2", CRT_CLK_EN2, 15,
	     RTD1296_OSC, 0),
	GATE(RTD1296_CRT_CLK_EN_HDMIRX, "hdmirx", CRT_CLK_EN2, 24,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SATA1, "sata1", CRT_CLK_EN2, 25,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_SATA_ALIVE1, "sata_alive1", CRT_CLK_EN2, 26,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_UR2, "ur2", CRT_CLK_EN2, 27,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_UR1, "ur1", CRT_CLK_EN2, 28,
	     RTD1296_CRT_CLK_SYS, 0),
	GATE(RTD1296_CRT_CLK_EN_FAN, "fan", CRT_CLK_EN2, 29,
	     RTD1296_CRT_CLK_SYS, 0),
	/* The DDR PHY must never be stopped */
	GATE(RTD1296_CRT_CLK_EN_DCPHY0, "dcphy0", CRT_CLK_EN2, 30,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_DCPHY1, "dcphy1", CRT_CLK_EN2, 31,
	     RTD1296_CRT_CLK_SYS, CLK_IS_CRITICAL),
	GATE(RTD1296_CRT_CLK_EN_LSADC, "lsadc", CRT_CLK_EN3, 2,
	     RTD1296_CRT_CLK_SYS, 0),
};

static const struct rtd1296_clk_desc rtd1296_crt_desc = {
	.plls = rtd1296_crt_plls,
	.num_plls = ARRAY_SIZE(rtd1296_crt_plls),
	.gates = rtd1296_crt_gates,
	.num_gates = ARRAY_SIZE(rtd1296_crt_gates),
	.has_sys_clks = true,
	.num_clks = RTD1296_CRT_CLK_EN_VO + 1,
};

/* Peripheral control clocks use the 27 MHz oscillator. */
static const struct rtd1296_gate_desc rtd1296_iso_gates[] = {
	GATE(RTD1296_ISO_CLK_EN_CEC0, "cec0", ISO_CLK_EN, 2, RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_CBUSRX_SYS, "cbusrx_sys", ISO_CLK_EN, 3,
	     RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_CBUSTX_SYS, "cbustx_sys", ISO_CLK_EN, 4,
	     RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_CBUS_SYS, "cbus_sys", ISO_CLK_EN, 5,
	     RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_CBUS_OSC, "cbus_osc", ISO_CLK_EN, 6,
	     RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_IR, "ir", ISO_CLK_EN, 7, RTD1296_OSC, 0),
	/*
	 * UART0 is the usual debug console. Keep it running for earlycon
	 * until the serial driver claims it.
	 */
	GATE(RTD1296_ISO_CLK_EN_UR0, "ur0", ISO_CLK_EN, 8, RTD1296_OSC,
	     CLK_IGNORE_UNUSED),
	GATE(RTD1296_ISO_CLK_EN_I2C0, "i2c0", ISO_CLK_EN, 9, RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_I2C1, "i2c1", ISO_CLK_EN, 10, RTD1296_OSC, 0),
	GATE(RTD1296_ISO_CLK_EN_ETN_250M, "etn_250m", ISO_CLK_EN, 11,
	     RTD1296_NO_PARENT, 0),
	GATE(RTD1296_ISO_CLK_EN_ETN_SYS, "etn_sys", ISO_CLK_EN, 12,
	     RTD1296_NO_PARENT, 0),
};

static const struct rtd1296_clk_desc rtd1296_iso_desc = {
	.gates = rtd1296_iso_gates,
	.num_gates = ARRAY_SIZE(rtd1296_iso_gates),
	.num_clks = RTD1296_ISO_CLK_EN_ETN_SYS + 1,
};

static struct clk_hw *rtd1296_clk_register(struct device *dev,
					   struct regmap *regmap,
					   const char *name,
					   const struct clk_ops *ops,
					   const struct clk_parent_data *parents,
					   u8 num_parents, unsigned long flags,
					   unsigned int reg, u32 mask)
{
	struct clk_init_data init = {
		.name = name,
		.ops = ops,
		.parent_data = parents,
		.num_parents = num_parents,
		.flags = flags,
	};
	struct rtd1296_clk *clk;
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->regmap = regmap;
	clk->reg = reg;
	clk->mask = mask;
	clk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return &clk->hw;
}

static int rtd1296_register_sys_clks(struct device *dev, struct regmap *regmap,
				     struct clk_hw **hws)
{
	const struct clk_parent_data scpu_parent = {
		.index = 0,
	};
	struct clk_parent_data sb2_parents[2] = {
		{ }, { .index = 0 },
	};
	struct clk_parent_data sys_parents[2] = {
		{ .hw = hws[RTD1296_CRT_PLL_BUS] },
	};
	struct clk_hw *hw;

	hw = devm_clk_hw_register_fixed_factor_parent_hw(dev, "pll_bus_div2",
							 hws[RTD1296_CRT_PLL_BUS],
							 0, 1, 2);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	hws[RTD1296_CRT_PLL_BUS_DIV2] = hw;

	sys_parents[1].hw = hw;
	hw = rtd1296_clk_register(dev, regmap, "sys", &rtd1296_sys_ops,
				  sys_parents, ARRAY_SIZE(sys_parents), 0,
				  CRT_SYS_CLKSEL, CRT_SYS_CLKSEL_SYS);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	hws[RTD1296_CRT_CLK_SYS] = hw;

	sb2_parents[0].hw = hw;
	hw = rtd1296_clk_register(dev, regmap, "sys_sb2", &rtd1296_sys_ops,
				  sb2_parents, ARRAY_SIZE(sb2_parents), 0,
				  CRT_SB2_CLKSEL, BIT(0));
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	hws[RTD1296_CRT_CLK_SYS_SB2] = hw;

	hw = rtd1296_clk_register(dev, regmap, "scpu", &rtd1296_scpu_ops,
				  &scpu_parent, 1, CLK_GET_RATE_NOCACHE,
				  CRT_SYS_CLKSEL, CRT_SYS_CLKSEL_SCPU);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	hws[RTD1296_CRT_CLK_SCPU] = hw;

	return 0;
}

static int rtd1296_clk_probe(struct platform_device *pdev)
{
	const struct clk_parent_data osc_parent = { .index = 0 };
	struct device *dev = &pdev->dev;
	const struct rtd1296_clk_desc *desc;
	struct clk_hw_onecell_data *data;
	struct regmap *regmap;
	struct clk_hw *hw;
	unsigned int i;
	int ret;

	desc = device_get_match_data(dev);
	if (!desc)
		return -EINVAL;

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get syscon regmap\n");

	data = devm_kzalloc(dev, struct_size(data, hws, desc->num_clks),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = desc->num_clks;

	for (i = 0; i < desc->num_plls; i++) {
		const struct rtd1296_pll_desc *pll = &desc->plls[i];

		hw = rtd1296_clk_register(dev, regmap, pll->name, pll->ops,
					  &osc_parent, 1, CLK_GET_RATE_NOCACHE, pll->reg, 0);
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw),
					     "failed to register %s\n",
					     pll->name);
		data->hws[pll->id] = hw;
	}

	if (desc->has_sys_clks) {
		ret = rtd1296_register_sys_clks(dev, regmap, data->hws);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register system clocks\n");
	}

	if (desc->has_sys_clks) {
		const struct clk_parent_data parent = {
			.hw = data->hws[RTD1296_CRT_PLL_EMMC],
		};

		hw = rtd1296_clk_register(dev, regmap, "emmc_drive",
					  &rtd1296_emmc_phase_ops, &parent, 1, 0,
					  CRT_PLL_EMMC_PHASE, GENMASK(7, 3));
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		data->hws[RTD1296_CRT_CLK_EMMC_DRIVE] = hw;
		hw = rtd1296_clk_register(dev, regmap, "emmc_sample",
					  &rtd1296_emmc_phase_ops, &parent, 1, 0,
					  CRT_PLL_EMMC_PHASE, GENMASK(12, 8));
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		data->hws[RTD1296_CRT_CLK_EMMC_SAMPLE] = hw;
	}

	for (i = 0; i < desc->num_gates; i++) {
		const struct rtd1296_gate_desc *gate = &desc->gates[i];
		struct clk_parent_data parent = osc_parent;
		u8 num_parents = gate->parent == RTD1296_NO_PARENT ? 0 : 1;

		if (gate->parent >= 0)
			parent.hw = data->hws[gate->parent];

		hw = rtd1296_clk_register(dev, regmap, gate->name,
					  gate->ops ?: &rtd1296_gate_ops,
					  num_parents ? &parent : NULL, num_parents,
					  gate->flags, gate->reg,
					  BIT(gate->bit));
		if (IS_ERR(hw))
			return dev_err_probe(dev, PTR_ERR(hw),
					     "failed to register %s\n",
					     gate->name);
		data->hws[gate->id] = hw;
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static const struct of_device_id rtd1296_clk_match[] = {
	{ .compatible = "realtek,rtd1296-crt-clk", .data = &rtd1296_crt_desc },
	{ .compatible = "realtek,rtd1296-iso-clk", .data = &rtd1296_iso_desc },
	{ /* sentinel */ }
};

static struct platform_driver rtd1296_clk_driver = {
	.probe = rtd1296_clk_probe,
	.driver = {
		.name = "rtd1296-clk",
		.of_match_table = rtd1296_clk_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rtd1296_clk_driver);
