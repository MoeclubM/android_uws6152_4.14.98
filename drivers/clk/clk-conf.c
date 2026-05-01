/*
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 * Sylwester Nawrocki <s.nawrocki@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/clk-conf.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/printk.h>

static int __set_clk_parents(struct device_node *node, bool clk_supplier)
{
	struct of_phandle_args clkspec;
	int index, rc, num_parents;
	struct clk *clk, *pclk;

	num_parents = of_count_phandle_with_args(node, "assigned-clock-parents",
						 "#clock-cells");
	if (num_parents == -EINVAL)
		pr_err("clk: invalid value of clock-parents property at %pOF\n",
		       node);

	for (index = 0; index < num_parents; index++) {
		pr_err("DEBUG: __set_clk_parents node=%pOF, index=%d\n", node, index);

		rc = of_parse_phandle_with_args(node, "assigned-clock-parents",
					"#clock-cells",	index, &clkspec);
		if (rc < 0) {
			pr_err("DEBUG: of_parse_phandle_with_args(parents) failed, rc=%d\n", rc);
			if (rc == -ENOENT)
				continue;
			else
				return rc;
		}

		pr_err("DEBUG: resolved parent phandle %u (%pOF), args_count=%u\n",
		       clkspec.np ? clkspec.np->phandle : 0,
		       clkspec.np,
		       clkspec.args_count);

		if (clkspec.np == node && !clk_supplier)
			return 0;
		pclk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(pclk)) {
			if (PTR_ERR(pclk) != -EPROBE_DEFER)
				pr_warn("clk: couldn't get parent clock %d for %pOF\n",
					index, node);
			return PTR_ERR(pclk);
		}

		/* For assigned-clocks in parent switching, we also need to parse
		 * the matching assigned-clocks property. Apply the same fallback
		 * logic as in __set_clk_rates.
		 */
		rc = of_parse_phandle_with_args(node, "assigned-clocks",
					"#clock-cells", index, &clkspec);
		if (rc == -EINVAL) {
			/* Fallback: phandle only? Assume missing arg is 0 */
			struct device_node *np;
			np = of_parse_phandle(node, "assigned-clocks", index);
			if (!np)
				goto err;
			u32 cells;
			if (of_property_read_u32(np, "#clock-cells", &cells))
				cells = 0;
			clkspec.np = np;
			clkspec.args_count = cells;
			memset(clkspec.args, 0, sizeof(clkspec.args));
			rc = 0;
			pr_err("DEBUG: fallback: set assigned-clocks phandle %u, cells=%u\n",
			       np->phandle, cells);
		}
		if (rc < 0) {
			pr_err("DEBUG: failed to parse assigned-clocks, rc=%d\n", rc);
			if (rc == -ENOENT)
				continue;
			else
				goto err;
		}

		if (clkspec.np == node && !clk_supplier) {
			rc = 0;
			goto err;
		}
		clk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) != -EPROBE_DEFER)
				pr_warn("clk: couldn't get assigned clock %d for %pOF\n",
					index, node);
			rc = PTR_ERR(clk);
			goto err;
		}

		rc = clk_set_parent(clk, pclk);
		if (rc < 0)
			pr_err("clk: failed to reparent %s to %s: %d\n",
			       __clk_get_name(clk), __clk_get_name(pclk), rc);
		clk_put(clk);
		clk_put(pclk);
	}
	return 0;
err:
	clk_put(pclk);
	return rc;
}

static int __set_clk_rates(struct device_node *node, bool clk_supplier)
{
	struct of_phandle_args clkspec;
	struct property	*prop;
	const __be32 *cur;
	int rc, index = 0;
	struct clk *clk;
	u32 rate;

	of_property_for_each_u32(node, "assigned-clock-rates", prop, cur, rate) {
		if (rate) {
			struct property *ac_prop;
			int ac_len = 0;
			const __be32 *ac_val;

			// 打印当前节点
			pr_err("DEBUG: __set_clk_rates node=%pOF, index=%d, rate=%u\n",
			       node, index, rate);

			// 尝试读取 assigned-clocks 属性的内容
			ac_prop = of_find_property(node, "assigned-clocks", &ac_len);
			if (ac_prop) {
				ac_val = ac_prop->value;
				pr_err("DEBUG: assigned-clocks len=%d, first cells: %08x %08x %08x\n",
				       ac_len,
				       ac_len >= 4 ? be32_to_cpu(ac_val[0]) : 0,
				       ac_len >= 8 ? be32_to_cpu(ac_val[1]) : 0,
				       ac_len >= 12 ? be32_to_cpu(ac_val[2]) : 0);
			} else {
				pr_err("DEBUG: no assigned-clocks property\n");
			}
                
			/* First try normal parsing, which checks #clock-cells */
			rc = of_parse_phandle_with_args(node, "assigned-clocks",
					"#clock-cells", index, &clkspec);
			if (rc == -EINVAL) {
				/*
				 * Fallback: the property likely contains only a phandle,
				 * but the target node requires more arguments (e.g. #clock-cells=1).
				 * We manually add default arguments (all zeros).
				 */
				struct device_node *np;
				np = of_parse_phandle(node, "assigned-clocks", index);
				if (np) {
					u32 cells;
					if (of_property_read_u32(np, "#clock-cells", &cells))
						cells = 0;
					clkspec.np = np;
					clkspec.args_count = cells;
					memset(clkspec.args, 0, sizeof(clkspec.args));
					rc = 0;
					pr_err("DEBUG: fallback: set assigned-clocks phandle %u, cells=%u\n",
					       np->phandle, cells);
				}
			}
			if (rc < 0) {
				pr_err("DEBUG: of_parse_phandle_with_args/fixed_args failed, rc=%d\n", rc);
				if (rc == -ENOENT)
					continue;
				else
					return rc;
			}

			// 成功解析后，打印 phandle 和参数
			pr_err("DEBUG: resolved phandle %u (%pOF), args_count=%u, args[0]=%08x\n",
			       clkspec.np ? clkspec.np->phandle : 0,
			       clkspec.np,
			       clkspec.args_count,
			       clkspec.args_count > 0 ? clkspec.args[0] : 0);
		} // 结束 if(rate)

		if (clkspec.np == node && !clk_supplier)
			return 0;

		clk = of_clk_get_from_provider(&clkspec);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) != -EPROBE_DEFER)
				pr_warn("clk: couldn't get clock %d for %pOF\n",
					index, node);
			return PTR_ERR(clk);
		}

		rc = clk_set_rate(clk, rate);
		if (rc < 0)
			pr_err("clk: couldn't set %s clk rate to %u (%d), current rate: %lu\n",
			       __clk_get_name(clk), rate, rc,
			       clk_get_rate(clk));
		clk_put(clk);

		index++;
	}
	return 0;
}

int of_clk_set_defaults(struct device_node *node, bool clk_supplier)
{
	int rc;

	if (!node)
		return 0;

	rc = __set_clk_parents(node, clk_supplier);
	if (rc < 0)
		return rc;

	return __set_clk_rates(node, clk_supplier);
}
EXPORT_SYMBOL_GPL(of_clk_set_defaults);