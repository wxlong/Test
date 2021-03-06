/*  $Header: /var/lib/cvs/prism54-ng/ksrc/islpci_dev.c,v 1.68 2004/02/28 03:06:07 mcgrof Exp $
 *  
 *  Copyright (C) 2002 Intersil Americas Inc.
 *  Copyright (C) 2003 Herbert Valerio Riedel <hvr@gnu.org>
 *  Copyright (C) 2003 Luis R. Rodriguez <mcgrof@ruslug.rutgers.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/version.h>
#include <linux/module.h>

#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/if_arp.h>

#include <asm/io.h>

#include "isl_38xx.h"
#include "isl_ioctl.h"
#include "islpci_dev.h"
#include "islpci_mgt.h"
#include "islpci_eth.h"
#include "oid_mgt.h"

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,5,0)
#define prism54_synchronize_irq(irq) synchronize_irq()
#else
#define prism54_synchronize_irq(irq) synchronize_irq(irq)
#endif

#define ISL3877_IMAGE_FILE	"isl3877"
#define ISL3890_IMAGE_FILE	"isl3890"

/* Temporary dummy MAC address to use until firmware is loaded.
 * The idea there is that some tools (such as nameif) may query
 * the MAC address before the netdev is 'open'. By using a valid
 * OUI prefix, they can process the netdev properly.
 * Of course, this is not the final/real MAC address. It doesn't
 * matter, as you are suppose to be able to change it anytime via
 * ndev->set_mac_address. Jean II */
const unsigned char	dummy_mac[6] = { 0x00, 0x30, 0xB4, 0x00, 0x00, 0x00 };

/******************************************************************************
    Device Interrupt Handler
******************************************************************************/

irqreturn_t
islpci_interrupt(int irq, void *config, struct pt_regs *regs)
{
	u32 reg;
	islpci_private *priv = config;
	struct net_device *ndev = priv->ndev;
	void *device = priv->device_base;
	int powerstate = ISL38XX_PSM_POWERSAVE_STATE;

	/* received an interrupt request on a shared IRQ line
	 * first check whether the device is in sleep mode */
	reg = readl(device + ISL38XX_CTRL_STAT_REG);
	if (reg & ISL38XX_CTRL_STAT_SLEEPMODE)
		/* device is in sleep mode, IRQ was generated by someone else */
	{
		printk(KERN_DEBUG "Assuming someone else called the IRQ\n");
		return IRQ_NONE;
	}

	if (islpci_get_state(priv) != PRV_STATE_SLEEP)
		powerstate = ISL38XX_PSM_ACTIVE_STATE;

	/* lock the interrupt handler */
	spin_lock(&priv->slock);

	/* check whether there is any source of interrupt on the device */
	reg = readl(device + ISL38XX_INT_IDENT_REG);

	/* also check the contents of the Interrupt Enable Register, because this
	 * will filter out interrupt sources from other devices on the same irq ! */
	reg &= readl(device + ISL38XX_INT_EN_REG);
	reg &= ISL38XX_INT_SOURCES;

	if (reg != 0) {
		/* reset the request bits in the Identification register */
		isl38xx_w32_flush(device, reg, ISL38XX_INT_ACK_REG);

#if VERBOSE > SHOW_ERROR_MESSAGES
		DEBUG(SHOW_FUNCTION_CALLS,
		      "IRQ: Identification register 0x%p 0x%x \n", device, reg);
#endif

		/* check for each bit in the register separately */
		if (reg & ISL38XX_INT_IDENT_UPDATE) {
#if VERBOSE > SHOW_ERROR_MESSAGES
			/* Queue has been updated */
			DEBUG(SHOW_TRACING, "IRQ: Update flag \n");

			DEBUG(SHOW_QUEUE_INDEXES,
			      "CB drv Qs: [%i][%i][%i][%i][%i][%i]\n",
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[0]),
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[1]),
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[2]),
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[3]),
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[4]),
			      le32_to_cpu(priv->control_block->
					  driver_curr_frag[5])
			    );

			DEBUG(SHOW_QUEUE_INDEXES,
			      "CB dev Qs: [%i][%i][%i][%i][%i][%i]\n",
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[0]),
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[1]),
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[2]),
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[3]),
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[4]),
			      le32_to_cpu(priv->control_block->
					  device_curr_frag[5])
			    );
#endif

			/* cleanup the data low transmit queue */
			islpci_eth_cleanup_transmit(priv, priv->control_block);

			/* device is in active state, update the
			 * powerstate flag if necessary */
			powerstate = ISL38XX_PSM_ACTIVE_STATE;

			/* check all three queues in priority order
			 * call the PIMFOR receive function until the
			 * queue is empty */
			if (isl38xx_in_queue(priv->control_block,
						ISL38XX_CB_RX_MGMTQ) != 0) {
#if VERBOSE > SHOW_ERROR_MESSAGES
				DEBUG(SHOW_TRACING,
				      "Received frame in Management Queue\n");
#endif
				islpci_mgt_receive(ndev);

				islpci_mgt_cleanup_transmit(ndev);

				/* Refill slots in receive queue */
				islpci_mgmt_rx_fill(ndev);

				/* no need to trigger the device, next
                                   islpci_mgt_transaction does it */
			}

			while (isl38xx_in_queue(priv->control_block,
						ISL38XX_CB_RX_DATA_LQ) != 0) {
#if VERBOSE > SHOW_ERROR_MESSAGES
				DEBUG(SHOW_TRACING,
				      "Received frame in Data Low Queue \n");
#endif
				islpci_eth_receive(priv);
			}

			/* check whether the data transmit queues were full */
			if (priv->data_low_tx_full) {
				/* check whether the transmit is not full anymore */
				if (ISL38XX_CB_TX_QSIZE -
				    isl38xx_in_queue(priv->control_block,
						     ISL38XX_CB_TX_DATA_LQ) >=
				    ISL38XX_MIN_QTHRESHOLD) {
					/* nope, the driver is ready for more network frames */
					netif_wake_queue(priv->ndev);

					/* reset the full flag */
					priv->data_low_tx_full = 0;
				}
			}
		}

		if (reg & ISL38XX_INT_IDENT_INIT) {
			/* Device has been initialized */
#if VERBOSE > SHOW_ERROR_MESSAGES
			DEBUG(SHOW_TRACING,
			      "IRQ: Init flag, device initialized \n");
#endif
			wake_up(&priv->reset_done);
		}

		if (reg & ISL38XX_INT_IDENT_SLEEP) {
			/* Device intends to move to powersave state */
#if VERBOSE > SHOW_ERROR_MESSAGES
			DEBUG(SHOW_TRACING, "IRQ: Sleep flag \n");
#endif
			isl38xx_handle_sleep_request(priv->control_block,
						     &powerstate,
						     priv->device_base);
		}

		if (reg & ISL38XX_INT_IDENT_WAKEUP) {
			/* Device has been woken up to active state */
#if VERBOSE > SHOW_ERROR_MESSAGES
			DEBUG(SHOW_TRACING, "IRQ: Wakeup flag \n");
#endif

			isl38xx_handle_wakeup(priv->control_block,
					      &powerstate, priv->device_base);
		}
	}

	/* sleep -> ready */
	if (islpci_get_state(priv) == PRV_STATE_SLEEP
	    && powerstate == ISL38XX_PSM_ACTIVE_STATE)
		islpci_set_state(priv, PRV_STATE_READY);

	/* !sleep -> sleep */
	if (islpci_get_state(priv) != PRV_STATE_SLEEP
	    && powerstate == ISL38XX_PSM_POWERSAVE_STATE)
		islpci_set_state(priv, PRV_STATE_SLEEP);

	/* unlock the interrupt handler */
	spin_unlock(&priv->slock);

	return IRQ_HANDLED;
}

/******************************************************************************
    Network Interface Control & Statistical functions
******************************************************************************/
static int
islpci_open(struct net_device *ndev)
{
	u32 rc;
	islpci_private *priv = netdev_priv(ndev);

	printk(KERN_DEBUG "%s: islpci_open()\n", ndev->name);

	/* reset data structures, upload firmware and reset device */
	rc = islpci_reset(priv,1);
	if (rc) {
		prism54_bring_down(priv);
		return rc; /* Returns informative message */
	}

	netif_start_queue(ndev);
/*      netif_mark_up( ndev ); */

	return 0;
}

static int
islpci_close(struct net_device *ndev)
{
	islpci_private *priv = netdev_priv(ndev);

	printk(KERN_DEBUG "%s: islpci_close ()\n", ndev->name);

	netif_stop_queue(ndev);

	return prism54_bring_down(priv);
}

int
prism54_bring_down(islpci_private *priv)
{
	void *device_base = priv->device_base;
	u32 reg;
	/* we are going to shutdown the device */
	islpci_set_state(priv, PRV_STATE_PREBOOT);

	/* disable all device interrupts in case they weren't */
	isl38xx_disable_interrupts(priv->device_base);  

	/* For safety reasons, we may want to ensure that no DMA transfer is
	 * currently in progress by emptying the TX and RX queues. */

	/* wait until interrupts have finished executing on other CPUs */
	prism54_synchronize_irq(priv->pdev->irq);

	reg = readl(device_base + ISL38XX_CTRL_STAT_REG);
	reg &= ~(ISL38XX_CTRL_STAT_RESET | ISL38XX_CTRL_STAT_RAMBOOT);
	writel(reg, device_base + ISL38XX_CTRL_STAT_REG);
	wmb();
	udelay(ISL38XX_WRITEIO_DELAY);

	reg |= ISL38XX_CTRL_STAT_RESET;
	writel(reg, device_base + ISL38XX_CTRL_STAT_REG);
	wmb();
	udelay(ISL38XX_WRITEIO_DELAY);

	/* clear the Reset bit */
	reg &= ~ISL38XX_CTRL_STAT_RESET;
	writel(reg, device_base + ISL38XX_CTRL_STAT_REG);
	wmb();

	/* wait a while for the device to reset */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(50*HZ/1000);

	return 0;
}

static int
islpci_upload_fw(islpci_private *priv)
{
	islpci_state_t old_state;
	u32 rc;

	old_state = islpci_set_state(priv, PRV_STATE_BOOT);

	printk(KERN_DEBUG "%s: uploading firmware...\n", priv->ndev->name);

	rc = isl38xx_upload_firmware(priv->firmware,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,75))
		&priv->pdev->dev,
#else
		pci_name(priv->pdev),
#endif
		priv->device_base,
		priv->device_host_address);
	if (rc) {
		/* error uploading the firmware */
		printk(KERN_ERR "%s: could not upload firmware ('%s')\n",
		       priv->ndev->name, priv->firmware);

		islpci_set_state(priv, old_state);
		return rc;
	}

	printk(KERN_DEBUG
	       "%s: firmware uploaded done, now triggering reset...\n",
	       priv->ndev->name);

	islpci_set_state(priv, PRV_STATE_POSTBOOT);

	return 0;
}

static int
islpci_reset_if(islpci_private *priv)
{
	long remaining;
	int result = -ETIME;
	int count;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	/* This is 2.6 specific, nicer, shorter, but not in 2.4 yet */
	DEFINE_WAIT(wait);
	prepare_to_wait(&priv->reset_done, &wait, TASK_UNINTERRUPTIBLE);
#else
	DECLARE_WAITQUEUE(wait, current);
	set_current_state(TASK_UNINTERRUPTIBLE);
	add_wait_queue(&priv->reset_done, &wait);
#endif
	
	/* now the last step is to reset the interface */
	isl38xx_interface_reset(priv->device_base, priv->device_host_address);
	islpci_set_state(priv, PRV_STATE_PREINIT);

        for(count = 0; count < 2 && result; count++) {
		/* The software reset acknowledge needs about 220 msec here.
		 * Be conservative and wait for up to one second. */
	
		remaining = schedule_timeout(HZ);

		if(remaining > 0) {
			result = 0;
			break;
		}

		/* If we're here it's because our IRQ hasn't yet gone through. 
		 * Retry a bit more...
		 */
		 printk(KERN_ERR "%s: device soft reset timed out\n",
		       priv->ndev->name);

	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	/* 2.6 specific too */
	finish_wait(&priv->reset_done, &wait);
#else
	remove_wait_queue(&priv->reset_done, &wait);
	set_current_state(TASK_RUNNING);
#endif

	if(result)
		return result;

	islpci_set_state(priv, PRV_STATE_INIT);

	/* Now that the device is 100% up, let's allow
	 * for the other interrupts --
	 * NOTE: this is not *yet* true since we've only allowed the 
	 * INIT interrupt on the IRQ line. We can perhaps poll
	 * the IRQ line until we know for sure the reset went through */
	isl38xx_enable_common_interrupts(priv->device_base);

	prism54_mib_init_work(priv);

	islpci_set_state(priv, PRV_STATE_READY);

	return 0;
}

int
islpci_reset(islpci_private *priv, int reload_firmware)
{
	isl38xx_control_block *cb =    /* volatile not needed */
		(isl38xx_control_block *) priv->control_block;
	unsigned counter;
	int rc;

	if (reload_firmware)
		islpci_set_state(priv, PRV_STATE_PREBOOT);
	else
		islpci_set_state(priv, PRV_STATE_POSTBOOT);

	printk(KERN_DEBUG "%s: resetting device...\n", priv->ndev->name);

	/* disable all device interrupts in case they weren't */
	isl38xx_disable_interrupts(priv->device_base);

	/* flush all management queues */
	priv->index_mgmt_tx = 0;
	priv->index_mgmt_rx = 0;

	/* clear the indexes in the frame pointer */
	for (counter = 0; counter < ISL38XX_CB_QCOUNT; counter++) {
		cb->driver_curr_frag[counter] = cpu_to_le32(0);
		cb->device_curr_frag[counter] = cpu_to_le32(0);
	}

	/* reset the mgmt receive queue */
	for (counter = 0; counter < ISL38XX_CB_MGMT_QSIZE; counter++) {
		isl38xx_fragment *frag = &cb->rx_data_mgmt[counter];
		frag->size = MGMT_FRAME_SIZE;
		frag->flags = 0;
		frag->address = priv->mgmt_rx[counter].pci_addr;
	}

	for (counter = 0; counter < ISL38XX_CB_RX_QSIZE; counter++) {
		cb->rx_data_low[counter].address =
		    cpu_to_le32((u32) priv->pci_map_rx_address[counter]);
	}

	/* since the receive queues are filled with empty fragments, now we can
	 * set the corresponding indexes in the Control Block */
	priv->control_block->driver_curr_frag[ISL38XX_CB_RX_DATA_LQ] =
	    cpu_to_le32(ISL38XX_CB_RX_QSIZE);
	priv->control_block->driver_curr_frag[ISL38XX_CB_RX_MGMTQ] =
	    cpu_to_le32(ISL38XX_CB_MGMT_QSIZE);

	/* reset the remaining real index registers and full flags */
	priv->free_data_rx = 0;
	priv->free_data_tx = 0;
	priv->data_low_tx_full = 0;

	if (reload_firmware) { /* Should we load the firmware ? */
	/* now that the data structures are cleaned up, upload
	 * firmware and reset interface */
		rc = islpci_upload_fw(priv);
		if (rc) 
			return rc;
	}

	/* finally reset interface */
	rc = islpci_reset_if(priv);
	if (!rc) /* If successful */
		return rc;
	
	printk(KERN_DEBUG  "prism54: Your card/socket may be faulty, or IRQ line too busy :(\n");
	return rc;

}

struct net_device_stats *
islpci_statistics(struct net_device *ndev)
{
	islpci_private *priv = netdev_priv(ndev);

#if VERBOSE > SHOW_ERROR_MESSAGES
	DEBUG(SHOW_FUNCTION_CALLS, "islpci_statistics \n");
#endif

	return &priv->statistics;
}

/******************************************************************************
    Network device configuration functions
******************************************************************************/
int
islpci_alloc_memory(islpci_private *priv)
{
	int counter;

#if VERBOSE > SHOW_ERROR_MESSAGES
	printk(KERN_DEBUG "islpci_alloc_memory\n");
#endif

	/* remap the PCI device base address to accessable */
	if (!(priv->device_base =
	      ioremap(pci_resource_start(priv->pdev, 0),
		      ISL38XX_PCI_MEM_SIZE))) {
		/* error in remapping the PCI device memory address range */
		printk(KERN_ERR "PCI memory remapping failed \n");
		return -1;
	}

	/* memory layout for consistent DMA region:
	 *
	 * Area 1: Control Block for the device interface
	 * Area 2: Power Save Mode Buffer for temporary frame storage. Be aware that
	 *         the number of supported stations in the AP determines the minimal
	 *         size of the buffer !
	 */

	/* perform the allocation */
	priv->driver_mem_address = pci_alloc_consistent(priv->pdev,
							HOST_MEM_BLOCK,
							&priv->
							device_host_address);

	if (!priv->driver_mem_address) {
		/* error allocating the block of PCI memory */
		printk(KERN_ERR "%s: could not allocate DMA memory, aborting!",
		       "prism54");
		return -1;
	}

	/* assign the Control Block to the first address of the allocated area */
	priv->control_block =
	    (isl38xx_control_block *) priv->driver_mem_address;

	/* set the Power Save Buffer pointer directly behind the CB */
	priv->device_psm_buffer =
		priv->device_host_address + CONTROL_BLOCK_SIZE;

	/* make sure all buffer pointers are initialized */
	for (counter = 0; counter < ISL38XX_CB_QCOUNT; counter++) {
		priv->control_block->driver_curr_frag[counter] = cpu_to_le32(0);
		priv->control_block->device_curr_frag[counter] = cpu_to_le32(0);
	}

	priv->index_mgmt_rx = 0;
	memset(priv->mgmt_rx, 0, sizeof(priv->mgmt_rx));
	memset(priv->mgmt_tx, 0, sizeof(priv->mgmt_tx));

	/* allocate rx queue for management frames */
	if (islpci_mgmt_rx_fill(priv->ndev) < 0)
		goto out_free;

	/* now get the data rx skb's */
	memset(priv->data_low_rx, 0, sizeof (priv->data_low_rx));
	memset(priv->pci_map_rx_address, 0, sizeof (priv->pci_map_rx_address));

	for (counter = 0; counter < ISL38XX_CB_RX_QSIZE; counter++) {
		struct sk_buff *skb;

		/* allocate an sk_buff for received data frames storage
		 * each frame on receive size consists of 1 fragment
		 * include any required allignment operations */
		if (!(skb = dev_alloc_skb(MAX_FRAGMENT_SIZE_RX + 2))) {
			/* error allocating an sk_buff structure elements */
			printk(KERN_ERR "Error allocating skb.\n");
			skb = NULL;
			goto out_free;
		}
		/* add the new allocated sk_buff to the buffer array */
		priv->data_low_rx[counter] = skb;

		/* map the allocated skb data area to pci */
		priv->pci_map_rx_address[counter] =
		    pci_map_single(priv->pdev, (void *) skb->data,
				   MAX_FRAGMENT_SIZE_RX + 2,
				   PCI_DMA_FROMDEVICE);
		if (!priv->pci_map_rx_address[counter]) {
			/* error mapping the buffer to device
			   accessable memory address */
			printk(KERN_ERR "failed to map skb DMA'able\n");
			goto out_free;
		}
	}

	prism54_acl_init(&priv->acl);
	prism54_wpa_ie_init(priv);
	if (mgt_init(priv)) 
		goto out_free;

	return 0;
 out_free:
	islpci_free_memory(priv);
	return -1;
}

int
islpci_free_memory(islpci_private *priv)
{
	int counter;

	if (priv->device_base)
		iounmap(priv->device_base);
	priv->device_base = 0;

	/* free consistent DMA area... */
	if (priv->driver_mem_address)
		pci_free_consistent(priv->pdev, HOST_MEM_BLOCK,
				    priv->driver_mem_address,
				    priv->device_host_address);

	/* clear some dangling pointers */
	priv->driver_mem_address = 0;
	priv->device_host_address = 0;
	priv->device_psm_buffer = 0;
	priv->control_block = 0;

        /* clean up mgmt rx buffers */
        for (counter = 0; counter < ISL38XX_CB_MGMT_QSIZE; counter++) {
		struct islpci_membuf *buf = &priv->mgmt_rx[counter];
		if (buf->pci_addr)
			pci_unmap_single(priv->pdev, buf->pci_addr,
					 buf->size, PCI_DMA_FROMDEVICE);
		buf->pci_addr = 0;
		if (buf->mem)
			kfree(buf->mem);
		buf->size = 0;
		buf->mem = NULL;
        }

	/* clean up data rx buffers */
	for (counter = 0; counter < ISL38XX_CB_RX_QSIZE; counter++) {
		if (priv->pci_map_rx_address[counter])
			pci_unmap_single(priv->pdev,
					 priv->pci_map_rx_address[counter],
					 MAX_FRAGMENT_SIZE_RX + 2,
					 PCI_DMA_FROMDEVICE);
		priv->pci_map_rx_address[counter] = 0;

		if (priv->data_low_rx[counter])
			dev_kfree_skb(priv->data_low_rx[counter]);
		priv->data_low_rx[counter] = 0;
	}

	/* Free the acces control list and the WPA list */
	prism54_acl_clean(&priv->acl);
	prism54_wpa_ie_clean(priv);
	mgt_clean(priv);

	return 0;
}

#if 0
static void
islpci_set_multicast_list(struct net_device *dev)
{
	/* put device into promisc mode and let network layer handle it */
}
#endif

struct net_device *
islpci_setup(struct pci_dev *pdev)
{
	islpci_private *priv;
	struct net_device *ndev = alloc_etherdev(sizeof (islpci_private));

	if (!ndev)
		return ndev;

	SET_MODULE_OWNER(ndev);
	pci_set_drvdata(pdev, ndev);
#if defined(SET_NETDEV_DEV)
	SET_NETDEV_DEV(ndev, &pdev->dev);
#endif

	/* setup the structure members */
	ndev->base_addr = pci_resource_start(pdev, 0);
	ndev->irq = pdev->irq;

	/* initialize the function pointers */
	ndev->open = &islpci_open;
	ndev->stop = &islpci_close;
	ndev->get_stats = &islpci_statistics;
	ndev->get_wireless_stats = &prism54_get_wireless_stats;
	ndev->do_ioctl = &prism54_ioctl;
	ndev->wireless_handlers =
	    (struct iw_handler_def *) &prism54_handler_def;

	ndev->hard_start_xmit = &islpci_eth_transmit;
	/* ndev->set_multicast_list = &islpci_set_multicast_list; */
	ndev->addr_len = ETH_ALEN;
	ndev->set_mac_address = &prism54_set_mac_address;
	/* Get a non-zero dummy MAC address for nameif. Jean II */
	memcpy(ndev->dev_addr, dummy_mac, 6);

#ifdef HAVE_TX_TIMEOUT
	ndev->watchdog_timeo = ISLPCI_TX_TIMEOUT;
	ndev->tx_timeout = &islpci_eth_tx_timeout;
#endif

	/* allocate a private device structure to the network device  */
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->pdev = pdev;

	priv->ndev->type = (priv->iw_mode == IW_MODE_MONITOR) ?
		ARPHRD_IEEE80211: ARPHRD_ETHER;

	/* save the start and end address of the PCI memory area */
	ndev->mem_start = (unsigned long) priv->device_base;
	ndev->mem_end = ndev->mem_start + ISL38XX_PCI_MEM_SIZE;

#if VERBOSE > SHOW_ERROR_MESSAGES
	DEBUG(SHOW_TRACING, "PCI Memory remapped to 0x%p\n", priv->device_base);
#endif

	init_waitqueue_head(&priv->reset_done);

	/* init the queue read locks, process wait counter */
	sema_init(&priv->mgmt_sem, 1);
	priv->mgmt_received = NULL;
	init_waitqueue_head(&priv->mgmt_wqueue);
	sema_init(&priv->stats_sem, 1);
	spin_lock_init(&priv->slock);

	/* init state machine with off#1 state */
	priv->state = PRV_STATE_OFF;
	priv->state_off = 1;

	/* initialize workqueue's */
	INIT_WORK(&priv->stats_work,
		  (void (*)(void *)) prism54_update_stats, priv);

	priv->stats_timestamp = 0;

	/* allocate various memory areas */
	if (islpci_alloc_memory(priv))
		goto do_free_netdev;

	/* select the firmware file depending on the device id */
	switch (pdev->device) {
	case PCIDEVICE_ISL3890:
	case PCIDEVICE_3COM6001:
		strcpy(priv->firmware, ISL3890_IMAGE_FILE);
		break;
	case PCIDEVICE_ISL3877:
		strcpy(priv->firmware, ISL3877_IMAGE_FILE);
		break;

	default:
		strcpy(priv->firmware, ISL3890_IMAGE_FILE);
		break;
	}

	if (register_netdev(ndev)) {
		DEBUG(SHOW_ERROR_MESSAGES,
		      "ERROR: register_netdev() failed \n");
		goto do_islpci_free_memory;
	}

	return ndev;

      do_islpci_free_memory:
	islpci_free_memory(priv);
      do_free_netdev:
	pci_set_drvdata(pdev, 0);
	free_netdev(ndev);
	priv = 0;
	return NULL;
}

islpci_state_t
islpci_set_state(islpci_private *priv, islpci_state_t new_state)
{
	islpci_state_t old_state;

	/* lock */
	old_state = priv->state;

	/* this means either a race condition or some serious error in
	 * the driver code */
	switch (new_state) {
	case PRV_STATE_OFF:
		priv->state_off++;
	default:
		priv->state = new_state;
		break;

	case PRV_STATE_PREBOOT:
		/* there are actually many off-states, enumerated by
		 * state_off */
		if (old_state == PRV_STATE_OFF)
			priv->state_off--;

		/* only if hw_unavailable is zero now it means we either
		 * were in off#1 state, or came here from
		 * somewhere else */
		if (!priv->state_off)
			priv->state = new_state;
		break;
	};
#if 0
	printk(KERN_DEBUG "%s: state transition %d -> %d (off#%d)\n",
	       priv->ndev->name, old_state, new_state, priv->state_off);
#endif

	/* invariants */
	BUG_ON(priv->state_off < 0);
	BUG_ON(priv->state_off && (priv->state != PRV_STATE_OFF));
	BUG_ON(!priv->state_off && (priv->state == PRV_STATE_OFF));

	/* unlock */
	return old_state;
}
