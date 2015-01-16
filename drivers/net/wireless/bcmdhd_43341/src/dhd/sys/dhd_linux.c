/*
 * Broadcom Dongle Host Driver (DHD), Linux-specific network interface
 * Basically selected code segments from usb-cdc.c and usb-rndis.c
 *
 * $Copyright Open Broadcom Corporation$
 *
 * $Id: dhd_linux.c 410252 2013-06-28 18:04:06Z $
 */

#include <typedefs.h>
#include <linuxver.h>
#include <osl.h>

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/ip.h>

#include <asm/uaccess.h>
#include <asm/unaligned.h>

#include <epivers.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <bcmdevs.h>

#include <proto/ethernet.h>
#include <proto/bcmevent.h>
#include <dngl_stats.h>
#include <dhd.h>
#include <dhd_bus.h>
#include <dhd_proto.h>
#include <dhd_dbg.h>
#ifdef CONFIG_HAS_WAKELOCK
#include <linux/wakelock.h>
#endif
#ifdef WL_CFG80211
#include <wl_cfg80211.h>
#endif

#ifdef WLBTAMP
#include <proto/802.11_bta.h>
#include <proto/bt_amp_hci.h>
#include <dhd_bta.h>
#endif

#ifdef WLMEDIA_HTSF
#include <linux/time.h>
#include <htsf.h>

#define HTSF_MINLEN 200    /* min. packet length to timestamp */
#define HTSF_BUS_DELAY 150 /* assume a fix propagation in us  */
#define TSMAX  1000        /* max no. of timing record kept   */
#define NUMBIN 34

static uint32 tsidx = 0;
static uint32 htsf_seqnum = 0;
uint32 tsfsync;
struct timeval tsync;
static uint32 tsport = 5010;

typedef struct histo_ {
	uint32 bin[NUMBIN];
} histo_t;

#if !ISPOWEROF2(DHD_SDALIGN)
#error DHD_SDALIGN is not a power of 2!
#endif

static histo_t vi_d1, vi_d2, vi_d3, vi_d4;
#endif /* WLMEDIA_HTSF */

#if defined(BLOCK_IPV6_PACKET) && defined(CUSTOMER_HW4)
#define HEX_PREF_STR	"0x"
#define UNI_FILTER_STR	"010000000000"
#define ZERO_ADDR_STR	"000000000000"
#define ETHER_TYPE_STR	"0000"
#define IPV6_FILTER_STR	"20"
#define ZERO_TYPE_STR	"00"
#endif /* BLOCK_IPV6_PACKET && CUSTOMER_HW4 */

#if defined(SOFTAP)
extern bool ap_cfg_running;
extern bool ap_fw_loaded;
#endif


/* enable HOSTIP cache update from the host side when an eth0:N is up */
#define AOE_IP_ALIAS_SUPPORT 1

#ifdef BCM_FD_AGGR
#include <bcm_rpc.h>
#include <bcm_rpc_tp.h>
#endif
#ifdef PROP_TXSTATUS
#include <wlfc_proto.h>
#include <dhd_wlfc.h>
#endif

#include <wl_android.h>

#ifdef ARP_OFFLOAD_SUPPORT
void aoe_update_host_ipv4_table(dhd_pub_t *dhd_pub, u32 ipa, bool add, int idx);
static int dhd_device_event(struct notifier_block *this,
	unsigned long event,
	void *ptr);

static struct notifier_block dhd_notifier = {
	.notifier_call = dhd_device_event
};
#endif /* ARP_OFFLOAD_SUPPORT */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP)
#include <linux/suspend.h>
volatile bool dhd_mmc_suspend = FALSE;
DECLARE_WAIT_QUEUE_HEAD(dhd_dpc_wait);
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP) */

#if defined(OOB_INTR_ONLY)
extern void dhd_enable_oob_intr(struct dhd_bus *bus, bool enable);
#endif 
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && (1)
static void dhd_hang_process(struct work_struct *work);
#endif 
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0))
MODULE_LICENSE("GPL v2");
#endif /* LinuxVer */

#include <dhd_bus.h>

#ifdef BCM_FD_AGGR
#define DBUS_RX_BUFFER_SIZE_DHD(net)	(BCM_RPC_TP_DNGL_AGG_MAX_BYTE)
#else
#ifndef PROP_TXSTATUS
#define DBUS_RX_BUFFER_SIZE_DHD(net)	(net->mtu + net->hard_header_len + dhd->pub.hdrlen)
#else
#define DBUS_RX_BUFFER_SIZE_DHD(net)	(net->mtu + net->hard_header_len + dhd->pub.hdrlen + 128)
#endif
#endif /* BCM_FD_AGGR */

#ifdef PROP_TXSTATUS
extern bool dhd_wlfc_skip_fc(void);
extern void dhd_wlfc_plat_enable(void *dhd);
extern void dhd_wlfc_plat_deinit(void *dhd);
#if defined(CUSTOMER_HW4) && defined(USE_DYNAMIC_F2_BLKSIZE)
extern uint sd_f2_blocksize;
extern int dhdsdio_func_blocksize(dhd_pub_t *dhd, int function_num, int block_size);
#endif /* CUSTOMER_HW4 && USE_DYNAMIC_F2_BLKSIZE */
#endif /* PROP_TXSTATUS */

#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 15)
const char *
print_tainted()
{
	return "";
}
#endif	/* LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 15) */

/* Linux wireless extension support */
#if defined(WL_WIRELESS_EXT)
#include <wl_iw.h>
extern wl_iw_extra_params_t  g_wl_iw_params;
#endif /* defined(WL_WIRELESS_EXT) */

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif /* defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND) */

extern int dhd_get_suspend_bcn_li_dtim(dhd_pub_t *dhd);

#ifdef PKT_FILTER_SUPPORT
extern void dhd_pktfilter_offload_set(dhd_pub_t * dhd, char *arg);
extern void dhd_pktfilter_offload_enable(dhd_pub_t * dhd, char *arg, int enable, int master_mode);
#endif


#ifdef READ_MACADDR
extern int dhd_read_macaddr(struct dhd_info *dhd);
#endif
#ifdef WRITE_MACADDR
extern int dhd_write_macaddr(struct ether_addr *mac);
#endif

typedef struct dhd_if_event {
	struct list_head	list;
	wl_event_data_if_t	event;
	char			name[IFNAMSIZ+1];
	uint8			mac[ETHER_ADDR_LEN];
} dhd_if_event_t;

/* Interface control information */
typedef struct dhd_if {
	struct dhd_info *info;			/* back pointer to dhd_info */
	/* OS/stack specifics */
	struct net_device *net;
	struct net_device_stats stats;
	int 			idx;			/* iface idx in dongle */
	uint 			subunit;		/* subunit */
	uint8			mac_addr[ETHER_ADDR_LEN];	/* assigned MAC address */
	bool			attached;		/* Delayed attachment when unset */
	bool			txflowcontrol;	/* Per interface flow control indicator */
	char			name[IFNAMSIZ+1]; /* linux interface name */
	uint8			bssidx;			/* bsscfg index for the interface */
	bool			set_macaddress;
	bool			set_multicast;
} dhd_if_t;

#ifdef WLMEDIA_HTSF
typedef struct {
	uint32 low;
	uint32 high;
} tsf_t;

typedef struct {
	uint32 last_cycle;
	uint32 last_sec;
	uint32 last_tsf;
	uint32 coef;     /* scaling factor */
	uint32 coefdec1; /* first decimal  */
	uint32 coefdec2; /* second decimal */
} htsf_t;

typedef struct {
	uint32 t1;
	uint32 t2;
	uint32 t3;
	uint32 t4;
} tstamp_t;

static tstamp_t ts[TSMAX];
static tstamp_t maxdelayts;
static uint32 maxdelay = 0, tspktcnt = 0, maxdelaypktno = 0;

#endif  /* WLMEDIA_HTSF */

/* Local private structure (extension of pub) */
typedef struct dhd_info {
#if defined(WL_WIRELESS_EXT)
	wl_iw_t		iw;		/* wireless extensions state (must be first) */
#endif /* defined(WL_WIRELESS_EXT) */

	dhd_pub_t pub;

	/* For supporting multiple interfaces */
	dhd_if_t *iflist[DHD_MAX_IFS];
	struct list_head if_event_list;
	spinlock_t	if_event_list_lock;

	struct semaphore proto_sem;
#ifdef PROP_TXSTATUS
	spinlock_t	wlfc_spinlock;
#endif /* PROP_TXSTATUS */
#ifdef WLMEDIA_HTSF
	htsf_t  htsf;
#endif
	wait_queue_head_t ioctl_resp_wait;
	uint32	default_wd_interval;

	struct timer_list timer;
	bool wd_timer_valid;
	struct tasklet_struct tasklet;
	spinlock_t	sdlock;
	spinlock_t	txqlock;
	spinlock_t	dhd_lock;

	struct semaphore sdsem;
	tsk_ctl_t	thr_dpc_ctl;
	tsk_ctl_t	thr_wdt_ctl;

	tsk_ctl_t	thr_rxf_ctl;
	spinlock_t	rxf_lock;
	bool		rxthread_enabled;
	struct work_struct	sysioc_work;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	struct work_struct work_hang;
#endif

	/* Wakelocks */
#if defined(CONFIG_HAS_WAKELOCK) && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	struct wake_lock wl_wifi;   /* Wifi wakelock */
	struct wake_lock wl_rxwake; /* Wifi rx wakelock */
	struct wake_lock wl_ctrlwake; /* Wifi ctrl wakelock */
	struct wake_lock wl_wdwake; /* Wifi wd wakelock */
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	/* net_device interface lock, prevent race conditions among net_dev interface
	 * calls and wifi_on or wifi_off
	 */
	struct mutex dhd_net_if_mutex;
	struct mutex dhd_suspend_mutex;
#endif
	spinlock_t wakelock_spinlock;
	int wakelock_counter;
	int wakelock_wd_counter;
	int wakelock_rx_timeout_enable;
	int wakelock_ctrl_timeout_enable;

	/* Thread to issue ioctl for multicast */
	wait_queue_head_t ctrl_wait;
	atomic_t pend_8021x_cnt;
	dhd_attach_states_t dhd_state;

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
	struct early_suspend early_suspend;
#endif /* CONFIG_HAS_EARLYSUSPEND && DHD_USE_EARLYSUSPEND */

#ifdef ARP_OFFLOAD_SUPPORT
	u32 pend_ipaddr;
#endif /* ARP_OFFLOAD_SUPPORT */
#ifdef BCM_FD_AGGR
	void *rpc_th;
	void *rpc_osh;
	struct timer_list rpcth_timer;
	bool rpcth_timer_active;
	bool fdaggr;
#endif
#ifdef DHDTCPACK_SUPPRESS
	spinlock_t	tcpack_lock;
#endif /* DHDTCPACK_SUPPRESS */
} dhd_info_t;

/* Flag to indicate if we should download firmware on driver load */
uint dhd_download_fw_on_driverload = TRUE;

/* Definitions to provide path to the firmware and nvram
 * example nvram_path[MOD_PARAM_PATHLEN]="/projects/wlan/nvram.txt"
 */
char firmware_path[MOD_PARAM_PATHLEN];
char nvram_path[MOD_PARAM_PATHLEN];

/* information string to keep firmware, chio, cheip version info visiable from log */
char info_string[MOD_PARAM_INFOLEN];
module_param_string(info_string, info_string, MOD_PARAM_INFOLEN, 0444);
int op_mode = 0;
int disable_proptx = 0;
module_param(op_mode, int, 0644);
extern int wl_control_wl_start(struct net_device *dev);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
struct semaphore dhd_registration_sem;
#if !defined(CUSTOMER_HW10)
struct semaphore dhd_chipup_sem;
#endif
int dhd_registration_check = FALSE;

#define DHD_REGISTRATION_TIMEOUT  12000  /* msec : allowed time to finished dhd registration */
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) */
#ifdef CUSTOMER_HW10
int roam_trigger[2] = {CUSTOM_ROAM_TRIGGER_SETTING, WLC_BAND_ALL};
#endif

/* Error bits */
module_param(dhd_msg_level, int, 0);

#ifdef ARP_OFFLOAD_SUPPORT
/* ARP offload enable */
uint dhd_arp_enable = TRUE;
module_param(dhd_arp_enable, uint, 0);

/* ARP offload agent mode : Enable ARP Host Auto-Reply and ARP Peer Auto-Reply */

#if defined(CUSTOMER_HW4)
uint dhd_arp_mode = ARP_OL_AGENT | ARP_OL_PEER_AUTO_REPLY | ARP_OL_SNOOP;
#else
uint dhd_arp_mode = ARP_OL_AGENT | ARP_OL_PEER_AUTO_REPLY;
#endif

module_param(dhd_arp_mode, uint, 0);
#endif /* ARP_OFFLOAD_SUPPORT */



/* Disable Prop tx */
module_param(disable_proptx, int, 0644);
/* load firmware and/or nvram values from the filesystem */
module_param_string(firmware_path, firmware_path, MOD_PARAM_PATHLEN, 0660);
#ifdef CUSTOMER_HW4
module_param_string(nvram_path, nvram_path, MOD_PARAM_PATHLEN, 0660);
#else
module_param_string(nvram_path, nvram_path, MOD_PARAM_PATHLEN, 0);
#endif /* CUSTOMER_HW4 */

/* Watchdog interval */

/* extend watchdog expiration to 2 seconds when DPC is running */
#define WATCHDOG_EXTEND_INTERVAL (2000)

uint dhd_watchdog_ms = 10;
module_param(dhd_watchdog_ms, uint, 0);

#if defined(DHD_DEBUG)
/* Console poll interval */
uint dhd_console_ms = 0;
module_param(dhd_console_ms, uint, 0644);
#endif /* defined(DHD_DEBUG) */


uint dhd_slpauto = TRUE;
module_param(dhd_slpauto, uint, 0);

#ifdef PKT_FILTER_SUPPORT
/* Global Pkt filter enable control */
uint dhd_pkt_filter_enable = TRUE;
module_param(dhd_pkt_filter_enable, uint, 0);
#endif

/* Pkt filter init setup */
uint dhd_pkt_filter_init = 0;
module_param(dhd_pkt_filter_init, uint, 0);

/* Pkt filter mode control */
#ifdef GAN_LITE_NAT_KEEPALIVE_FILTER
uint dhd_master_mode = FALSE;
#else
uint dhd_master_mode = TRUE;
#endif /* GAL_LITE_NAT_KEEPALIVE_FILTER */
module_param(dhd_master_mode, uint, 0);

int dhd_watchdog_prio = 0;
module_param(dhd_watchdog_prio, int, 0);

/* DPC thread priority */
int dhd_dpc_prio = CUSTOM_DPC_PRIO_SETTING;
module_param(dhd_dpc_prio, int, 0);

/* RX frame thread priority */
int dhd_rxf_prio = CUSTOM_RXF_PRIO_SETTING;
module_param(dhd_rxf_prio, int, 0);

/* DPC thread priority, -1 to use tasklet */
extern int dhd_dongle_ramsize;
module_param(dhd_dongle_ramsize, int, 0);
/* Control fw roaming */
#ifdef BCMCCX
uint dhd_roam_disable = 0;
#else
uint dhd_roam_disable = 0;
#endif /* BCMCCX */

/* Control radio state */
uint dhd_radio_up = 1;

/* Network inteface name */
char iface_name[IFNAMSIZ] = {'\0'};
module_param_string(iface_name, iface_name, IFNAMSIZ, 0);

/* The following are specific to the SDIO dongle */

/* IOCTL response timeout */
int dhd_ioctl_timeout_msec = IOCTL_RESP_TIMEOUT;

/* Idle timeout for backplane clock */
int dhd_idletime = DHD_IDLETIME_TICKS;
module_param(dhd_idletime, int, 0);

/* Use polling */
uint dhd_poll = FALSE;
module_param(dhd_poll, uint, 0);

/* Use interrupts */
uint dhd_intr = TRUE;
module_param(dhd_intr, uint, 0);

/* SDIO Drive Strength (in milliamps) */
uint dhd_sdiod_drive_strength = 6;
module_param(dhd_sdiod_drive_strength, uint, 0);

/* Tx/Rx bounds */
extern uint dhd_txbound;
extern uint dhd_rxbound;
module_param(dhd_txbound, uint, 0);
module_param(dhd_rxbound, uint, 0);

/* Deferred transmits */
extern uint dhd_deferred_tx;
module_param(dhd_deferred_tx, uint, 0);

#ifdef BCMDBGFS
extern void dhd_dbg_init(dhd_pub_t *dhdp);
extern void dhd_dbg_remove(void);
#endif /* BCMDBGFS */



#ifdef SDTEST
/* Echo packet generator (pkts/s) */
uint dhd_pktgen = 0;
module_param(dhd_pktgen, uint, 0);

/* Echo packet len (0 => sawtooth, max 2040) */
uint dhd_pktgen_len = 0;
module_param(dhd_pktgen_len, uint, 0);
#endif /* SDTEST */

#if defined(BCMSUP_4WAY_HANDSHAKE)
/* Use in dongle supplicant for 4-way handshake */
uint dhd_use_idsup = 0;
module_param(dhd_use_idsup, uint, 0);
#endif /* BCMSUP_4WAY_HANDSHAKE */

extern char dhd_version[];

static void dhd_net_if_lock_local(dhd_info_t *dhd);
static void dhd_net_if_unlock_local(dhd_info_t *dhd);
static void dhd_suspend_lock(dhd_pub_t *dhdp);
static void dhd_suspend_unlock(dhd_pub_t *dhdp);

#ifdef WLMEDIA_HTSF
void htsf_update(dhd_info_t *dhd, void *data);
tsf_t prev_tsf, cur_tsf;

uint32 dhd_get_htsf(dhd_info_t *dhd, int ifidx);
static int dhd_ioctl_htsf_get(dhd_info_t *dhd, int ifidx);
static void dhd_dump_latency(void);
static void dhd_htsf_addtxts(dhd_pub_t *dhdp, void *pktbuf);
static void dhd_htsf_addrxts(dhd_pub_t *dhdp, void *pktbuf);
static void dhd_dump_htsfhisto(histo_t *his, char *s);
#endif /* WLMEDIA_HTSF */

/* Monitor interface */
int dhd_monitor_init(void *dhd_pub);
int dhd_monitor_uninit(void);

#if defined(CONFIG_PM_LOCK)
bool g_pm_control;
#endif /* CONFIG_PM_LOCK */



#if defined(WL_WIRELESS_EXT)
struct iw_statistics *dhd_get_wireless_stats(struct net_device *dev);
#endif /* defined(WL_WIRELESS_EXT) */

static void dhd_dpc(ulong data);
/* forward decl */
extern int dhd_wait_pend8021x(struct net_device *dev);
void dhd_os_wd_timer_extend(void *bus, bool extend);

#ifdef TOE
#ifndef BDC
#error TOE requires BDC
#endif /* !BDC */
static int dhd_toe_get(dhd_info_t *dhd, int idx, uint32 *toe_ol);
static int dhd_toe_set(dhd_info_t *dhd, int idx, uint32 toe_ol);
#endif /* TOE */

static int dhd_wl_host_event(dhd_info_t *dhd, int *ifidx, void *pktdata,
                             wl_event_msg_t *event_ptr, void **data_ptr);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP)
static int dhd_sleep_pm_callback(struct notifier_block *nfb, unsigned long action, void *ignored)
{
	int ret = NOTIFY_DONE;

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 39))
	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		dhd_mmc_suspend = TRUE;
		ret = NOTIFY_OK;
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		dhd_mmc_suspend = FALSE;
		ret = NOTIFY_OK;
		break;
	}
	smp_mb();
#endif
	return ret;
}

static struct notifier_block dhd_sleep_pm_notifier = {
	.notifier_call = dhd_sleep_pm_callback,
	.priority = 10
};
extern int register_pm_notifier(struct notifier_block *nb);
extern int unregister_pm_notifier(struct notifier_block *nb);
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP) */

/* Request scheduling of the bus rx frame */
static void dhd_sched_rxf(dhd_pub_t *dhdp, void *skb);
static void dhd_os_rxflock(dhd_pub_t *pub);
static void dhd_os_rxfunlock(dhd_pub_t *pub);

static inline int dhd_rxf_enqueue(dhd_pub_t *dhdp, void* skb)
{
	uint32 store_idx;
	uint32 sent_idx;

	if (!skb) {
		DHD_ERROR(("dhd_rxf_enqueue: NULL skb!!!\n"));
		return BCME_ERROR;
	}

	dhd_os_rxflock(dhdp);
	store_idx = dhdp->store_idx;
	sent_idx = dhdp->sent_idx;
	if (dhdp->skbbuf[store_idx] != NULL) {
		/* Make sure the previous packets are processed */
		dhd_os_rxfunlock(dhdp);
		DHD_ERROR(("dhd_rxf_enqueue: pktbuf not consumed %p, store idx %d sent idx %d\n",
			skb, store_idx, sent_idx));
		/* removed msleep here, should use wait_event_timeout if we
		 * want to give rx frame thread a chance to run
		 */
		return BCME_ERROR;
	}
	DHD_TRACE(("dhd_rxf_enqueue: Store SKB %p. idx %d -> %d\n",
		skb, store_idx, (store_idx + 1) & (MAXSKBPEND - 1)));
	dhdp->skbbuf[store_idx] = skb;
	dhdp->store_idx = (store_idx + 1) & (MAXSKBPEND - 1);
	dhd_os_rxfunlock(dhdp);

	return BCME_OK;
}

static inline void* dhd_rxf_dequeue(dhd_pub_t *dhdp)
{
	uint32 store_idx;
	uint32 sent_idx;
	void *skb;

	dhd_os_rxflock(dhdp);

	store_idx = dhdp->store_idx;
	sent_idx = dhdp->sent_idx;
	skb = dhdp->skbbuf[sent_idx];

	if (skb == NULL) {
		dhd_os_rxfunlock(dhdp);
		DHD_ERROR(("dhd_rxf_dequeue: Dequeued packet is NULL, store idx %d sent idx %d\n",
			store_idx, sent_idx));
		return NULL;
	}

	dhdp->skbbuf[sent_idx] = NULL;
	dhdp->sent_idx = (sent_idx + 1) & (MAXSKBPEND - 1);

	DHD_TRACE(("dhd_rxf_dequeue: netif_rx_ni(%p), sent idx %d\n",
		skb, sent_idx));

	dhd_os_rxfunlock(dhdp);

	return skb;
}

void dhd_set_packet_filter(dhd_pub_t *dhd)
{
#ifdef PKT_FILTER_SUPPORT
	int i;

	DHD_TRACE(("%s: enter\n", __FUNCTION__));
	if (dhd_pkt_filter_enable) {
		for (i = 0; i < dhd->pktfilter_count; i++) {
			dhd_pktfilter_offload_set(dhd, dhd->pktfilter[i]);
		}
	}
#endif /* PKT_FILTER_SUPPORT */
}

void dhd_enable_packet_filter(int value, dhd_pub_t *dhd)
{
#ifdef PKT_FILTER_SUPPORT
	int i;

	DHD_TRACE(("%s: enter, value = %d\n", __FUNCTION__, value));
	/* 1 - Enable packet filter, only allow unicast packet to send up */
	/* 0 - Disable packet filter */
	if (dhd_pkt_filter_enable && (!value ||
	    (dhd_support_sta_mode(dhd) && !dhd->dhcp_in_progress)))
	    {
		for (i = 0; i < dhd->pktfilter_count; i++) {
#if !defined(GAN_LITE_NAT_KEEPALIVE_FILTER) && defined(PASS_ARP_PACKET)
			if (value && (i == dhd->pktfilter_count -1) &&
				!(dhd->op_mode & (DHD_FLAG_P2P_GC_MODE | DHD_FLAG_P2P_GO_MODE))) {
				DHD_TRACE_HW4(("Do not turn on ARP white list pkt filter:"
					"val %d, cnt %d, op_mode 0x%x\n",
					value, i, dhd->op_mode));
				continue;
			}
#endif /* !GAN_LITE_NAT_KEEPALIVE_FILTER && PASS_ARP_PACKET */
			dhd_pktfilter_offload_enable(dhd, dhd->pktfilter[i],
				value, dhd_master_mode);
		}
	}
#endif /* PKT_FILTER_SUPPORT */
}

static int dhd_set_suspend(int value, dhd_pub_t *dhd)
{
#ifndef SUPPORT_PM2_ONLY
	int power_mode = PM_MAX;
#endif /* SUPPORT_PM2_ONLY */
	/* wl_pkt_filter_enable_t	enable_parm; */
	char iovbuf[32];
	int bcn_li_dtim = 0; /* Default bcn_li_dtim in resume mode is 0 */
#ifndef ENABLE_FW_ROAM_SUSPEND
	uint roamvar = 1;
#endif /* ENABLE_FW_ROAM_SUSPEND */
#if defined(CUSTOMER_HW4) && defined(ENABLE_BCN_LI_BCN_WAKEUP)
	int bcn_li_bcn;
#endif /* CUSTOMER_HW4 && ENABLE_BCN_LI_BCN_WAKEUP */
#if defined(PASS_ALL_MCAST_PKTS) && defined(CUSTOMER_HW4)
	struct dhd_info *dhdinfo = dhd->info;
	uint32 allmulti;
	uint i;
#endif /* PASS_ALL_MCAST_PKTS && CUSTOMER_HW4 */

	if (!dhd)
		return -ENODEV;

	DHD_TRACE(("%s: enter, value = %d in_suspend=%d\n",
		__FUNCTION__, value, dhd->in_suspend));

	dhd_suspend_lock(dhd);
	if (dhd->up) {
		if (value && dhd->in_suspend) {
#ifdef PKT_FILTER_SUPPORT
				dhd->early_suspended = 1;
#endif
				/* Kernel suspended */
				DHD_ERROR(("%s: force extra Suspend setting \n", __FUNCTION__));

#ifndef SUPPORT_PM2_ONLY
				dhd_wl_ioctl_cmd(dhd, WLC_SET_PM, (char *)&power_mode,
				                 sizeof(power_mode), TRUE, 0);
#endif /* SUPPORT_PM2_ONLY */

				/* Enable packet filter, only allow unicast packet to send up */
				dhd_enable_packet_filter(1, dhd);

#if defined(PASS_ALL_MCAST_PKTS) && defined(CUSTOMER_HW4)
				allmulti = 0;
				bcm_mkiovar("allmulti", (char *)&allmulti, 4,
					iovbuf, sizeof(iovbuf));
				for (i = 0; i < DHD_MAX_IFS; i++) {
					if (dhdinfo->iflist[i] && dhdinfo->iflist[i]->net)
						dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
							sizeof(iovbuf), TRUE, i);
				}
#endif /* PASS_ALL_MCAST_PKTS && CUSTOMER_HW4 */

				/* If DTIM skip is set up as default, force it to wake
				 * each third DTIM for better power savings.  Note that
				 * one side effect is a chance to miss BC/MC packet.
				 */
				bcn_li_dtim = dhd_get_suspend_bcn_li_dtim(dhd);
				bcm_mkiovar("bcn_li_dtim", (char *)&bcn_li_dtim,
					4, iovbuf, sizeof(iovbuf));
				if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf),
					TRUE, 0) < 0)
					DHD_ERROR(("%s: set dtim failed\n", __FUNCTION__));

#ifndef ENABLE_FW_ROAM_SUSPEND
				/* Disable firmware roaming during suspend */
				bcm_mkiovar("roam_off", (char *)&roamvar, 4,
					iovbuf, sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* ENABLE_FW_ROAM_SUSPEND */
#if defined(CUSTOMER_HW4) && defined(ENABLE_BCN_LI_BCN_WAKEUP)
				bcn_li_bcn = 0;
				bcm_mkiovar("bcn_li_bcn", (char *)&bcn_li_bcn,
					4, iovbuf, sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* CUSTOMER_HW4 && ENABLE_BCN_LI_BCN_WAKEUP */
			} else {
#ifdef PKT_FILTER_SUPPORT
				dhd->early_suspended = 0;
#endif
				/* Kernel resumed  */
				DHD_ERROR(("%s: Remove extra suspend setting \n", __FUNCTION__));

#ifndef SUPPORT_PM2_ONLY
				power_mode = PM_FAST;
				dhd_wl_ioctl_cmd(dhd, WLC_SET_PM, (char *)&power_mode,
				                 sizeof(power_mode), TRUE, 0);
#endif /* SUPPORT_PM2_ONLY */
#ifdef PKT_FILTER_SUPPORT
				/* disable pkt filter */
				dhd_enable_packet_filter(0, dhd);
#endif /* PKT_FILTER_SUPPORT */
#if defined(PASS_ALL_MCAST_PKTS) && defined(CUSTOMER_HW4)
				allmulti = 1;
				bcm_mkiovar("allmulti", (char *)&allmulti, 4,
					iovbuf, sizeof(iovbuf));
				for (i = 0; i < DHD_MAX_IFS; i++) {
					if (dhdinfo->iflist[i] && dhdinfo->iflist[i]->net)
						dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
							sizeof(iovbuf), TRUE, i);
				}
#endif /* PASS_ALL_MCAST_PKTS && CUSTOMER_HW4 */

				/* restore pre-suspend setting for dtim_skip */
				bcm_mkiovar("bcn_li_dtim", (char *)&bcn_li_dtim,
					4, iovbuf, sizeof(iovbuf));

				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#ifndef ENABLE_FW_ROAM_SUSPEND
				roamvar = dhd_roam_disable;
				bcm_mkiovar("roam_off", (char *)&roamvar, 4, iovbuf,
					sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* ENABLE_FW_ROAM_SUSPEND */
#if defined(CUSTOMER_HW4) && defined(ENABLE_BCN_LI_BCN_WAKEUP)
				bcn_li_bcn = 1;
				bcm_mkiovar("bcn_li_bcn", (char *)&bcn_li_bcn,
					4, iovbuf, sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* CUSTOMER_HW4 && ENABLE_BCN_LI_BCN_WAKEUP */
			}
	}
	dhd_suspend_unlock(dhd);

	return 0;
}

static int dhd_suspend_resume_helper(struct dhd_info *dhd, int val, int force)
{
	dhd_pub_t *dhdp = &dhd->pub;
	int ret = 0;

	DHD_OS_WAKE_LOCK(dhdp);
	/* Set flag when early suspend was called */
	dhdp->in_suspend = val;
	if ((force || !dhdp->suspend_disable_flag) &&
		dhd_support_sta_mode(dhdp))
	{
		ret = dhd_set_suspend(val, dhdp);
	}

	DHD_OS_WAKE_UNLOCK(dhdp);
	return ret;
}

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
static void dhd_early_suspend(struct early_suspend *h)
{
	struct dhd_info *dhd = container_of(h, struct dhd_info, early_suspend);
	DHD_TRACE_HW4(("%s: enter\n", __FUNCTION__));

	if (dhd)
		dhd_suspend_resume_helper(dhd, 1, 0);
}

static void dhd_late_resume(struct early_suspend *h)
{
	struct dhd_info *dhd = container_of(h, struct dhd_info, early_suspend);
	DHD_TRACE_HW4(("%s: enter\n", __FUNCTION__));

	if (dhd)
		dhd_suspend_resume_helper(dhd, 0, 0);
}
#endif /* CONFIG_HAS_EARLYSUSPEND && DHD_USE_EARLYSUSPEND */

/*
 * Generalized timeout mechanism.  Uses spin sleep with exponential back-off until
 * the sleep time reaches one jiffy, then switches over to task delay.  Usage:
 *
 *      dhd_timeout_start(&tmo, usec);
 *      while (!dhd_timeout_expired(&tmo))
 *              if (poll_something())
 *                      break;
 *      if (dhd_timeout_expired(&tmo))
 *              fatal();
 */

void
dhd_timeout_start(dhd_timeout_t *tmo, uint usec)
{
	tmo->limit = usec;
	tmo->increment = 0;
	tmo->elapsed = 0;
	tmo->tick = jiffies_to_usecs(1);
}

int
dhd_timeout_expired(dhd_timeout_t *tmo)
{
	/* Does nothing the first call */
	if (tmo->increment == 0) {
		tmo->increment = 1;
		return 0;
	}

	if (tmo->elapsed >= tmo->limit)
		return 1;

	/* Add the delay that's about to take place */
	tmo->elapsed += tmo->increment;

	if (tmo->increment < tmo->tick) {
		OSL_DELAY(tmo->increment);
		tmo->increment *= 2;
		if (tmo->increment > tmo->tick)
			tmo->increment = tmo->tick;
	} else {
		wait_queue_head_t delay_wait;
		DECLARE_WAITQUEUE(wait, current);
		init_waitqueue_head(&delay_wait);
		add_wait_queue(&delay_wait, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(1);
		remove_wait_queue(&delay_wait, &wait);
		set_current_state(TASK_RUNNING);
	}

	return 0;
}

int
dhd_net2idx(dhd_info_t *dhd, struct net_device *net)
{
	int i = 0;

	ASSERT(dhd);
	while (i < DHD_MAX_IFS) {
		if (dhd->iflist[i] && (dhd->iflist[i]->net == net))
			return i;
		i++;
	}

	return DHD_BAD_IF;
}

struct net_device * dhd_idx2net(void *pub, int ifidx)
{
	struct dhd_pub *dhd_pub = (struct dhd_pub *)pub;
	struct dhd_info *dhd_info;

	if (!dhd_pub || ifidx < 0 || ifidx >= DHD_MAX_IFS)
		return NULL;
	dhd_info = dhd_pub->info;
	if (dhd_info && dhd_info->iflist[ifidx])
		return dhd_info->iflist[ifidx]->net;
	return NULL;
}

int
dhd_ifname2idx(dhd_info_t *dhd, char *name)
{
	int i = DHD_MAX_IFS;

	ASSERT(dhd);

	if (name == NULL || *name == '\0')
		return 0;

	while (--i > 0)
		if (dhd->iflist[i] && !strncmp(dhd->iflist[i]->name, name, IFNAMSIZ))
				break;

	DHD_TRACE(("%s: return idx %d for \"%s\"\n", __FUNCTION__, i, name));

	return i;	/* default - the primary interface */
}

char *
dhd_ifname(dhd_pub_t *dhdp, int ifidx)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;

	ASSERT(dhd);

	if (ifidx < 0 || ifidx >= DHD_MAX_IFS) {
		DHD_ERROR(("%s: ifidx %d out of range\n", __FUNCTION__, ifidx));
		return "<if_bad>";
	}

	if (dhd->iflist[ifidx] == NULL) {
		DHD_ERROR(("%s: null i/f %d\n", __FUNCTION__, ifidx));
		return "<if_null>";
	}

	if (dhd->iflist[ifidx]->net)
		return dhd->iflist[ifidx]->net->name;

	return "<if_none>";
}

uint8 *
dhd_bssidx2bssid(dhd_pub_t *dhdp, int idx)
{
	int i;
	dhd_info_t *dhd = (dhd_info_t *)dhdp;

	ASSERT(dhd);
	for (i = 0; i < DHD_MAX_IFS; i++)
	if (dhd->iflist[i] && dhd->iflist[i]->bssidx == idx)
		return dhd->iflist[i]->mac_addr;

	return NULL;
}


static void
_dhd_set_multicast_list(dhd_info_t *dhd, int ifidx)
{
	struct net_device *dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
	struct netdev_hw_addr *ha;
#else
	struct dev_mc_list *mclist;
#endif
	uint32 allmulti, cnt;

	wl_ioctl_t ioc;
	char *buf, *bufp;
	uint buflen;
	int ret;

			ASSERT(dhd && dhd->iflist[ifidx]);
			dev = dhd->iflist[ifidx]->net;
			if (!dev)
				return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
			netif_addr_lock_bh(dev);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
			cnt = netdev_mc_count(dev);
#else
			cnt = dev->mc_count;
#endif /* LINUX_VERSION_CODE */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
			netif_addr_unlock_bh(dev);
#endif

			/* Determine initial value of allmulti flag */
	allmulti = (dev->flags & IFF_ALLMULTI) ? TRUE : FALSE;
#if defined(PASS_ALL_MCAST_PKTS) && defined(CUSTOMER_HW4)
#ifdef PKT_FILTER_SUPPORT
	if (!dhd->pub.early_suspended)
#endif /* PKT_FILTER_SUPPORT */
		allmulti = TRUE;
#endif /* PASS_ALL_MCAST_PKTS && CUSTOMER_HW4 */

	/* Send down the multicast list first. */


	buflen = sizeof("mcast_list") + sizeof(cnt) + (cnt * ETHER_ADDR_LEN);
	if (!(bufp = buf = MALLOC(dhd->pub.osh, buflen))) {
		DHD_ERROR(("%s: out of memory for mcast_list, cnt %d\n",
		           dhd_ifname(&dhd->pub, ifidx), cnt));
		return;
	}

	strncpy(bufp, "mcast_list", buflen - 1);
	bufp[buflen - 1] = '\0';
	bufp += strlen("mcast_list") + 1;

	cnt = htol32(cnt);
	memcpy(bufp, &cnt, sizeof(cnt));
	bufp += sizeof(cnt);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
			netif_addr_lock_bh(dev);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
			netdev_for_each_mc_addr(ha, dev) {
				if (!cnt)
					break;
				memcpy(bufp, ha->addr, ETHER_ADDR_LEN);
				bufp += ETHER_ADDR_LEN;
				cnt--;
	}
#else
	for (mclist = dev->mc_list; (mclist && (cnt > 0));
		cnt--, mclist = mclist->next) {
				memcpy(bufp, (void *)mclist->dmi_addr, ETHER_ADDR_LEN);
				bufp += ETHER_ADDR_LEN;
			}
#endif /* LINUX_VERSION_CODE */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
			netif_addr_unlock_bh(dev);
#endif

	memset(&ioc, 0, sizeof(ioc));
	ioc.cmd = WLC_SET_VAR;
	ioc.buf = buf;
	ioc.len = buflen;
	ioc.set = TRUE;

	ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len);
	if (ret < 0) {
		DHD_ERROR(("%s: set mcast_list failed, cnt %d\n",
			dhd_ifname(&dhd->pub, ifidx), cnt));
		allmulti = cnt ? TRUE : allmulti;
	}

	MFREE(dhd->pub.osh, buf, buflen);

	/* Now send the allmulti setting.  This is based on the setting in the
	 * net_device flags, but might be modified above to be turned on if we
	 * were trying to set some addresses and dongle rejected it...
	 */

	buflen = sizeof("allmulti") + sizeof(allmulti);
	if (!(buf = MALLOC(dhd->pub.osh, buflen))) {
		DHD_ERROR(("%s: out of memory for allmulti\n", dhd_ifname(&dhd->pub, ifidx)));
		return;
	}
	allmulti = htol32(allmulti);

	if (!bcm_mkiovar("allmulti", (void*)&allmulti, sizeof(allmulti), buf, buflen)) {
		DHD_ERROR(("%s: mkiovar failed for allmulti, datalen %d buflen %u\n",
		           dhd_ifname(&dhd->pub, ifidx), (int)sizeof(allmulti), buflen));
		MFREE(dhd->pub.osh, buf, buflen);
		return;
	}


	memset(&ioc, 0, sizeof(ioc));
	ioc.cmd = WLC_SET_VAR;
	ioc.buf = buf;
	ioc.len = buflen;
	ioc.set = TRUE;

	ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len);
	if (ret < 0) {
		DHD_ERROR(("%s: set allmulti %d failed\n",
		           dhd_ifname(&dhd->pub, ifidx), ltoh32(allmulti)));
	}

	MFREE(dhd->pub.osh, buf, buflen);

	/* Finally, pick up the PROMISC flag as well, like the NIC driver does */

	allmulti = (dev->flags & IFF_PROMISC) ? TRUE : FALSE;

	allmulti = htol32(allmulti);

	memset(&ioc, 0, sizeof(ioc));
	ioc.cmd = WLC_SET_PROMISC;
	ioc.buf = &allmulti;
	ioc.len = sizeof(allmulti);
	ioc.set = TRUE;

	ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len);
	if (ret < 0) {
		DHD_ERROR(("%s: set promisc %d failed\n",
		           dhd_ifname(&dhd->pub, ifidx), ltoh32(allmulti)));
	}
}

int
_dhd_set_mac_address(dhd_info_t *dhd, int ifidx, uint8 *addr)
{
	char buf[32];
	wl_ioctl_t ioc;
	int ret;

	if (!bcm_mkiovar("cur_etheraddr", (char*)addr, ETHER_ADDR_LEN, buf, 32)) {
		DHD_ERROR(("%s: mkiovar failed for cur_etheraddr\n", dhd_ifname(&dhd->pub, ifidx)));
		return -1;
	}
	memset(&ioc, 0, sizeof(ioc));
	ioc.cmd = WLC_SET_VAR;
	ioc.buf = buf;
	ioc.len = 32;
	ioc.set = TRUE;

	ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len);
	if (ret < 0) {
		DHD_ERROR(("%s: set cur_etheraddr failed\n", dhd_ifname(&dhd->pub, ifidx)));
	} else {
		memcpy(dhd->iflist[ifidx]->net->dev_addr, addr, ETHER_ADDR_LEN);
		if (ifidx == 0)
			memcpy(dhd->pub.mac.octet, addr, ETHER_ADDR_LEN);
	}

	return ret;
}

#ifdef SOFTAP
extern struct net_device *ap_net_dev;
extern tsk_ctl_t ap_eth_ctl; /* ap netdev heper thread ctl */
#endif

#ifdef DHDTCPACK_SUPPRESS
uint dhd_use_tcpack_suppress = TRUE;
module_param(dhd_use_tcpack_suppress, uint, FALSE);
extern bool dhd_tcpack_suppress(dhd_pub_t *dhdp, void *pkt);
#endif /* DHDTCPACK_SUPPRESS */

static void sysioc_work_callback(struct work_struct *work)
{
	int i;
	dhd_info_t *dhd = container_of(work, dhd_info_t, sysioc_work);
	struct list_head *list_cur, *list_temp;
#ifdef SOFTAP
	bool in_ap = FALSE;
	unsigned long flags;
#endif

	dhd_net_if_lock_local(dhd);
	DHD_OS_WAKE_LOCK(&dhd->pub);

	spin_lock_bh(&dhd->if_event_list_lock);
	list_for_each_safe(list_cur, list_temp,  &dhd->if_event_list) {
		int ret;
		int ifidx, bssidx;
		struct net_device *ndev;
		dhd_if_event_t *if_event = list_entry(list_cur, dhd_if_event_t, list);

		list_del(list_cur);
		spin_unlock_bh(&dhd->if_event_list_lock);

		ifidx = if_event->event.ifidx;
		bssidx = if_event->event.bssidx;
		if (if_event->event.opcode == WLC_E_IF_ADD) {
			DHD_ERROR(("sysioc work WLC_E_IF_ADD, %d\n", ifidx));
			ndev = dhd_allocate_if(&dhd->pub, ifidx, if_event->name,
				if_event->mac, bssidx, TRUE);
			if (ndev != NULL) {
				ret = dhd_register_if(&dhd->pub, ifidx, TRUE);
				if (ret != BCME_OK) {
					DHD_ERROR(("%s: dhd_register_if failed\n", __FUNCTION__));
					dhd_remove_if(&dhd->pub, ifidx, TRUE);
				}
			}
		} else if (if_event->event.opcode == WLC_E_IF_DEL) {
			DHD_ERROR(("sysioc work WLC_E_IF_DEL, %d\n", ifidx));
			dhd_remove_if(&dhd->pub, ifidx, TRUE);
		}

		MFREE(dhd->pub.osh, if_event, sizeof(dhd_if_event_t));
		spin_lock_bh(&dhd->if_event_list_lock);
	}
	spin_unlock_bh(&dhd->if_event_list_lock);

	for (i = 0; i < DHD_MAX_IFS; i++) {
		dhd_if_t *ifp = dhd->iflist[i];

		if (ifp == NULL || !dhd->pub.up)
				continue;

#ifdef SOFTAP
		flags = dhd_os_spin_lock(&dhd->pub);
		in_ap = (ap_net_dev != NULL);
		dhd_os_spin_unlock(&dhd->pub, flags);
		if (in_ap && ifp->set_macaddress)  {
			DHD_ERROR(("attempt to set MAC for %s in AP Mode, blocked. \n",
				ifp->net->name));
			ifp->set_macaddress = FALSE;
			continue;
		}

		if (in_ap && ifp->set_multicast)  {
			DHD_ERROR(("attempt to set MULTICAST list for %s in AP Mode, blocked. \n",
				ifp->net->name));
			ifp->set_multicast = FALSE;
			continue;
		}
#endif /* SOFTAP */

		if (ifp->set_multicast) {
			int ifidx = i;

			ifp->set_multicast = FALSE;
			_dhd_set_multicast_list(dhd, ifidx);
			DHD_INFO(("%s: set multicast list for if %d\n", __FUNCTION__, ifidx));
		}

		if (ifp->set_macaddress) {
			ifp->set_macaddress = FALSE;
			if (_dhd_set_mac_address(dhd, i, ifp->mac_addr) == 0)
				DHD_INFO(("%s: MACID is overwritten\n",	__FUNCTION__));
			else
				DHD_ERROR(("%s: _dhd_set_mac_address() failed\n", __FUNCTION__));
		}
	}

	DHD_OS_WAKE_UNLOCK(&dhd->pub);
	dhd_net_if_unlock_local(dhd);
}

static int
dhd_set_mac_address(struct net_device *dev, void *addr)
{
	int ret = 0;

	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	struct sockaddr *sa = (struct sockaddr *)addr;
	int ifidx;
	dhd_if_t *dhdif;

	ifidx = dhd_net2idx(dhd, dev);
	if (ifidx == DHD_BAD_IF)
		return -1;

	dhdif = dhd->iflist[ifidx];

	dhd_net_if_lock_local(dhd);
	memcpy(dhdif->mac_addr, sa->sa_data, ETHER_ADDR_LEN);
	dhdif->set_macaddress = TRUE;
	dhd_net_if_unlock_local(dhd);
	schedule_work(&dhd->sysioc_work);

	return ret;
}

static void
dhd_set_multicast_list(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ifidx;

	ifidx = dhd_net2idx(dhd, dev);
	if (ifidx == DHD_BAD_IF)
		return;

	dhd->iflist[ifidx]->set_multicast = TRUE;
	schedule_work(&dhd->sysioc_work);
}

#ifdef PROP_TXSTATUS
int
dhd_os_wlfc_block(dhd_pub_t *pub)
{
	dhd_info_t *di = (dhd_info_t *)(pub->info);
	ASSERT(di != NULL);
	spin_lock_bh(&di->wlfc_spinlock);
	return 1;
}

int
dhd_os_wlfc_unblock(dhd_pub_t *pub)
{
	dhd_info_t *di = (dhd_info_t *)(pub->info);

	ASSERT(di != NULL);
	spin_unlock_bh(&di->wlfc_spinlock);
	return 1;
}

const uint8 wme_fifo2ac[] = { 0, 1, 2, 3, 1, 1 };
uint8 prio2fifo[8] = { 1, 0, 0, 1, 2, 2, 3, 3 };
#define WME_PRIO2AC(prio)	wme_fifo2ac[prio2fifo[(prio)]]

#endif /* PROP_TXSTATUS */
int
dhd_sendpkt(dhd_pub_t *dhdp, int ifidx, void *pktbuf)
{
	int ret = BCME_OK;
	dhd_info_t *dhd = (dhd_info_t *)(dhdp->info);
	struct ether_header *eh = NULL;

	/* Reject if down */
	if (!dhdp->up || (dhdp->busstate == DHD_BUS_DOWN)) {
		/* free the packet here since the caller won't */
		PKTFREE(dhdp->osh, pktbuf, TRUE);
		return -ENODEV;
	}

	/* Update multicast statistic */
	if (PKTLEN(dhdp->osh, pktbuf) >= ETHER_HDR_LEN) {
		uint8 *pktdata = (uint8 *)PKTDATA(dhdp->osh, pktbuf);
		eh = (struct ether_header *)pktdata;

		if (ETHER_ISMULTI(eh->ether_dhost))
			dhdp->tx_multicast++;
		if (ntoh16(eh->ether_type) == ETHER_TYPE_802_1X)
			atomic_inc(&dhd->pend_8021x_cnt);
	} else {
			PKTFREE(dhd->pub.osh, pktbuf, TRUE);
			return BCME_ERROR;
	}

	/* Look into the packet and update the packet priority */
#ifndef PKTPRIO_OVERRIDE
	if (PKTPRIO(pktbuf) == 0)
#endif 
		pktsetprio(pktbuf, FALSE);

#ifdef PROP_TXSTATUS
	if (dhd_wlfc_is_supported(dhdp)) {
		/* store the interface ID */
		DHD_PKTTAG_SETIF(PKTTAG(pktbuf), ifidx);

		/* store destination MAC in the tag as well */
		DHD_PKTTAG_SETDSTN(PKTTAG(pktbuf), eh->ether_dhost);

		/* decide which FIFO this packet belongs to */
		if (ETHER_ISMULTI(eh->ether_dhost))
			/* one additional queue index (highest AC + 1) is used for bc/mc queue */
			DHD_PKTTAG_SETFIFO(PKTTAG(pktbuf), AC_COUNT);
		else
			DHD_PKTTAG_SETFIFO(PKTTAG(pktbuf), WME_PRIO2AC(PKTPRIO(pktbuf)));
	} else
#endif /* PROP_TXSTATUS */
	/* If the protocol uses a data header, apply it */
	dhd_prot_hdrpush(dhdp, ifidx, pktbuf);

	/* Use bus module to send data frame */
#ifdef WLMEDIA_HTSF
	dhd_htsf_addtxts(dhdp, pktbuf);
#endif
#ifdef DHDTCPACK_SUPPRESS
	if (dhd_use_tcpack_suppress && dhd_tcpack_suppress(dhdp, pktbuf))
		ret = BCME_OK;
	else
#endif /* DHDTCPACK_SUPPRESS */
#ifdef PROP_TXSTATUS
	{
		if (dhd_wlfc_commit_packets(dhdp, (f_commitpkt_t)dhd_bus_txdata,
			dhdp->bus, pktbuf, TRUE) == WLFC_UNSUPPORTED) {
			/* non-proptxstatus way */
			ret = dhd_bus_txdata(dhdp->bus, pktbuf);
		}
	}
#else
	ret = dhd_bus_txdata(dhdp->bus, pktbuf);
#endif /* PROP_TXSTATUS */

	return ret;
}

int
dhd_start_xmit(struct sk_buff *skb, struct net_device *net)
{
	int ret;
	uint datalen;
	void *pktbuf;
	dhd_info_t *dhd  =  *(dhd_info_t **)netdev_priv(net);
	dhd_if_t *ifp = NULL;
	int ifidx;
#ifdef WLMEDIA_HTSF
	uint8 htsfdlystat_sz = dhd->pub.htsfdlystat_sz;
#else
	uint8 htsfdlystat_sz = 0;
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	DHD_OS_WAKE_LOCK(&dhd->pub);

	/* Reject if down */
	if (dhd->pub.busstate == DHD_BUS_DOWN || dhd->pub.hang_was_sent) {
		DHD_ERROR(("%s: xmit rejected pub.up=%d busstate=%d \n",
			__FUNCTION__, dhd->pub.up, dhd->pub.busstate));
		netif_stop_queue(net);
		/* Send Event when bus down detected during data session */
		if (dhd->pub.up) {
			DHD_ERROR(("%s: Event HANG sent up\n", __FUNCTION__));
			net_os_send_hang_message(net);
		}
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20))
		return -ENODEV;
#else
		return NETDEV_TX_BUSY;
#endif
	}

	ifidx = dhd_net2idx(dhd, net);
	if (ifidx == DHD_BAD_IF) {
		DHD_ERROR(("%s: bad ifidx %d\n", __FUNCTION__, ifidx));
		netif_stop_queue(net);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20))
		return -ENODEV;
#else
		return NETDEV_TX_BUSY;
#endif
	}

	ifp = dhd->iflist[ifidx];
	datalen  = PKTLEN(dhdp->osh, skb);

	/* Make sure there's enough room for any header */

	if (skb_headroom(skb) < dhd->pub.hdrlen + htsfdlystat_sz) {
		struct sk_buff *skb2;

		DHD_INFO(("%s: insufficient headroom\n",
		          dhd_ifname(&dhd->pub, ifidx)));
		dhd->pub.tx_realloc++;

		skb2 = skb_realloc_headroom(skb, dhd->pub.hdrlen + htsfdlystat_sz);

		dev_kfree_skb(skb);
		if ((skb = skb2) == NULL) {
			DHD_ERROR(("%s: skb_realloc_headroom failed\n",
			           dhd_ifname(&dhd->pub, ifidx)));
			ret = -ENOMEM;
			goto done;
		}
	}

	/* Convert to packet */
	if (!(pktbuf = PKTFRMNATIVE(dhd->pub.osh, skb))) {
		DHD_ERROR(("%s: PKTFRMNATIVE failed\n",
		           dhd_ifname(&dhd->pub, ifidx)));
		dev_kfree_skb_any(skb);
		ret = -ENOMEM;
		goto done;
	}
#ifdef WLMEDIA_HTSF
	if (htsfdlystat_sz && PKTLEN(dhd->pub.osh, pktbuf) >= ETHER_ADDR_LEN) {
		uint8 *pktdata = (uint8 *)PKTDATA(dhd->pub.osh, pktbuf);
		struct ether_header *eh = (struct ether_header *)pktdata;

		if (!ETHER_ISMULTI(eh->ether_dhost) &&
			(ntoh16(eh->ether_type) == ETHER_TYPE_IP)) {
			eh->ether_type = hton16(ETHER_TYPE_BRCM_PKTDLYSTATS);
		}
	}
#endif

	ret = dhd_sendpkt(&dhd->pub, ifidx, pktbuf);

done:
	if (ret) {
		ifp->stats.tx_dropped++;
	}
	else {
		dhd->pub.tx_packets++;
		ifp->stats.tx_packets++;
		dhd->pub.dstats.tx_bytes += datalen;// kdongsuk dstats patch.
		ifp->stats.tx_bytes += datalen;
	}

	DHD_OS_WAKE_UNLOCK(&dhd->pub);

	/* Return ok: we always eat the packet */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20))
	return 0;
#else
	return NETDEV_TX_OK;
#endif
}

void
dhd_txflowcontrol(dhd_pub_t *dhdp, int ifidx, bool state)
{
	struct net_device *net;
	dhd_info_t *dhd = dhdp->info;
	int i;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(dhd);

	if (ifidx == ALL_INTERFACES) {
		/* Flow control on all active interfaces */
		dhdp->txoff = state;
		for (i = 0; i < DHD_MAX_IFS; i++) {
			if (dhd->iflist[i]) {
				net = dhd->iflist[i]->net;
				if (state == ON)
					netif_stop_queue(net);
				else
					netif_wake_queue(net);
			}
		}
	}
	else {
		if (dhd->iflist[ifidx]) {
			net = dhd->iflist[ifidx]->net;
			if (state == ON)
				netif_stop_queue(net);
			else
				netif_wake_queue(net);
		}
	}
}

#ifdef DHD_RX_DUMP
typedef struct {
	uint16 type;
	const char *str;
} PKTTYPE_INFO;

static const PKTTYPE_INFO packet_type_info[] =
{
	{ ETHER_TYPE_IP, "IP" },
	{ ETHER_TYPE_ARP, "ARP" },
	{ ETHER_TYPE_BRCM, "BRCM" },
	{ ETHER_TYPE_802_1X, "802.1X" },
	{ ETHER_TYPE_WAI, "WAPI" },
	{ 0, ""}
};

static const char *_get_packet_type_str(uint16 type)
{
	int i;
	int n = sizeof(packet_type_info)/sizeof(packet_type_info[1]) - 1;

	for (i = 0; i < n; i++) {
		if (packet_type_info[i].type == type)
			return packet_type_info[i].str;
	}

	return packet_type_info[n].str;
}
#endif /* DHD_RX_DUMP */

void
dhd_rx_frame(dhd_pub_t *dhdp, int ifidx, void *pktbuf, int numpkt, uint8 chan)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;
	struct sk_buff *skb;
	uchar *eth;
	uint len;
	void *data, *pnext = NULL;
	int i;
	dhd_if_t *ifp;
	wl_event_msg_t event;
	int tout_rx = 0;
	int tout_ctrl = 0;
	void *skbhead = NULL;
	void *skbprev = NULL;
#ifdef DHD_RX_DUMP
#ifdef DHD_RX_FULL_DUMP
	int k;
#endif /* DHD_RX_FULL_DUMP */
	char *dump_data;
	uint16 protocol;
#endif /* DHD_RX_DUMP */

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	for (i = 0; pktbuf && i < numpkt; i++, pktbuf = pnext) {
#ifdef WLBTAMP
		struct ether_header *eh;
		struct dot11_llc_snap_header *lsh;
#endif

		pnext = PKTNEXT(dhdp->osh, pktbuf);
		PKTSETNEXT(wl->sh.osh, pktbuf, NULL);

		ifp = dhd->iflist[ifidx];
		if (ifp == NULL) {
			DHD_ERROR(("%s: ifp is NULL. drop packet\n",
				__FUNCTION__));
			PKTFREE(dhdp->osh, pktbuf, TRUE);
			continue;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
		/* Dropping packets before registering net device to avoid kernel panic */
#ifndef PROP_TXSTATUS_VSDB
		if (!ifp->net || ifp->net->reg_state != NETREG_REGISTERED) {
#else
		if (!ifp->net || ifp->net->reg_state != NETREG_REGISTERED || !dhd->pub.up) {
#endif /* PROP_TXSTATUS_VSDB */
			DHD_ERROR(("%s: net device is NOT registered yet. drop packet\n",
			__FUNCTION__));
			PKTFREE(dhdp->osh, pktbuf, TRUE);
			continue;
		}
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) */

#ifdef WLBTAMP
		eh = (struct ether_header *)PKTDATA(wl->sh.osh, pktbuf);
		lsh = (struct dot11_llc_snap_header *)&eh[1];

		if ((ntoh16(eh->ether_type) < ETHER_TYPE_MIN) &&
		    (PKTLEN(wl->sh.osh, pktbuf) >= RFC1042_HDR_LEN) &&
		    bcmp(lsh, BT_SIG_SNAP_MPROT, DOT11_LLC_SNAP_HDR_LEN - 2) == 0 &&
		    lsh->type == HTON16(BTA_PROT_L2CAP)) {
			amp_hci_ACL_data_t *ACL_data = (amp_hci_ACL_data_t *)
			        ((uint8 *)eh + RFC1042_HDR_LEN);
			ACL_data = NULL;
		}
#endif /* WLBTAMP */

#ifdef PROP_TXSTATUS
		if (dhd_wlfc_is_header_only_pkt(dhdp, pktbuf)) {
			/* WLFC may send header only packet when
			there is an urgent message but no packet to
			piggy-back on
			*/
			PKTFREE(dhdp->osh, pktbuf, TRUE);
			continue;
		}
#endif

		skb = PKTTONATIVE(dhdp->osh, pktbuf);

		/* Get the protocol, maintain skb around eth_type_trans()
		 * The main reason for this hack is for the limitation of
		 * Linux 2.4 where 'eth_type_trans' uses the 'net->hard_header_len'
		 * to perform skb_pull inside vs ETH_HLEN. Since to avoid
		 * coping of the packet coming from the network stack to add
		 * BDC, Hardware header etc, during network interface registration
		 * we set the 'net->hard_header_len' to ETH_HLEN + extra space required
		 * for BDC, Hardware header etc. and not just the ETH_HLEN
		 */
		eth = skb->data;
		len = skb->len;

#ifdef DHD_RX_DUMP
		dump_data = skb->data;
		protocol = (dump_data[12] << 8) | dump_data[13];
		DHD_ERROR(("RX DUMP - %s\n", _get_packet_type_str(protocol)));

#ifdef DHD_RX_FULL_DUMP
		if (protocol != ETHER_TYPE_BRCM) {
			for (k = 0; k < skb->len; k++) {
				DHD_ERROR(("%02X ", dump_data[k]));
				if ((k & 15) == 15)
					DHD_ERROR(("\n"));
			}
			DHD_ERROR(("\n"));
		}
#endif /* DHD_RX_FULL_DUMP */

		if (protocol != ETHER_TYPE_BRCM) {
			if (dump_data[0] == 0xFF) {
				DHD_ERROR(("%s: BROADCAST\n", __FUNCTION__));

				if ((dump_data[12] == 8) &&
					(dump_data[13] == 6)) {
					DHD_ERROR(("%s: ARP %d\n",
						__FUNCTION__, dump_data[0x15]));
				}
			} else if (dump_data[0] & 1) {
				DHD_ERROR(("%s: MULTICAST: " MACDBG "\n",
					__FUNCTION__, MAC2STRDBG(dump_data)));
			}

			if (protocol == ETHER_TYPE_802_1X) {
				DHD_ERROR(("ETHER_TYPE_802_1X: "
					"ver %d, type %d, replay %d\n",
					dump_data[14], dump_data[15],
					dump_data[30]));
			}
		}

#endif /* DHD_RX_DUMP */

		ifp = dhd->iflist[ifidx];
		if (ifp == NULL)
			ifp = dhd->iflist[0];

		ASSERT(ifp);
		skb->dev = ifp->net;
		skb->protocol = eth_type_trans(skb, skb->dev);

		if (skb->pkt_type == PACKET_MULTICAST) {
			dhd->pub.rx_multicast++;
		}

		skb->data = eth;
		skb->len = len;

#ifdef WLMEDIA_HTSF
		dhd_htsf_addrxts(dhdp, pktbuf);
#endif
		/* Strip header, count, deliver upward */
		skb_pull(skb, ETH_HLEN);

		/* Process special event packets and then discard them */
		memset(&event, 0, sizeof(event));
		if (ntoh16(skb->protocol) == ETHER_TYPE_BRCM) {
			dhd_wl_host_event(dhd, &ifidx,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22)
			skb_mac_header(skb),
#else
			skb->mac.raw,
#endif
			&event,
			&data);

#if defined(WLBTAMP) || defined(PNO_SUPPORT)
			wl_event_to_host_order(&event);
			if (!tout_ctrl)
				tout_ctrl = DHD_PACKET_TIMEOUT_MS;
#ifdef WLBTAMP
			if (event.event_type == WLC_E_BTA_HCI_EVENT) {
				dhd_bta_doevt(dhdp, data, event.datalen);
			}
#endif /* WLBTAMP */

#if defined(PNO_SUPPORT)
			if (event.event_type == WLC_E_PFN_NET_FOUND) {
				/* enforce custom wake lock to garantee that Kernel not suspended */
				tout_ctrl = CUSTOM_PNO_EVENT_LOCK_xTIME * DHD_PACKET_TIMEOUT_MS;
			}
#endif /* PNO_SUPPORT */
#endif /* defined(WLBTAMP) || defined(PNO_SUPPORT) */
#ifdef DHD_DONOT_FORWARD_BCMEVENT_AS_NETWORK_PKT
			PKTFREE(dhdp->osh, pktbuf, TRUE);
			continue;
#endif
		} else {
			tout_rx = DHD_PACKET_TIMEOUT_MS;
		}

		ASSERT(ifidx < DHD_MAX_IFS && dhd->iflist[ifidx]);
		ifp = dhd->iflist[ifidx];

		if (ifp->net)
			ifp->net->last_rx = jiffies;

		dhdp->rx_packets++; /* Local count */
		dhdp->dstats.rx_bytes += skb->len; // kdongsuk dstats patch
		ifp->stats.rx_bytes += skb->len;
		ifp->stats.rx_packets++;

		if (in_interrupt()) {
			netif_rx(skb);
		} else {
			if (dhd->rxthread_enabled) {
				if (!skbhead)
					skbhead = skb;
				else
					PKTSETNEXT(wl->sh.osh, skbprev, skb);
				skbprev = skb;
			} else {

				/* If the receive is not processed inside an ISR,
				 * the softirqd must be woken explicitly to service
				 * the NET_RX_SOFTIRQ.	In 2.6 kernels, this is handled
				 * by netif_rx_ni(), but in earlier kernels, we need
				 * to do it manually.
				 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
				netif_rx_ni(skb);
#else
				ulong flags;
				netif_rx(skb);
				local_irq_save(flags);
				RAISE_RX_SOFTIRQ();
				local_irq_restore(flags);
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0) */
			}
		}
	}

	if (dhd->rxthread_enabled && skbhead)
		dhd_sched_rxf(dhdp, skbhead);

	DHD_OS_WAKE_LOCK_RX_TIMEOUT_ENABLE(dhdp, tout_rx);
	DHD_OS_WAKE_LOCK_CTRL_TIMEOUT_ENABLE(dhdp, tout_ctrl);
}

void
dhd_event(struct dhd_info *dhd, char *evpkt, int evlen, int ifidx)
{
	/* Linux version has nothing to do */
	return;
}

void
dhd_txcomplete(dhd_pub_t *dhdp, void *txp, bool success)
{
	dhd_info_t *dhd = (dhd_info_t *)(dhdp->info);
	struct ether_header *eh;
	uint16 type;
#ifdef WLBTAMP
	uint len;
#endif

	dhd_prot_hdrpull(dhdp, NULL, txp, NULL, NULL);

	eh = (struct ether_header *)PKTDATA(dhdp->osh, txp);
	type  = ntoh16(eh->ether_type);

	if (type == ETHER_TYPE_802_1X)
		atomic_dec(&dhd->pend_8021x_cnt);

#ifdef WLBTAMP
	/* Crack open the packet and check to see if it is BT HCI ACL data packet.
	 * If yes generate packet completion event.
	 */
	len = PKTLEN(dhdp->osh, txp);

	/* Generate ACL data tx completion event locally to avoid SDIO bus transaction */
	if ((type < ETHER_TYPE_MIN) && (len >= RFC1042_HDR_LEN)) {
		struct dot11_llc_snap_header *lsh = (struct dot11_llc_snap_header *)&eh[1];

		if (bcmp(lsh, BT_SIG_SNAP_MPROT, DOT11_LLC_SNAP_HDR_LEN - 2) == 0 &&
		    ntoh16(lsh->type) == BTA_PROT_L2CAP) {

			dhd_bta_tx_hcidata_complete(dhdp, txp, success);
		}
	}
#endif /* WLBTAMP */
}

static struct net_device_stats *
dhd_get_stats(struct net_device *net)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(net);
	dhd_if_t *ifp;
	int ifidx;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ifidx = dhd_net2idx(dhd, net);
	if (ifidx == DHD_BAD_IF) {
		DHD_ERROR(("%s: BAD_IF\n", __FUNCTION__));
		return NULL;
	}

	ifp = dhd->iflist[ifidx];
	ASSERT(dhd && ifp);

	if (dhd->pub.up) {
		/* Use the protocol to get dongle stats */
		dhd_prot_dstats(&dhd->pub);
	}

	/* Copy dongle stats to net device stats */
	ifp->stats.rx_packets = dhd->pub.dstats.rx_packets;
	ifp->stats.tx_packets = dhd->pub.dstats.tx_packets;
	ifp->stats.rx_bytes = dhd->pub.dstats.rx_bytes;
	ifp->stats.tx_bytes = dhd->pub.dstats.tx_bytes;
	ifp->stats.rx_errors = dhd->pub.dstats.rx_errors;
	ifp->stats.tx_errors = dhd->pub.dstats.tx_errors;
	ifp->stats.rx_dropped = dhd->pub.dstats.rx_dropped;
	ifp->stats.tx_dropped = dhd->pub.dstats.tx_dropped;
	ifp->stats.multicast = dhd->pub.dstats.multicast;

	return &ifp->stats;
}

static int
dhd_watchdog_thread(void *data)
{
	tsk_ctl_t *tsk = (tsk_ctl_t *)data;
	dhd_info_t *dhd = (dhd_info_t *)tsk->parent;
	/* This thread doesn't need any user-level access,
	 * so get rid of all our resources
	 */
	if (dhd_watchdog_prio > 0) {
		struct sched_param param;
		param.sched_priority = (dhd_watchdog_prio < MAX_RT_PRIO)?
			dhd_watchdog_prio:(MAX_RT_PRIO-1);
		setScheduler(current, SCHED_FIFO, &param);
	}
#ifndef USE_KTHREAD_API
	DAEMONIZE("dhd_watchdog");

	/* Run until signal received */
	complete(&tsk->completed);
#endif

	while (1)
		if (down_interruptible (&tsk->sema) == 0) {
			unsigned long flags;
			unsigned long jiffies_at_start = jiffies;
			unsigned long time_lapse;

			SMP_RD_BARRIER_DEPENDS();
			if (tsk->terminated) {
				break;
			}

			dhd_os_sdlock(&dhd->pub);
			if (dhd->pub.dongle_reset == FALSE) {
				DHD_TIMER(("%s:\n", __FUNCTION__));

				/* Call the bus module watchdog */
				dhd_bus_watchdog(&dhd->pub);

				flags = dhd_os_spin_lock(&dhd->pub);
				/* Count the tick for reference */
				dhd->pub.tickcnt++;
				time_lapse = jiffies - jiffies_at_start;

				/* Reschedule the watchdog */
				if (dhd->wd_timer_valid)
					mod_timer(&dhd->timer,
					jiffies +
					msecs_to_jiffies(dhd_watchdog_ms) -
					min(msecs_to_jiffies(dhd_watchdog_ms), time_lapse));
				dhd_os_spin_unlock(&dhd->pub, flags);
			}
			dhd_os_sdunlock(&dhd->pub);
		} else {
			break;
	}

	complete_and_exit(&tsk->completed, 0);
}

static void dhd_watchdog(ulong data)
{
	dhd_info_t *dhd = (dhd_info_t *)data;
	unsigned long flags;

	if (dhd->pub.dongle_reset) {
		return;
	}

	if (dhd->thr_wdt_ctl.thr_pid >= 0) {
		up(&dhd->thr_wdt_ctl.sema);
		return;
	}

	dhd_os_sdlock(&dhd->pub);
	/* Call the bus module watchdog */
	dhd_bus_watchdog(&dhd->pub);

	flags = dhd_os_spin_lock(&dhd->pub);
	/* Count the tick for reference */
	dhd->pub.tickcnt++;

	/* Reschedule the watchdog */
	if (dhd->wd_timer_valid)
		mod_timer(&dhd->timer, jiffies + msecs_to_jiffies(dhd_watchdog_ms));
	dhd_os_spin_unlock(&dhd->pub, flags);
	dhd_os_sdunlock(&dhd->pub);
}

static int
dhd_dpc_thread(void *data)
{
	tsk_ctl_t *tsk = (tsk_ctl_t *)data;
	dhd_info_t *dhd = (dhd_info_t *)tsk->parent;

	/* This thread doesn't need any user-level access,
	 * so get rid of all our resources
	 */
	if (dhd_dpc_prio > 0)
	{
		struct sched_param param;
		param.sched_priority = (dhd_dpc_prio < MAX_RT_PRIO)?dhd_dpc_prio:(MAX_RT_PRIO-1);
		setScheduler(current, SCHED_FIFO, &param);
	}
#ifndef USE_KTHREAD_API
	DAEMONIZE("dhd_dpc");
	/* DHD_OS_WAKE_LOCK is called in dhd_sched_dpc[dhd_linux.c] down below  */

	/*  signal: thread has started */
	complete(&tsk->completed);
#endif

#ifdef CUSTOM_DPC_CPUCORE
	set_cpus_allowed_ptr(current, cpumask_of(CUSTOM_DPC_CPUCORE));
#endif

	/* Run until signal received */
	while (1) {
		if (down_interruptible(&tsk->sema) == 0) {

			SMP_RD_BARRIER_DEPENDS();
			if (tsk->terminated) {
				break;
			}

			/* Call bus dpc unless it indicated down (then clean stop) */
			if (dhd->pub.busstate != DHD_BUS_DOWN) {
				dhd_os_wd_timer_extend(&dhd->pub, TRUE);
				while (dhd_bus_dpc(dhd->pub.bus)) {
					/* process all data */
				}
				dhd_os_wd_timer_extend(&dhd->pub, FALSE);
				DHD_OS_WAKE_UNLOCK(&dhd->pub);

			} else {
				if (dhd->pub.up)
					dhd_bus_stop(dhd->pub.bus, TRUE);
				DHD_OS_WAKE_UNLOCK(&dhd->pub);
			}
		}
		else
			break;
	}

	complete_and_exit(&tsk->completed, 0);
}

static int
dhd_rxf_thread(void *data)
{
	tsk_ctl_t *tsk = (tsk_ctl_t *)data;
	dhd_info_t *dhd = (dhd_info_t *)tsk->parent;
	dhd_pub_t *pub = &dhd->pub;

	/* This thread doesn't need any user-level access,
	 * so get rid of all our resources
	 */
	if (dhd_rxf_prio > 0)
	{
		struct sched_param param;
		param.sched_priority = (dhd_rxf_prio < MAX_RT_PRIO)?dhd_rxf_prio:(MAX_RT_PRIO-1);
		setScheduler(current, SCHED_FIFO, &param);
	}

	DAEMONIZE("dhd_rxf");
	/* DHD_OS_WAKE_LOCK is called in dhd_sched_dpc[dhd_linux.c] down below  */

	/*  signal: thread has started */
	complete(&tsk->completed);

	/* Run until signal received */
	while (1) {
		if (down_interruptible(&tsk->sema) == 0) {
			void *skb;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
			ulong flags;
#endif

			SMP_RD_BARRIER_DEPENDS();

			if (tsk->terminated) {
				break;
			}
			skb = dhd_rxf_dequeue(pub);

			if (skb == NULL) {
				continue;
			}
			while (skb) {
				void *skbnext = PKTNEXT(pub->osh, skb);
				PKTSETNEXT(pub->osh, skb, NULL);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
				netif_rx_ni(skb);
#else
				netif_rx(skb);
				local_irq_save(flags);
				RAISE_RX_SOFTIRQ();
				local_irq_restore(flags);

#endif
				skb = skbnext;
			}

			DHD_OS_WAKE_UNLOCK(pub);
		}
		else
			break;
	}

	complete_and_exit(&tsk->completed, 0);
}

static void
dhd_dpc(ulong data)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)data;

	/* this (tasklet) can be scheduled in dhd_sched_dpc[dhd_linux.c]
	 * down below , wake lock is set,
	 * the tasklet is initialized in dhd_attach()
	 */
	/* Call bus dpc unless it indicated down (then clean stop) */
	if (dhd->pub.busstate != DHD_BUS_DOWN) {
		if (dhd_bus_dpc(dhd->pub.bus))
			tasklet_schedule(&dhd->tasklet);
		else
			DHD_OS_WAKE_UNLOCK(&dhd->pub);
	} else {
		dhd_bus_stop(dhd->pub.bus, TRUE);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
	}
}

void
dhd_sched_dpc(dhd_pub_t *dhdp)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;

	DHD_OS_WAKE_LOCK(dhdp);
	if (dhd->thr_dpc_ctl.thr_pid >= 0) {
		up(&dhd->thr_dpc_ctl.sema);
	}
}

static void
dhd_sched_rxf(dhd_pub_t *dhdp, void *skb)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;

	DHD_OS_WAKE_LOCK(dhdp);

	DHD_TRACE(("dhd_sched_rxf: Enter\n"));

	do {
		if (dhd_rxf_enqueue(dhdp, skb) == BCME_OK)
			break;
	} while (1);
	if (dhd->thr_rxf_ctl.thr_pid >= 0) {
		up(&dhd->thr_rxf_ctl.sema);
	}
	return;
}

#ifdef TOE
/* Retrieve current toe component enables, which are kept as a bitmap in toe_ol iovar */
static int
dhd_toe_get(dhd_info_t *dhd, int ifidx, uint32 *toe_ol)
{
	wl_ioctl_t ioc;
	char buf[32];
	int ret;

	memset(&ioc, 0, sizeof(ioc));

	ioc.cmd = WLC_GET_VAR;
	ioc.buf = buf;
	ioc.len = (uint)sizeof(buf);
	ioc.set = FALSE;

	strncpy(buf, "toe_ol", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	if ((ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len)) < 0) {
		/* Check for older dongle image that doesn't support toe_ol */
		if (ret == -EIO) {
			DHD_ERROR(("%s: toe not supported by device\n",
				dhd_ifname(&dhd->pub, ifidx)));
			return -EOPNOTSUPP;
		}

		DHD_INFO(("%s: could not get toe_ol: ret=%d\n", dhd_ifname(&dhd->pub, ifidx), ret));
		return ret;
	}

	memcpy(toe_ol, buf, sizeof(uint32));
	return 0;
}

/* Set current toe component enables in toe_ol iovar, and set toe global enable iovar */
static int
dhd_toe_set(dhd_info_t *dhd, int ifidx, uint32 toe_ol)
{
	wl_ioctl_t ioc;
	char buf[32];
	int toe, ret;

	memset(&ioc, 0, sizeof(ioc));

	ioc.cmd = WLC_SET_VAR;
	ioc.buf = buf;
	ioc.len = (uint)sizeof(buf);
	ioc.set = TRUE;

	/* Set toe_ol as requested */

	strncpy(buf, "toe_ol", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	memcpy(&buf[sizeof("toe_ol")], &toe_ol, sizeof(uint32));

	if ((ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len)) < 0) {
		DHD_ERROR(("%s: could not set toe_ol: ret=%d\n",
			dhd_ifname(&dhd->pub, ifidx), ret));
		return ret;
	}

	/* Enable toe globally only if any components are enabled. */

	toe = (toe_ol != 0);

	strcpy(buf, "toe");
	memcpy(&buf[sizeof("toe")], &toe, sizeof(uint32));

	if ((ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len)) < 0) {
		DHD_ERROR(("%s: could not set toe: ret=%d\n", dhd_ifname(&dhd->pub, ifidx), ret));
		return ret;
	}

	return 0;
}
#endif /* TOE */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
static void
dhd_ethtool_get_drvinfo(struct net_device *net, struct ethtool_drvinfo *info)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(net);

	snprintf(info->driver, sizeof(info->driver), "wl");
	snprintf(info->version, sizeof(info->version), "%lu", dhd->pub.drv_version);
}

struct ethtool_ops dhd_ethtool_ops = {
	.get_drvinfo = dhd_ethtool_get_drvinfo
};
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24) */


#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 4, 2)
static int
dhd_ethtool(dhd_info_t *dhd, void *uaddr)
{
	struct ethtool_drvinfo info;
	char drvname[sizeof(info.driver)];
	uint32 cmd;
#ifdef TOE
	struct ethtool_value edata;
	uint32 toe_cmpnt, csum_dir;
	int ret;
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	/* all ethtool calls start with a cmd word */
	if (copy_from_user(&cmd, uaddr, sizeof (uint32)))
		return -EFAULT;

	switch (cmd) {
	case ETHTOOL_GDRVINFO:
		/* Copy out any request driver name */
		if (copy_from_user(&info, uaddr, sizeof(info)))
			return -EFAULT;
		strncpy(drvname, info.driver, sizeof(info.driver));
		drvname[sizeof(info.driver)-1] = '\0';

		/* clear struct for return */
		memset(&info, 0, sizeof(info));
		info.cmd = cmd;

		/* if dhd requested, identify ourselves */
		if (strcmp(drvname, "?dhd") == 0) {
			snprintf(info.driver, sizeof(info.driver), "dhd");
			strncpy(info.version, EPI_VERSION_STR, sizeof(info.version) - 1);
			info.version[sizeof(info.version) - 1] = '\0';
		}

		/* otherwise, require dongle to be up */
		else if (!dhd->pub.up) {
			DHD_ERROR(("%s: dongle is not up\n", __FUNCTION__));
			return -ENODEV;
		}

		/* finally, report dongle driver type */
		else if (dhd->pub.iswl)
			snprintf(info.driver, sizeof(info.driver), "wl");
		else
			snprintf(info.driver, sizeof(info.driver), "xx");

		snprintf(info.version, sizeof(info.version), "%lu", dhd->pub.drv_version);
		if (copy_to_user(uaddr, &info, sizeof(info)))
			return -EFAULT;
		DHD_CTL(("%s: given %*s, returning %s\n", __FUNCTION__,
		         (int)sizeof(drvname), drvname, info.driver));
		break;

#ifdef TOE
	/* Get toe offload components from dongle */
	case ETHTOOL_GRXCSUM:
	case ETHTOOL_GTXCSUM:
		if ((ret = dhd_toe_get(dhd, 0, &toe_cmpnt)) < 0)
			return ret;

		csum_dir = (cmd == ETHTOOL_GTXCSUM) ? TOE_TX_CSUM_OL : TOE_RX_CSUM_OL;

		edata.cmd = cmd;
		edata.data = (toe_cmpnt & csum_dir) ? 1 : 0;

		if (copy_to_user(uaddr, &edata, sizeof(edata)))
			return -EFAULT;
		break;

	/* Set toe offload components in dongle */
	case ETHTOOL_SRXCSUM:
	case ETHTOOL_STXCSUM:
		if (copy_from_user(&edata, uaddr, sizeof(edata)))
			return -EFAULT;

		/* Read the current settings, update and write back */
		if ((ret = dhd_toe_get(dhd, 0, &toe_cmpnt)) < 0)
			return ret;

		csum_dir = (cmd == ETHTOOL_STXCSUM) ? TOE_TX_CSUM_OL : TOE_RX_CSUM_OL;

		if (edata.data != 0)
			toe_cmpnt |= csum_dir;
		else
			toe_cmpnt &= ~csum_dir;

		if ((ret = dhd_toe_set(dhd, 0, toe_cmpnt)) < 0)
			return ret;

		/* If setting TX checksum mode, tell Linux the new mode */
		if (cmd == ETHTOOL_STXCSUM) {
			if (edata.data)
				dhd->iflist[0]->net->features |= NETIF_F_IP_CSUM;
			else
				dhd->iflist[0]->net->features &= ~NETIF_F_IP_CSUM;
		}

		break;
#endif /* TOE */

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 4, 2) */

static bool dhd_check_hang(struct net_device *net, dhd_pub_t *dhdp, int error)
{
	dhd_info_t *dhd;

	if (!dhdp)
		return FALSE;

	dhd = (dhd_info_t *)dhdp->info;
	if (dhd->thr_dpc_ctl.thr_pid < 0) {
		DHD_ERROR(("%s : skipped due to negative pid - unloading?\n", __FUNCTION__));
		return FALSE;
	}

	if ((error == -ETIMEDOUT) || (error == -EREMOTEIO) ||
		((dhdp->busstate == DHD_BUS_DOWN) && (!dhdp->dongle_reset))) {
		DHD_ERROR(("%s: Event HANG send up due to  re=%d te=%d e=%d s=%d\n", __FUNCTION__,
			dhdp->rxcnt_timeout, dhdp->txcnt_timeout, error, dhdp->busstate));
		net_os_send_hang_message(net);
		return TRUE;
	}
	return FALSE;
}

int dhd_ioctl_process(dhd_pub_t *pub, int ifidx, dhd_ioctl_t *ioc)
{
	int bcmerror = BCME_OK;
	int buflen = 0;
	void *buf = NULL;
	struct net_device *net;

	net = dhd_idx2net(pub, ifidx);
	if (!net) {
		bcmerror = BCME_BADARG;
		goto done;
	}

	/* Copy out any buffer passed */
	if (ioc->buf) {
		if (ioc->len == 0) {
			DHD_TRACE(("%s: ioc->len=0, returns BCME_BADARG \n", __FUNCTION__));
			bcmerror = BCME_BADARG;
			goto done;
		}
		buflen = MIN(ioc->len, DHD_IOCTL_MAXLEN);
		/* optimization for direct ioctl calls from kernel */
		/*
		if (segment_eq(get_fs(), KERNEL_DS)) {
			buf = ioc->buf;
		} else {
		*/
		{
			if (!(buf = MALLOC(pub->osh, buflen+1))) {
				bcmerror = BCME_NOMEM;
				goto done;
			}
			if (copy_from_user(buf, ioc->buf, buflen)) {
				bcmerror = BCME_BADADDR;
				goto done;
			}
			*(char *)(buf + buflen) = '\0';
		}
	}

	/* check for local dhd ioctl and handle it */
	if (ioc->driver == DHD_IOCTL_MAGIC) {
		bcmerror = dhd_ioctl((void *)pub, ioc, buf, buflen);
		if (bcmerror)
			pub->bcmerror = bcmerror;
		goto done;
	}

	/* send to dongle (must be up, and wl). */
	if (pub->busstate != DHD_BUS_DATA) {
		bcmerror = BCME_DONGLE_DOWN;
		goto done;
	}

	if (!pub->iswl) {
		bcmerror = BCME_DONGLE_DOWN;
		goto done;
	}

	/*
	 * Flush the TX queue if required for proper message serialization:
	 * Intercept WLC_SET_KEY IOCTL - serialize M4 send and set key IOCTL to
	 * prevent M4 encryption and
	 * intercept WLC_DISASSOC IOCTL - serialize WPS-DONE and WLC_DISASSOC IOCTL to
	 * prevent disassoc frame being sent before WPS-DONE frame.
	 */
	if (ioc->cmd == WLC_SET_KEY ||
	    (ioc->cmd == WLC_SET_VAR && ioc->buf != NULL &&
	     strncmp("wsec_key", ioc->buf, 9) == 0) ||
	    (ioc->cmd == WLC_SET_VAR && ioc->buf != NULL &&
	     strncmp("bsscfg:wsec_key", ioc->buf, 15) == 0) ||
	    ioc->cmd == WLC_DISASSOC)
		dhd_wait_pend8021x(net);

#ifdef WLMEDIA_HTSF
	if (ioc->buf) {
		/*  short cut wl ioctl calls here  */
		if (strcmp("htsf", ioc->buf) == 0) {
			dhd_ioctl_htsf_get(dhd, 0);
			return BCME_OK;
		}

		if (strcmp("htsflate", ioc->buf) == 0) {
			if (ioc->set) {
				memset(ts, 0, sizeof(tstamp_t)*TSMAX);
				memset(&maxdelayts, 0, sizeof(tstamp_t));
				maxdelay = 0;
				tspktcnt = 0;
				maxdelaypktno = 0;
				memset(&vi_d1.bin, 0, sizeof(uint32)*NUMBIN);
				memset(&vi_d2.bin, 0, sizeof(uint32)*NUMBIN);
				memset(&vi_d3.bin, 0, sizeof(uint32)*NUMBIN);
				memset(&vi_d4.bin, 0, sizeof(uint32)*NUMBIN);
			} else {
				dhd_dump_latency();
			}
			return BCME_OK;
		}
		if (strcmp("htsfclear", ioc->buf) == 0) {
			memset(&vi_d1.bin, 0, sizeof(uint32)*NUMBIN);
			memset(&vi_d2.bin, 0, sizeof(uint32)*NUMBIN);
			memset(&vi_d3.bin, 0, sizeof(uint32)*NUMBIN);
			memset(&vi_d4.bin, 0, sizeof(uint32)*NUMBIN);
			htsf_seqnum = 0;
			return BCME_OK;
		}
		if (strcmp("htsfhis", ioc->buf) == 0) {
			dhd_dump_htsfhisto(&vi_d1, "H to D");
			dhd_dump_htsfhisto(&vi_d2, "D to D");
			dhd_dump_htsfhisto(&vi_d3, "D to H");
			dhd_dump_htsfhisto(&vi_d4, "H to H");
			return BCME_OK;
		}
		if (strcmp("tsport", ioc->buf) == 0) {
			if (ioc->set) {
				memcpy(&tsport, ioc->buf + 7, 4);
			} else {
				DHD_ERROR(("current timestamp port: %d \n", tsport));
			}
			return BCME_OK;
		}
	}
#endif /* WLMEDIA_HTSF */

	if ((ioc->cmd == WLC_SET_VAR || ioc->cmd == WLC_GET_VAR) &&
		ioc->buf != NULL && strncmp("rpc_", ioc->buf, 4) == 0) {
#ifdef BCM_FD_AGGR
		bcmerror = dhd_fdaggr_ioctl(pub, ifidx, (wl_ioctl_t *)ioc, buf, buflen);
#else
		bcmerror = BCME_UNSUPPORTED;
#endif
		goto done;
	}
	bcmerror = dhd_wl_ioctl(pub, ifidx, (wl_ioctl_t *)ioc, buf, buflen);

done:
	dhd_check_hang(net, pub, bcmerror);

	if (!bcmerror && buf && ioc->buf) {
		if (copy_to_user(ioc->buf, buf, buflen))
			bcmerror = -EFAULT;
	}

	if (buf)
		MFREE(pub->osh, buf, buflen+1);

	return bcmerror;
}

static int
dhd_ioctl_entry(struct net_device *net, struct ifreq *ifr, int cmd)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(net);
	dhd_ioctl_t ioc;
	int bcmerror = 0;
	int ifidx;
	int ret;

	DHD_OS_WAKE_LOCK(&dhd->pub);

	/* send to dongle only if we are not waiting for reload already */
	if (dhd->pub.hang_was_sent) {
		DHD_ERROR(("%s: HANG was sent up earlier\n", __FUNCTION__));
		DHD_OS_WAKE_LOCK_CTRL_TIMEOUT_ENABLE(&dhd->pub, DHD_EVENT_TIMEOUT_MS);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return OSL_ERROR(BCME_DONGLE_DOWN);
	}

	ifidx = dhd_net2idx(dhd, net);
	DHD_TRACE(("%s: ifidx %d, cmd 0x%04x\n", __FUNCTION__, ifidx, cmd));

	if (ifidx == DHD_BAD_IF) {
		DHD_ERROR(("%s: BAD IF\n", __FUNCTION__));
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return -1;
	}

#if defined(WL_WIRELESS_EXT)
	/* linux wireless extensions */
	if ((cmd >= SIOCIWFIRST) && (cmd <= SIOCIWLAST)) {
		/* may recurse, do NOT lock */
		ret = wl_iw_ioctl(net, ifr, cmd);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return ret;
	}
#endif /* defined(WL_WIRELESS_EXT) */

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 4, 2)
	if (cmd == SIOCETHTOOL) {
		ret = dhd_ethtool(dhd, (void*)ifr->ifr_data);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return ret;
	}
#endif /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 4, 2) */

	if (cmd == SIOCDEVPRIVATE+1) {
		ret = wl_android_priv_cmd(net, ifr, cmd);
		dhd_check_hang(net, &dhd->pub, ret);
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return ret;
	}

	if (cmd != SIOCDEVPRIVATE) {
		DHD_OS_WAKE_UNLOCK(&dhd->pub);
		return -EOPNOTSUPP;
	}

	memset(&ioc, 0, sizeof(ioc));

	/* Copy the ioc control structure part of ioctl request */
	if (copy_from_user(&ioc, ifr->ifr_data, sizeof(wl_ioctl_t))) {
		bcmerror = BCME_BADADDR;
		goto done;
	}

	/* To differentiate between wl and dhd read 4 more byes */
	if ((copy_from_user(&ioc.driver, (char *)ifr->ifr_data + sizeof(wl_ioctl_t),
		sizeof(uint)) != 0)) {
		bcmerror = BCME_BADADDR;
		goto done;
	}

	if (!capable(CAP_NET_ADMIN)) {
		bcmerror = BCME_EPERM;
		goto done;
	}

	bcmerror = dhd_ioctl_process(&dhd->pub, ifidx, &ioc);

done:
	DHD_OS_WAKE_UNLOCK(&dhd->pub);

	return OSL_ERROR(bcmerror);
}



#if defined(WL_CFG80211) && defined(SUPPORT_DEEP_SLEEP)
/* Flags to indicate if we distingish power off policy when
 * user set the memu "Keep Wi-Fi on during sleep" to "Never"
 */
int trigger_deep_sleep = 0;
#endif /* WL_CFG80211 && SUPPORT_DEEP_SLEEP */
static int
dhd_stop(struct net_device *net)
{
	int ifidx = 0;
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(net);
	DHD_OS_WAKE_LOCK(&dhd->pub);
	DHD_TRACE(("%s: Enter %p\n", __FUNCTION__, net));
	if (dhd->pub.up == 0) {
		goto exit;
	}


	ifidx = dhd_net2idx(dhd, net);
	BCM_REFERENCE(ifidx);

	/* Set state and stop OS transmissions */
	netif_stop_queue(net);
	dhd->pub.up = 0;

#ifdef WL_CFG80211
	if (ifidx == 0) {
		wl_cfg80211_down(NULL);

		/*
		 * For CFG80211: Clean up all the left over virtual interfaces
		 * when the primary Interface is brought down. [ifconfig wlan0 down]
		 */
		if ((dhd->dhd_state & DHD_ATTACH_STATE_ADD_IF) &&
			(dhd->dhd_state & DHD_ATTACH_STATE_CFG80211)) {
			int i;

			dhd_net_if_lock_local(dhd);
			for (i = 1; i < DHD_MAX_IFS; i++)
				dhd_remove_if(&dhd->pub, i, TRUE);
			dhd_net_if_unlock_local(dhd);
		}
	}
#endif /* WL_CFG80211 */

#ifdef PROP_TXSTATUS
	dhd_wlfc_cleanup(&dhd->pub, NULL, 0);
#endif
	/* Stop the protocol module */
	dhd_prot_stop(&dhd->pub);

	OLD_MOD_DEC_USE_COUNT;
exit:
#if defined(WL_CFG80211)
	if (ifidx == 0 && !dhd_download_fw_on_driverload)
		wl_android_wifi_off(net);
#ifdef SUPPORT_DEEP_SLEEP
	else {
		/* CSP#505233: Flags to indicate if we distingish
		 * power off policy when user set the memu
		 * "Keep Wi-Fi on during sleep" to "Never"
		 */
		if (trigger_deep_sleep) {
			dhd_deepsleep(net, 1);
			trigger_deep_sleep = 0;
		}
	}
#endif /* SUPPORT_DEEP_SLEEP */
#endif 
	dhd->pub.rxcnt_timeout = 0;
	dhd->pub.txcnt_timeout = 0;

	DHD_OS_WAKE_UNLOCK(&dhd->pub);
	return 0;
}

static int
dhd_open(struct net_device *net)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(net);
#ifdef TOE
	uint32 toe_ol;
#endif
	int ifidx;
	int32 ret = 0;

	/* WAR : to prevent calling dhd_open abnormally in quick succession after hang event */
	if (dhd->pub.hang_was_sent == 1) {
		DHD_ERROR(("%s: HANG was sent up earlier\n", __FUNCTION__));
		return -1;
	}

#if defined(MULTIPLE_SUPPLICANT)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1 && 1
	if (mutex_is_locked(&_dhd_sdio_mutex_lock_) != 0) {
		DHD_ERROR(("%s : dhd_open: call dev open before insmod complete!\n", __FUNCTION__));
	}
	mutex_lock(&_dhd_sdio_mutex_lock_);
#endif
#endif /* MULTIPLE_SUPPLICANT */


	DHD_OS_WAKE_LOCK(&dhd->pub);
	/* Update FW path if it was changed */
	if (strlen(firmware_path) != 0) {
		if (firmware_path[strlen(firmware_path)-1] == '\n')
			firmware_path[strlen(firmware_path)-1] = '\0';
		bzero(fw_path, MOD_PARAM_PATHLEN);
		strncpy(fw_path, firmware_path, sizeof(fw_path)-1);
		firmware_path[0] = '\0';
	}

#ifdef CUSTOMER_HW4
	/* Update NVRAM path if it was changed */
	if (!dhd_download_fw_on_driverload && (strlen(nvram_path) != 0)) {
		if (nvram_path[strlen(nvram_path)-1] == '\n')
			nvram_path[strlen(nvram_path)-1] = '\0';
		bzero(nv_path, MOD_PARAM_PATHLEN);
		strncpy(nv_path, nvram_path, sizeof(nv_path)-1);
	}
#endif /* CUSTOMER_HW4 */


	dhd->pub.dongle_trap_occured = 0;
	dhd->pub.hang_was_sent = 0;
#if !defined(WL_CFG80211)
	/*
	 * Force start if ifconfig_up gets called before START command
	 *  We keep WEXT's wl_control_wl_start to provide backward compatibility
	 *  This should be removed in the future
	 */
	ret = wl_control_wl_start(net);
	if (ret != 0) {
		DHD_ERROR(("%s: failed with code %d\n", __FUNCTION__, ret));
		ret = -1;
		goto exit;
	}

#endif 

	ifidx = dhd_net2idx(dhd, net);
	DHD_TRACE(("%s: ifidx %d\n", __FUNCTION__, ifidx));

	if (ifidx < 0) {
		DHD_ERROR(("%s: Error: called with invalid IF\n", __FUNCTION__));
		ret = -1;
		goto exit;
	}

	if (!dhd->iflist[ifidx]) {
		DHD_ERROR(("%s: Error: called when IF already deleted\n", __FUNCTION__));
		ret = -1;
		goto exit;
	}

	if (ifidx == 0) {
		atomic_set(&dhd->pend_8021x_cnt, 0);
#if defined(WL_CFG80211)
		if (!dhd_download_fw_on_driverload) {
			ret = wl_android_wifi_on(net);
			if (ret != 0) {
				DHD_ERROR(("%s : wl_android_wifi_on failed (%d)\n",
					__FUNCTION__, ret));
				ret = -1;
				goto exit;
			}
		}
#ifdef SUPPORT_DEEP_SLEEP
		else {
			/* Flags to indicate if we distingish
			 * power off policy when user set the memu
			 * "Keep Wi-Fi on during sleep" to "Never"
			 */
			if (trigger_deep_sleep) {
				dhd_deepsleep(net, 0);
				trigger_deep_sleep = 0;
			}
		}
#endif /* SUPPORT_DEEP_SLEEP */
#endif 

		if (dhd->pub.busstate != DHD_BUS_DATA) {

			/* try to bring up bus */
			if ((ret = dhd_bus_start(&dhd->pub)) != 0) {
				DHD_ERROR(("%s: failed with code %d\n", __FUNCTION__, ret));
				ret = -1;
				goto exit;
			}

		}

		/* dhd_prot_init has been called in dhd_bus_start or wl_android_wifi_on */
		memcpy(net->dev_addr, dhd->pub.mac.octet, ETHER_ADDR_LEN);

#ifdef TOE
		/* Get current TOE mode from dongle */
		if (dhd_toe_get(dhd, ifidx, &toe_ol) >= 0 && (toe_ol & TOE_TX_CSUM_OL) != 0)
			dhd->iflist[ifidx]->net->features |= NETIF_F_IP_CSUM;
		else
			dhd->iflist[ifidx]->net->features &= ~NETIF_F_IP_CSUM;
#endif /* TOE */

#if defined(WL_CFG80211)
		if (unlikely(wl_cfg80211_up(NULL))) {
			DHD_ERROR(("%s: failed to bring up cfg80211\n", __FUNCTION__));
			ret = -1;
			goto exit;
		}
#endif /* WL_CFG80211 */
	}

	/* Allow transmit calls */
	netif_start_queue(net);
	dhd->pub.up = 1;

#ifdef BCMDBGFS
	dhd_dbg_init(&dhd->pub);
#endif

	OLD_MOD_INC_USE_COUNT;
exit:
	if (ret)
		dhd_stop(net);

	DHD_OS_WAKE_UNLOCK(&dhd->pub);

#if defined(MULTIPLE_SUPPLICANT)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1 && 1
	mutex_unlock(&_dhd_sdio_mutex_lock_);
#endif
#endif /* MULTIPLE_SUPPLICANT */

	return ret;
}

int dhd_do_driver_init(struct net_device *net)
{
	dhd_info_t *dhd = NULL;

	if (!net) {
		DHD_ERROR(("Primary Interface not initialized \n"));
		return -EINVAL;
	}

#ifdef MULTIPLE_SUPPLICANT
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1 && 1
	if (mutex_is_locked(&_dhd_sdio_mutex_lock_) != 0) {
		DHD_ERROR(("%s : dhdsdio_probe is already running!\n", __FUNCTION__));
		return 0;
	}
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) */
#endif /* MULTIPLE_SUPPLICANT */

	/*  && defined(OEM_ANDROID) && defined(BCMSDIO) */
	dhd = *(dhd_info_t **)netdev_priv(net);

	/* If driver is already initialized, do nothing
	 */
	if (dhd->pub.busstate == DHD_BUS_DATA) {
		DHD_TRACE(("Driver already Inititalized. Nothing to do"));
		return 0;
	}

	if (dhd_open(net) < 0) {
		DHD_ERROR(("Driver Init Failed \n"));
		return -1;
	}

	return 0;
}

osl_t *
dhd_osl_attach(void *pdev, uint bustype)
{
	return osl_attach(pdev, bustype, TRUE);
}

void
dhd_osl_detach(osl_t *osh)
{
	if (MALLOCED(osh)) {
		DHD_ERROR(("%s: MEMORY LEAK %d bytes\n", __FUNCTION__, MALLOCED(osh)));
	}
	osl_detach(osh);
#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	dhd_registration_check = FALSE;
	up(&dhd_registration_sem);
#if !defined(CUSTOMER_HW10)
#if	defined(BCMLXSDMMC)
	up(&dhd_chipup_sem);
#endif
#endif
#endif 
}

int
dhd_event_ifadd(dhd_info_t *dhdinfo, wl_event_data_if_t *ifevent, char *name, uint8 *mac)
{

#ifdef WL_CFG80211
	if (wl_cfg80211_notify_ifadd(ifevent->ifidx, name, mac, ifevent->bssidx) == BCME_OK)
		return BCME_OK;
#endif

	/* handle IF event caused by wl commands, SoftAP, WEXT and
	 * anything else. This has to be done asynchronously otherwise
	 * DPC will be blocked (and iovars will timeout as DPC has no chance
	 * to read the response back)
	 */
	if (ifevent->ifidx > 0) {
		dhd_if_event_t *if_event;

		if (!(if_event = MALLOC(dhdinfo->pub.osh, sizeof(dhd_if_event_t)))) {
			DHD_ERROR(("%s: out of memory for if_event, alloc size:%d)\n",__FUNCTION__, sizeof(dhd_if_event_t)));
			return -ENOMEM;
		}

		memcpy(&if_event->event, ifevent, sizeof(if_event->event));
		memcpy(if_event->mac, mac, ETHER_ADDR_LEN);
		strncpy(if_event->name, name, IFNAMSIZ);
		if_event->name[IFNAMSIZ - 1] = '\0';
		spin_lock_bh(&dhdinfo->if_event_list_lock);
		list_add(&if_event->list, &dhdinfo->if_event_list);
		spin_unlock_bh(&dhdinfo->if_event_list_lock);
		schedule_work(&dhdinfo->sysioc_work);
	}

	return BCME_OK;
}

int
dhd_event_ifdel(dhd_info_t *dhdinfo, wl_event_data_if_t *ifevent, char *name, uint8 *mac)
{
	dhd_if_event_t *if_event;

#ifdef WL_CFG80211
	if (wl_cfg80211_notify_ifdel(ifevent->ifidx, name, mac, ifevent->bssidx) == BCME_OK)
		return BCME_OK;
#endif

	/* handle IF event caused by wl commands, SoftAP, WEXT and
	 * anything else
	 */
	if (!(if_event = MALLOC(dhdinfo->pub.osh, sizeof(dhd_if_event_t)))) {
		DHD_ERROR(("%s: out of memory for if_event, alloc size:%d)\n",__FUNCTION__, sizeof(dhd_if_event_t)));
		return -ENOMEM;
	}
	memcpy(&if_event->event, ifevent, sizeof(if_event->event));
	memcpy(if_event->mac, mac, ETHER_ADDR_LEN);
	strncpy(if_event->name, name, IFNAMSIZ);
	if_event->name[IFNAMSIZ - 1] = '\0';
	spin_lock_bh(&dhdinfo->if_event_list_lock);
	list_add(&if_event->list, &dhdinfo->if_event_list);
	spin_unlock_bh(&dhdinfo->if_event_list_lock);
	schedule_work(&dhdinfo->sysioc_work);

	return BCME_OK;
}

/* unregister and free the existing net_device interface (if any) in iflist and
 * allocate a new one. the slot is reused. this function does NOT register the
 * new interface to linux kernel. dhd_register_if does the job
 */
struct net_device*
dhd_allocate_if(dhd_pub_t *dhdpub, int ifidx, char *name,
	uint8 *mac, uint8 bssidx, bool need_rtnl_lock)
{
	dhd_info_t *dhdinfo = (dhd_info_t *)dhdpub->info;
	dhd_if_t *ifp;

	ASSERT(dhdinfo && (ifidx < DHD_MAX_IFS));
	ifp = dhdinfo->iflist[ifidx];

	if (ifp != NULL) {
		if (ifp->net != NULL) {
			DHD_ERROR(("%s: free existing IF %s\n", __FUNCTION__, ifp->net->name));

			/* in unregister_netdev case, the interface gets freed by net->destructor
			 * (which is set to free_netdev)
			 */
			if (ifp->net->reg_state == NETREG_UNINITIALIZED) {
				free_netdev(ifp->net);
			} else {
				netif_stop_queue(ifp->net);
				if (need_rtnl_lock)
					unregister_netdev(ifp->net);
				else
					unregister_netdevice(ifp->net);
			}
			ifp->net = NULL;
		}
	} else {
		ifp = MALLOC(dhdinfo->pub.osh, sizeof(dhd_if_t));
		if (ifp == NULL) {
			DHD_ERROR(("%s: OOM - dhd_if_t(%d)\n", __FUNCTION__, sizeof(dhd_if_t)));
			return NULL;
		}
	}

	memset(ifp, 0, sizeof(dhd_if_t));
	ifp->info = dhdinfo;
	ifp->idx = ifidx;
	ifp->bssidx = bssidx;
	if (mac != NULL)
		memcpy(&ifp->mac_addr, mac, ETHER_ADDR_LEN);

	/* Allocate etherdev, including space for private structure */
	ifp->net = alloc_etherdev(sizeof(dhdinfo));
	if (ifp->net == NULL) {
		DHD_ERROR(("%s: OOM - alloc_etherdev(%d)\n", __FUNCTION__, sizeof(dhdinfo)));
		goto fail;
	}
	memcpy(netdev_priv(ifp->net), &dhdinfo, sizeof(dhdinfo));
	if (name && name[0]) {
		strncpy(ifp->net->name, name, IFNAMSIZ);
		ifp->net->name[IFNAMSIZ - 1] = '\0';
	}
	ifp->net->destructor = free_netdev;
	strncpy(ifp->name, ifp->net->name, IFNAMSIZ);
	ifp->name[IFNAMSIZ - 1] = '\0';
	dhdinfo->iflist[ifidx] = ifp;
	return ifp->net;

fail:
	if (ifp != NULL) {
		if (ifp->net != NULL) {
			free_netdev(ifp->net);
			ifp->net = NULL;
		}
		MFREE(dhdinfo->pub.osh, ifp, sizeof(*ifp));
		ifp = NULL;
	}
	dhdinfo->iflist[ifidx] = NULL;
	return NULL;
}

/* unregister and free the the net_device interface associated with the indexed
 * slot, also free the slot memory and set the slot pointer to NULL
 */
int
dhd_remove_if(dhd_pub_t *dhdpub, int ifidx, bool need_rtnl_lock)
{
	dhd_info_t *dhdinfo = (dhd_info_t *)dhdpub->info;
	dhd_if_t *ifp;

	ifp = dhdinfo->iflist[ifidx];
	if (ifp != NULL) {
		if (ifp->net != NULL) {
			DHD_ERROR(("deleting interface '%s' idx %d\n", ifp->net->name, ifp->idx));

			/* in unregister_netdev case, the interface gets freed by net->destructor
			 * (which is set to free_netdev)
			 */
			if (ifp->net->reg_state == NETREG_UNINITIALIZED) {
				free_netdev(ifp->net);
			} else {
				netif_stop_queue(ifp->net);
				if (need_rtnl_lock)
					unregister_netdev(ifp->net);
				else
					unregister_netdevice(ifp->net);
			}
			ifp->net = NULL;
		}

		dhdinfo->iflist[ifidx] = NULL;
		MFREE(dhdinfo->pub.osh, ifp, sizeof(*ifp));

	}

	return BCME_OK;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31))
static struct net_device_ops dhd_ops_pri = {
	.ndo_open = dhd_open,
	.ndo_stop = dhd_stop,
	.ndo_get_stats = dhd_get_stats,
	.ndo_do_ioctl = dhd_ioctl_entry,
	.ndo_start_xmit = dhd_start_xmit,
	.ndo_set_mac_address = dhd_set_mac_address,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0))
	.ndo_set_rx_mode = dhd_set_multicast_list,
#else
	.ndo_set_multicast_list = dhd_set_multicast_list,
#endif
};

static struct net_device_ops dhd_ops_virt = {
	.ndo_get_stats = dhd_get_stats,
	.ndo_do_ioctl = dhd_ioctl_entry,
	.ndo_start_xmit = dhd_start_xmit,
	.ndo_set_mac_address = dhd_set_mac_address,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0))
	.ndo_set_rx_mode = dhd_set_multicast_list,
#else
	.ndo_set_multicast_list = dhd_set_multicast_list,
#endif
};
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31)) */

#ifdef DEBUGGER
extern void debugger_init(void *bus_handle);
#endif

dhd_pub_t *
dhd_attach(osl_t *osh, struct dhd_bus *bus, uint bus_hdrlen)
{
	dhd_info_t *dhd = NULL;
	struct net_device *net = NULL;
	char if_name[IFNAMSIZ] = {'\0'};
#ifdef PROP_TXSTATUS
	int enabled = FALSE;
#endif

	dhd_attach_states_t dhd_state = DHD_ATTACH_STATE_INIT;
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	/* updates firmware nvram path if it was provided as module parameters */
	if (strlen(firmware_path) != 0) {
		bzero(fw_path, MOD_PARAM_PATHLEN);
		strncpy(fw_path, firmware_path, sizeof(fw_path) - 1);
	}
	if (strlen(nvram_path) != 0) {
		bzero(nv_path, MOD_PARAM_PATHLEN);
		strncpy(nv_path, nvram_path, sizeof(nv_path) -1);
	}

	/* Allocate primary dhd_info */
#if defined(CONFIG_DHD_USE_STATIC_BUF)
	dhd = (void *)dhd_os_prealloc(osh, DHD_PREALLOC_DHD_INFO, sizeof(dhd_info_t));
	if (!dhd) {
		DHD_INFO(("%s: OOM - Pre-alloc dhd_info\n", __FUNCTION__));
#endif /* CONFIG_DHD_USE_STATIC_BUF */
		if (!(dhd = MALLOC(osh, sizeof(dhd_info_t)))) {
			DHD_ERROR(("%s: OOM - alloc dhd_info\n", __FUNCTION__));
			goto fail;
		}
#if defined(CONFIG_DHD_USE_STATIC_BUF)
	}
#endif /* CONFIG_DHD_USE_STATIC_BUF */
	memset(dhd, 0, sizeof(dhd_info_t));

	dhd->thr_dpc_ctl.thr_pid = DHD_PID_KT_TL_INVALID;
	dhd->thr_wdt_ctl.thr_pid = DHD_PID_KT_INVALID;
	dhd_state |= DHD_ATTACH_STATE_DHD_ALLOC;

	dhd->pub.osh = osh;

	/* Link to info module */
	dhd->pub.info = dhd;
	/* Link to bus module */
	dhd->pub.bus = bus;
	dhd->pub.hdrlen = bus_hdrlen;

	/* Set network interface name if it was provided as module parameter */
	if (iface_name[0]) {
		int len;
		char ch;
		strncpy(if_name, iface_name, IFNAMSIZ);
		if_name[IFNAMSIZ - 1] = 0;
		len = strlen(if_name);
		ch = if_name[len - 1];
		if ((ch > '9' || ch < '0') && (len < IFNAMSIZ - 2))
			strcat(if_name, "%d");
	}
	net = dhd_allocate_if(&dhd->pub, 0, if_name, NULL, 0, TRUE);
	if (net == NULL)
		goto fail;
	dhd_state |= DHD_ATTACH_STATE_ADD_IF;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31))
	net->open = NULL;
#else
	net->netdev_ops = NULL;
#endif

	sema_init(&dhd->proto_sem, 1);

#ifdef PROP_TXSTATUS
	spin_lock_init(&dhd->wlfc_spinlock);
#if !defined(PROP_TXSTATUS_VSDB)
	enabled = TRUE;
#endif 
	dhd_wlfc_set_enable(&dhd->pub, enabled);

	dhd->pub.ptx_opt_enabled = FALSE;
	dhd->pub.skip_fc = dhd_wlfc_skip_fc;
	dhd->pub.plat_enable = dhd_wlfc_plat_enable;
	dhd->pub.plat_deinit = dhd_wlfc_plat_deinit;
#endif /* PROP_TXSTATUS */

	/* Initialize other structure content */
	init_waitqueue_head(&dhd->ioctl_resp_wait);
	init_waitqueue_head(&dhd->ctrl_wait);

	/* Initialize the spinlocks */
	spin_lock_init(&dhd->sdlock);
	spin_lock_init(&dhd->txqlock);
	spin_lock_init(&dhd->dhd_lock);
	spin_lock_init(&dhd->rxf_lock);
#if defined(RXFRAME_THREAD)
	dhd->rxthread_enabled = TRUE;
#endif /* defined(RXFRAME_THREAD) */

#ifdef DHDTCPACK_SUPPRESS
	spin_lock_init(&dhd->tcpack_lock);
#endif /* DHDTCPACK_SUPPRESS */

	/* Initialize Wakelock stuff */
	spin_lock_init(&dhd->wakelock_spinlock);
	dhd->wakelock_counter = 0;
	dhd->wakelock_wd_counter = 0;
	dhd->wakelock_rx_timeout_enable = 0;
	dhd->wakelock_ctrl_timeout_enable = 0;
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&dhd->wl_wifi, WAKE_LOCK_SUSPEND, "wlan_wake");
	wake_lock_init(&dhd->wl_rxwake, WAKE_LOCK_SUSPEND, "wlan_rx_wake");
	wake_lock_init(&dhd->wl_ctrlwake, WAKE_LOCK_SUSPEND, "wlan_ctrl_wake");
	wake_lock_init(&dhd->wl_wdwake, WAKE_LOCK_SUSPEND, "wlan_wd_wake");
#endif /* CONFIG_HAS_WAKELOCK */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	mutex_init(&dhd->dhd_net_if_mutex);
	mutex_init(&dhd->dhd_suspend_mutex);
#endif
	dhd_state |= DHD_ATTACH_STATE_WAKELOCKS_INIT;

	/* Attach and link in the protocol */
	if (dhd_prot_attach(&dhd->pub) != 0) {
		DHD_ERROR(("dhd_prot_attach failed\n"));
		goto fail;
	}
	dhd_state |= DHD_ATTACH_STATE_PROT_ATTACH;

#ifdef WL_CFG80211
	/* Attach and link in the cfg80211 */
	if (unlikely(wl_cfg80211_attach(net, &dhd->pub))) {
		DHD_ERROR(("wl_cfg80211_attach failed\n"));
		goto fail;
	}

	dhd_monitor_init(&dhd->pub);
	dhd_state |= DHD_ATTACH_STATE_CFG80211;
#endif
#if defined(WL_WIRELESS_EXT)
	/* Attach and link in the iw */
	if (!(dhd_state &  DHD_ATTACH_STATE_CFG80211)) {
		if (wl_iw_attach(net, (void *)&dhd->pub) != 0) {
		DHD_ERROR(("wl_iw_attach failed\n"));
		goto fail;
	}
	dhd_state |= DHD_ATTACH_STATE_WL_ATTACH;
	}
#endif /* defined(WL_WIRELESS_EXT) */


	/* Set up the watchdog timer */
	init_timer(&dhd->timer);
	dhd->timer.data = (ulong)dhd;
	dhd->timer.function = dhd_watchdog;
	dhd->default_wd_interval = dhd_watchdog_ms;

	/* Initialize thread based operation and lock */
	sema_init(&dhd->sdsem, 1);

	if (dhd_watchdog_prio >= 0) {
		/* Initialize watchdog thread */
		PROC_START(dhd_watchdog_thread, dhd, &dhd->thr_wdt_ctl, 0, "dhd_watchdog_thread");

	} else {
		dhd->thr_wdt_ctl.thr_pid = -1;
	}

#ifdef DEBUGGER
	debugger_init((void *) bus);
#endif

	/* Set up the bottom half handler */
	if (dhd_dpc_prio >= 0) {
		/* Initialize DPC thread */
		PROC_START(dhd_dpc_thread, dhd, &dhd->thr_dpc_ctl, 0, "dhd_dpc");
	} else {
		/*  use tasklet for dpc */
		tasklet_init(&dhd->tasklet, dhd_dpc, (ulong)dhd);
		dhd->thr_dpc_ctl.thr_pid = -1;
	}

	if (dhd->rxthread_enabled) {
		bzero(&dhd->pub.skbbuf[0], sizeof(void *) * MAXSKBPEND);
		/* Initialize RXF thread */
		PROC_START(dhd_rxf_thread, dhd, &dhd->thr_rxf_ctl, 0, "dhd_rxf");
	}

	MY_INIT_WORK(&dhd->sysioc_work, sysioc_work_callback);
	spin_lock_init(&dhd->if_event_list_lock);
	INIT_LIST_HEAD(&dhd->if_event_list);

	dhd_state |= DHD_ATTACH_STATE_THREADS_CREATED;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && (1)
	INIT_WORK(&dhd->work_hang, dhd_hang_process);
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))  */

	/*
	 * Save the dhd_info into the priv
	 */
	memcpy(netdev_priv(net), &dhd, sizeof(dhd));

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP)
	register_pm_notifier(&dhd_sleep_pm_notifier);
#endif /*  (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP) */

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
	dhd->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 20;
	dhd->early_suspend.suspend = dhd_early_suspend;
	dhd->early_suspend.resume = dhd_late_resume;
	register_early_suspend(&dhd->early_suspend);
	dhd_state |= DHD_ATTACH_STATE_EARLYSUSPEND_DONE;
#endif /* CONFIG_HAS_EARLYSUSPEND && DHD_USE_EARLYSUSPEND */

#ifdef ARP_OFFLOAD_SUPPORT
	dhd->pend_ipaddr = 0;
	register_inetaddr_notifier(&dhd_notifier);
#endif /* ARP_OFFLOAD_SUPPORT */

#ifdef DHDTCPACK_SUPPRESS
	dhd->pub.tcp_ack_info_cnt = 0;
	bzero(dhd->pub.tcp_ack_info_tbl, sizeof(struct tcp_ack_info)*MAXTCPSTREAMS);
#endif /* DHDTCPACK_SUPPRESS */

	dhd_state |= DHD_ATTACH_STATE_DONE;
	dhd->dhd_state = dhd_state;
	return &dhd->pub;

fail:
	if (dhd_state >= DHD_ATTACH_STATE_DHD_ALLOC) {
		DHD_TRACE(("%s: Calling dhd_detach dhd_state 0x%x &dhd->pub %p\n",
			__FUNCTION__, dhd_state, &dhd->pub));
		dhd->dhd_state = dhd_state;
		dhd_detach(&dhd->pub);
		dhd_free(&dhd->pub);
	}

	return NULL;
}

int
dhd_bus_start(dhd_pub_t *dhdp)
{
	int ret = -1;
	dhd_info_t *dhd = (dhd_info_t*)dhdp->info;
	unsigned long flags;

	ASSERT(dhd);

	DHD_TRACE(("Enter %s:\n", __FUNCTION__));

	dhd_os_sdlock(dhdp);

	/* try to download image and nvram to the dongle */
	if  ((dhd->pub.busstate == DHD_BUS_DOWN) &&
		(fw_path[0] != '\0') && (nv_path[0] != '\0')) {
#ifdef SHOW_NVRAM_TYPE
		{	/* Show nvram type in the kernel log */
			int i;
			for (i = 0; nv_path[i] != '\0'; ++i) {
				if (nv_path[i] == '.') {
					++i;
					break;
				}
			}
			DHD_ERROR(("%s: nvram_type = [%s]\n", __FUNCTION__, &nv_path[i]));
		}
#endif /* SHOW_NVRAM_TYPE */
		/* wake lock moved to dhdsdio_download_firmware */
		if (!(dhd_bus_download_firmware(dhd->pub.bus, dhd->pub.osh,
			fw_path, nv_path))) {
#ifdef CUSTOMER_HW4
			DHD_ERROR(("%s: dhdsdio_probe_download failed. "
					"firmware or nvram wrong\n",
					__FUNCTION__));
#else
			DHD_ERROR(("%s: dhdsdio_probe_download failed. firmware = %s nvram = %s\n",
				__FUNCTION__, fw_path, nv_path));
#endif /* CUSTOMER_HW4 */
			dhd_os_sdunlock(dhdp);
			return -1;
		}
	}
	if (dhd->pub.busstate != DHD_BUS_LOAD) {
		dhd_os_sdunlock(dhdp);
		return -ENETDOWN;
	}

	/* Start the watchdog timer */
	dhd->pub.tickcnt = 0;
	dhd_os_wd_timer(&dhd->pub, dhd_watchdog_ms);

	/* Bring up the bus */
	if ((ret = dhd_bus_init(&dhd->pub, FALSE)) != 0) {

		DHD_ERROR(("%s, dhd_bus_init failed %d\n", __FUNCTION__, ret));
		dhd_os_sdunlock(dhdp);
		return ret;
	}
#if defined(OOB_INTR_ONLY)
	/* Host registration for OOB interrupt */
	if (bcmsdh_register_oob_intr(dhdp)) {
		/* deactivate timer and wait for the handler to finish */

		flags = dhd_os_spin_lock(&dhd->pub);
		dhd->wd_timer_valid = FALSE;
		dhd_os_spin_unlock(&dhd->pub, flags);
		del_timer_sync(&dhd->timer);

		DHD_ERROR(("%s Host failed to register for OOB\n", __FUNCTION__));
		dhd_os_sdunlock(dhdp);
		DHD_OS_WD_WAKE_UNLOCK(&dhd->pub);
		return -ENODEV;
	}

	/* Enable oob at firmware */
	dhd_enable_oob_intr(dhd->pub.bus, TRUE);
#endif 

	/* If bus is not ready, can't come up */
	if (dhd->pub.busstate != DHD_BUS_DATA) {
		flags = dhd_os_spin_lock(&dhd->pub);
		dhd->wd_timer_valid = FALSE;
		dhd_os_spin_unlock(&dhd->pub, flags);
		del_timer_sync(&dhd->timer);
		DHD_ERROR(("%s failed bus is not ready\n", __FUNCTION__));
		dhd_os_sdunlock(dhdp);
		DHD_OS_WD_WAKE_UNLOCK(&dhd->pub);
		return -ENODEV;
	}

	dhd_os_sdunlock(dhdp);

	if ((dhd->pub.busstate == DHD_BUS_DATA))
		dhd_txglom_enable(dhdp, TRUE);

#ifdef READ_MACADDR
	dhd_read_macaddr(dhd);
#endif

	/* Bus is ready, do any protocol initialization */
	if ((ret = dhd_prot_init(&dhd->pub)) < 0)
		return ret;

#ifdef WRITE_MACADDR
	dhd_write_macaddr(dhd->pub.mac.octet);
#endif

#ifdef ARP_OFFLOAD_SUPPORT
	if (dhd->pend_ipaddr) {
#ifdef AOE_IP_ALIAS_SUPPORT
		aoe_update_host_ipv4_table(&dhd->pub, dhd->pend_ipaddr, TRUE, 0);
#endif /* AOE_IP_ALIAS_SUPPORT */
		dhd->pend_ipaddr = 0;
	}
#endif /* ARP_OFFLOAD_SUPPORT */

	return 0;
}

bool dhd_is_concurrent_mode(dhd_pub_t *dhd)
{
	if (!dhd)
		return FALSE;

	if (dhd->op_mode & DHD_FLAG_CONCURR_MULTI_CHAN_MODE)
		return TRUE;
	else if ((dhd->op_mode & DHD_FLAG_CONCURR_SINGLE_CHAN_MODE) ==
		DHD_FLAG_CONCURR_SINGLE_CHAN_MODE)
		return TRUE;
	else
		return FALSE;
}

/* LGE_patch : S : config file setting */
#if defined(CONFIG_LGE_BCM433X_PATCH)
#include <linux/fs.h>
#include <linux/ctype.h>

#define strtoul(nptr, endptr, base) bcm_strtoul((nptr), (endptr), (base))
//char config_path[MOD_PARAM_PATHLEN] = "";
bool PM_control = TRUE;

static int dhd_preinit_proc(dhd_pub_t *dhd, int ifidx, char *name, char *value)
{
	int var_int;
	wl_country_t cspec = {{0}, -1, {0}};
	char *revstr;
	char *endptr = NULL;
	int iolen;        
	char smbuf[WLC_IOCTL_SMLEN*2];
#ifdef ROAM_AP_ENV_DETECTION
	int roam_env_mode = AP_ENV_INDETERMINATE;
#endif /* ROAM_AP_ENV_DETECTION */

	if (!strcmp(name, "country")) {
		revstr = strchr(value, '/');
		if (revstr) {
			cspec.rev = strtoul(revstr + 1, &endptr, 10);
			memcpy(cspec.country_abbrev,value,WLC_CNTRY_BUF_SZ);
			cspec.country_abbrev[2] = '\0';
			memcpy(cspec.ccode,cspec.country_abbrev,WLC_CNTRY_BUF_SZ);
			memset(smbuf, 0, sizeof(smbuf));
			printf("config country code is country : %s, rev : %d !!\n", cspec.country_abbrev,cspec.rev);
			iolen = bcm_mkiovar("country", (char*)&cspec, sizeof(cspec), smbuf, sizeof(smbuf));
			return dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR,
					smbuf, iolen, TRUE, 0);
		}
		return dhd_wl_ioctl_cmd(dhd, WLC_SET_COUNTRY,
				value, WLC_CNTRY_BUF_SZ, TRUE, 0);
	} else if (!strcmp(name, "roam_scan_period")) {
		var_int = (int)simple_strtol(value, NULL, 0);
		return dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_SCAN_PERIOD,
				&var_int, sizeof(var_int), TRUE, 0);
	} else if (!strcmp(name, "roam_delta")) {
		struct {
			int val;
			int band;
		} x;
		x.val = (int)simple_strtol(value, NULL, 0);
		//x.band = WLC_BAND_AUTO;
		x.band = WLC_BAND_ALL;
		return dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_DELTA, &x, sizeof(x), TRUE, 0);
	} else if (!strcmp(name, "roam_trigger")) {
		int ret = 0;
		
		roam_trigger[0] = (int)simple_strtol(value, NULL, 0);
		roam_trigger[1] = WLC_BAND_ALL;
		ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_TRIGGER, &roam_trigger, sizeof(roam_trigger), TRUE, 0);

#ifdef ROAM_AP_ENV_DETECTION
		if (roam_trigger[0] == WL_AUTO_ROAM_TRIGGER) {
			char iovbuf[128];
			bcm_mkiovar("roam_env_detection", (char *)&roam_env_mode,
				4, iovbuf, sizeof(iovbuf));
			if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) == BCME_OK) {
				dhd->roam_env_detection = TRUE;
			} else {
				dhd->roam_env_detection = FALSE;
			}
		}
#endif /* ROAM_AP_ENV_DETECTION */
		return ret;
	} else if (!strcmp(name, "PM")) {
		int ret = 0;
		var_int = (int)simple_strtol(value, NULL, 0);
		
		ret =  dhd_wl_ioctl_cmd(dhd, WLC_SET_PM,
				&var_int, sizeof(var_int), TRUE, 0);

#if defined(CONFIG_CONTROL_PM) || defined(CONFIG_PM_LOCK)
		if (var_int == 0) {
			g_pm_control = TRUE;
			printk("%s var_int=%d don't control PM\n", __func__,var_int);
		} else {
			g_pm_control = FALSE;
			printk("%s var_int=%d do control PM\n", __func__,var_int);
		}
#endif

		return ret;
	}
#ifdef WLBTAMP
	else if(!strcmp(name, "btamp_chan")) {
		int btamp_chan;
		int iov_len=0;
		char iovbuf[128];
		int ret;
		
		btamp_chan = (int)simple_strtol(value, NULL, 0);
		iov_len = bcm_mkiovar("btamp_chan", (char *)&btamp_chan, 4, iovbuf, sizeof(iovbuf));
		if ((ret  = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, iov_len, TRUE, 0) < 0))
			DHD_ERROR(("%s btamp_chan=%d set failed code %d\n", __FUNCTION__,btamp_chan, ret));	
		else
			DHD_ERROR(("%s btamp_chan %d set success\n", __FUNCTION__,btamp_chan));
	}
#endif
	 else if(!strcmp(name, "band")) {
		int ret;
		if(!strcmp(value,"auto"))
			var_int = WLC_BAND_AUTO;
		else if(!strcmp(value, "a"))
			var_int = WLC_BAND_5G;
		else if(!strcmp(value, "b"))
			var_int = WLC_BAND_2G;
		else if(!strcmp(value, "all"))
			var_int = WLC_BAND_ALL;
		else{
			printk(" set band value should be one of the a or b or all\n");
			var_int = WLC_BAND_AUTO;
		}
		if( (ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_BAND, &var_int,sizeof(var_int), TRUE, 0)) < 0)
			printk(" set band err=%d\n", ret);
		wl_update_wiphybands(NULL, true);
		return ret;

	} else if(!strcmp(name,"cur_etheraddr")) {
		struct ether_addr ea;
		char buf[32];
		uint iovlen;
		int ret;

		bcm_ether_atoe(value, &ea);

		ret = memcmp( &ea.octet, dhd->mac.octet, ETHER_ADDR_LEN);
		if(ret == 0){
			DHD_ERROR(("%s: Same Macaddr\n",__FUNCTION__));
			return 0;
		}

		DHD_ERROR(("%s: Change Macaddr = %02X:%02X:%02X:%02X:%02X:%02X\n",__FUNCTION__,
					ea.octet[0], ea.octet[1], ea.octet[2],
					ea.octet[3], ea.octet[4], ea.octet[5]));

		iovlen = bcm_mkiovar("cur_etheraddr", (char*)&ea, ETHER_ADDR_LEN, buf, 32);

		ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, buf, iovlen, TRUE, 0);
		if (ret < 0) {
			DHD_ERROR(("%s: can't set MAC address , error=%d\n", __FUNCTION__, ret));
			return ret;
		}
		else{
			memcpy(dhd->mac.octet, (void *)&ea, ETHER_ADDR_LEN);
			return ret;
		}
	} else {
		uint iovlen;
		char iovbuf[WLC_IOCTL_SMLEN];

		/* wlu_iovar_setint */
		var_int = (int)simple_strtol(value, NULL, 0);

		/* Setup timeout bcn_timeout from dhd driver 4.217.48 */
		if(!strcmp(name, "roam_off")) {
			/* Setup timeout if Beacons are lost to report link down */
			if (var_int) {
				uint bcn_timeout = 2;
				bcm_mkiovar("bcn_timeout", (char *)&bcn_timeout, 4, iovbuf, sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
			}
		}
		/* Setup timeout bcm_timeout from dhd driver 4.217.48 */

		DHD_INFO(("%s:[%s]=[%d]\n", __FUNCTION__, name, var_int));

		iovlen = bcm_mkiovar(name, (char *)&var_int, sizeof(var_int),
				iovbuf, sizeof(iovbuf));
		return dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR,
				iovbuf, iovlen, TRUE, 0);
	}

	return 0;
}

static int dhd_preinit_config(dhd_pub_t *dhd, int ifidx)
{
	mm_segment_t old_fs;
	struct kstat stat;
	struct file *fp = NULL;
	unsigned int len;
	char *buf = NULL, *p, *name, *value;
	int ret = 0;
	char *config_path;

	config_path = CONFIG_BCMDHD_CONFIG_PATH;

	if (!config_path)
	{
		printk(KERN_ERR "config_path can't read. \n");
		return 0;
	}

	old_fs = get_fs();
	set_fs(get_ds());
	if ((ret = vfs_stat(config_path, &stat))) {
		set_fs(old_fs);
		printk(KERN_ERR "%s: Failed to get information (%d)\n",
				config_path, ret);
		return ret;
	}
	set_fs(old_fs);

	if (!(buf = MALLOC(dhd->osh, stat.size + 1))) {
		printk(KERN_ERR "Failed to allocate memory %llu bytes\n", stat.size);
		return -ENOMEM;
	}

	printk("dhd_preinit_config : config path : %s \n",config_path);

	if (!(fp = dhd_os_open_image(config_path)) ||
		(len = dhd_os_get_image_block(buf, stat.size, fp)) < 0)
		goto err;

	buf[stat.size] = '\0';
	for (p = buf; *p; p++) {
		if (isspace(*p))
			continue;
		for (name = p++; *p && !isspace(*p); p++) {
			if (*p == '=') {
				*p = '\0';
				p++;
				for (value = p; *p && !isspace(*p); p++);
				*p = '\0';
				if ((ret = dhd_preinit_proc(dhd, ifidx, name, value)) < 0) {
					printk(KERN_ERR "%s: %s=%s\n",
							bcmerrorstr(ret), name, value);
				}
				break;
			}
		}
	}
	ret = 0;

out:
	if (fp)
		dhd_os_close_image(fp);
	if (buf)
		MFREE(dhd->osh, buf, stat.size+1);
	return ret;

err:
	ret = -1;
	goto out;
}
#endif /* CONFIG_LGE_BCM433X_PATCH */
/* LGE_patch : E : config file setting */

#if !defined(AP) && defined(WLP2P)
/* From Android JerryBean release, the concurrent mode is enabled by default and the firmware
 * name would be fw_bcmdhd.bin. So we need to determine whether P2P is enabled in the STA
 * firmware and accordingly enable concurrent mode (Apply P2P settings). SoftAP firmware
 * would still be named as fw_bcmdhd_apsta.
 */
uint32
dhd_get_concurrent_capabilites(dhd_pub_t *dhd)
{
	int32 ret = 0;
	char buf[WLC_IOCTL_SMLEN];
	bool mchan_supported = FALSE;
	/* if dhd->op_mode is already set for HOSTAP,
	  * that means we only will use the mode as it is
	  */
	if (dhd->op_mode & DHD_FLAG_HOSTAP_MODE)
		return 0;
	memset(buf, 0, sizeof(buf));
	bcm_mkiovar("cap", 0, 0, buf, sizeof(buf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, buf, sizeof(buf),
		FALSE, 0)) < 0) {
		DHD_ERROR(("%s: Get Capability failed (error=%d)\n",
			__FUNCTION__, ret));
		return 0;
	}
	if (strstr(buf, "vsdb")) {
		mchan_supported = TRUE;
	}
	if (strstr(buf, "p2p") == NULL) {
		DHD_TRACE(("Chip does not support p2p\n"));
		return 0;
	}
	else {
		/* Chip supports p2p but ensure that p2p is really implemented in firmware or not */
		memset(buf, 0, sizeof(buf));
		bcm_mkiovar("p2p", 0, 0, buf, sizeof(buf));
		if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, buf, sizeof(buf),
			FALSE, 0)) < 0) {
			DHD_ERROR(("%s: Get P2P failed (error=%d)\n", __FUNCTION__, ret));
			return 0;
		}
		else {
			if (buf[0] == 1) {
				/* By default, chip supports single chan concurrency,
				* now lets check for mchan
				*/
				ret = DHD_FLAG_CONCURR_SINGLE_CHAN_MODE;
				if (mchan_supported)
					ret |= DHD_FLAG_CONCURR_MULTI_CHAN_MODE;
#if defined(WL_ENABLE_P2P_IF) || defined(WL_CFG80211_P2P_DEV_IF)
				/* For customer_hw4, although ICS,
				* we still support concurrent mode
				*/
				return ret;
#else
				return 0;
#endif 
			}
		}
	}
	return 0;
}
#endif 
int
dhd_preinit_ioctls(dhd_pub_t *dhd)
{
	int ret = 0;
	char eventmask[WL_EVENTING_MASK_LEN];
	char iovbuf[WL_EVENTING_MASK_LEN + 12];	/*  Room for "event_msgs" + '\0' + bitvec  */
	uint32 buf_key_b4_m4 = 1;
#if defined(BCMSUP_4WAY_HANDSHAKE) && defined(WLAN_AKM_SUITE_FT_8021X)
	uint32 sup_wpa = 0;
#endif
#ifdef CUSTOM_AMPDU_BA_WSIZE
	uint32 ampdu_ba_wsize = CUSTOM_AMPDU_BA_WSIZE;
#endif /* CUSTOM_AMPDU_BA_WSIZE */
#ifdef DHD_ENABLE_LPC
	uint32 lpc = 1;
#endif /* DHD_ENABLE_LPC */
	uint power_mode = PM_FAST;
	uint32 dongle_align = DHD_SDALIGN;
	uint32 glom = CUSTOM_GLOM_SETTING;
#if defined(CUSTOMER_HW2) && defined(USE_WL_CREDALL)
	uint32 credall = 1;
#endif
#if defined(CUSTOMER_HW4) && (defined(VSDB) || defined(ROAM_ENABLE))
	uint bcn_timeout = 8;
#else
	uint bcn_timeout = 4;
#endif /* CUSTOMER_HW4 && (VSDB || ROAM_ENABLE) */
#if defined(CUSTOMER_HW4) && defined(ENABLE_BCN_LI_BCN_WAKEUP)
	uint32 bcn_li_bcn = 1;
#endif /* CUSTOMER_HW4 && ENABLE_BCN_LI_BCN_WAKEUP */
	uint retry_max = 3;
#if defined(ARP_OFFLOAD_SUPPORT)
	int arpoe = 1;
#endif
	int scan_assoc_time = DHD_SCAN_ASSOC_ACTIVE_TIME;
	int scan_unassoc_time = DHD_SCAN_UNASSOC_ACTIVE_TIME;
	int scan_passive_time = DHD_SCAN_PASSIVE_TIME;
	char buf[WLC_IOCTL_SMLEN];
	char *ptr;
	uint32 listen_interval = CUSTOM_LISTEN_INTERVAL; /* Default Listen Interval in Beacons */
#ifdef ROAM_ENABLE
	uint roamvar = 0;
#ifndef CUSTOMER_HW10
	int roam_trigger[2] = {CUSTOM_ROAM_TRIGGER_SETTING, WLC_BAND_ALL};
#endif
	int roam_scan_period[2] = {10, WLC_BAND_ALL};
	int roam_delta[2] = {CUSTOM_ROAM_DELTA_SETTING, WLC_BAND_ALL};
#ifndef CUSTOMER_HW10
#ifdef ROAM_AP_ENV_DETECTION
	int roam_env_mode = AP_ENV_INDETERMINATE;
#endif /* ROAM_AP_ENV_DETECTION */
#endif
#ifdef FULL_ROAMING_SCAN_PERIOD_60_SEC
	int roam_fullscan_period = 60;
#else /* FULL_ROAMING_SCAN_PERIOD_60_SEC */
	int roam_fullscan_period = 120;
#endif /* FULL_ROAMING_SCAN_PERIOD_60_SEC */
#else
#ifdef DISABLE_BUILTIN_ROAM
	uint roamvar = 1;
#endif /* DISABLE_BUILTIN_ROAM */
#endif /* ROAM_ENABLE */

#if defined(SOFTAP)
	uint dtim = 1;
#endif
#if (defined(AP) && !defined(WLP2P)) || (!defined(AP) && defined(WL_CFG80211))
	uint32 mpc = 0; /* Turn MPC off for AP/APSTA mode */
	struct ether_addr p2p_ea;
#endif

#if defined(AP) || defined(WLP2P)
	uint32 apsta = 1; /* Enable APSTA mode */
#endif /* defined(AP) || defined(WLP2P) */
#ifdef GET_CUSTOM_MAC_ENABLE
	struct ether_addr ea_addr;
#endif /* GET_CUSTOM_MAC_ENABLE */
#ifdef WLTDLS
	uint32 tdls = 1;
	uint32 tdls_auto_op = 1;
	uint32 tdls_idle_time = CUSTOM_TDLS_IDLE_MODE_SETTING;
#ifdef CUSTOMER_HW4
	int32 tdls_rssi_high = -80;
	int32 tdls_rssi_low = -85;
#endif
#endif /* WLTDLS */

#ifdef DISABLE_11N
	uint32 nmode = 0;
#endif /* DISABLE_11N */
#if defined(VSDB) && defined(CUSTOMER_HW4)
	int interference_mode = 3;
#endif
#ifdef USE_WL_TXBF
	uint32 txbf = 1;
#endif /* USE_WL_TXBF */
#ifdef USE_WL_FRAMEBURST
	uint32 frameburst = 1;
#endif /* USE_WL_FRAMEBURST */
#ifdef DHD_SET_FW_HIGHSPEED
	uint32 ack_ratio = 250;
	uint32 ack_ratio_depth = 64;
#endif /* DHD_SET_FW_HIGHSPEED */
#ifdef PROP_TXSTATUS
	int enabled;
#ifdef PROP_TXSTATUS_VSDB
	/* In case the host does not support proptxstatus, hostreorder in dongle should be off */
	uint32 hostreorder = 0;
	enabled = FALSE;
	/* enable WLFC only if the firmware is VSDB */
#else
	enabled = TRUE;
#endif /* PROP_TXSTATUS_VSDB */
	dhd_wlfc_set_enable(dhd, enabled);
#endif /* PROP_TXSTATUS */
	dhd->suspend_bcn_li_dtim = CUSTOM_SUSPEND_BCN_LI_DTIM;
	DHD_TRACE(("Enter %s\n", __FUNCTION__));
	dhd->op_mode = 0;
#ifdef GET_CUSTOM_MAC_ENABLE
	ret = dhd_custom_get_mac_address(ea_addr.octet);
	if (!ret) {
		memset(buf, 0, sizeof(buf));
		bcm_mkiovar("cur_etheraddr", (void *)&ea_addr, ETHER_ADDR_LEN, buf, sizeof(buf));
		ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, buf, sizeof(buf), TRUE, 0);
		if (ret < 0) {
			DHD_ERROR(("%s: can't set MAC address , error=%d\n", __FUNCTION__, ret));
			return BCME_NOTUP;
		}
		memcpy(dhd->mac.octet, ea_addr.octet, ETHER_ADDR_LEN);
	} else {
#endif /* GET_CUSTOM_MAC_ENABLE */
		/* Get the default device MAC address directly from firmware */
		memset(buf, 0, sizeof(buf));
		bcm_mkiovar("cur_etheraddr", 0, 0, buf, sizeof(buf));
		if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, buf, sizeof(buf),
			FALSE, 0)) < 0) {
			DHD_ERROR(("%s: can't get MAC address , error=%d\n", __FUNCTION__, ret));
			return BCME_NOTUP;
		}
		/* Update public MAC address after reading from Firmware */
		memcpy(dhd->mac.octet, buf, ETHER_ADDR_LEN);

#ifdef GET_CUSTOM_MAC_ENABLE
	}
#endif /* GET_CUSTOM_MAC_ENABLE */

	DHD_TRACE(("Firmware = %s\n", fw_path));

	if ((!op_mode && strstr(fw_path, "_apsta") != NULL) ||
		(op_mode == DHD_FLAG_HOSTAP_MODE)) {
#ifdef SET_RANDOM_MAC_SOFTAP
		uint rand_mac;
#endif
		dhd->op_mode = DHD_FLAG_HOSTAP_MODE;
#if defined(ARP_OFFLOAD_SUPPORT)
			arpoe = 0;
#endif
#ifdef PKT_FILTER_SUPPORT
			dhd_pkt_filter_enable = FALSE;
#endif
#ifdef SET_RANDOM_MAC_SOFTAP
		srandom32((uint)jiffies);
		rand_mac = random32();
		iovbuf[0] = 0x02;			   /* locally administered bit */
		iovbuf[1] = 0x1A;
		iovbuf[2] = 0x11;
		iovbuf[3] = (unsigned char)(rand_mac & 0x0F) | 0xF0;
		iovbuf[4] = (unsigned char)(rand_mac >> 8);
		iovbuf[5] = (unsigned char)(rand_mac >> 16);

		bcm_mkiovar("cur_etheraddr", (void *)iovbuf, ETHER_ADDR_LEN, buf, sizeof(buf));
		ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, buf, sizeof(buf), TRUE, 0);
		if (ret < 0) {
			DHD_ERROR(("%s: can't set MAC address , error=%d\n", __FUNCTION__, ret));
		} else
			memcpy(dhd->mac.octet, iovbuf, ETHER_ADDR_LEN);
#endif /* SET_RANDOM_MAC_SOFTAP */
#if !defined(AP) && defined(WL_CFG80211)
		/* Turn off MPC in AP mode */
		bcm_mkiovar("mpc", (char *)&mpc, 4, iovbuf, sizeof(iovbuf));
		if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
			sizeof(iovbuf), TRUE, 0)) < 0) {
			DHD_ERROR(("%s mpc for HostAPD failed  %d\n", __FUNCTION__, ret));
		}
#endif

	}
	else {
		uint32 concurrent_mode = 0;
		if ((!op_mode && strstr(fw_path, "_p2p") != NULL) ||
			(op_mode == DHD_FLAG_P2P_MODE)) {
#if defined(ARP_OFFLOAD_SUPPORT)
			arpoe = 0;
#endif
#ifdef PKT_FILTER_SUPPORT
			dhd_pkt_filter_enable = FALSE;
#endif
			dhd->op_mode = DHD_FLAG_P2P_MODE;
		}
		else
			dhd->op_mode = DHD_FLAG_STA_MODE;
#if !defined(AP) && defined(WLP2P)
		if ((concurrent_mode = dhd_get_concurrent_capabilites(dhd))) {
#if defined(ARP_OFFLOAD_SUPPORT)
			arpoe = 1;
#endif
			dhd->op_mode |= concurrent_mode;
		}

		/* Check if we are enabling p2p */
		if (dhd->op_mode & DHD_FLAG_P2P_MODE) {
			bcm_mkiovar("apsta", (char *)&apsta, 4, iovbuf, sizeof(iovbuf));
			if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR,
				iovbuf, sizeof(iovbuf), TRUE, 0)) < 0) {
				DHD_ERROR(("%s APSTA for P2P failed ret= %d\n", __FUNCTION__, ret));
			}

			memcpy(&p2p_ea, &dhd->mac, ETHER_ADDR_LEN);
			ETHER_SET_LOCALADDR(&p2p_ea);
			bcm_mkiovar("p2p_da_override", (char *)&p2p_ea,
				ETHER_ADDR_LEN, iovbuf, sizeof(iovbuf));
			if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR,
				iovbuf, sizeof(iovbuf), TRUE, 0)) < 0) {
				DHD_ERROR(("%s p2p_da_override ret= %d\n", __FUNCTION__, ret));
			} else {
				DHD_INFO(("dhd_preinit_ioctls: p2p_da_override succeeded\n"));
			}
		}
#else
	(void)concurrent_mode;
#endif 
	}

	DHD_ERROR(("Firmware up: op_mode=0x%04x, "
		"Broadcom Dongle Host Driver mac="MACDBG"\n",
		dhd->op_mode,
		MAC2STRDBG(dhd->mac.octet)));
	/* Set Country code  */
	if (dhd->dhd_cspec.ccode[0] != 0) {
		bcm_mkiovar("country", (char *)&dhd->dhd_cspec,
			sizeof(wl_country_t), iovbuf, sizeof(iovbuf));
		if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
			DHD_ERROR(("%s: country code setting failed\n", __FUNCTION__));
	}

	/* Set Listen Interval */
	bcm_mkiovar("assoc_listen", (char *)&listen_interval, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s assoc_listen failed %d\n", __FUNCTION__, ret));

#if defined(ROAM_ENABLE) || defined(DISABLE_BUILTIN_ROAM)
	/* Disable built-in roaming to allowed ext supplicant to take care of roaming */
	bcm_mkiovar("roam_off", (char *)&roamvar, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* ROAM_ENABLE || DISABLE_BUILTIN_ROAM */
#if defined(ROAM_ENABLE)
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_TRIGGER, roam_trigger,
		sizeof(roam_trigger), TRUE, 0)) < 0)
		DHD_ERROR(("%s: roam trigger set failed %d\n", __FUNCTION__, ret));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_SCAN_PERIOD, roam_scan_period,
		sizeof(roam_scan_period), TRUE, 0)) < 0)
		DHD_ERROR(("%s: roam scan period set failed %d\n", __FUNCTION__, ret));
	if ((dhd_wl_ioctl_cmd(dhd, WLC_SET_ROAM_DELTA, roam_delta,
		sizeof(roam_delta), TRUE, 0)) < 0)
		DHD_ERROR(("%s: roam delta set failed %d\n", __FUNCTION__, ret));
	bcm_mkiovar("fullroamperiod", (char *)&roam_fullscan_period, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s: roam fullscan period set failed %d\n", __FUNCTION__, ret));
#ifndef CUSTOMER_HW10
#ifdef ROAM_AP_ENV_DETECTION
	if (roam_trigger[0] == WL_AUTO_ROAM_TRIGGER) {
		bcm_mkiovar("roam_env_detection", (char *)&roam_env_mode,
			4, iovbuf, sizeof(iovbuf));
		if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) == BCME_OK)
			dhd->roam_env_detection = TRUE;
		else {
			dhd->roam_env_detection = FALSE;
		}
	}
#endif /* ROAM_AP_ENV_DETECTION */
#endif
#endif /* ROAM_ENABLE */

#ifdef WLTDLS
	bcm_mkiovar("tdls_enable", (char *)&tdls, 4, iovbuf, sizeof(iovbuf));
	if ((dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s: tdls enable failed %d\n", __FUNCTION__, ret));

	bcm_mkiovar("tdls_auto_op", (char *)&tdls_auto_op, 4, iovbuf, sizeof(iovbuf));
	if ((dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s: tdls_auto_op failed %d\n", __FUNCTION__, ret));

	bcm_mkiovar("tdls_idle_time", (char *)&tdls_idle_time, 4, iovbuf, sizeof(iovbuf));
	if ((dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s: tdls_idle_time failed %d\n", __FUNCTION__, ret));
#ifdef CUSTOMER_HW4
	bcm_mkiovar("tdls_rssi_high", (char *)&tdls_rssi_high, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	bcm_mkiovar("tdls_rssi_low", (char *)&tdls_rssi_low, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* CUSTOMER_HW4 */
#endif /* WLTDLS */

#ifdef DHD_ENABLE_LPC
	/* Set lpc 1 */
	bcm_mkiovar("lpc", (char *)&lpc, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set lpc failed  %d\n", __FUNCTION__, ret));
	}
#endif /* DHD_ENABLE_LPC */

	/* Set PowerSave mode */
	dhd_wl_ioctl_cmd(dhd, WLC_SET_PM, (char *)&power_mode, sizeof(power_mode), TRUE, 0);

	/* Match Host and Dongle rx alignment */
	bcm_mkiovar("bus:txglomalign", (char *)&dongle_align, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

#if defined(CUSTOMER_HW2) && defined(USE_WL_CREDALL)
	/* enable credall to reduce the chance of no bus credit happened. */
	bcm_mkiovar("bus:credall", (char *)&credall, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif

	if (glom != DEFAULT_GLOM_VALUE) {
		DHD_INFO(("%s set glom=0x%X\n", __FUNCTION__, glom));
		bcm_mkiovar("bus:txglom", (char *)&glom, 4, iovbuf, sizeof(iovbuf));
		dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
	}

	/* Setup timeout if Beacons are lost and roam is off to report link down */
	bcm_mkiovar("bcn_timeout", (char *)&bcn_timeout, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
	/* Setup assoc_retry_max count to reconnect target AP in dongle */
	bcm_mkiovar("assoc_retry_max", (char *)&retry_max, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#if defined(AP) && !defined(WLP2P)
	/* Turn off MPC in AP mode */
	bcm_mkiovar("mpc", (char *)&mpc, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
	bcm_mkiovar("apsta", (char *)&apsta, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* defined(AP) && !defined(WLP2P) */


#if defined(SOFTAP)
	if (ap_fw_loaded == TRUE) {
		dhd_wl_ioctl_cmd(dhd, WLC_SET_DTIMPRD, (char *)&dtim, sizeof(dtim), TRUE, 0);
	}
#endif 

#if defined(KEEP_ALIVE)
	{
	/* Set Keep Alive : be sure to use FW with -keepalive */
	int res;

#if defined(SOFTAP)
	if (ap_fw_loaded == FALSE)
#endif 
		if (!(dhd->op_mode & DHD_FLAG_HOSTAP_MODE)) {
			if ((res = dhd_keep_alive_onoff(dhd)) < 0)
				DHD_ERROR(("%s set keeplive failed %d\n",
				__FUNCTION__, res));
		}
	}
#endif /* defined(KEEP_ALIVE) */
#ifdef USE_WL_TXBF
	bcm_mkiovar("txbf", (char *)&txbf, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set txbf failed  %d\n", __FUNCTION__, ret));
	}
#endif /* USE_WL_TXBF */
#ifdef USE_WL_FRAMEBURST
	/* Set frameburst to value */
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_FAKEFRAG, (char *)&frameburst,
		sizeof(frameburst), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set frameburst failed  %d\n", __FUNCTION__, ret));
	}
#endif /* USE_WL_FRAMEBURST */
#ifdef DHD_SET_FW_HIGHSPEED
	/* Set ack_ratio */
	bcm_mkiovar("ack_ratio", (char *)&ack_ratio, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set ack_ratio failed  %d\n", __FUNCTION__, ret));
	}

	/* Set ack_ratio_depth */
	bcm_mkiovar("ack_ratio_depth", (char *)&ack_ratio_depth, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set ack_ratio_depth failed  %d\n", __FUNCTION__, ret));
	}
#endif /* DHD_SET_FW_HIGHSPEED */
#ifdef CUSTOM_AMPDU_BA_WSIZE
	/* Set ampdu ba wsize to 64 */
	bcm_mkiovar("ampdu_ba_wsize", (char *)&ampdu_ba_wsize, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set ampdu_ba_wsize to %d failed  %d\n",
			__FUNCTION__, CUSTOM_AMPDU_BA_WSIZE, ret));
	}
#endif /* CUSTOM_AMPDU_BA_WSIZE */
#if defined(BCMSUP_4WAY_HANDSHAKE) && defined(WLAN_AKM_SUITE_FT_8021X)
	/* Read 4-way handshake requirements */
	if (dhd_use_idsup == 1) {
		bcm_mkiovar("sup_wpa", (char *)&sup_wpa, 4, iovbuf, sizeof(iovbuf));
		ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0);
		/* sup_wpa iovar returns NOTREADY status on some platforms using modularized
		 * in-dongle supplicant.
		 */
		if (ret >= 0 || ret == BCME_NOTREADY)
			dhd->fw_4way_handshake = TRUE;
		DHD_TRACE(("4-way handshake mode is: %d\n", dhd->fw_4way_handshake));
	}
#endif /* BCMSUP_4WAY_HANDSHAKE && WLAN_AKM_SUITE_FT_8021X */

	bcm_mkiovar("buf_key_b4_m4", (char *)&buf_key_b4_m4, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf,
		sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s buf_key_b4_m4 set failed %d\n", __FUNCTION__, ret));
	}

	/* Read event_msgs mask */
	bcm_mkiovar("event_msgs", eventmask, WL_EVENTING_MASK_LEN, iovbuf, sizeof(iovbuf));
	if ((ret  = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0)) < 0) {
		DHD_ERROR(("%s read Event mask failed %d\n", __FUNCTION__, ret));
		goto done;
	}
	bcopy(iovbuf, eventmask, WL_EVENTING_MASK_LEN);

	/* Setup event_msgs */
	setbit(eventmask, WLC_E_SET_SSID);
	setbit(eventmask, WLC_E_PRUNE);
	setbit(eventmask, WLC_E_AUTH);
	setbit(eventmask, WLC_E_ASSOC);
	setbit(eventmask, WLC_E_REASSOC);
	setbit(eventmask, WLC_E_REASSOC_IND);
	setbit(eventmask, WLC_E_DEAUTH);
	setbit(eventmask, WLC_E_DEAUTH_IND);
	setbit(eventmask, WLC_E_DISASSOC_IND);
	setbit(eventmask, WLC_E_DISASSOC);
	setbit(eventmask, WLC_E_JOIN);
	setbit(eventmask, WLC_E_START);
	setbit(eventmask, WLC_E_ASSOC_IND);
	setbit(eventmask, WLC_E_PSK_SUP);
	setbit(eventmask, WLC_E_LINK);
	setbit(eventmask, WLC_E_NDIS_LINK);
	setbit(eventmask, WLC_E_MIC_ERROR);
	setbit(eventmask, WLC_E_ASSOC_REQ_IE);
	setbit(eventmask, WLC_E_ASSOC_RESP_IE);
#ifndef WL_CFG80211
	setbit(eventmask, WLC_E_PMKID_CACHE);
	setbit(eventmask, WLC_E_TXFAIL);
#endif
	setbit(eventmask, WLC_E_JOIN_START);
	setbit(eventmask, WLC_E_SCAN_COMPLETE);
#ifdef WLMEDIA_HTSF
	setbit(eventmask, WLC_E_HTSFSYNC);
#endif /* WLMEDIA_HTSF */
#ifdef PNO_SUPPORT
	setbit(eventmask, WLC_E_PFN_NET_FOUND);
#endif /* PNO_SUPPORT */
	/* enable dongle roaming event */
	setbit(eventmask, WLC_E_ROAM);
	setbit(eventmask, WLC_E_BSSID);
#ifdef BCMCCX
	setbit(eventmask, WLC_E_ADDTS_IND);
	setbit(eventmask, WLC_E_DELTS_IND);
#endif /* BCMCCX */
#ifdef WLTDLS
	setbit(eventmask, WLC_E_TDLS_PEER_EVENT);
#endif /* WLTDLS */
#ifdef WL_CFG80211
	setbit(eventmask, WLC_E_ESCAN_RESULT);
	if (dhd->op_mode & DHD_FLAG_P2P_MODE) {
		setbit(eventmask, WLC_E_ACTION_FRAME_RX);
		setbit(eventmask, WLC_E_P2P_DISC_LISTEN_COMPLETE);
	}
#if defined(CUSTOMER_HW4) && defined(WES_SUPPORT)
	else {
		setbit(eventmask, WLC_E_ACTION_FRAME_RX);
	}
#endif /* WES_SUPPORT */
#endif /* WL_CFG80211 */

	/* Write updated Event mask */
	bcm_mkiovar("event_msgs", eventmask, WL_EVENTING_MASK_LEN, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0) {
		DHD_ERROR(("%s Set Event mask failed %d\n", __FUNCTION__, ret));
		goto done;
	}

	dhd_wl_ioctl_cmd(dhd, WLC_SET_SCAN_CHANNEL_TIME, (char *)&scan_assoc_time,
		sizeof(scan_assoc_time), TRUE, 0);
	dhd_wl_ioctl_cmd(dhd, WLC_SET_SCAN_UNASSOC_TIME, (char *)&scan_unassoc_time,
		sizeof(scan_unassoc_time), TRUE, 0);
	dhd_wl_ioctl_cmd(dhd, WLC_SET_SCAN_PASSIVE_TIME, (char *)&scan_passive_time,
		sizeof(scan_passive_time), TRUE, 0);

#ifdef ARP_OFFLOAD_SUPPORT
	/* Set and enable ARP offload feature for STA only  */
#if defined(SOFTAP)
	if (arpoe && !ap_fw_loaded) {
#else
	if (arpoe) {
#endif 
		dhd_arp_offload_enable(dhd, TRUE);
		dhd_arp_offload_set(dhd, dhd_arp_mode);
	} else {
		dhd_arp_offload_enable(dhd, FALSE);
		dhd_arp_offload_set(dhd, 0);
	}
	dhd_arp_enable = arpoe;
#endif /* ARP_OFFLOAD_SUPPORT */

#ifdef PKT_FILTER_SUPPORT
	/* Setup default defintions for pktfilter , enable in suspend */
	dhd->pktfilter_count = 5;
	dhd->pktfilter[1] = NULL;
	dhd->pktfilter[2] = NULL;
	dhd->pktfilter[3] = NULL;
	/* Add filter to pass multicastDNS packet and NOT filter out as Broadcast */
	dhd->pktfilter[4] = "104 0 0 0 0xFFFFFFFFFFFF 0x01005E0000FB";

#if defined(CUSTOMER_HW4) && defined(GAN_LITE_NAT_KEEPALIVE_FILTER)
	dhd->pktfilter_count = 4;
	/* Setup filter to block broadcast and NAT Keepalive packets */
	dhd->pktfilter[0] = "100 0 0 0 0xffffff 0xffffff"; /* discard all broadcast packets */
	dhd->pktfilter[1] = "102 0 0 36 0xffffffff 0x11940009"; /* discard NAT Keepalive packets */
	dhd->pktfilter[2] = "104 0 0 38 0xffffffff 0x11940009"; /* discard NAT Keepalive packets */
	dhd->pktfilter[3] = NULL;
#else
	/* Setup filter to allow only unicast */
#if defined(BLOCK_IPV6_PACKET) && defined(CUSTOMER_HW4)
	dhd->pktfilter[0] = "100 0 0 0 "
		HEX_PREF_STR UNI_FILTER_STR ZERO_ADDR_STR ETHER_TYPE_STR IPV6_FILTER_STR
		" "
		HEX_PREF_STR ZERO_ADDR_STR ZERO_ADDR_STR ETHER_TYPE_STR ZERO_TYPE_STR;
#else
	dhd->pktfilter[0] = "100 0 0 0 0x01 0x00";
#endif /* BLOCK_IPV6_PACKET && CUSTOMER_HW4 */
#if defined(PASS_IPV4_SUSPEND) && defined(CUSTOMER_HW4)
	dhd->pktfilter_count = 5;
	dhd->pktfilter[4] = "104 0 0 0 0xFFFFFF 0x01005E";
#endif /* PASS_IPV4_SUSPEND && CUSTOMER_HW4 */
#ifdef PASS_ARP_PACKET
	dhd->pktfilter[dhd->pktfilter_count++] = "105 0 0 12 0xFFFF 0x0806";
#endif
#endif /* GAN_LITE_NAT_KEEPALIVE_FILTER  && CUSTOMER_HW4 */

#if defined(SOFTAP)
	if (ap_fw_loaded) {
		dhd_enable_packet_filter(0, dhd);
	}
#endif /* defined(SOFTAP) */
	dhd_set_packet_filter(dhd);
#endif /* PKT_FILTER_SUPPORT */
#ifdef DISABLE_11N
	bcm_mkiovar("nmode", (char *)&nmode, 4, iovbuf, sizeof(iovbuf));
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0)) < 0)
		DHD_ERROR(("%s wl nmode 0 failed %d\n", __FUNCTION__, ret));
#else
#if defined(PROP_TXSTATUS) && defined(PROP_TXSTATUS_VSDB)
	bcm_mkiovar("ampdu_hostreorder", (char *)&hostreorder, 4, buf, sizeof(buf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, buf, sizeof(buf), TRUE, 0);
#endif 
#endif /* DISABLE_11N */

#if defined(VSDB) && defined(CUSTOMER_HW4)
	dhd_wl_ioctl_cmd(dhd, WLC_SET_INTERFERENCE_MODE,
		(int *)&interference_mode, sizeof(int), TRUE, 0);
#endif

#if defined(CUSTOMER_HW4) && defined(ENABLE_BCN_LI_BCN_WAKEUP)
	bcm_mkiovar("bcn_li_bcn", (char *)&bcn_li_bcn, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* CUSTOMER_HW4 && ENABLE_BCN_LI_BCN_WAKEUP */

#if defined(CONFIG_LGE_BCM433X_PATCH)	/* LGE_patch : config file setting */
	dhd_preinit_config(dhd, 0);
#endif /* CONFIG_LGE_BCM433X_PATCH */

	/* query for 'ver' to get version info from firmware */
	memset(buf, 0, sizeof(buf));
	ptr = buf;
	bcm_mkiovar("ver", (char *)&buf, 4, buf, sizeof(buf));
	if ((ret  = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, buf, sizeof(buf), FALSE, 0)) < 0)
		DHD_ERROR(("%s failed %d\n", __FUNCTION__, ret));
	else {
		bcmstrtok(&ptr, "\n", 0);
		/* Print fw version info */
		DHD_ERROR(("Firmware version = %s\n", buf));
		dhd_set_version_info(dhd, buf);

		/* Check and adjust IOCTL response timeout for Manufactring firmware */
		if (strstr(buf, MANUFACTRING_FW) != NULL) {
			dhd_os_set_ioctl_resp_timeout(20000);
			DHD_ERROR(("%s : adjust IOCTL response time for Manufactring Firmware\n",
			__FUNCTION__));
		}
	}

#if defined(PROP_TXSTATUS) && !defined(PROP_TXSTATUS_VSDB)
	dhd_wlfc_init(dhd);
#endif /* PROP_TXSTATUS && !PROP_TXSTATUS_VSDB */

done:
	return ret;
}


int
dhd_iovar(dhd_pub_t *pub, int ifidx, char *name, char *cmd_buf, uint cmd_len, int set)
{
	char buf[strlen(name) + 1 + cmd_len];
	int len = sizeof(buf);
	wl_ioctl_t ioc;
	int ret;

	len = bcm_mkiovar(name, cmd_buf, cmd_len, buf, len);

	memset(&ioc, 0, sizeof(ioc));

	ioc.cmd = set? WLC_SET_VAR : WLC_GET_VAR;
	ioc.buf = buf;
	ioc.len = len;
	ioc.set = TRUE;

	ret = dhd_wl_ioctl(pub, ifidx, &ioc, ioc.buf, ioc.len);
	if (!set && ret >= 0)
		memcpy(cmd_buf, buf, cmd_len);

	return ret;
}

int dhd_change_mtu(dhd_pub_t *dhdp, int new_mtu, int ifidx)
{
	struct dhd_info *dhd = dhdp->info;
	struct net_device *dev = NULL;

	ASSERT(dhd && dhd->iflist[ifidx]);
	dev = dhd->iflist[ifidx]->net;
	ASSERT(dev);

	if (netif_running(dev)) {
		DHD_ERROR(("%s: Must be down to change its MTU", dev->name));
		return BCME_NOTDOWN;
	}

#define DHD_MIN_MTU 1500
#define DHD_MAX_MTU 1752

	if ((new_mtu < DHD_MIN_MTU) || (new_mtu > DHD_MAX_MTU)) {
		DHD_ERROR(("%s: MTU size %d is invalid.\n", __FUNCTION__, new_mtu));
		return BCME_BADARG;
	}

	dev->mtu = new_mtu;
	return 0;
}

#ifdef ARP_OFFLOAD_SUPPORT
/* add or remove AOE host ip(s) (up to 8 IPs on the interface)  */
void
aoe_update_host_ipv4_table(dhd_pub_t *dhd_pub, u32 ipa, bool add, int idx)
{
	u32 ipv4_buf[MAX_IPV4_ENTRIES]; /* temp save for AOE host_ip table */
	int i;
	int ret;

	bzero(ipv4_buf, sizeof(ipv4_buf));

	/* display what we've got */
	ret = dhd_arp_get_arp_hostip_table(dhd_pub, ipv4_buf, sizeof(ipv4_buf), idx);
	DHD_ARPOE(("%s: hostip table read from Dongle:\n", __FUNCTION__));
#ifdef AOE_DBG
	dhd_print_buf(ipv4_buf, 32, 4); /* max 8 IPs 4b each */
#endif
	/* now we saved hoste_ip table, clr it in the dongle AOE */
	dhd_aoe_hostip_clr(dhd_pub, idx);

	if (ret) {
		DHD_ERROR(("%s failed\n", __FUNCTION__));
		return;
	}

	for (i = 0; i < MAX_IPV4_ENTRIES; i++) {
		if (add && (ipv4_buf[i] == 0)) {
				ipv4_buf[i] = ipa;
				add = FALSE; /* added ipa to local table  */
				DHD_ARPOE(("%s: Saved new IP in temp arp_hostip[%d]\n",
				__FUNCTION__, i));
		} else if (ipv4_buf[i] == ipa) {
			ipv4_buf[i]	= 0;
			DHD_ARPOE(("%s: removed IP:%x from temp table %d\n",
				__FUNCTION__, ipa, i));
		}

		if (ipv4_buf[i] != 0) {
			/* add back host_ip entries from our local cache */
			dhd_arp_offload_add_ip(dhd_pub, ipv4_buf[i], idx);
			DHD_ARPOE(("%s: added IP:%x to dongle arp_hostip[%d]\n\n",
				__FUNCTION__, ipv4_buf[i], i));
		}
	}
#ifdef AOE_DBG
	/* see the resulting hostip table */
	dhd_arp_get_arp_hostip_table(dhd_pub, ipv4_buf, sizeof(ipv4_buf), idx);
	DHD_ARPOE(("%s: read back arp_hostip table:\n", __FUNCTION__));
	dhd_print_buf(ipv4_buf, 32, 4); /* max 8 IPs 4b each */
#endif
}

/*
 * Notification mechanism from kernel to our driver. This function is called by the Linux kernel
 * whenever there is an event related to an IP address.
 * ptr : kernel provided pointer to IP address that has changed
 */
static int dhd_device_event(struct notifier_block *this,
	unsigned long event,
	void *ptr)
{
	struct in_ifaddr *ifa = (struct in_ifaddr *)ptr;

	dhd_info_t *dhd;
	dhd_pub_t *dhd_pub;
	int idx;

	if (!dhd_arp_enable)
		return NOTIFY_DONE;
	if (!ifa || !(ifa->ifa_dev->dev))
		return NOTIFY_DONE;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31))
	/* Filter notifications meant for non Broadcom devices */
	if ((ifa->ifa_dev->dev->netdev_ops != &dhd_ops_pri) &&
	    (ifa->ifa_dev->dev->netdev_ops != &dhd_ops_virt)) {
#if defined(WL_ENABLE_P2P_IF)
		if (!wl_cfgp2p_is_ifops(ifa->ifa_dev->dev->netdev_ops))
#endif /* WL_ENABLE_P2P_IF */
			return NOTIFY_DONE;
	}
#endif /* LINUX_VERSION_CODE */

	dhd = *(dhd_info_t **)netdev_priv(ifa->ifa_dev->dev);
	if (!dhd)
		return NOTIFY_DONE;

	dhd_pub = &dhd->pub;

	if (dhd_pub->arp_version == 1) {
		idx = 0;
	}
	else {
		for (idx = 0; idx < DHD_MAX_IFS; idx++) {
			if (dhd->iflist[idx] && dhd->iflist[idx]->net == ifa->ifa_dev->dev)
			break;
		}
		if (idx < DHD_MAX_IFS)
			DHD_TRACE(("ifidx : %p %s %d\n", dhd->iflist[idx]->net,
				dhd->iflist[idx]->name, dhd->iflist[idx]->idx));
		else {
			DHD_ERROR(("Cannot find ifidx for(%s) set to 0\n", ifa->ifa_label));
			idx = 0;
		}
	}

	switch (event) {
		case NETDEV_UP:
			DHD_ARPOE(("%s: [%s] Up IP: 0x%x\n",
				__FUNCTION__, ifa->ifa_label, ifa->ifa_address));

			if (dhd->pub.busstate != DHD_BUS_DATA) {
				DHD_ERROR(("%s: bus not ready, exit\n", __FUNCTION__));
				if (dhd->pend_ipaddr) {
					DHD_ERROR(("%s: overwrite pending ipaddr: 0x%x\n",
						__FUNCTION__, dhd->pend_ipaddr));
				}
				dhd->pend_ipaddr = ifa->ifa_address;
				break;
			}

#ifdef AOE_IP_ALIAS_SUPPORT
			DHD_ARPOE(("%s:add aliased IP to AOE hostip cache\n",
				__FUNCTION__));
			aoe_update_host_ipv4_table(dhd_pub, ifa->ifa_address, TRUE, idx);
#endif /* AOE_IP_ALIAS_SUPPORT */
			break;

		case NETDEV_DOWN:
			DHD_ARPOE(("%s: [%s] Down IP: 0x%x\n",
				__FUNCTION__, ifa->ifa_label, ifa->ifa_address));
			dhd->pend_ipaddr = 0;
#ifdef AOE_IP_ALIAS_SUPPORT
			DHD_ARPOE(("%s:interface is down, AOE clr all for this if\n",
				__FUNCTION__));
			aoe_update_host_ipv4_table(dhd_pub, ifa->ifa_address, FALSE, idx);
#else
			dhd_aoe_hostip_clr(&dhd->pub, idx);
			dhd_aoe_arp_clr(&dhd->pub, idx);
#endif /* AOE_IP_ALIAS_SUPPORT */
			break;

		default:
			DHD_ARPOE(("%s: do noting for [%s] Event: %lu\n",
				__func__, ifa->ifa_label, event));
			break;
	}
	return NOTIFY_DONE;
}
#endif /* ARP_OFFLOAD_SUPPORT */

int
dhd_register_if(dhd_pub_t *dhdp, int ifidx, bool need_rtnl_lock)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;
	struct net_device *net = NULL;
	int err = 0;
	uint8 temp_addr[ETHER_ADDR_LEN] = { 0x00, 0x90, 0x4c, 0x11, 0x22, 0x33 };

	DHD_TRACE(("%s: ifidx %d\n", __FUNCTION__, ifidx));

	ASSERT(dhd && dhd->iflist[ifidx]);
	net = dhd->iflist[ifidx]->net;
	ASSERT(net);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31))
	ASSERT(!net->open);
	net->get_stats = dhd_get_stats;
	net->do_ioctl = dhd_ioctl_entry;
	net->hard_start_xmit = dhd_start_xmit;
	net->set_mac_address = dhd_set_mac_address;
	net->set_multicast_list = dhd_set_multicast_list;
	net->open = net->stop = NULL;
#else
	ASSERT(!net->netdev_ops);
	net->netdev_ops = &dhd_ops_virt;
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31) */

	/* Ok, link into the network layer... */
	if (ifidx == 0) {
		/*
		 * device functions for the primary interface only
		 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31))
		net->open = dhd_open;
		net->stop = dhd_stop;
#else
		net->netdev_ops = &dhd_ops_pri;
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31) */
		if (!ETHER_ISNULLADDR(dhd->pub.mac.octet))
			memcpy(temp_addr, dhd->pub.mac.octet, ETHER_ADDR_LEN);
	} else {
		/*
		 * We have to use the primary MAC for virtual interfaces
		 */
		memcpy(temp_addr, dhd->iflist[ifidx]->mac_addr, ETHER_ADDR_LEN);
		/*
		 * Android sets the locally administered bit to indicate that this is a
		 * portable hotspot.  This will not work in simultaneous AP/STA mode,
		 * nor with P2P.  Need to set the Donlge's MAC address, and then use that.
		 */
		if (!memcmp(temp_addr, dhd->iflist[0]->mac_addr,
			ETHER_ADDR_LEN)) {
			DHD_ERROR(("%s interface [%s]: set locally administered bit in MAC\n",
			__func__, net->name));
			temp_addr[0] |= 0x02;
		}
	}

	net->hard_header_len = ETH_HLEN + dhd->pub.hdrlen;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
	net->ethtool_ops = &dhd_ethtool_ops;
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24) */

#if defined(WL_WIRELESS_EXT)
#if WIRELESS_EXT < 19
	net->get_wireless_stats = dhd_get_wireless_stats;
#endif /* WIRELESS_EXT < 19 */
#if WIRELESS_EXT > 12
	net->wireless_handlers = (struct iw_handler_def *)&wl_iw_handler_def;
#endif /* WIRELESS_EXT > 12 */
#endif /* defined(WL_WIRELESS_EXT) */

	dhd->pub.rxsz = DBUS_RX_BUFFER_SIZE_DHD(net);

	memcpy(net->dev_addr, temp_addr, ETHER_ADDR_LEN);

	if (ifidx == 0)
		printf("%s\n", dhd_version);

	if (need_rtnl_lock)
		err = register_netdev(net);
	else
		err = register_netdevice(net);

	if (err != 0) {
		DHD_ERROR(("couldn't register the net device [%s], err %d\n", net->name, err));
		goto fail;
	}

	printf("Register interface [%s]  MAC: "MACDBG"\n\n", net->name,
#if defined(CUSTOMER_HW4)
		MAC2STRDBG(dhd->pub.mac.octet));
#else
		MAC2STRDBG(net->dev_addr));
#endif /* CUSTOMER_HW4 */

#if defined(SOFTAP) && defined(WL_WIRELESS_EXT) && !defined(WL_CFG80211)
		wl_iw_iscan_set_scan_broadcast_prep(net, 1);
#endif

#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	if (ifidx == 0) {
		dhd_registration_check = TRUE;
		up(&dhd_registration_sem);
	}
#endif 
	return 0;

fail:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31)
	net->open = NULL;
#else
	net->netdev_ops = NULL;
#endif
	return err;
}

void
dhd_bus_detach(dhd_pub_t *dhdp)
{
	dhd_info_t *dhd;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (dhdp) {
		dhd = (dhd_info_t *)dhdp->info;
		if (dhd) {

			/*
			 * In case of Android cfg80211 driver, the bus is down in dhd_stop,
			 *  calling stop again will cuase SD read/write errors.
			 */
			if (dhd->pub.busstate != DHD_BUS_DOWN) {
				/* Stop the protocol module */
				dhd_prot_stop(&dhd->pub);

				/* Stop the bus module */
				dhd_bus_stop(dhd->pub.bus, TRUE);
			}

#if defined(OOB_INTR_ONLY)
			bcmsdh_unregister_oob_intr();
#endif 
		}
	}
}


void dhd_detach(dhd_pub_t *dhdp)
{
	dhd_info_t *dhd;
	unsigned long flags;
	int timer_valid = FALSE;
	struct list_head *list_cur, *list_temp;

	if (!dhdp)
		return;

	dhd = (dhd_info_t *)dhdp->info;
	if (!dhd)
		return;

	DHD_TRACE(("%s: Enter state 0x%x\n", __FUNCTION__, dhd->dhd_state));

	dhd->pub.up = 0;
	if (!(dhd->dhd_state & DHD_ATTACH_STATE_DONE)) {
		/* Give sufficient time for threads to start running in case
		 * dhd_attach() has failed
		 */
		osl_delay(1000*100);
	}

	if (dhd->dhd_state & DHD_ATTACH_STATE_PROT_ATTACH) {
		dhd_bus_detach(dhdp);

		if (dhdp->prot)
			dhd_prot_detach(dhdp);
	}

#ifdef ARP_OFFLOAD_SUPPORT
	unregister_inetaddr_notifier(&dhd_notifier);
#endif /* ARP_OFFLOAD_SUPPORT */

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
	if (dhd->dhd_state & DHD_ATTACH_STATE_EARLYSUSPEND_DONE) {
		if (dhd->early_suspend.suspend)
			unregister_early_suspend(&dhd->early_suspend);
	}
#endif /* CONFIG_HAS_EARLYSUSPEND && DHD_USE_EARLYSUSPEND */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	cancel_work_sync(&dhd->work_hang);
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))  */

#if defined(WL_WIRELESS_EXT)
	if (dhd->dhd_state & DHD_ATTACH_STATE_WL_ATTACH) {
		/* Detatch and unlink in the iw */
		wl_iw_detach();
	}
#endif /* defined(WL_WIRELESS_EXT) */

	cancel_work_sync(&dhd->sysioc_work);
	spin_lock_bh(&dhd->if_event_list_lock);
	list_for_each_safe(list_cur, list_temp,  &dhd->if_event_list) {
		dhd_if_event_t *if_event = list_entry(list_cur, dhd_if_event_t, list);
		list_del(list_cur);
		MFREE(dhd->pub.osh, if_event, sizeof(dhd_if_event_t));
	}
	spin_unlock_bh(&dhd->if_event_list_lock);

	/* delete all interfaces, start with virtual  */
	if (dhd->dhd_state & DHD_ATTACH_STATE_ADD_IF) {
		int i = 1;
		dhd_if_t *ifp;

		/* Cleanup virtual interfaces */
		dhd_net_if_lock_local(dhd);
		for (i = 1; i < DHD_MAX_IFS; i++) {
			if (dhd->iflist[i])
				dhd_remove_if(&dhd->pub, i, TRUE);
		}
		dhd_net_if_unlock_local(dhd);

		/*  delete primary interface 0 */
		ifp = dhd->iflist[0];
		ASSERT(ifp);
		ASSERT(ifp->net);
		if (ifp && ifp->net) {
			/* in unregister_netdev case, the interface gets freed by net->destructor
			 * (which is set to free_netdev)
			 */
			if (ifp->net->reg_state == NETREG_UNINITIALIZED)
				free_netdev(ifp->net);
			else
				unregister_netdev(ifp->net);
			ifp->net = NULL;
			MFREE(dhd->pub.osh, ifp, sizeof(*ifp));
			dhd->iflist[0] = NULL;
		}
	}

	/* Clear the watchdog timer */
	flags = dhd_os_spin_lock(&dhd->pub);
	timer_valid = dhd->wd_timer_valid;
	dhd->wd_timer_valid = FALSE;
	dhd_os_spin_unlock(&dhd->pub, flags);
	if (timer_valid)
		del_timer_sync(&dhd->timer);

	if (dhd->dhd_state & DHD_ATTACH_STATE_THREADS_CREATED) {
		if (dhd->thr_wdt_ctl.thr_pid >= 0) {
			PROC_STOP(&dhd->thr_wdt_ctl);
		}

		if (dhd->rxthread_enabled && dhd->thr_rxf_ctl.thr_pid >= 0) {
			PROC_STOP(&dhd->thr_rxf_ctl);
		}

		if (dhd->thr_dpc_ctl.thr_pid >= 0) {
			PROC_STOP(&dhd->thr_dpc_ctl);
		} else
			tasklet_kill(&dhd->tasklet);
	}
#ifdef WL_CFG80211
	if (dhd->dhd_state & DHD_ATTACH_STATE_CFG80211) {
		wl_cfg80211_detach(NULL);
		dhd_monitor_uninit();
	}
#endif


#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP)
		unregister_pm_notifier(&dhd_sleep_pm_notifier);
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP) */
	/* && defined(CONFIG_PM_SLEEP) */

	if (dhd->dhd_state & DHD_ATTACH_STATE_WAKELOCKS_INIT) {
		DHD_TRACE(("wd wakelock count:%d\n", dhd->wakelock_wd_counter));
#ifdef CONFIG_HAS_WAKELOCK
		dhd->wakelock_counter = 0;
		dhd->wakelock_wd_counter = 0;
		dhd->wakelock_rx_timeout_enable = 0;
		dhd->wakelock_ctrl_timeout_enable = 0;
		wake_lock_destroy(&dhd->wl_wifi);
		wake_lock_destroy(&dhd->wl_rxwake);
		wake_lock_destroy(&dhd->wl_ctrlwake);
		wake_lock_destroy(&dhd->wl_wdwake);
#endif /* CONFIG_HAS_WAKELOCK */
	}
}


void
dhd_free(dhd_pub_t *dhdp)
{
	dhd_info_t *dhd;
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (dhdp) {
		int i;
		for (i = 0; i < ARRAYSIZE(dhdp->reorder_bufs); i++) {
			if (dhdp->reorder_bufs[i]) {
				reorder_info_t *ptr;
				uint32 buf_size = sizeof(struct reorder_info);

				ptr = dhdp->reorder_bufs[i];

				buf_size += ((ptr->max_idx + 1) * sizeof(void*));
				DHD_REORDER(("free flow id buf %d, maxidx is %d, buf_size %d\n",
					i, ptr->max_idx, buf_size));

				MFREE(dhdp->osh, dhdp->reorder_bufs[i], buf_size);
				dhdp->reorder_bufs[i] = NULL;
			}
		}
		dhd = (dhd_info_t *)dhdp->info;
#if defined(CONFIG_DHD_USE_STATIC_BUF)
		/* If pointer is allocated by dhd_os_prealloc then avoid MFREE */
		if (dhd != (dhd_info_t *)dhd_os_prealloc(NULL, DHD_PREALLOC_DHD_INFO, 0)) {
#endif /* CONFIG_DHD_USE_STATIC_BUF */
			if (dhd)
				MFREE(dhd->pub.osh, dhd, sizeof(*dhd));
#if defined(CONFIG_DHD_USE_STATIC_BUF)
		}
		else {
			if (dhd)
				dhd = NULL;
		}
#endif /* CONFIG_DHD_USE_STATIC_BUF */
	}
}

static void __exit
dhd_module_cleanup(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	dhd_bus_unregister();

#if defined(CONFIG_WIFI_CONTROL_FUNC)
	wl_android_wifictrl_func_del();
#endif /* CONFIG_WIFI_CONTROL_FUNC */
	wl_android_exit();

	/* Call customer gpio to turn off power with WL_REG_ON signal */
	dhd_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);
}


#if defined(CONFIG_WIFI_CONTROL_FUNC)
extern bool g_wifi_poweron;
#endif /* CONFIG_WIFI_CONTROL_FUNC */

static int __init
dhd_module_init(void)
{
	int error = 0;
	
#if !defined(CUSTOMER_HW10) && 1 && defined(BCMLXSDMMC) && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	int retry = POWERUP_MAX_RETRY;
	int chip_up = 0;
#endif 

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	wl_android_init();

	/* Sanity check on the module parameters */
	do {
		/* Both watchdog and DPC as tasklets are ok */
		if ((dhd_watchdog_prio < 0) && (dhd_dpc_prio < 0))
			break;

		/* If both watchdog and DPC are threads, TX must be deferred */
		if ((dhd_watchdog_prio >= 0) && (dhd_dpc_prio >= 0) && dhd_deferred_tx)
			break;

		DHD_ERROR(("Invalid module parameters.\n"));
		error = -EINVAL;
	} while (0);

	if (error)
		goto fail_0;
		
#if !defined(CUSTOMER_HW10) && 1 && defined(BCMLXSDMMC) && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	do {
		sema_init(&dhd_chipup_sem, 0);
		dhd_bus_reg_sdio_notify(&dhd_chipup_sem);
		dhd_customer_gpio_wlan_ctrl(WLAN_POWER_ON);
#if defined(CONFIG_WIFI_CONTROL_FUNC)
		if (wl_android_wifictrl_func_add() < 0) {
			dhd_bus_unreg_sdio_notify();
			goto fail_1;
		}
#endif /* defined(CONFIG_WIFI_CONTROL_FUNC) */
		if (down_timeout(&dhd_chipup_sem,
			msecs_to_jiffies(POWERUP_WAIT_MS)) == 0) {
			dhd_bus_unreg_sdio_notify();
			chip_up = 1;
			break;
		}
		DHD_ERROR(("\nfailed to power up wifi chip, retry again (%d left) **\n\n",
			retry+1));
		dhd_bus_unreg_sdio_notify();
#if defined(CONFIG_WIFI_CONTROL_FUNC)
		wl_android_wifictrl_func_del();
#endif /* defined(CONFIG_WIFI_CONTROL_FUNC) */
		dhd_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);
	} while (retry-- > 0);

	if (!chip_up) {
		DHD_ERROR(("\nfailed to power up wifi chip, max retry reached, exits **\n\n"));
		error = -ENODEV;
		goto fail_0;
	}
#else
	dhd_customer_gpio_wlan_ctrl(WLAN_POWER_ON);
#if defined(CONFIG_WIFI_CONTROL_FUNC)
	if (wl_android_wifictrl_func_add() < 0)
		goto fail_1;
#endif /* defined(CONFIG_WIFI_CONTROL_FUNC) */

#if defined(CONFIG_WIFI_CONTROL_FUNC) && defined(BCMLXSDMMC)
	/* If the wifi_set_power() is failed,
	 * we need to jump error handling routines.
	 */
	if (!g_wifi_poweron) {
		DHD_ERROR(("%s: wifi_set_power() failed\n", __FUNCTION__));
		error = -ENODEV;
		goto fail_1;
	}
#endif /* defined(CONFIG_WIFI_CONTROL_FUNC) */

#endif 

#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	sema_init(&dhd_registration_sem, 0);
#endif 


	error = dhd_bus_register();

	if (error) {
		DHD_ERROR(("%s: sdio_register_driver failed\n", __FUNCTION__));
		goto fail_1;
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(BCMLXSDMMC)
	/*
	 * Wait till MMC sdio_register_driver callback called and made driver attach.
	 * It's needed to make sync up exit from dhd insmod  and
	 * Kernel MMC sdio device callback registration
	 */
	if ((down_timeout(&dhd_registration_sem,
		msecs_to_jiffies(DHD_REGISTRATION_TIMEOUT)) != 0) ||
		(dhd_registration_check != TRUE)) {
		error = -ENODEV;
		DHD_ERROR(("%s: sdio_register_driver timeout or error \n", __FUNCTION__));
		goto fail_2;
	}
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) */
#if defined(WL_CFG80211)
	wl_android_post_init();
#endif /* defined(WL_CFG80211) */

	return error;

#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(BCMLXSDMMC)
fail_2:
	dhd_bus_unregister();
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) */

fail_1:

#if defined(CONFIG_WIFI_CONTROL_FUNC)
	wl_android_wifictrl_func_del();
#endif 

	/* Call customer gpio to turn off power with WL_REG_ON signal */
	dhd_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);

fail_0:

	wl_android_exit();

	return error;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
late_initcall(dhd_module_init);
#else
module_init(dhd_module_init);
#endif

module_exit(dhd_module_cleanup);

/*
 * OS specific functions required to implement DHD driver in OS independent way
 */
int
dhd_os_proto_block(dhd_pub_t *pub)
{
	dhd_info_t * dhd = (dhd_info_t *)(pub->info);

	if (dhd) {
		down(&dhd->proto_sem);
		return 1;
	}

	return 0;
}

int
dhd_os_proto_unblock(dhd_pub_t *pub)
{
	dhd_info_t * dhd = (dhd_info_t *)(pub->info);

	if (dhd) {
		up(&dhd->proto_sem);
		return 1;
	}

	return 0;
}

unsigned int
dhd_os_get_ioctl_resp_timeout(void)
{
	return ((unsigned int)dhd_ioctl_timeout_msec);
}

void
dhd_os_set_ioctl_resp_timeout(unsigned int timeout_msec)
{
	dhd_ioctl_timeout_msec = (int)timeout_msec;
}

int
dhd_os_ioctl_resp_wait(dhd_pub_t *pub, uint *condition, bool *pending)
{
	dhd_info_t * dhd = (dhd_info_t *)(pub->info);
	int timeout;

	/* Convert timeout in millsecond to jiffies */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	timeout = msecs_to_jiffies(dhd_ioctl_timeout_msec);
#else
	timeout = dhd_ioctl_timeout_msec * HZ / 1000;
#endif

	timeout = wait_event_timeout(dhd->ioctl_resp_wait, (*condition), timeout);
	return timeout;
}

int
dhd_os_ioctl_resp_wake(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);

	if (waitqueue_active(&dhd->ioctl_resp_wait)) {
		wake_up(&dhd->ioctl_resp_wait);
	}

	return 0;
}

void
dhd_os_wd_timer_extend(void *bus, bool extend)
{
	dhd_pub_t *pub = bus;
	dhd_info_t *dhd = (dhd_info_t *)pub->info;

	if (extend)
		dhd_os_wd_timer(bus, WATCHDOG_EXTEND_INTERVAL);
	else
		dhd_os_wd_timer(bus, dhd->default_wd_interval);
}


void
dhd_os_wd_timer(void *bus, uint wdtick)
{
	dhd_pub_t *pub = bus;
	dhd_info_t *dhd = (dhd_info_t *)pub->info;
	unsigned long flags;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (!dhd) {
		DHD_ERROR(("%s: dhd NULL\n", __FUNCTION__));
		return;
	}

	flags = dhd_os_spin_lock(pub);

	/* don't start the wd until fw is loaded */
	if (pub->busstate == DHD_BUS_DOWN) {
		dhd_os_spin_unlock(pub, flags);
		if (!wdtick)
			DHD_OS_WD_WAKE_UNLOCK(pub);
		return;
	}

	/* Totally stop the timer */
	if (!wdtick && dhd->wd_timer_valid == TRUE) {
		dhd->wd_timer_valid = FALSE;
		dhd_os_spin_unlock(pub, flags);
		del_timer_sync(&dhd->timer);
		DHD_OS_WD_WAKE_UNLOCK(pub);
		return;
	}

	if (wdtick) {
		DHD_OS_WD_WAKE_LOCK(pub);
		dhd_watchdog_ms = (uint)wdtick;
		/* Re arm the timer, at last watchdog period */
		mod_timer(&dhd->timer, jiffies + msecs_to_jiffies(dhd_watchdog_ms));
		dhd->wd_timer_valid = TRUE;
	}
	dhd_os_spin_unlock(pub, flags);
}

void *
dhd_os_open_image(char *filename)
{
	struct file *fp;

	fp = filp_open(filename, O_RDONLY, 0);
	/*
	 * 2.6.11 (FC4) supports filp_open() but later revs don't?
	 * Alternative:
	 * fp = open_namei(AT_FDCWD, filename, O_RD, 0);
	 * ???
	 */
	 if (IS_ERR(fp))
		 fp = NULL;

	 return fp;
}

int
dhd_os_get_image_block(char *buf, int len, void *image)
{
	struct file *fp = (struct file *)image;
	int rdlen;

	if (!image)
		return 0;

	rdlen = kernel_read(fp, fp->f_pos, buf, len);
	if (rdlen > 0)
		fp->f_pos += rdlen;

	return rdlen;
}

void
dhd_os_close_image(void *image)
{
	if (image)
		filp_close((struct file *)image, NULL);
}

void
dhd_os_sdlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);

	down(&dhd->sdsem);
}

void
dhd_os_sdunlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);

	up(&dhd->sdsem);
}

void
dhd_os_sdlock_txq(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_lock_bh(&dhd->txqlock);
}

void
dhd_os_sdunlock_txq(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_unlock_bh(&dhd->txqlock);
}

void
dhd_os_sdlock_rxq(dhd_pub_t *pub)
{
}

void
dhd_os_sdunlock_rxq(dhd_pub_t *pub)
{
}

void
dhd_os_sdtxlock(dhd_pub_t *pub)
{
	dhd_os_sdlock(pub);
}

void
dhd_os_sdtxunlock(dhd_pub_t *pub)
{
	dhd_os_sdunlock(pub);
}

static void
dhd_os_rxflock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_lock_bh(&dhd->rxf_lock);

}

static void
dhd_os_rxfunlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_unlock_bh(&dhd->rxf_lock);
}

#ifdef DHDTCPACK_SUPPRESS
void
dhd_os_tcpacklock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_lock_bh(&dhd->tcpack_lock);

}

void
dhd_os_tcpackunlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd;

	dhd = (dhd_info_t *)(pub->info);
	spin_unlock_bh(&dhd->tcpack_lock);
}
#endif /* DHDTCPACK_SUPPRESS */

#if defined(CONFIG_DHD_USE_STATIC_BUF)
uint8* dhd_os_prealloc(void *osh, int section, uint size)
{
	return (uint8*)wl_android_prealloc(section, size);
}

void dhd_os_prefree(void *osh, void *addr, uint size)
{
}
#endif /* defined(CONFIG_WIFI_CONTROL_FUNC) */

#if defined(WL_WIRELESS_EXT)
struct iw_statistics *
dhd_get_wireless_stats(struct net_device *dev)
{
	int res = 0;
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	if (!dhd->pub.up) {
		return NULL;
	}

	res = wl_iw_get_wireless_stats(dev, &dhd->iw.wstats);

	if (res == 0)
		return &dhd->iw.wstats;
	else
		return NULL;
}
#endif /* defined(WL_WIRELESS_EXT) */

static int
dhd_wl_host_event(dhd_info_t *dhd, int *ifidx, void *pktdata,
	wl_event_msg_t *event, void **data)
{
	int bcmerror = 0;
	ASSERT(dhd != NULL);

	bcmerror = wl_host_event(&dhd->pub, ifidx, pktdata, event, data);
	if (bcmerror != BCME_OK)
		return (bcmerror);

#if defined(WL_WIRELESS_EXT)
	if (event->bsscfgidx == 0) {
		/*
		 * Wireless ext is on primary interface only
		 */

	ASSERT(dhd->iflist[*ifidx] != NULL);
	ASSERT(dhd->iflist[*ifidx]->net != NULL);

		if (dhd->iflist[*ifidx]->net) {
		wl_iw_event(dhd->iflist[*ifidx]->net, event, *data);
		}
	}
#endif /* defined(WL_WIRELESS_EXT)  */

#ifdef WL_CFG80211
	ASSERT(dhd->iflist[*ifidx] != NULL);
	ASSERT(dhd->iflist[*ifidx]->net != NULL);
	if (dhd->iflist[*ifidx]->net)
		wl_cfg80211_event(dhd->iflist[*ifidx]->net, event, *data);
#endif /* defined(WL_CFG80211) */

	return (bcmerror);
}

/* send up locally generated event */
void
dhd_sendup_event(dhd_pub_t *dhdp, wl_event_msg_t *event, void *data)
{
	switch (ntoh32(event->event_type)) {
#ifdef WLBTAMP
	/* Send up locally generated AMP HCI Events */
	case WLC_E_BTA_HCI_EVENT: {
		struct sk_buff *p, *skb;
		bcm_event_t *msg;
		wl_event_msg_t *p_bcm_event;
		char *ptr;
		uint32 len;
		uint32 pktlen;
		dhd_if_t *ifp;
		dhd_info_t *dhd;
		uchar *eth;
		int ifidx;

		len = ntoh32(event->datalen);
		pktlen = sizeof(bcm_event_t) + len + 2;
		dhd = dhdp->info;
		ifidx = dhd_ifname2idx(dhd, event->ifname);

		if ((p = PKTGET(dhdp->osh, pktlen, FALSE))) {
			ASSERT(ISALIGNED((uintptr)PKTDATA(dhdp->osh, p), sizeof(uint32)));

			msg = (bcm_event_t *) PKTDATA(dhdp->osh, p);

			bcopy(&dhdp->mac, &msg->eth.ether_dhost, ETHER_ADDR_LEN);
			bcopy(&dhdp->mac, &msg->eth.ether_shost, ETHER_ADDR_LEN);
			ETHER_TOGGLE_LOCALADDR(&msg->eth.ether_shost);

			msg->eth.ether_type = hton16(ETHER_TYPE_BRCM);

			/* BCM Vendor specific header... */
			msg->bcm_hdr.subtype = hton16(BCMILCP_SUBTYPE_VENDOR_LONG);
			msg->bcm_hdr.version = BCMILCP_BCM_SUBTYPEHDR_VERSION;
			bcopy(BRCM_OUI, &msg->bcm_hdr.oui[0], DOT11_OUI_LEN);

			/* vendor spec header length + pvt data length (private indication
			 *  hdr + actual message itself)
			 */
			msg->bcm_hdr.length = hton16(BCMILCP_BCM_SUBTYPEHDR_MINLENGTH +
				BCM_MSG_LEN + sizeof(wl_event_msg_t) + (uint16)len);
			msg->bcm_hdr.usr_subtype = hton16(BCMILCP_BCM_SUBTYPE_EVENT);

			PKTSETLEN(dhdp->osh, p, (sizeof(bcm_event_t) + len + 2));

			/* copy  wl_event_msg_t into sk_buf */

			/* pointer to wl_event_msg_t in sk_buf */
			p_bcm_event = &msg->event;
			bcopy(event, p_bcm_event, sizeof(wl_event_msg_t));

			/* copy hci event into sk_buf */
			bcopy(data, (p_bcm_event + 1), len);

			msg->bcm_hdr.length  = hton16(sizeof(wl_event_msg_t) +
				ntoh16(msg->bcm_hdr.length));
			PKTSETLEN(dhdp->osh, p, (sizeof(bcm_event_t) + len + 2));

			ptr = (char *)(msg + 1);
			/* Last 2 bytes of the message are 0x00 0x00 to signal that there
			 * are no ethertypes which are following this
			 */
			ptr[len+0] = 0x00;
			ptr[len+1] = 0x00;

			skb = PKTTONATIVE(dhdp->osh, p);
			eth = skb->data;
			len = skb->len;

			ifp = dhd->iflist[ifidx];
			if (ifp == NULL)
			     ifp = dhd->iflist[0];

			ASSERT(ifp);
			skb->dev = ifp->net;
			skb->protocol = eth_type_trans(skb, skb->dev);

			skb->data = eth;
			skb->len = len;

			/* Strip header, count, deliver upward */
			skb_pull(skb, ETH_HLEN);

			/* Send the packet */
			if (in_interrupt()) {
				netif_rx(skb);
			} else {
				netif_rx_ni(skb);
			}
		}
		else {
			/* Could not allocate a sk_buf */
			DHD_ERROR(("%s: unable to alloc sk_buf", __FUNCTION__));
		}
		break;
	} /* case WLC_E_BTA_HCI_EVENT */
#endif /* WLBTAMP */

	default:
		break;
	}
}

void dhd_wait_for_event(dhd_pub_t *dhd, bool *lockvar)
{
#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0))
	struct dhd_info *dhdinfo =  dhd->info;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
	int timeout = msecs_to_jiffies(IOCTL_RESP_TIMEOUT);
#else
	int timeout = (IOCTL_RESP_TIMEOUT / 1000) * HZ;
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) */

	dhd_os_sdunlock(dhd);
	wait_event_timeout(dhdinfo->ctrl_wait, (*lockvar == FALSE), timeout);
	dhd_os_sdlock(dhd);
#endif 
	return;
}

void dhd_wait_event_wakeup(dhd_pub_t *dhd)
{
#if 1 && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0))
	struct dhd_info *dhdinfo =  dhd->info;
	if (waitqueue_active(&dhdinfo->ctrl_wait))
		wake_up(&dhdinfo->ctrl_wait);
#endif
	return;
}

int
dhd_dev_reset(struct net_device *dev, uint8 flag)
{
	int ret;

	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	if (flag == TRUE) {
		/* Issue wl down command before resetting the chip */
		if (dhd_wl_ioctl_cmd(&dhd->pub, WLC_DOWN, NULL, 0, TRUE, 0) < 0) {
			DHD_TRACE(("%s: wl down failed\n", __FUNCTION__));
		}
#if defined(PROP_TXSTATUS) && !defined(PROP_TXSTATUS_VSDB)
		dhd_wlfc_deinit(&dhd->pub);
#endif /* PROP_TXSTATUS && !PROP_TXSTATUS_VSDB */
	}

	ret = dhd_bus_devreset(&dhd->pub, flag);
	if (ret) {
		DHD_ERROR(("%s: dhd_bus_devreset: %d\n", __FUNCTION__, ret));
		return ret;
	}

	return ret;
}

int net_os_set_suspend_disable(struct net_device *dev, int val)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd) {
		ret = dhd->pub.suspend_disable_flag;
		dhd->pub.suspend_disable_flag = val;
	}
	return ret;
}

int net_os_set_suspend(struct net_device *dev, int val, int force)
{
	int ret = 0;
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	if (dhd) {
#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
		ret = dhd_set_suspend(val, &dhd->pub);
#else
		ret = dhd_suspend_resume_helper(dhd, val, force);
#endif
#ifdef WL_CFG80211
		wl_cfg80211_update_power_mode(dev);
#endif
	}
	return ret;
}

int net_os_set_suspend_bcn_li_dtim(struct net_device *dev, int val)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	if (dhd)
		dhd->pub.suspend_bcn_li_dtim = val;

	return 0;
}

#ifdef PKT_FILTER_SUPPORT
int net_os_rxfilter_add_remove(struct net_device *dev, int add_remove, int num)
{
#ifndef GAN_LITE_NAT_KEEPALIVE_FILTER
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	char *filterp = NULL;
	int ret = 0;

	if (!dhd || (num == DHD_UNICAST_FILTER_NUM) ||
	    (num == DHD_MDNS_FILTER_NUM))
		return ret;
	if (num >= dhd->pub.pktfilter_count)
		return -EINVAL;
	if (add_remove) {
		switch (num) {
		case DHD_BROADCAST_FILTER_NUM:
			filterp = "101 0 0 0 0xFFFFFFFFFFFF 0xFFFFFFFFFFFF";
			break;
		case DHD_MULTICAST4_FILTER_NUM:
			filterp = "102 0 0 0 0xFFFFFF 0x01005E";
			break;
		case DHD_MULTICAST6_FILTER_NUM:
#if defined(BLOCK_IPV6_PACKET) && defined(CUSTOMER_HW4)
/* customer want to use NO IPV6 packets only */
			return ret;
#endif /* BLOCK_IPV6_PACKET && CUSTOMER_HW4 */
			filterp = "103 0 0 0 0xFFFF 0x3333";
			break;
		default:
			return -EINVAL;
		}
	}
	dhd->pub.pktfilter[num] = filterp;
	dhd_pktfilter_offload_set(&dhd->pub, dhd->pub.pktfilter[num]);
	return ret;
#else
	return 0;
#endif /* GAN_LITE_NAT_KEEPALIVE_FILTER */
}

int dhd_os_enable_packet_filter(dhd_pub_t *dhdp, int val)

{
	int ret = 0;

	/* Packet filtering is set only if we still in early-suspend and
	 * we need either to turn it ON or turn it OFF
	 * We can always turn it OFF in case of early-suspend, but we turn it
	 * back ON only if suspend_disable_flag was not set
	*/
	if (dhdp && dhdp->up) {
		if (dhdp->in_suspend) {
			if (!val || (val && !dhdp->suspend_disable_flag))
				dhd_enable_packet_filter(val, dhdp);
		}
	}
	return ret;
}

/* function to enable/disable packet for Network device */
int net_os_enable_packet_filter(struct net_device *dev, int val)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	return dhd_os_enable_packet_filter(&dhd->pub, val);
}
#endif /* PKT_FILTER_SUPPORT */

int
dhd_dev_init_ioctl(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

#if defined(CUSTOMER_HW4) && defined(USE_STAMAC_4SOFTAP)
	/* Writing STA's MAC ID to the Dongle for SOFTAP */
	if (_dhd_set_mac_address(dhd, 0, &dhd->pub.mac) == 0)
		DHD_INFO(("dhd_bus_start: MAC ID is overwritten\n"));
#endif /* CUSTOMER_HW4 && USE_STAMAC_4SOFTAP */

	return dhd_preinit_ioctls(&dhd->pub);
}

#ifdef PNO_SUPPORT
/* Linux wrapper to call common dhd_pno_clean */
int
dhd_dev_pno_reset(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	return (dhd_pno_clean(&dhd->pub));
}


/* Linux wrapper to call common dhd_pno_enable */
int
dhd_dev_pno_enable(struct net_device *dev,  int pfn_enabled)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	return (dhd_pno_enable(&dhd->pub, pfn_enabled));
}


/* Linux wrapper to call common dhd_pno_set */
int
dhd_dev_pno_set(struct net_device *dev, wlc_ssid_t* ssids_local, int nssid,
	ushort  scan_fr, int pno_repeat, int pno_freq_expo_max)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	return (dhd_pno_set(&dhd->pub, ssids_local, nssid, scan_fr, pno_repeat, pno_freq_expo_max));
}

/* Linux wrapper to get  pno status */
int
dhd_dev_get_pno_status(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);

	return (dhd_pno_get_status(&dhd->pub));
}

#endif /* PNO_SUPPORT */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && (1)
static void dhd_hang_process(struct work_struct *work)
{
	dhd_info_t *dhd;
	struct net_device *dev;

	dhd = (dhd_info_t *)container_of(work, dhd_info_t, work_hang);
	dev = dhd->iflist[0]->net;

	if (dev) {
		rtnl_lock();
		dev_close(dev);
		rtnl_unlock();
#if defined(WL_WIRELESS_EXT)
		wl_iw_send_priv_event(dev, "HANG");
#endif
#if defined(WL_CFG80211)
		wl_cfg80211_hang(dev, WLAN_REASON_UNSPECIFIED);
#endif
	}
}

int dhd_os_send_hang_message(dhd_pub_t *dhdp)
{
	int ret = 0;
	if (dhdp) {
		if (!dhdp->hang_was_sent) {
			dhdp->hang_was_sent = 1;
			schedule_work(&dhdp->info->work_hang);
		}
	}
	return ret;
}

int net_os_send_hang_message(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd) {
		/* Report FW problem when enabled */
		if (dhd->pub.hang_report) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27))
			ret = dhd_os_send_hang_message(&dhd->pub);
#else
			ret = wl_cfg80211_hang(dev, WLAN_REASON_UNSPECIFIED);
#endif
		} else {
			DHD_ERROR(("%s: FW HANG ignored (for testing purpose) and not sent up\n",
				__FUNCTION__));
			/* Enforce bus down to stop any future traffic */
			dhd->pub.busstate = DHD_BUS_DOWN;
		}
	}
	return ret;
}
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27) && OEM_ANDROID */

void dhd_bus_country_set(struct net_device *dev, wl_country_t *cspec, bool notify)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	if (dhd && dhd->pub.up) {
		memcpy(&dhd->pub.dhd_cspec, cspec, sizeof(wl_country_t));
#ifdef WL_CFG80211
		wl_update_wiphybands(NULL, notify);
#endif
	}
}

void dhd_bus_band_set(struct net_device *dev, uint band)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	if (dhd && dhd->pub.up) {
#ifdef WL_CFG80211
		wl_update_wiphybands(NULL, true);
#endif
	}
}

void dhd_net_if_lock(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	dhd_net_if_lock_local(dhd);
}

void dhd_net_if_unlock(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	dhd_net_if_unlock_local(dhd);
}

static void dhd_net_if_lock_local(dhd_info_t *dhd)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	if (dhd)
		mutex_lock(&dhd->dhd_net_if_mutex);
#endif
}

static void dhd_net_if_unlock_local(dhd_info_t *dhd)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	if (dhd)
		mutex_unlock(&dhd->dhd_net_if_mutex);
#endif
}

static void dhd_suspend_lock(dhd_pub_t *pub)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	if (dhd)
		mutex_lock(&dhd->dhd_suspend_mutex);
#endif
}

static void dhd_suspend_unlock(dhd_pub_t *pub)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && 1
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	if (dhd)
		mutex_unlock(&dhd->dhd_suspend_mutex);
#endif
}

unsigned long dhd_os_spin_lock(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags = 0;

	if (dhd)
		spin_lock_irqsave(&dhd->dhd_lock, flags);

	return flags;
}

void dhd_os_spin_unlock(dhd_pub_t *pub, unsigned long flags)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);

	if (dhd)
		spin_unlock_irqrestore(&dhd->dhd_lock, flags);
}

static int
dhd_get_pend_8021x_cnt(dhd_info_t *dhd)
{
	return (atomic_read(&dhd->pend_8021x_cnt));
}

#define MAX_WAIT_FOR_8021X_TX	50

int
dhd_wait_pend8021x(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int timeout = msecs_to_jiffies(10);
	int ntimes = MAX_WAIT_FOR_8021X_TX;
	int pend = dhd_get_pend_8021x_cnt(dhd);

	while (ntimes && pend) {
		if (pend) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(timeout);
			set_current_state(TASK_RUNNING);
			ntimes--;
		}
		pend = dhd_get_pend_8021x_cnt(dhd);
	}
	if (ntimes == 0)
	{
		atomic_set(&dhd->pend_8021x_cnt, 0);
		DHD_ERROR(("%s: TIMEOUT\n", __FUNCTION__));
	}
	return pend;
}

#ifdef DHD_DEBUG
int
write_to_file(dhd_pub_t *dhd, uint8 *buf, int size)
{
	int ret = 0;
	struct file *fp;
	mm_segment_t old_fs;
	loff_t pos = 0;

	/* change to KERNEL_DS address limit */
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	/* open file to write */
	fp = filp_open("/tmp/mem_dump", O_WRONLY|O_CREAT, 0640);
	if (!fp) {
		printf("%s: open file error\n", __FUNCTION__);
		ret = -1;
		goto exit;
	}

	/* Write buf to file */
	fp->f_op->write(fp, buf, size, &pos);

exit:
	/* free buf before return */
	MFREE(dhd->osh, buf, size);
	/* close file before return */
	if (fp)
		filp_close(fp, current->files);
	/* restore previous address limit */
	set_fs(old_fs);

	return ret;
}
#endif /* DHD_DEBUG */

int dhd_os_wake_lock_timeout(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;
	int ret = 0;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
		ret = dhd->wakelock_rx_timeout_enable > dhd->wakelock_ctrl_timeout_enable ?
			dhd->wakelock_rx_timeout_enable : dhd->wakelock_ctrl_timeout_enable;
#ifdef CONFIG_HAS_WAKELOCK
		if (dhd->wakelock_rx_timeout_enable)
			wake_lock_timeout(&dhd->wl_rxwake,
				msecs_to_jiffies(dhd->wakelock_rx_timeout_enable));
		if (dhd->wakelock_ctrl_timeout_enable)
			wake_lock_timeout(&dhd->wl_ctrlwake,
				msecs_to_jiffies(dhd->wakelock_ctrl_timeout_enable));
#endif
		dhd->wakelock_rx_timeout_enable = 0;
		dhd->wakelock_ctrl_timeout_enable = 0;
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return ret;
}

int net_os_wake_lock_timeout(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd)
		ret = dhd_os_wake_lock_timeout(&dhd->pub);
	return ret;
}

int dhd_os_wake_lock_rx_timeout_enable(dhd_pub_t *pub, int val)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
		if (val > dhd->wakelock_rx_timeout_enable)
			dhd->wakelock_rx_timeout_enable = val;
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return 0;
}

int dhd_os_wake_lock_ctrl_timeout_enable(dhd_pub_t *pub, int val)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
		if (val > dhd->wakelock_ctrl_timeout_enable)
			dhd->wakelock_ctrl_timeout_enable = val;
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return 0;
}

int net_os_wake_lock_rx_timeout_enable(struct net_device *dev, int val)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd)
		ret = dhd_os_wake_lock_rx_timeout_enable(&dhd->pub, val);
	return ret;
}

int net_os_wake_lock_ctrl_timeout_enable(struct net_device *dev, int val)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd)
		ret = dhd_os_wake_lock_ctrl_timeout_enable(&dhd->pub, val);
	return ret;
}

int dhd_os_wake_lock(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;
	int ret = 0;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
#ifdef CONFIG_HAS_WAKELOCK
		if (!dhd->wakelock_counter)
			wake_lock(&dhd->wl_wifi);
#elif 1 && (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36))
		if (pm_dev)
			pm_stay_awake(pm_dev);
#endif
		dhd->wakelock_counter++;
		ret = dhd->wakelock_counter;
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return ret;
}

int net_os_wake_lock(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd)
		ret = dhd_os_wake_lock(&dhd->pub);
	return ret;
}

int dhd_os_wake_unlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;
	int ret = 0;

	dhd_os_wake_lock_timeout(pub);
	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
		if (dhd->wakelock_counter) {
			dhd->wakelock_counter--;
#ifdef CONFIG_HAS_WAKELOCK
			if (!dhd->wakelock_counter)
				wake_unlock(&dhd->wl_wifi);
#elif 1 && (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36))
			if (pm_dev)
				pm_relax(pm_dev);
#endif
			ret = dhd->wakelock_counter;
		}
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return ret;
}

int dhd_os_check_wakelock(void *dhdp)
{
#if defined(CONFIG_HAS_WAKELOCK) || (1 && (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, \
	36)))
	dhd_pub_t *pub = (dhd_pub_t *)dhdp;
	dhd_info_t *dhd;

	if (!pub)
		return 0;
	dhd = (dhd_info_t *)(pub->info);
#endif /* CONFIG_HAS_WAKELOCK || BCMSDIO */

#ifdef CONFIG_HAS_WAKELOCK
	/* Indicate to the SD Host to avoid going to suspend if internal locks are up */
	if (dhd && (wake_lock_active(&dhd->wl_wifi) ||
		(wake_lock_active(&dhd->wl_wdwake))))
		return 1;
#elif 1 && (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36))
	if (dhd && (dhd->wakelock_counter > 0) && pm_dev)
		return 1;
#endif
	return 0;
}
int net_os_wake_unlock(struct net_device *dev)
{
	dhd_info_t *dhd = *(dhd_info_t **)netdev_priv(dev);
	int ret = 0;

	if (dhd)
		ret = dhd_os_wake_unlock(&dhd->pub);
	return ret;
}

int dhd_os_wd_wake_lock(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;
	int ret = 0;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
#ifdef CONFIG_HAS_WAKELOCK
		/* if wakelock_wd_counter was never used : lock it at once */
		if (!dhd->wakelock_wd_counter)
			wake_lock(&dhd->wl_wdwake);
#endif
		dhd->wakelock_wd_counter++;
		ret = dhd->wakelock_wd_counter;
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return ret;
}

int dhd_os_wd_wake_unlock(dhd_pub_t *pub)
{
	dhd_info_t *dhd = (dhd_info_t *)(pub->info);
	unsigned long flags;
	int ret = 0;

	if (dhd) {
		spin_lock_irqsave(&dhd->wakelock_spinlock, flags);
		if (dhd->wakelock_wd_counter) {
			dhd->wakelock_wd_counter = 0;
#ifdef CONFIG_HAS_WAKELOCK
			wake_unlock(&dhd->wl_wdwake);
#endif
		}
		spin_unlock_irqrestore(&dhd->wakelock_spinlock, flags);
	}
	return ret;
}
int dhd_os_check_if_up(void *dhdp)
{
	dhd_pub_t *pub = (dhd_pub_t *)dhdp;

	if (!pub)
		return 0;
	return pub->up;
}
/* function to collect firmware, chip id and chip version info */
void dhd_set_version_info(dhd_pub_t *dhdp, char *fw)
{
	int i;

	i = snprintf(info_string, sizeof(info_string),
		"  Driver: %s\n  Firmware: %s ", EPI_VERSION_STR, fw);

	if (!dhdp)
		return;

	i = snprintf(&info_string[i], sizeof(info_string) - i,
		"\n  Chip: %x Rev %x Pkg %x", dhd_bus_chip_id(dhdp),
		dhd_bus_chiprev_id(dhdp), dhd_bus_chippkg_id(dhdp));
}
int dhd_ioctl_entry_local(struct net_device *net, wl_ioctl_t *ioc, int cmd)
{
	int ifidx;
	int ret = 0;
	dhd_info_t *dhd = NULL;

	if (!net || !netdev_priv(net)) {
		DHD_ERROR(("%s invalid parameter\n", __FUNCTION__));
		return -EINVAL;
	}

	dhd = *(dhd_info_t **)netdev_priv(net);
	if (!dhd)
		return -EINVAL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31))
	if ((net->netdev_ops != &dhd_ops_pri) &&
		(net->netdev_ops != &dhd_ops_virt)) {
		DHD_ERROR(("%s no brcm device\n", __FUNCTION__));
		return -ENODEV;
	}
#endif

	ifidx = dhd_net2idx(dhd, net);
	if (ifidx == DHD_BAD_IF) {
		DHD_ERROR(("%s bad ifidx\n", __FUNCTION__));
		return -ENODEV;
	}

	DHD_OS_WAKE_LOCK(&dhd->pub);
	ret = dhd_wl_ioctl(&dhd->pub, ifidx, ioc, ioc->buf, ioc->len);
	dhd_check_hang(net, &dhd->pub, ret);
	DHD_OS_WAKE_UNLOCK(&dhd->pub);

	return ret;
}

bool dhd_os_check_hang(dhd_pub_t *dhdp, int ifidx, int ret)
{
	struct net_device *net;

	net = dhd_idx2net(dhdp, ifidx);
	return dhd_check_hang(net, dhdp, ret);
}

#if defined(WL_CFG80211) && defined(SUPPORT_DEEP_SLEEP)
#define MAX_TRY_CNT             5 /* Number of tries to disable deepsleep */
int dhd_deepsleep(struct net_device *dev, int flag)
{
	char iovbuf[20];
	uint powervar = 0;
	dhd_info_t *dhd;
	dhd_pub_t *dhdp;
	int cnt = 0;
	int ret = 0;

	dhd = *(dhd_info_t **)netdev_priv(dev);
	dhdp = &dhd->pub;

	switch (flag) {
		case 1 :  /* Deepsleep on */
			DHD_ERROR(("[WiFi] Deepsleep On\n"));
			/* give some time to _dhd_sysioc_thread() before deepsleep */
			msleep(200);
#ifdef PKT_FILTER_SUPPORT
		/* disable pkt filter */
		dhd_enable_packet_filter(0, dhdp);
#endif /* PKT_FILTER_SUPPORT */
			/* Disable MPC */
			powervar = 0;
			memset(iovbuf, 0, sizeof(iovbuf));
			bcm_mkiovar("mpc", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
			dhd_wl_ioctl_cmd(dhdp, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

			/* Enable Deepsleep */
			powervar = 1;
			memset(iovbuf, 0, sizeof(iovbuf));
			bcm_mkiovar("deepsleep", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
			dhd_wl_ioctl_cmd(dhdp, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
			break;

		case 0: /* Deepsleep Off */
			DHD_ERROR(("[WiFi] Deepsleep Off\n"));

			/* Disable Deepsleep */
			for (cnt = 0; cnt < MAX_TRY_CNT; cnt++) {
				powervar = 0;
				memset(iovbuf, 0, sizeof(iovbuf));
				bcm_mkiovar("deepsleep", (char *)&powervar, 4,
					iovbuf, sizeof(iovbuf));
				dhd_wl_ioctl_cmd(dhdp, WLC_SET_VAR, iovbuf,
					sizeof(iovbuf), TRUE, 0);


				memset(iovbuf, 0, sizeof(iovbuf));
				bcm_mkiovar("deepsleep", (char *)&powervar, 4,
					iovbuf, sizeof(iovbuf));
				if ((ret = dhd_wl_ioctl_cmd(dhdp, WLC_GET_VAR, iovbuf,
					sizeof(iovbuf),	FALSE, 0)) < 0) {
					DHD_ERROR(("the error of dhd deepsleep status"
						" ret value :%d\n", ret));
				} else {
					if (!(*(int *)iovbuf)) {
						DHD_ERROR(("deepsleep mode is 0,"
							" count: %d\n", cnt));
						break;
					}
				}
			}

			/* Enable MPC */
			powervar = 1;
			memset(iovbuf, 0, sizeof(iovbuf));
			bcm_mkiovar("mpc", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
			dhd_wl_ioctl_cmd(dhdp, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
			break;
	}

	return 0;
}
#endif /* WL_CFG80211 && SUPPORT_DEEP_SLEEP */
#ifdef PROP_TXSTATUS

void dhd_wlfc_plat_enable(void *dhd)
{
#if defined(CUSTOMER_HW4) && defined(USE_DYNAMIC_F2_BLKSIZE)
	dhdsdio_func_blocksize((dhd_pub_t *)dhd, 2, DYNAMIC_F2_BLKSIZE_FOR_NONLEGACY);
#endif /* CUSTOMER_HW4 && USE_DYNAMIC_F2_BLKSIZE */
	return;
}

void dhd_wlfc_plat_deinit(void *dhd)
{
#if defined(CUSTOMER_HW4) && defined(USE_DYNAMIC_F2_BLKSIZE)
	dhdsdio_func_blocksize((dhd_pub_t *)dhd, 2, sd_f2_blocksize);
#endif /* CUSTOMER_HW4 && USE_DYNAMIC_F2_BLKSIZE */
	return;
}

bool dhd_wlfc_skip_fc(void)
{

#ifdef WL_CFG80211

	/* enable flow control in vsdb mode */
	return !(wl_cfg80211_get_vsdb_mode());
#else
	return TRUE; /* skip flow control */
#endif /* WL_CFG80211 */
}
#endif /* PROP_TXSTATUS */

#ifdef BCMDBGFS

#include <linux/debugfs.h>

extern uint32 dhd_readregl(void *bp, uint32 addr);
extern uint32 dhd_writeregl(void *bp, uint32 addr, uint32 data);

typedef struct dhd_dbgfs {
	struct dentry	*debugfs_dir;
	struct dentry	*debugfs_mem;
	dhd_pub_t 	*dhdp;
	uint32 		size;
} dhd_dbgfs_t;

dhd_dbgfs_t g_dbgfs;

static int
dhd_dbg_state_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t
dhd_dbg_state_read(struct file *file, char __user *ubuf,
                       size_t count, loff_t *ppos)
{
	ssize_t rval;
	uint32 tmp;
	loff_t pos = *ppos;
	size_t ret;

	if (pos < 0)
		return -EINVAL;
	if (pos >= g_dbgfs.size || !count)
		return 0;
	if (count > g_dbgfs.size - pos)
		count = g_dbgfs.size - pos;

	/* Basically enforce aligned 4 byte reads. It's up to the user to work out the details */
	tmp = dhd_readregl(g_dbgfs.dhdp->bus, file->f_pos & (~3));

	ret = copy_to_user(ubuf, &tmp, 4);
	if (ret == count)
		return -EFAULT;

	count -= ret;
	*ppos = pos + count;
	rval = count;

	return rval;
}


static ssize_t
dhd_debugfs_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t ret;
	uint32 buf;

	if (pos < 0)
		return -EINVAL;
	if (pos >= g_dbgfs.size || !count)
		return 0;
	if (count > g_dbgfs.size - pos)
		count = g_dbgfs.size - pos;

	ret = copy_from_user(&buf, ubuf, sizeof(uint32));
	if (ret == count)
		return -EFAULT;

	/* Basically enforce aligned 4 byte writes. It's up to the user to work out the details */
	dhd_writeregl(g_dbgfs.dhdp->bus, file->f_pos & (~3), buf);

	return count;
}


loff_t
dhd_debugfs_lseek(struct file *file, loff_t off, int whence)
{
	loff_t pos = -1;

	switch (whence) {
		case 0:
			pos = off;
			break;
		case 1:
			pos = file->f_pos + off;
			break;
		case 2:
			pos = g_dbgfs.size - off;
	}
	return (pos < 0 || pos > g_dbgfs.size) ? -EINVAL : (file->f_pos = pos);
}

static const struct file_operations dhd_dbg_state_ops = {
	.read   = dhd_dbg_state_read,
	.write	= dhd_debugfs_write,
	.open   = dhd_dbg_state_open,
	.llseek	= dhd_debugfs_lseek
};

static void dhd_dbg_create(void)
{
	if (g_dbgfs.debugfs_dir) {
		g_dbgfs.debugfs_mem = debugfs_create_file("mem", 0644, g_dbgfs.debugfs_dir,
			NULL, &dhd_dbg_state_ops);
	}
}

void dhd_dbg_init(dhd_pub_t *dhdp)
{
	int err;

	g_dbgfs.dhdp = dhdp;
	g_dbgfs.size = 0x20000000; /* Allow access to various cores regs */

	g_dbgfs.debugfs_dir = debugfs_create_dir("dhd", 0);
	if (IS_ERR(g_dbgfs.debugfs_dir)) {
		err = PTR_ERR(g_dbgfs.debugfs_dir);
		g_dbgfs.debugfs_dir = NULL;
		return;
	}

	dhd_dbg_create();

	return;
}

void dhd_dbg_remove(void)
{
	debugfs_remove(g_dbgfs.debugfs_mem);
	debugfs_remove(g_dbgfs.debugfs_dir);

	bzero((unsigned char *) &g_dbgfs, sizeof(g_dbgfs));

}
#endif /* ifdef BCMDBGFS */

#ifdef WLMEDIA_HTSF

static
void dhd_htsf_addtxts(dhd_pub_t *dhdp, void *pktbuf)
{
	dhd_info_t *dhd = (dhd_info_t *)(dhdp->info);
	struct sk_buff *skb;
	uint32 htsf = 0;
	uint16 dport = 0, oldmagic = 0xACAC;
	char *p1;
	htsfts_t ts;

	/*  timestamp packet  */

	p1 = (char*) PKTDATA(dhdp->osh, pktbuf);

	if (PKTLEN(dhdp->osh, pktbuf) > HTSF_MINLEN) {
/*		memcpy(&proto, p1+26, 4);  	*/
		memcpy(&dport, p1+40, 2);
/* 	proto = ((ntoh32(proto))>> 16) & 0xFF;  */
		dport = ntoh16(dport);
	}

	/* timestamp only if  icmp or udb iperf with port 5555 */
/*	if (proto == 17 && dport == tsport) { */
	if (dport >= tsport && dport <= tsport + 20) {

		skb = (struct sk_buff *) pktbuf;

		htsf = dhd_get_htsf(dhd, 0);
		memset(skb->data + 44, 0, 2); /* clear checksum */
		memcpy(skb->data+82, &oldmagic, 2);
		memcpy(skb->data+84, &htsf, 4);

		memset(&ts, 0, sizeof(htsfts_t));
		ts.magic  = HTSFMAGIC;
		ts.prio   = PKTPRIO(pktbuf);
		ts.seqnum = htsf_seqnum++;
		ts.c10    = get_cycles();
		ts.t10    = htsf;
		ts.endmagic = HTSFENDMAGIC;

		memcpy(skb->data + HTSF_HOSTOFFSET, &ts, sizeof(ts));
	}
}

static void dhd_dump_htsfhisto(histo_t *his, char *s)
{
	int pktcnt = 0, curval = 0, i;
	for (i = 0; i < (NUMBIN-2); i++) {
		curval += 500;
		printf("%d ",  his->bin[i]);
		pktcnt += his->bin[i];
	}
	printf(" max: %d TotPkt: %d neg: %d [%s]\n", his->bin[NUMBIN-2], pktcnt,
		his->bin[NUMBIN-1], s);
}

static
void sorttobin(int value, histo_t *histo)
{
	int i, binval = 0;

	if (value < 0) {
		histo->bin[NUMBIN-1]++;
		return;
	}
	if (value > histo->bin[NUMBIN-2])  /* store the max value  */
		histo->bin[NUMBIN-2] = value;

	for (i = 0; i < (NUMBIN-2); i++) {
		binval += 500; /* 500m s bins */
		if (value <= binval) {
			histo->bin[i]++;
			return;
		}
	}
	histo->bin[NUMBIN-3]++;
}

static
void dhd_htsf_addrxts(dhd_pub_t *dhdp, void *pktbuf)
{
	dhd_info_t *dhd = (dhd_info_t *)dhdp->info;
	struct sk_buff *skb;
	char *p1;
	uint16 old_magic;
	int d1, d2, d3, end2end;
	htsfts_t *htsf_ts;
	uint32 htsf;

	skb = PKTTONATIVE(dhdp->osh, pktbuf);
	p1 = (char*)PKTDATA(dhdp->osh, pktbuf);

	if (PKTLEN(osh, pktbuf) > HTSF_MINLEN) {
		memcpy(&old_magic, p1+78, 2);
		htsf_ts = (htsfts_t*) (p1 + HTSF_HOSTOFFSET - 4);
	}
	else
		return;

	if (htsf_ts->magic == HTSFMAGIC) {
		htsf_ts->tE0 = dhd_get_htsf(dhd, 0);
		htsf_ts->cE0 = get_cycles();
	}

	if (old_magic == 0xACAC) {

		tspktcnt++;
		htsf = dhd_get_htsf(dhd, 0);
		memcpy(skb->data+92, &htsf, sizeof(uint32));

		memcpy(&ts[tsidx].t1, skb->data+80, 16);

		d1 = ts[tsidx].t2 - ts[tsidx].t1;
		d2 = ts[tsidx].t3 - ts[tsidx].t2;
		d3 = ts[tsidx].t4 - ts[tsidx].t3;
		end2end = ts[tsidx].t4 - ts[tsidx].t1;

		sorttobin(d1, &vi_d1);
		sorttobin(d2, &vi_d2);
		sorttobin(d3, &vi_d3);
		sorttobin(end2end, &vi_d4);

		if (end2end > 0 && end2end >  maxdelay) {
			maxdelay = end2end;
			maxdelaypktno = tspktcnt;
			memcpy(&maxdelayts, &ts[tsidx], 16);
		}
		if (++tsidx >= TSMAX)
			tsidx = 0;
	}
}

uint32 dhd_get_htsf(dhd_info_t *dhd, int ifidx)
{
	uint32 htsf = 0, cur_cycle, delta, delta_us;
	uint32    factor, baseval, baseval2;
	cycles_t t;

	t = get_cycles();
	cur_cycle = t;

	if (cur_cycle >  dhd->htsf.last_cycle)
		delta = cur_cycle -  dhd->htsf.last_cycle;
	else {
		delta = cur_cycle + (0xFFFFFFFF -  dhd->htsf.last_cycle);
	}

	delta = delta >> 4;

	if (dhd->htsf.coef) {
		/* times ten to get the first digit */
	        factor = (dhd->htsf.coef*10 + dhd->htsf.coefdec1);
		baseval  = (delta*10)/factor;
		baseval2 = (delta*10)/(factor+1);
		delta_us  = (baseval -  (((baseval - baseval2) * dhd->htsf.coefdec2)) / 10);
		htsf = (delta_us << 4) +  dhd->htsf.last_tsf + HTSF_BUS_DELAY;
	}
	else {
		DHD_ERROR(("-------dhd->htsf.coef = 0 -------\n"));
	}

	return htsf;
}

static void dhd_dump_latency(void)
{
	int i, max = 0;
	int d1, d2, d3, d4, d5;

	printf("T1       T2       T3       T4           d1  d2   t4-t1     i    \n");
	for (i = 0; i < TSMAX; i++) {
		d1 = ts[i].t2 - ts[i].t1;
		d2 = ts[i].t3 - ts[i].t2;
		d3 = ts[i].t4 - ts[i].t3;
		d4 = ts[i].t4 - ts[i].t1;
		d5 = ts[max].t4-ts[max].t1;
		if (d4 > d5 && d4 > 0)  {
			max = i;
		}
		printf("%08X %08X %08X %08X \t%d %d %d   %d i=%d\n",
			ts[i].t1, ts[i].t2, ts[i].t3, ts[i].t4,
			d1, d2, d3, d4, i);
	}

	printf("current idx = %d \n", tsidx);

	printf("Highest latency %d pkt no.%d total=%d\n", maxdelay, maxdelaypktno, tspktcnt);
	printf("%08X %08X %08X %08X \t%d %d %d   %d\n",
	maxdelayts.t1, maxdelayts.t2, maxdelayts.t3, maxdelayts.t4,
	maxdelayts.t2 - maxdelayts.t1,
	maxdelayts.t3 - maxdelayts.t2,
	maxdelayts.t4 - maxdelayts.t3,
	maxdelayts.t4 - maxdelayts.t1);
}


static int
dhd_ioctl_htsf_get(dhd_info_t *dhd, int ifidx)
{
	wl_ioctl_t ioc;
	char buf[32];
	int ret;
	uint32 s1, s2;

	struct tsf {
		uint32 low;
		uint32 high;
	} tsf_buf;

	memset(&ioc, 0, sizeof(ioc));
	memset(&tsf_buf, 0, sizeof(tsf_buf));

	ioc.cmd = WLC_GET_VAR;
	ioc.buf = buf;
	ioc.len = (uint)sizeof(buf);
	ioc.set = FALSE;

	strncpy(buf, "tsf", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	s1 = dhd_get_htsf(dhd, 0);
	if ((ret = dhd_wl_ioctl(&dhd->pub, ifidx, &ioc, ioc.buf, ioc.len)) < 0) {
		if (ret == -EIO) {
			DHD_ERROR(("%s: tsf is not supported by device\n",
				dhd_ifname(&dhd->pub, ifidx)));
			return -EOPNOTSUPP;
		}
		return ret;
	}
	s2 = dhd_get_htsf(dhd, 0);

	memcpy(&tsf_buf, buf, sizeof(tsf_buf));
	printf(" TSF_h=%04X lo=%08X Calc:htsf=%08X, coef=%d.%d%d delta=%d ",
		tsf_buf.high, tsf_buf.low, s2, dhd->htsf.coef, dhd->htsf.coefdec1,
		dhd->htsf.coefdec2, s2-tsf_buf.low);
	printf("lasttsf=%08X lastcycle=%08X\n", dhd->htsf.last_tsf, dhd->htsf.last_cycle);
	return 0;
}

void htsf_update(dhd_info_t *dhd, void *data)
{
	static ulong  cur_cycle = 0, prev_cycle = 0;
	uint32 htsf, tsf_delta = 0;
	uint32 hfactor = 0, cyc_delta, dec1 = 0, dec2, dec3, tmp;
	ulong b, a;
	cycles_t t;

	/* cycles_t in inlcude/mips/timex.h */

	t = get_cycles();

	prev_cycle = cur_cycle;
	cur_cycle = t;

	if (cur_cycle > prev_cycle)
		cyc_delta = cur_cycle - prev_cycle;
	else {
		b = cur_cycle;
		a = prev_cycle;
		cyc_delta = cur_cycle + (0xFFFFFFFF - prev_cycle);
	}

	if (data == NULL)
		printf(" tsf update ata point er is null \n");

	memcpy(&prev_tsf, &cur_tsf, sizeof(tsf_t));
	memcpy(&cur_tsf, data, sizeof(tsf_t));

	if (cur_tsf.low == 0) {
		DHD_INFO((" ---- 0 TSF, do not update, return\n"));
		return;
	}

	if (cur_tsf.low > prev_tsf.low)
		tsf_delta = (cur_tsf.low - prev_tsf.low);
	else {
		DHD_INFO((" ---- tsf low is smaller cur_tsf= %08X, prev_tsf=%08X, \n",
		 cur_tsf.low, prev_tsf.low));
		if (cur_tsf.high > prev_tsf.high) {
			tsf_delta = cur_tsf.low + (0xFFFFFFFF - prev_tsf.low);
			DHD_INFO((" ---- Wrap around tsf coutner  adjusted TSF=%08X\n", tsf_delta));
		}
		else
			return; /* do not update */
	}

	if (tsf_delta)  {
		hfactor = cyc_delta / tsf_delta;
		tmp  = 	(cyc_delta - (hfactor * tsf_delta))*10;
		dec1 =  tmp/tsf_delta;
		dec2 =  ((tmp - dec1*tsf_delta)*10) / tsf_delta;
		tmp  = 	(tmp   - (dec1*tsf_delta))*10;
		dec3 =  ((tmp - dec2*tsf_delta)*10) / tsf_delta;

		if (dec3 > 4) {
			if (dec2 == 9) {
				dec2 = 0;
				if (dec1 == 9) {
					dec1 = 0;
					hfactor++;
				}
				else {
					dec1++;
				}
			}
			else
				dec2++;
		}
	}

	if (hfactor) {
		htsf = ((cyc_delta * 10)  / (hfactor*10+dec1)) + prev_tsf.low;
		dhd->htsf.coef = hfactor;
		dhd->htsf.last_cycle = cur_cycle;
		dhd->htsf.last_tsf = cur_tsf.low;
		dhd->htsf.coefdec1 = dec1;
		dhd->htsf.coefdec2 = dec2;
	}
	else {
		htsf = prev_tsf.low;
	}
}

#endif /* WLMEDIA_HTSF */