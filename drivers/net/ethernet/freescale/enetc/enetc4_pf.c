// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/* Copyright 2023 NXP */
#include <asm/unaligned.h>
#include <linux/module.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/pinctrl/consumer.h>
#include <linux/fsl/netc_prb_ierb.h>

#include "enetc_pf.h"

#define ENETC_SI_MAX_RING_NUM	8
#define ENETC_SI_BITMAP(a)	BIT(a)

static void enetc4_get_port_caps(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;
	u32 val;

	val = enetc_port_rd(hw, ENETC4_ECAPR1);
	pf->caps.num_vsi = (val & ECAPR1_NUM_VSI) >> 24;
	pf->caps.num_msix = ((val & ECAPR1_NUM_MSIX) >> 12) + 1;

	val = enetc_port_rd(hw, ENETC4_ECAPR2);
	pf->caps.num_rx_bdr = (val & ECAPR2_NUM_RX_BDR) >> 16;
	pf->caps.num_tx_bdr = val & ECAPR2_NUM_TX_BDR;

	val = enetc_port_rd(hw, ENETC4_PMCAPR);
	pf->caps.half_duplex = !!(val & PMCAPR_HD);

	val = enetc_port_rd(hw, ENETC4_PSIMAFCAPR);
	pf->caps.mac_filter_num = val & PSIMAFCAPR_NUM_MAC_AFTE;

	val = enetc_port_rd(hw, ENETC4_PSIVLANFCAPR);
	pf->caps.vlan_filter_num = val & PSIVLANFCAPR_NUM_VLAN_FTE;

	val = enetc_port_rd(hw, ENETC4_IPFTCAPR);
	pf->caps.ipf_words_num = val & IPFTCAPR_NUM_WORDS;
}

static void enetc4_pf_set_tc_msdu(struct enetc_hw *hw, u32 *max_sdu)
{
	int tc;

	for (tc = 0; tc < 8; tc++) {
		u32 val = ENETC4_MAC_MAXFRM_SIZE;

		if (max_sdu[tc])
			val = max_sdu[tc] + VLAN_ETH_HLEN;

		val = u32_replace_bits(val, SDU_TYPE_MPDU, PTCTMSDUR_SDU_TYPE);
		enetc_port_wr(hw, ENETC4_PTCTMSDUR(tc), val);
	}
}

static void enetc4_pf_reset_tc_msdu(struct enetc_hw *hw)
{
	u32 val = ENETC4_MAC_MAXFRM_SIZE;
	int tc;

	val = u32_replace_bits(val, SDU_TYPE_MPDU, PTCTMSDUR_SDU_TYPE);

	for (tc = 0; tc < 8; tc++)
		enetc_port_wr(hw, ENETC4_PTCTMSDUR(tc), val);
}

static void enetc4_set_trx_frame_size(struct enetc_pf *pf)
{
	struct enetc_si *si = pf->si;

	enetc_port_mac_wr(si, ENETC4_PM_MAXFRM(0),
			  ENETC_SET_MAXFRM(ENETC4_MAC_MAXFRM_SIZE));

	enetc4_pf_reset_tc_msdu(&si->hw);
}

/* Allocate the number of MSI-X vectors for per SI. */
static void enetc4_set_si_msix_num(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;
	int i, num_msix, total_si;
	u32 val;

	total_si = pf->caps.num_vsi + 1;

	num_msix = pf->caps.num_msix / total_si +
		   pf->caps.num_msix % total_si - 1;
	val = num_msix & 0x3f;
	enetc_port_wr(hw, ENETC4_PSICFGR2(0), val);

	num_msix = pf->caps.num_msix / total_si - 1;
	val = num_msix & 0x3f;
	for (i = 0; i < pf->caps.num_vsi; i++)
		enetc_port_wr(hw, ENETC4_PSICFGR2(i + 1), val);
}

static void enetc4_port_si_configure(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;
	int num_rx_bdr, num_tx_bdr;
	u32 val;
	int i;

	if (pf->caps.num_rx_bdr < ENETC_SI_MAX_RING_NUM + pf->caps.num_vsi)
		num_rx_bdr = pf->caps.num_rx_bdr - pf->caps.num_vsi;
	else
		num_rx_bdr = ENETC_SI_MAX_RING_NUM;

	if (pf->caps.num_tx_bdr < ENETC_SI_MAX_RING_NUM + pf->caps.num_vsi)
		num_tx_bdr = pf->caps.num_tx_bdr - pf->caps.num_vsi;
	else
		num_tx_bdr = ENETC_SI_MAX_RING_NUM;

	val = (num_rx_bdr << 16) | num_tx_bdr;
	val |= ENETC_PSICFGR0_SIVC(ENETC_VLAN_TYPE_C | ENETC_VLAN_TYPE_S);
	enetc_port_wr(hw, ENETC4_PSICFGR0(0), val);

	num_rx_bdr = (pf->caps.num_rx_bdr - num_rx_bdr) / pf->caps.num_vsi;
	num_tx_bdr = (pf->caps.num_tx_bdr - num_tx_bdr) / pf->caps.num_vsi;

	val = ENETC_PSICFGR0_SET_TXBDR(num_tx_bdr);
	val |= ENETC_PSICFGR0_SET_RXBDR(num_rx_bdr);
	val |= ENETC_PSICFGR0_SIVC(ENETC_VLAN_TYPE_C | ENETC_VLAN_TYPE_S);
	val |= ENETC_PSICFGR0_VTE | ENETC_PSICFGR0_SIVIE;
	for (i = 0; i < pf->caps.num_vsi; i++)
		enetc_port_wr(hw, ENETC4_PSICFGR0(i + 1), val);

	/* Outer VLAN tag will be used for VLAN filtering */
	enetc_port_wr(hw, ENETC4_PSIVLANFMR, PSIVLANFMR_VS);

	/* enforce VLAN promisc mode for all SIs */
	pf->vlan_promisc_simap = ENETC_VLAN_PROMISC_MAP_ALL;
	if (pf->hw_ops->set_si_vlan_promisc)
		pf->hw_ops->set_si_vlan_promisc(hw, pf->vlan_promisc_simap);

	/* Disable SI MAC multicast & unicast promiscuous */
	enetc_port_wr(hw, ENETC4_PSIPMMR, 0);

	enetc4_set_si_msix_num(pf);
}

static void enetc4_set_default_rss_key(struct enetc_hw *hw)
{
	u8 hash_key[ENETC_RSSHASH_KEY_SIZE];

	/* set up hash key */
	get_random_bytes(hash_key, ENETC_RSSHASH_KEY_SIZE);
	enetc_set_rss_key(hw, hash_key);
}

static void enetc4_set_isit_key_construct_rule(struct enetc_hw *hw)
{
	u32 val;

	/* Key construction rule 0: SMAC + VID */
	val = ISIDKCCR0_VALID | ISIDKCCR0_SMACP | ISIDKCCR0_OVIDP;
	enetc_port_wr(hw, ENETC4_ISIDKC0CR0, val);

	/* Key construction rule 1: DMAC + VID */
	val = ISIDKCCR0_VALID | ISIDKCCR0_DMACP | ISIDKCCR0_OVIDP;
	enetc_port_wr(hw, ENETC4_ISIDKC1CR0, val);

	/* Enable key construction rule 0 and 1 */
	val = enetc_port_rd(hw, ENETC4_PISIDCR);
	val |= PISIDCR_KC0EN | PISIDCR_KC1EN;
	enetc_port_wr(hw, ENETC4_PISIDCR, val);
}

static void enetc4_configure_port(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;

	enetc4_port_si_configure(pf);

	enetc4_set_trx_frame_size(pf);

	enetc4_set_default_rss_key(hw);

	enetc4_set_isit_key_construct_rule(hw);

	/* Master enable for all SIs */
	enetc_port_wr(hw, ENETC4_PMR, PMR_SI0_EN | PMR_SI1_EN | PMR_SI2_EN);

	/* Enable port transmit/receive */
	enetc_port_wr(hw, ENETC4_POR, 0);
}

/* To simplify the usage of ENETC MAC filter, only the PSI can
 * use the exact match MAC address table. If the total MAC
 * address number is less than or equal to PSIMAFCAPR, the
 * exact match MAC address filter table is applied. Otherwise,
 * the MAC address filter hash table is applied.
 */
static void enetc4_pf_set_rx_mode(struct net_device *ndev)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_pf *pf = enetc_si_priv(priv->si);
	struct enetc_mac_filter *uc_filter;
	struct enetc_mac_filter *mc_filter;
	struct enetc_si *si = priv->si;
	struct enetc_hw *hw = &si->hw;
	struct netdev_hw_addr *ha;
	bool exact_match = false;
	bool uc_promisc = false;
	bool mc_promisc = false;
	int i, num_macs = 0;

	if (!pf->hw_ops->set_si_mac_filter || !pf->hw_ops->set_si_mac_promisc)
		return;

	uc_filter = &si->mac_filter[UC];
	mc_filter = &si->mac_filter[MC];

	if (ndev->flags & IFF_PROMISC) {
		uc_promisc = true;
		mc_promisc = true;
	} else if (ndev->flags & IFF_ALLMULTI) {
		mc_promisc = true;
	}

	/* Clear all MAC filter hash tables (software) first, then update
	 * corresponding hash table if the promisc mode is disabled.
	 */
	enetc_reset_mac_addr_filter(uc_filter);
	enetc_reset_mac_addr_filter(mc_filter);

	/* If unicast promisc mode is disabled, set unicast filter rules. */
	if (!uc_promisc) {
		netdev_for_each_uc_addr(ha, ndev)
			enetc_add_mac_addr_ht_filter(uc_filter, ha->addr);

		num_macs += uc_filter->mac_addr_cnt;
	}

	/* If multicast promisc mode is disabled, set multicast filter rules. */
	if (!mc_promisc) {
		netdev_for_each_mc_addr(ha, ndev) {
			if (!is_multicast_ether_addr(ha->addr))
				continue;

			enetc_add_mac_addr_ht_filter(mc_filter, ha->addr);
		}

		num_macs += mc_filter->mac_addr_cnt;
	}

	if (num_macs && num_macs <= pf->caps.mac_filter_num)
		exact_match = true;

	/* Clear exact match MAC filter table (hardware) first, then if exact_match
	 * condition is true, add new MAC filter entry to the exact match table.
	 */
	for (i = 0; i < si->num_mac_fe; i++)
		ntmp_maft_delete_entry(&si->cbdr, i);

	if (exact_match) {
		i = 0;

		/* Clear MAC filter hash table (hardware) */
		pf->hw_ops->set_si_mac_filter(hw, 0, UC, 0);
		pf->hw_ops->set_si_mac_filter(hw, 0, MC, 0);

		if (!uc_promisc)
			netdev_for_each_uc_addr(ha, ndev) {
				ntmp_maft_add_entry(&si->cbdr, i, ha->addr,
						    ENETC_SI_BITMAP(0));
				i++;
			}

		if (!mc_promisc)
			netdev_for_each_mc_addr(ha, ndev) {
				ntmp_maft_add_entry(&si->cbdr, i, ha->addr,
						    ENETC_SI_BITMAP(0));
				i++;
			}

		/* Update the number of MAC filter table entries. */
		si->num_mac_fe = num_macs;
	} else {
		si->num_mac_fe = 0;
		if (!uc_promisc)
			pf->hw_ops->set_si_mac_filter(hw, 0, UC,
						      *uc_filter->mac_hash_table);
		else
			/* Clear SI0 MAC unicast hash filter */
			pf->hw_ops->set_si_mac_filter(hw, 0, UC, 0);

		if (!mc_promisc)
			pf->hw_ops->set_si_mac_filter(hw, 0, MC,
						      *mc_filter->mac_hash_table);
		else
			/* Clear SI0 MAC multicast hash filter */
			pf->hw_ops->set_si_mac_filter(hw, 0, MC, 0);
	}

	pf->hw_ops->set_si_mac_promisc(hw, 0, UC, uc_promisc);
	pf->hw_ops->set_si_mac_promisc(hw, 0, MC, mc_promisc);
}

static const struct net_device_ops enetc4_ndev_ops = {
	.ndo_open		= enetc_open,
	.ndo_stop		= enetc_close,
	.ndo_start_xmit		= enetc_xmit,
	.ndo_get_stats		= enetc_get_stats,
	.ndo_set_mac_address	= enetc_pf_set_mac_addr,
	.ndo_set_rx_mode	= enetc4_pf_set_rx_mode,
	.ndo_vlan_rx_add_vid	= enetc_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= enetc_vlan_rx_del_vid,
	.ndo_set_vf_mac		= enetc_pf_set_vf_mac,
	.ndo_set_vf_vlan	= enetc_pf_set_vf_vlan,
	.ndo_set_vf_spoofchk	= enetc_pf_set_vf_spoofchk,
	.ndo_set_features	= enetc_pf_set_features,
	.ndo_eth_ioctl		= enetc_ioctl,
	.ndo_setup_tc		= enetc_pf_setup_tc,
};

static void enetc4_mac_config(struct enetc_pf *pf, unsigned int mode,
			      phy_interface_t phy_mode)
{
	struct enetc_ndev_priv *priv = netdev_priv(pf->si->ndev);
	struct enetc_si *si = pf->si;
	u32 val;

	val = enetc_port_mac_rd(si, ENETC4_PM_IF_MODE(0));
	val &= ~(PM_IF_MODE_IFMODE | PM_IF_MODE_ENA);

	switch (phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val |= IFMODE_RGMII;
		/* We need to enable auto-negotiation for the MAC
		 * if its RGMII interface support In-Band status.
		 */
		if (phylink_autoneg_inband(mode))
			val |= PM_IF_MODE_ENA;
		break;
	case PHY_INTERFACE_MODE_RMII:
		val |= IFMODE_RMII;
		break;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
		val |= IFMODE_SGMII;
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_XGMII:
	case PHY_INTERFACE_MODE_USXGMII:
		val |= IFMODE_XGMII;
		break;
	default:
		dev_err(priv->dev,
			"Unsupported PHY mode:%d\n", phy_mode);
		return;
	}

	enetc_port_mac_wr(si, ENETC4_PM_IF_MODE(0), val);
}

static struct phylink_pcs *
enetc4_pl_mac_select_pcs(struct phylink_config *config, phy_interface_t iface)
{
	struct enetc_pf *pf = phylink_to_enetc_pf(config);

	return pf->pcs;
}

static void enetc4_pl_mac_config(struct phylink_config *config,
				 unsigned int mode,
				 const struct phylink_link_state *state)
{
	struct enetc_pf *pf = phylink_to_enetc_pf(config);

	enetc4_mac_config(pf, mode, state->interface);
}

static void enetc4_set_port_speed(struct enetc_ndev_priv *priv, int speed)
{
	u32 old_speed = priv->speed;
	u32 val;

	if (speed == old_speed)
		return;

	val = enetc_port_rd(&priv->si->hw, ENETC4_PCR);
	val &= ~PCR_PSPEED;

	switch (speed) {
	case SPEED_10:
	case SPEED_100:
	case SPEED_1000:
	case SPEED_2500:
	case SPEED_10000:
		val |= (PCR_PSPEED & PCR_PSPEED_VAL(speed));
		break;
	default:
		val |= (PCR_PSPEED & PCR_PSPEED_VAL(SPEED_10));
	}

	priv->speed = speed;
	enetc_port_wr(&priv->si->hw, ENETC4_PCR, val);
}

static void enetc4_set_rgmii_mac(struct enetc_pf *pf, int speed, int duplex)
{
	struct enetc_si *si = pf->si;
	u32 old_val, val;

	old_val = enetc_port_mac_rd(si, ENETC4_PM_IF_MODE(0));
	val = old_val & ~(PM_IF_MODE_ENA | PM_IF_MODE_M10 | PM_IF_MODE_REVMII);

	switch (speed) {
	case SPEED_1000:
		val = u32_replace_bits(val, SSP_1G, PM_IF_MODE_SSP);
		break;
	case SPEED_100:
		val = u32_replace_bits(val, SSP_100M, PM_IF_MODE_SSP);
		break;
	case SPEED_10:
		val = u32_replace_bits(val, SSP_10M, PM_IF_MODE_SSP);
	}

	val = u32_replace_bits(val, duplex == DUPLEX_FULL ? 0 : 1,
			       PM_IF_MODE_HD);

	if (val == old_val)
		return;

	enetc_port_mac_wr(si, ENETC4_PM_IF_MODE(0), val);
}

static void enetc4_set_rmii_mac(struct enetc_pf *pf, int speed, int duplex)
{
	struct enetc_si *si = pf->si;
	u32 old_val, val;

	old_val = enetc_port_mac_rd(si, ENETC4_PM_IF_MODE(0));
	val = old_val & ~(PM_IF_MODE_ENA | PM_IF_MODE_SSP);

	switch (speed) {
	case SPEED_100:
		val &= ~PM_IF_MODE_M10;
		break;
	case SPEED_10:
		val |= PM_IF_MODE_M10;
	}

	val = u32_replace_bits(val, duplex == DUPLEX_FULL ? 0 : 1,
			       PM_IF_MODE_HD);

	if (val == old_val)
		return;

	enetc_port_mac_wr(si, ENETC4_PM_IF_MODE(0), val);
}

static void enetc4_set_hd_flow_control(struct enetc_pf *pf, bool enable)
{
	struct enetc_si *si = pf->si;
	u32 old_val, val;

	if (!pf->caps.half_duplex)
		return;

	old_val = enetc_port_mac_rd(si, ENETC4_PM_CMD_CFG(0));
	val = u32_replace_bits(old_val, enable ? 1 : 0, PM_CMD_CFG_HD_FCEN);
	if (val == old_val)
		return;

	enetc_port_mac_wr(si, ENETC4_PM_CMD_CFG(0), val);
}

static void enetc4_set_rx_pause(struct enetc_pf *pf, bool rx_pause)
{
	struct enetc_si *si = pf->si;
	u32 old_val, val;

	old_val = enetc_port_mac_rd(si, ENETC4_PM_CMD_CFG(0));
	val = u32_replace_bits(old_val, rx_pause ? 0 : 1, PM_CMD_CFG_PAUSE_IGN);
	if (val == old_val)
		return;

	enetc_port_mac_wr(si, ENETC4_PM_CMD_CFG(0), val);
}

static void enetc4_set_tx_pause(struct enetc_pf *pf, int num_rxbdr, bool tx_pause)
{
	u32 pause_off_thresh = 0, pause_on_thresh = 0;
	u32 init_quanta = 0, refresh_quanta = 0;
	struct enetc_hw *hw = &pf->si->hw;
	u32 rbmr, old_rbmr;
	int i;

	for (i = 0; i < num_rxbdr; i++) {
		old_rbmr = enetc_rxbdr_rd(hw, i, ENETC_RBMR);
		rbmr = u32_replace_bits(old_rbmr, tx_pause ? 1 : 0, ENETC_RBMR_CM);
		if (rbmr == old_rbmr)
			continue;

		enetc_rxbdr_wr(hw, i, ENETC_RBMR, rbmr);
	}

	if (tx_pause) {
		/* When the port first enters congestion, send a PAUSE request
		 * with the maximum number of quanta. When the port exits
		 * congestion, it will automatically send a PAUSE frame with
		 * zero quanta.
		 */
		init_quanta = 0xffff;

		/* Also, set up the refresh timer to send follow-up PAUSE
		 * frames at half the quanta value, in case the congestion
		 * condition persists.
		 */
		refresh_quanta = 0xffff / 2;

		/* Start emitting PAUSE frames when 3 large frames (or more
		 * smaller frames) have accumulated in the FIFO waiting to be
		 * DMAed to the RX ring.
		 */
		pause_on_thresh = 3 * ENETC4_MAC_MAXFRM_SIZE;
		pause_off_thresh = 1 * ENETC4_MAC_MAXFRM_SIZE;
	}

	enetc_port_mac_wr(pf->si, ENETC4_PM_PAUSE_QUANTA(0), init_quanta);
	enetc_port_mac_wr(pf->si, ENETC4_PM_PAUSE_THRESH(0), refresh_quanta);
	enetc_port_wr(hw, ENETC4_PPAUONTR, pause_on_thresh);
	enetc_port_wr(hw, ENETC4_PPAUOFFTR, pause_off_thresh);
}

static void enetc4_enable_mac(struct enetc_pf *pf, bool en)
{
	struct enetc_si *si = pf->si;
	u32 val;

	val = enetc_port_mac_rd(si, ENETC4_PM_CMD_CFG(0));
	val &= ~(PM_CMD_CFG_TX_EN | PM_CMD_CFG_RX_EN);
	val |= en ? (PM_CMD_CFG_TX_EN | PM_CMD_CFG_RX_EN) : 0;

	enetc_port_mac_wr(si, ENETC4_PM_CMD_CFG(0), val);
}

static void enetc4_pl_mac_link_up(struct phylink_config *config,
				  struct phy_device *phy, unsigned int mode,
				  phy_interface_t interface, int speed,
				  int duplex, bool tx_pause, bool rx_pause)
{
	struct enetc_pf *pf = phylink_to_enetc_pf(config);
	struct enetc_si *si = pf->si;
	struct enetc_ndev_priv *priv;
	bool hd_fc = false;

	priv = netdev_priv(si->ndev);
	enetc4_set_port_speed(priv, speed);

	if (!phylink_autoneg_inband(mode) &&
	    phy_interface_mode_is_rgmii(interface))
		enetc4_set_rgmii_mac(pf, speed, duplex);

	if (interface == PHY_INTERFACE_MODE_RMII)
		enetc4_set_rmii_mac(pf, speed, duplex);

	if (duplex == DUPLEX_FULL) {
		/* When preemption is enabled, generation of PAUSE frames
		 * must be disabled, as stated in the IEEE 802.3 standard.
		 */
		if (priv->active_offloads & ENETC_F_QBU)
			tx_pause = false;
	} else { /* DUPLEX_HALF */
		if (tx_pause || rx_pause)
			hd_fc = true;

		/* As per 802.3 annex 31B, PAUSE frames are only supported
		 * when the link is configured for full duplex operation.
		 */
		tx_pause = false;
		rx_pause = false;
	}

	enetc4_set_hd_flow_control(pf, hd_fc);
	enetc4_set_tx_pause(pf, priv->num_rx_rings, tx_pause);
	enetc4_set_rx_pause(pf, rx_pause);
	enetc4_enable_mac(pf, true);

	if (si->hw_features & ENETC_SI_F_QBU)
		enetc_mm_link_state_update(priv, true);
}

static void enetc4_pl_mac_link_down(struct phylink_config *config,
				    unsigned int mode,
				    phy_interface_t interface)
{
	struct enetc_pf *pf = phylink_to_enetc_pf(config);
	struct enetc_si *si = pf->si;
	struct enetc_ndev_priv *priv;

	priv = netdev_priv(si->ndev);

	if (si->hw_features & ENETC_SI_F_QBU)
		enetc_mm_link_state_update(priv, false);

	enetc4_enable_mac(pf, false);
}

static const struct phylink_mac_ops enetc_pl_mac_ops = {
	.validate = phylink_generic_validate,
	.mac_select_pcs = enetc4_pl_mac_select_pcs,
	.mac_config = enetc4_pl_mac_config,
	.mac_link_up = enetc4_pl_mac_link_up,
	.mac_link_down = enetc4_pl_mac_link_down,
};

static int enetc4_alloc_cls_rules(struct enetc_ndev_priv *priv)
{
	struct enetc_pf *pf = enetc_si_priv(priv->si);

	/* Each ingress port filter entry occupies 2 words at least. */
	priv->max_ipf_entries = pf->caps.ipf_words_num / 2;
	priv->cls_rules = kcalloc(priv->max_ipf_entries, sizeof(*priv->cls_rules),
				  GFP_KERNEL);
	if (!priv->cls_rules)
		return -ENOMEM;

	return 0;
}

static void enetc4_free_cls_rules(struct enetc_ndev_priv *priv)
{
	kfree(priv->cls_rules);
}

static void enetc4_pf_set_si_primary_mac(struct enetc_hw *hw, int si, const u8 *addr)
{
	u16 lower = get_unaligned_le16(addr + 4);
	u32 upper = get_unaligned_le32(addr);

	if (si != 0) {
		__raw_writel(upper, hw->port + ENETC4_PSIPMAR0(si));
		__raw_writew(lower, hw->port + ENETC4_PSIPMAR1(si));
	} else {
		__raw_writel(upper, hw->port + ENETC4_PMAR0);
		__raw_writew(lower, hw->port + ENETC4_PMAR1);
	}
}

static void enetc4_pf_get_si_primary_mac(struct enetc_hw *hw, int si, u8 *addr)
{
	u32 upper;
	u16 lower;

	upper = __raw_readl(hw->port + ENETC4_PSIPMAR0(si));
	lower = __raw_readw(hw->port + ENETC4_PSIPMAR1(si));

	put_unaligned_le32(upper, addr);
	put_unaligned_le16(lower, addr + 4);
}

static void enetc4_pf_set_si_based_vlan(struct enetc_hw *hw, int si,
					u16 vlan, u8 qos)
{
	u32 val = 0;

	if (vlan) {
		val = PSIVLANR_E | (vlan & PSIVLANR_VID);
		val = u32_replace_bits(val, qos, PSIVLANR_PCP);
	}

	enetc_port_wr(hw, ENETC4_PSIVLANR(si), val);
}

static void enetc4_pf_set_si_anti_spoofing(struct enetc_hw *hw, int si, bool en)
{
	u32 val = enetc_port_rd(hw, ENETC4_PSICFGR0(si));

	val = (val & ~PSICFGR0_ANTI_SPOOFING) | (en ? PSICFGR0_ANTI_SPOOFING : 0);
	enetc_port_wr(hw, ENETC4_PSICFGR0(si), val);
}

static void enetc4_pf_set_si_vlan_promisc(struct enetc_hw *hw, char si_map)
{
	u32 val = enetc_port_rd(hw, ENETC4_PSIPVMR);

	val = u32_replace_bits(val, ENETC_PSIPVMR_SET_VP(si_map),
			       ENETC_VLAN_PROMISC_MAP_ALL);
	enetc_port_wr(hw, ENETC4_PSIPVMR, val);
}

static void enetc4_pf_set_si_mac_promisc(struct enetc_hw *hw, int si, int type, bool en)
{
	u32 val = enetc_port_rd(hw, ENETC4_PSIPMMR);

	if (type == UC) {
		if (en)
			val |= ENETC_PSIPMR_SET_UP(si);
		else
			val &= ~ENETC_PSIPMR_SET_UP(si);
	} else { /* Multicast promiscuous mode. */
		if (en)
			val |= ENETC_PSIPMR_SET_MP(si);
		else
			val &= ~ENETC_PSIPMR_SET_MP(si);
	}

	enetc_port_wr(hw, ENETC4_PSIPMMR, val);
}

static void enetc4_pf_set_si_mac_filter(struct enetc_hw *hw, int si,
					int type, u64 hash)
{
	if (type == UC) {
		enetc_port_wr(hw, ENETC4_PSIUMHFR0(si), lower_32_bits(hash));
		enetc_port_wr(hw, ENETC4_PSIUMHFR1(si), upper_32_bits(hash));
	} else { /* MC */
		enetc_port_wr(hw, ENETC4_PSIMMHFR0(si), lower_32_bits(hash));
		enetc_port_wr(hw, ENETC4_PSIMMHFR1(si), upper_32_bits(hash));
	}
}

static void enetc4_pf_set_si_vlan_filter(struct enetc_hw *hw, int si, u64 hash)
{
	enetc_port_wr(hw, ENETC4_PSIVHFR0(si), lower_32_bits(hash));
	enetc_port_wr(hw, ENETC4_PSIVHFR1(si), upper_32_bits(hash));
}

static void enetc4_pf_set_loopback(struct net_device *ndev, bool en)
{
	struct enetc_ndev_priv *priv = netdev_priv(ndev);
	struct enetc_si *si = priv->si;
	u32 val;

	val = enetc_port_mac_rd(si, ENETC4_PM_CMD_CFG(0));
	/* Enable or disable loopback. */
	val = u32_replace_bits(val, en ? 1 : 0, PM_CMD_CFG_LOOP_EN);
	/* Default to select MAC level loopback mode if loopback is enabled. */
	val = u32_replace_bits(val, en ? LPBCK_MODE_MAC_LEVEL : 0,
			       PM_CMD_CFG_LPBK_MODE);

	enetc_port_mac_wr(si, ENETC4_PM_CMD_CFG(0), val);
}

static void enetc4_pf_set_tc_tsd(struct enetc_hw *hw, int tc, bool en)
{
	enetc_port_wr(hw, ENETC4_PTCTSDR(tc), en ? PTCTSDR_TSDE : 0);
}

static bool enetc4_pf_get_time_gating(struct enetc_hw *hw)
{
	return !!(enetc_port_rd(hw, ENETC4_PTGSCR) & PTGSCR_TGE);
}

static void enetc4_pf_set_time_gating(struct enetc_hw *hw, bool en)
{
	u32 old_val, val;

	old_val = enetc_port_rd(hw, ENETC4_PTGSCR);
	val = u32_replace_bits(old_val, en ? 1 : 0, PTGSCR_TGE);
	if (val != old_val)
		enetc_port_wr(hw, ENETC4_PTGSCR, val);
}

static const struct enetc_pf_hw_ops enetc4_pf_hw_ops = {
	.set_si_primary_mac = enetc4_pf_set_si_primary_mac,
	.get_si_primary_mac = enetc4_pf_get_si_primary_mac,
	.set_si_based_vlan = enetc4_pf_set_si_based_vlan,
	.set_si_anti_spoofing = enetc4_pf_set_si_anti_spoofing,
	.set_si_vlan_promisc = enetc4_pf_set_si_vlan_promisc,
	.set_si_mac_promisc = enetc4_pf_set_si_mac_promisc,
	.set_si_mac_filter = enetc4_pf_set_si_mac_filter,
	.set_si_vlan_filter = enetc4_pf_set_si_vlan_filter,
	.set_loopback = enetc4_pf_set_loopback,
	.set_tc_tsd = enetc4_pf_set_tc_tsd,
	.set_tc_msdu = enetc4_pf_set_tc_msdu,
	.reset_tc_msdu = enetc4_pf_reset_tc_msdu,
	.get_time_gating = enetc4_pf_get_time_gating,
	.set_time_gating = enetc4_pf_set_time_gating,
};

static int enetc4_pf_enable_clk(struct enetc_ndev_priv *priv, bool en)
{
	int ret;

	if (en) {
		ret = clk_prepare_enable(priv->ipg_clk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(priv->ref_clk);
		if (ret) {
			clk_disable_unprepare(priv->ipg_clk);
			return ret;
		}
	} else {
		clk_disable_unprepare(priv->ipg_clk);
		clk_disable_unprepare(priv->ref_clk);
	}

	return 0;
}

static int enetc4_pf_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct enetc_ndev_priv *priv;
	struct net_device *ndev;
	struct enetc_si *si;
	struct enetc_pf *pf;
	int err;

	err = netc_ierb_get_init_status();
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(dev, "Couldn't get IERB init status: %d\n", err);
		return err;
	}

	ndev = alloc_etherdev_mq(sizeof(struct enetc_ndev_priv), ENETC_MAX_NUM_TXQS);
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	mutex_init(&priv->mm_lock);

	priv->ipg_clk = devm_clk_get(dev, "ipg_clk");
	if (IS_ERR(priv->ipg_clk)) {
		dev_err(dev, "Get ipg_clk failed\n");
		return PTR_ERR(priv->ipg_clk);
	}

	priv->ref_clk = devm_clk_get_optional(dev, "enet_ref_clk");
	if (IS_ERR(priv->ref_clk)) {
		dev_err(dev, "Get enet_ref_clk failed\n");
		return PTR_ERR(priv->ref_clk);
	}

	err = enetc4_pf_enable_clk(priv, true);
	if (err) {
		dev_err(dev, "Enable clocks failed\n");
		return err;
	}

	pinctrl_pm_select_default_state(&pdev->dev);

	err = enetc_pci_probe(pdev, KBUILD_MODNAME, sizeof(*pf));
	if (err) {
		dev_err(dev, "PCIe probing failed\n");
		goto err_pci_probe;
	}

	/* si is the private data. */
	si = pci_get_drvdata(pdev);
	if (!si->hw.port || !si->hw.global) {
		err = -ENODEV;
		dev_err(dev, "Couldn't map PF only space!\n");
		goto err_map_pf_space;
	}

	pf = enetc_si_priv(si);
	pf->si = si;
	/* Get total VFs supported on this device. */
	pf->total_vfs = pci_sriov_get_totalvfs(pdev);
	enetc_pf_register_hw_ops(pf, &enetc4_pf_hw_ops);

	/* Set the control BD ring for PF. */
	err = enetc_init_cbdr(si);
	if (err)
		goto err_init_cbdr;

	/* Initialize the MAC address for PF and VFs */
	err = enetc_setup_mac_addresses(node, pf);
	if (err)
		goto err_init_address;

	enetc4_get_port_caps(pf);
	enetc4_configure_port(pf);
	enetc_get_si_caps(si);
	enetc_pf_netdev_setup(si, ndev, &enetc4_ndev_ops);

	enetc_init_si_rings_params(priv);
	err = enetc_configure_si(priv);
	if (err) {
		dev_err(dev, "Failed to configure SI\n");
		goto err_config_si;
	}

	err = enetc4_alloc_cls_rules(priv);
	if (err) {
		dev_err(dev, "Failed to alloc cls rules memory\n");
		goto err_alloc_cls_rules;
	}

	err = enetc_alloc_msix(priv);
	if (err) {
		dev_err(dev, "MSIX alloc failed\n");
		goto err_alloc_msix;
	}

	err = of_get_phy_mode(node, &pf->if_mode);
	if (err) {
		dev_err(dev, "Failed to read PHY mode\n");
		goto err_phy_mode;
	}

	err = enetc_mdiobus_create(pf, node);
	if (err)
		goto err_mdiobus_create;

	err = enetc_phylink_create(priv, node, &enetc_pl_mac_ops);
	if (err)
		goto err_phylink_create;

	err = register_netdev(ndev);
	if (err)
		goto err_reg_netdev;

	enetc_create_debugfs(si);

	return 0;

err_reg_netdev:
	enetc_phylink_destroy(priv);
err_phylink_create:
	enetc_mdiobus_destroy(pf);
err_mdiobus_create:
err_phy_mode:
	enetc_free_msix(priv);
err_alloc_msix:
	enetc4_free_cls_rules(priv);
err_alloc_cls_rules:
err_config_si:
	si->ndev = NULL;
	free_netdev(ndev);
err_init_address:
	enetc_free_cbdr(si);
err_init_cbdr:
err_map_pf_space:
	enetc_pci_remove(pdev);
err_pci_probe:
	enetc4_pf_enable_clk(priv, false);

	return err;
}

static void enetc4_pf_remove(struct pci_dev *pdev)
{
	struct enetc_si *si = pci_get_drvdata(pdev);
	struct enetc_pf *pf = enetc_si_priv(si);
	struct enetc_ndev_priv *priv;

	enetc_remove_debugfs(si);
	priv = netdev_priv(si->ndev);

	if (pf->num_vfs)
		enetc_sriov_configure(pdev, 0);

	unregister_netdev(si->ndev);

	enetc_phylink_destroy(priv);
	enetc_mdiobus_destroy(pf);

	enetc_free_msix(priv);

	enetc4_free_cls_rules(priv);
	enetc_free_cbdr(si);

	free_netdev(si->ndev);

	enetc_pci_remove(pdev);

	enetc4_pf_enable_clk(priv, false);
}

/* Only ENETC PF Function can be probed. */
static const struct pci_device_id enetc4_pf_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_NXP2, PCI_DEVICE_ID_NXP2_ENETC_PF) },
	{ 0, } /* End of table. */
};
MODULE_DEVICE_TABLE(pci, enetc4_pf_id_table);

static int __maybe_unused enetc4_pf_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct enetc_ndev_priv *priv;
	struct enetc_si *si;

	si = pci_get_drvdata(pdev);
	if (!netif_running(si->ndev))
		return 0;

	priv = netdev_priv(si->ndev);
	netif_device_detach(si->ndev);
	rtnl_lock();
	phylink_suspend(priv->phylink, false);
	rtnl_unlock();

	return 0;
}

static int __maybe_unused enetc4_pf_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct enetc_ndev_priv *priv;
	struct enetc_si *si;

	si = pci_get_drvdata(pdev);
	if (!netif_running(si->ndev))
		return 0;

	priv = netdev_priv(si->ndev);
	rtnl_lock();
	phylink_resume(priv->phylink);
	rtnl_unlock();

	netif_device_attach(si->ndev);

	return 0;
}

static SIMPLE_DEV_PM_OPS(enetc4_pf_pm_ops, enetc4_pf_suspend, enetc4_pf_resume);

static struct pci_driver enetc4_pf_driver = {
	.name = KBUILD_MODNAME,
	.id_table = enetc4_pf_id_table,
	.probe = enetc4_pf_probe,
	.remove = enetc4_pf_remove,
	.driver.pm = &enetc4_pf_pm_ops,
#ifdef CONFIG_PCI_IOV
	.sriov_configure = enetc_sriov_configure,
#endif
};
module_pci_driver(enetc4_pf_driver);

MODULE_DESCRIPTION("ENETC4 PF Driver");
MODULE_LICENSE("Dual BSD/GPL");
