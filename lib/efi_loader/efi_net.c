// SPDX-License-Identifier: GPL-2.0+
/*
 * Simple network protocol
 * PXE base code protocol
 *
 * Copyright (c) 2016 Alexander Graf
 *
 * The simple network protocol has the following statuses and services
 * to move between them:
 *
 * Start():	 EfiSimpleNetworkStopped     -> EfiSimpleNetworkStarted
 * Initialize(): EfiSimpleNetworkStarted     -> EfiSimpleNetworkInitialized
 * Shutdown():	 EfiSimpleNetworkInitialized -> EfiSimpleNetworkStarted
 * Stop():	 EfiSimpleNetworkStarted     -> EfiSimpleNetworkStopped
 * Reset():	 EfiSimpleNetworkInitialized -> EfiSimpleNetworkInitialized
 */

#define LOG_CATEGORY LOGC_EFI

#include <efi_loader.h>
#include <dm.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <vsprintf.h>
#include <net.h>

static const efi_guid_t efi_net_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static const efi_guid_t efi_pxe_base_code_protocol_guid =
					EFI_PXE_BASE_CODE_PROTOCOL_GUID;
static struct efi_pxe_packet *dhcp_ack;
static void *new_tx_packet;
static void *transmit_buffer;
static uchar **receive_buffer;
static size_t *receive_lengths;
static int rx_packet_idx;
static int rx_packet_num;
static struct efi_net_obj *netobj;

/*
 * The current network device path. This device path is updated when a new
 * bootfile is downloaded from the network. If then the bootfile is loaded
 * as an efi image, net_dp is passed as the device path of the loaded image.
 */
static struct efi_device_path *net_dp;

static struct wget_http_info efi_wget_info = {
	.set_bootdev = false,
	.check_buffer_size = true,

};

/*
 * The notification function of this event is called in every timer cycle
 * to check if a new network packet has been received.
 */
static struct efi_event *network_timer_event;
/*
 * This event is signaled when a packet has been received.
 */
static struct efi_event *wait_for_packet;

/**
 * struct efi_net_obj - EFI object representing a network interface
 *
 * @header:			EFI object header
 * @net:			simple network protocol interface
 * @net_mode:			status of the network interface
 * @pxe:			PXE base code protocol interface
 * @pxe_mode:			status of the PXE base code protocol
 * @ip4_config2:		IP4 Config2 protocol interface
 * @http_service_binding:	Http service binding protocol interface
 */
struct efi_net_obj {
	struct efi_object header;
	struct efi_simple_network net;
	struct efi_simple_network_mode net_mode;
	struct efi_pxe_base_code_protocol pxe;
	struct efi_pxe_mode pxe_mode;
#if IS_ENABLED(CONFIG_EFI_IP4_CONFIG2_PROTOCOL)
	struct efi_ip4_config2_protocol ip4_config2;
#endif
#if IS_ENABLED(CONFIG_EFI_HTTP_PROTOCOL)
	struct efi_service_binding_protocol http_service_binding;
#endif
};

/*
 * efi_net_start() - start the network interface
 *
 * This function implements the Start service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_start(struct efi_simple_network *this)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p", this);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (this->mode->state != EFI_NETWORK_STOPPED) {
		ret = EFI_ALREADY_STARTED;
	} else {
		this->int_status = 0;
		wait_for_packet->is_signaled = false;
		this->mode->state = EFI_NETWORK_STARTED;
	}
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_stop() - stop the network interface
 *
 * This function implements the Stop service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_stop(struct efi_simple_network *this)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p", this);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (this->mode->state == EFI_NETWORK_STOPPED) {
		ret = EFI_NOT_STARTED;
	} else {
		/* Disable hardware and put it into the reset state */
		eth_halt();
		/* Clear cache of packets */
		rx_packet_num = 0;
		this->mode->state = EFI_NETWORK_STOPPED;
	}
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_initialize() - initialize the network interface
 *
 * This function implements the Initialize service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @extra_rx:	extra receive buffer to be allocated
 * @extra_tx:	extra transmit buffer to be allocated
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_initialize(struct efi_simple_network *this,
					      ulong extra_rx, ulong extra_tx)
{
	int ret;
	efi_status_t r = EFI_SUCCESS;

	EFI_ENTRY("%p, %lx, %lx", this, extra_rx, extra_tx);

	/* Check parameters */
	if (!this) {
		r = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
	case EFI_NETWORK_STARTED:
		break;
	default:
		r = EFI_NOT_STARTED;
		goto out;
	}

	/* Setup packet buffers */
	net_init();
	/* Disable hardware and put it into the reset state */
	eth_halt();
	/* Clear cache of packets */
	rx_packet_num = 0;
	/* Set current device according to environment variables */
	eth_set_current();
	/* Get hardware ready for send and receive operations */
	ret = eth_init();
	if (ret < 0) {
		eth_halt();
		this->mode->state = EFI_NETWORK_STOPPED;
		r = EFI_DEVICE_ERROR;
		goto out;
	} else {
		this->int_status = 0;
		wait_for_packet->is_signaled = false;
		this->mode->state = EFI_NETWORK_INITIALIZED;
	}
out:
	return EFI_EXIT(r);
}

/*
 * efi_net_reset() - reinitialize the network interface
 *
 * This function implements the Reset service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:			pointer to the protocol instance
 * @extended_verification:	execute exhaustive verification
 * Return:			status code
 */
static efi_status_t EFIAPI efi_net_reset(struct efi_simple_network *this,
					 int extended_verification)
{
	efi_status_t ret;

	EFI_ENTRY("%p, %x", this, extended_verification);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
		break;
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	default:
		ret = EFI_DEVICE_ERROR;
		goto out;
	}

	this->mode->state = EFI_NETWORK_STARTED;
	ret = EFI_CALL(efi_net_initialize(this, 0, 0));
out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_shutdown() - shut down the network interface
 *
 * This function implements the Shutdown service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_shutdown(struct efi_simple_network *this)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p", this);

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
		break;
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	default:
		ret = EFI_DEVICE_ERROR;
		goto out;
	}

	eth_halt();
	this->int_status = 0;
	wait_for_packet->is_signaled = false;
	this->mode->state = EFI_NETWORK_STARTED;

out:
	return EFI_EXIT(ret);
}

/*
 * efi_net_receive_filters() - mange multicast receive filters
 *
 * This function implements the ReceiveFilters service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:		pointer to the protocol instance
 * @enable:		bit mask of receive filters to enable
 * @disable:		bit mask of receive filters to disable
 * @reset_mcast_filter:	true resets contents of the filters
 * @mcast_filter_count:	number of hardware MAC addresses in the new filters list
 * @mcast_filter:	list of new filters
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_receive_filters
		(struct efi_simple_network *this, u32 enable, u32 disable,
		 int reset_mcast_filter, ulong mcast_filter_count,
		 struct efi_mac_address *mcast_filter)
{
	EFI_ENTRY("%p, %x, %x, %x, %lx, %p", this, enable, disable,
		  reset_mcast_filter, mcast_filter_count, mcast_filter);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_station_address() - set the hardware MAC address
 *
 * This function implements the StationAddress service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @reset:	if true reset the address to default
 * @new_mac:	new MAC address
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_station_address
		(struct efi_simple_network *this, int reset,
		 struct efi_mac_address *new_mac)
{
	EFI_ENTRY("%p, %x, %p", this, reset, new_mac);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_statistics() - reset or collect statistics of the network interface
 *
 * This function implements the Statistics service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @reset:	if true, the statistics are reset
 * @stat_size:	size of the statistics table
 * @stat_table:	table to receive the statistics
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_statistics(struct efi_simple_network *this,
					      int reset, ulong *stat_size,
					      void *stat_table)
{
	EFI_ENTRY("%p, %x, %p, %p", this, reset, stat_size, stat_table);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/*
 * efi_net_mcastiptomac() - translate multicast IP address to MAC address
 *
 * This function implements the MCastIPtoMAC service of the
 * EFI_SIMPLE_NETWORK_PROTOCOL. See the Unified Extensible Firmware Interface
 * (UEFI) specification for details.
 *
 * @this:	pointer to the protocol instance
 * @ipv6:	true if the IP address is an IPv6 address
 * @ip:		IP address
 * @mac:	MAC address
 * Return:	status code
 */
static efi_status_t EFIAPI efi_net_mcastiptomac(struct efi_simple_network *this,
						int ipv6,
						struct efi_ip_address *ip,
						struct efi_mac_address *mac)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p, %x, %p, %p", this, ipv6, ip, mac);

	if (!this || !ip || !mac) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	if (ipv6) {
		ret = EFI_UNSUPPORTED;
		goto out;
	}

	/* Multi-cast addresses are in the range 224.0.0.0 - 239.255.255.255 */
	if ((ip->ip_addr[0] & 0xf0) != 0xe0) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	};

	switch (this->mode->state) {
	case EFI_NETWORK_INITIALIZED:
	case EFI_NETWORK_STARTED:
		break;
	default:
		ret = EFI_NOT_STARTED;
		goto out;
	}

	memset(mac, 0, sizeof(struct efi_mac_address));

	/*
	 * Copy lower 23 bits of IPv4 multi-cast address
	 * RFC 1112, RFC 7042 2.1.1.
	 */
	mac->mac_addr[0] = 0x01;
	mac->mac_addr[1] = 0x00;
	mac->mac_addr[2] = 0x5E;
	mac->mac_addr[3] = ip->ip_addr[1] & 0x7F;
	mac->mac_addr[4] = ip->ip_addr[2];
	mac->mac_addr[5] = ip->ip_addr[3];
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_nvdata() - read or write NVRAM
 *
 * This function implements the GetStatus service of the Simple Network
 * Protocol. See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @read_write:		true for read, false for write
 * @offset:		offset in NVRAM
 * @buffer_size:	size of buffer
 * @buffer:		buffer
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_nvdata(struct efi_simple_network *this,
					  int read_write, ulong offset,
					  ulong buffer_size, char *buffer)
{
	EFI_ENTRY("%p, %x, %lx, %lx, %p", this, read_write, offset, buffer_size,
		  buffer);

	return EFI_EXIT(EFI_UNSUPPORTED);
}

/**
 * efi_net_get_status() - get interrupt status
 *
 * This function implements the GetStatus service of the Simple Network
 * Protocol. See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @int_status:		interface status
 * @txbuf:		transmission buffer
 */
static efi_status_t EFIAPI efi_net_get_status(struct efi_simple_network *this,
					      u32 *int_status, void **txbuf)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p, %p, %p", this, int_status, txbuf);

	efi_timer_check();

	/* Check parameters */
	if (!this) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	if (int_status) {
		*int_status = this->int_status;
		this->int_status = 0;
	}
	if (txbuf)
		*txbuf = new_tx_packet;

	new_tx_packet = NULL;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_transmit() - transmit a packet
 *
 * This function implements the Transmit service of the Simple Network Protocol.
 * See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @header_size:	size of the media header
 * @buffer_size:	size of the buffer to receive the packet
 * @buffer:		buffer to receive the packet
 * @src_addr:		source hardware MAC address
 * @dest_addr:		destination hardware MAC address
 * @protocol:		type of header to build
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_transmit
		(struct efi_simple_network *this, size_t header_size,
		 size_t buffer_size, void *buffer,
		 struct efi_mac_address *src_addr,
		 struct efi_mac_address *dest_addr, u16 *protocol)
{
	efi_status_t ret = EFI_SUCCESS;

	EFI_ENTRY("%p, %lu, %lu, %p, %p, %p, %p", this,
		  (unsigned long)header_size, (unsigned long)buffer_size,
		  buffer, src_addr, dest_addr, protocol);

	efi_timer_check();

	/* Check parameters */
	if (!this || !buffer) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	/* We do not support jumbo packets */
	if (buffer_size > PKTSIZE_ALIGN) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	/* At least the IP header has to fit into the buffer */
	if (buffer_size < this->mode->media_header_size) {
		ret = EFI_BUFFER_TOO_SMALL;
		goto out;
	}

	/*
	 * TODO:
	 * Support VLANs. Use net_set_ether() for copying the header. Use a
	 * U_BOOT_ENV_CALLBACK to update the media header size.
	 */
	if (header_size) {
		struct ethernet_hdr *header = buffer;

		if (!dest_addr || !protocol ||
		    header_size != this->mode->media_header_size) {
			ret = EFI_INVALID_PARAMETER;
			goto out;
		}
		if (!src_addr)
			src_addr = &this->mode->current_address;

		memcpy(header->et_dest, dest_addr, ARP_HLEN);
		memcpy(header->et_src, src_addr, ARP_HLEN);
		header->et_protlen = htons(*protocol);
	}

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	/* Ethernet packets always fit, just bounce */
	memcpy(transmit_buffer, buffer, buffer_size);
	net_send_packet(transmit_buffer, buffer_size);

	new_tx_packet = buffer;
	this->int_status |= EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_receive() - receive a packet from a network interface
 *
 * This function implements the Receive service of the Simple Network Protocol.
 * See the UEFI spec for details.
 *
 * @this:		the instance of the Simple Network Protocol
 * @header_size:	size of the media header
 * @buffer_size:	size of the buffer to receive the packet
 * @buffer:		buffer to receive the packet
 * @src_addr:		source MAC address
 * @dest_addr:		destination MAC address
 * @protocol:		protocol
 * Return:		status code
 */
static efi_status_t EFIAPI efi_net_receive
		(struct efi_simple_network *this, size_t *header_size,
		 size_t *buffer_size, void *buffer,
		 struct efi_mac_address *src_addr,
		 struct efi_mac_address *dest_addr, u16 *protocol)
{
	efi_status_t ret = EFI_SUCCESS;
	struct ethernet_hdr *eth_hdr;
	size_t hdr_size = sizeof(struct ethernet_hdr);
	u16 protlen;

	EFI_ENTRY("%p, %p, %p, %p, %p, %p, %p", this, header_size,
		  buffer_size, buffer, src_addr, dest_addr, protocol);

	/* Execute events */
	efi_timer_check();

	/* Check parameters */
	if (!this || !buffer || !buffer_size) {
		ret = EFI_INVALID_PARAMETER;
		goto out;
	}

	switch (this->mode->state) {
	case EFI_NETWORK_STOPPED:
		ret = EFI_NOT_STARTED;
		goto out;
	case EFI_NETWORK_STARTED:
		ret = EFI_DEVICE_ERROR;
		goto out;
	default:
		break;
	}

	if (!rx_packet_num) {
		ret = EFI_NOT_READY;
		goto out;
	}
	/* Fill export parameters */
	eth_hdr = (struct ethernet_hdr *)receive_buffer[rx_packet_idx];
	protlen = ntohs(eth_hdr->et_protlen);
	if (protlen == 0x8100) {
		hdr_size += 4;
		protlen = ntohs(*(u16 *)&receive_buffer[rx_packet_idx][hdr_size - 2]);
	}
	if (header_size)
		*header_size = hdr_size;
	if (dest_addr)
		memcpy(dest_addr, eth_hdr->et_dest, ARP_HLEN);
	if (src_addr)
		memcpy(src_addr, eth_hdr->et_src, ARP_HLEN);
	if (protocol)
		*protocol = protlen;
	if (*buffer_size < receive_lengths[rx_packet_idx]) {
		/* Packet doesn't fit, try again with bigger buffer */
		*buffer_size = receive_lengths[rx_packet_idx];
		ret = EFI_BUFFER_TOO_SMALL;
		goto out;
	}
	/* Copy packet */
	memcpy(buffer, receive_buffer[rx_packet_idx],
	       receive_lengths[rx_packet_idx]);
	*buffer_size = receive_lengths[rx_packet_idx];
	rx_packet_idx = (rx_packet_idx + 1) % ETH_PACKETS_BATCH_RECV;
	rx_packet_num--;
	if (rx_packet_num)
		wait_for_packet->is_signaled = true;
	else
		this->int_status &= ~EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
out:
	return EFI_EXIT(ret);
}

/**
 * efi_net_set_dhcp_ack() - take note of a selected DHCP IP address
 *
 * This function is called by dhcp_handler().
 *
 * @pkt:	packet received by dhcp_handler()
 * @len:	length of the packet received
 */
void efi_net_set_dhcp_ack(void *pkt, int len)
{
	int maxsize = sizeof(*dhcp_ack);

	if (!dhcp_ack) {
		dhcp_ack = malloc(maxsize);
		if (!dhcp_ack)
			return;
	}
	memset(dhcp_ack, 0, maxsize);
	memcpy(dhcp_ack, pkt, min(len, maxsize));

	if (netobj)
		netobj->pxe_mode.dhcp_ack = *dhcp_ack;
}

/**
 * efi_net_push() - callback for received network packet
 *
 * This function is called when a network packet is received by eth_rx().
 *
 * @pkt:	network packet
 * @len:	length
 */
static void efi_net_push(void *pkt, int len)
{
	int rx_packet_next;

	/* Check that we at least received an Ethernet header */
	if (len < sizeof(struct ethernet_hdr))
		return;

	/* Check that the buffer won't overflow */
	if (len > PKTSIZE_ALIGN)
		return;

	/* Can't store more than pre-alloced buffer */
	if (rx_packet_num >= ETH_PACKETS_BATCH_RECV)
		return;

	rx_packet_next = (rx_packet_idx + rx_packet_num) %
	    ETH_PACKETS_BATCH_RECV;
	memcpy(receive_buffer[rx_packet_next], pkt, len);
	receive_lengths[rx_packet_next] = len;

	rx_packet_num++;
}

/**
 * efi_network_timer_notify() - check if a new network packet has been received
 *
 * This notification function is called in every timer cycle.
 *
 * @event:	the event for which this notification function is registered
 * @context:	event context - not used in this function
 */
static void EFIAPI efi_network_timer_notify(struct efi_event *event,
					    void *context)
{
	struct efi_simple_network *this = (struct efi_simple_network *)context;

	EFI_ENTRY("%p, %p", event, context);

	/*
	 * Some network drivers do not support calling eth_rx() before
	 * initialization.
	 */
	if (!this || this->mode->state != EFI_NETWORK_INITIALIZED)
		goto out;

	if (!rx_packet_num) {
		push_packet = efi_net_push;
		eth_rx();
		push_packet = NULL;
		if (rx_packet_num) {
			this->int_status |=
				EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT;
			wait_for_packet->is_signaled = true;
		}
	}
out:
	EFI_EXIT(EFI_SUCCESS);
}

static efi_status_t EFIAPI efi_pxe_base_code_start(
				struct efi_pxe_base_code_protocol *this,
				u8 use_ipv6)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_stop(
				struct efi_pxe_base_code_protocol *this)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_dhcp(
				struct efi_pxe_base_code_protocol *this,
				u8 sort_offers)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_discover(
				struct efi_pxe_base_code_protocol *this,
				u16 type, u16 *layer, u8 bis,
				struct efi_pxe_base_code_discover_info *info)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_mtftp(
				struct efi_pxe_base_code_protocol *this,
				u32 operation, void *buffer_ptr,
				u8 overwrite, efi_uintn_t *buffer_size,
				struct efi_ip_address server_ip, char *filename,
				struct efi_pxe_base_code_mtftp_info *info,
				u8 dont_use_buffer)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_udp_write(
				struct efi_pxe_base_code_protocol *this,
				u16 op_flags, struct efi_ip_address *dest_ip,
				u16 *dest_port,
				struct efi_ip_address *gateway_ip,
				struct efi_ip_address *src_ip, u16 *src_port,
				efi_uintn_t *header_size, void *header_ptr,
				efi_uintn_t *buffer_size, void *buffer_ptr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_udp_read(
				struct efi_pxe_base_code_protocol *this,
				u16 op_flags, struct efi_ip_address *dest_ip,
				u16 *dest_port, struct efi_ip_address *src_ip,
				u16 *src_port, efi_uintn_t *header_size,
				void *header_ptr, efi_uintn_t *buffer_size,
				void *buffer_ptr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_ip_filter(
				struct efi_pxe_base_code_protocol *this,
				struct efi_pxe_base_code_filter *new_filter)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_arp(
				struct efi_pxe_base_code_protocol *this,
				struct efi_ip_address *ip_addr,
				struct efi_mac_address *mac_addr)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_parameters(
				struct efi_pxe_base_code_protocol *this,
				u8 *new_auto_arp, u8 *new_send_guid,
				u8 *new_ttl, u8 *new_tos,
				u8 *new_make_callback)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_station_ip(
				struct efi_pxe_base_code_protocol *this,
				struct efi_ip_address *new_station_ip,
				struct efi_ip_address *new_subnet_mask)
{
	return EFI_UNSUPPORTED;
}

static efi_status_t EFIAPI efi_pxe_base_code_set_packets(
				struct efi_pxe_base_code_protocol *this,
				u8 *new_dhcp_discover_valid,
				u8 *new_dhcp_ack_received,
				u8 *new_proxy_offer_received,
				u8 *new_pxe_discover_valid,
				u8 *new_pxe_reply_received,
				u8 *new_pxe_bis_reply_received,
				EFI_PXE_BASE_CODE_PACKET *new_dchp_discover,
				EFI_PXE_BASE_CODE_PACKET *new_dhcp_acc,
				EFI_PXE_BASE_CODE_PACKET *new_proxy_offer,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_discover,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_reply,
				EFI_PXE_BASE_CODE_PACKET *new_pxe_bis_reply)
{
	return EFI_UNSUPPORTED;
}

/**
 * efi_net_register() - register the simple network protocol
 *
 * This gets called from do_bootefi_exec().
 */
efi_status_t efi_net_register(void)
{
	efi_status_t r;
	int i;

	if (!eth_get_dev()) {
		/* No network device active, don't expose any */
		return EFI_SUCCESS;
	}

	/* We only expose the "active" network device, so one is enough */
	netobj = calloc(1, sizeof(*netobj));
	if (!netobj)
		goto out_of_resources;

	/* Allocate an aligned transmit buffer */
	transmit_buffer = calloc(1, PKTSIZE_ALIGN + PKTALIGN);
	if (!transmit_buffer)
		goto out_of_resources;
	transmit_buffer = (void *)ALIGN((uintptr_t)transmit_buffer, PKTALIGN);

	/* Allocate a number of receive buffers */
	receive_buffer = calloc(ETH_PACKETS_BATCH_RECV,
				sizeof(*receive_buffer));
	if (!receive_buffer)
		goto out_of_resources;
	for (i = 0; i < ETH_PACKETS_BATCH_RECV; i++) {
		receive_buffer[i] = malloc(PKTSIZE_ALIGN);
		if (!receive_buffer[i])
			goto out_of_resources;
	}
	receive_lengths = calloc(ETH_PACKETS_BATCH_RECV,
				 sizeof(*receive_lengths));
	if (!receive_lengths)
		goto out_of_resources;

	/* Hook net up to the device list */
	efi_add_handle(&netobj->header);

	/* Fill in object data */
	r = efi_add_protocol(&netobj->header, &efi_net_guid,
			     &netobj->net);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
	if (!net_dp)
		efi_net_set_dp("Net", NULL);
	r = efi_add_protocol(&netobj->header, &efi_guid_device_path,
			     net_dp);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
	r = efi_add_protocol(&netobj->header, &efi_pxe_base_code_protocol_guid,
			     &netobj->pxe);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
	netobj->net.revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
	netobj->net.start = efi_net_start;
	netobj->net.stop = efi_net_stop;
	netobj->net.initialize = efi_net_initialize;
	netobj->net.reset = efi_net_reset;
	netobj->net.shutdown = efi_net_shutdown;
	netobj->net.receive_filters = efi_net_receive_filters;
	netobj->net.station_address = efi_net_station_address;
	netobj->net.statistics = efi_net_statistics;
	netobj->net.mcastiptomac = efi_net_mcastiptomac;
	netobj->net.nvdata = efi_net_nvdata;
	netobj->net.get_status = efi_net_get_status;
	netobj->net.transmit = efi_net_transmit;
	netobj->net.receive = efi_net_receive;
	netobj->net.mode = &netobj->net_mode;
	netobj->net_mode.state = EFI_NETWORK_STOPPED;
	memcpy(netobj->net_mode.current_address.mac_addr, eth_get_ethaddr(), 6);
	netobj->net_mode.hwaddr_size = ARP_HLEN;
	netobj->net_mode.media_header_size = ETHER_HDR_SIZE;
	netobj->net_mode.max_packet_size = PKTSIZE;
	netobj->net_mode.if_type = ARP_ETHER;

	netobj->pxe.revision = EFI_PXE_BASE_CODE_PROTOCOL_REVISION;
	netobj->pxe.start = efi_pxe_base_code_start;
	netobj->pxe.stop = efi_pxe_base_code_stop;
	netobj->pxe.dhcp = efi_pxe_base_code_dhcp;
	netobj->pxe.discover = efi_pxe_base_code_discover;
	netobj->pxe.mtftp = efi_pxe_base_code_mtftp;
	netobj->pxe.udp_write = efi_pxe_base_code_udp_write;
	netobj->pxe.udp_read = efi_pxe_base_code_udp_read;
	netobj->pxe.set_ip_filter = efi_pxe_base_code_set_ip_filter;
	netobj->pxe.arp = efi_pxe_base_code_arp;
	netobj->pxe.set_parameters = efi_pxe_base_code_set_parameters;
	netobj->pxe.set_station_ip = efi_pxe_base_code_set_station_ip;
	netobj->pxe.set_packets = efi_pxe_base_code_set_packets;
	netobj->pxe.mode = &netobj->pxe_mode;
	if (dhcp_ack)
		netobj->pxe_mode.dhcp_ack = *dhcp_ack;

	/*
	 * Create WaitForPacket event.
	 */
	r = efi_create_event(EVT_NOTIFY_WAIT, TPL_CALLBACK,
			     efi_network_timer_notify, NULL, NULL,
			     &wait_for_packet);
	if (r != EFI_SUCCESS) {
		printf("ERROR: Failed to register network event\n");
		return r;
	}
	netobj->net.wait_for_packet = wait_for_packet;
	/*
	 * Create a timer event.
	 *
	 * The notification function is used to check if a new network packet
	 * has been received.
	 *
	 * iPXE is running at TPL_CALLBACK most of the time. Use a higher TPL.
	 */
	r = efi_create_event(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
			     efi_network_timer_notify, &netobj->net, NULL,
			     &network_timer_event);
	if (r != EFI_SUCCESS) {
		printf("ERROR: Failed to register network event\n");
		return r;
	}
	/* Network is time critical, create event in every timer cycle */
	r = efi_set_timer(network_timer_event, EFI_TIMER_PERIODIC, 0);
	if (r != EFI_SUCCESS) {
		printf("ERROR: Failed to set network timer\n");
		return r;
	}

#if IS_ENABLED(CONFIG_EFI_IP4_CONFIG2_PROTOCOL)
	r = efi_ipconfig_register(&netobj->header, &netobj->ip4_config2);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
#endif

#ifdef CONFIG_EFI_HTTP_PROTOCOL
	r = efi_http_register(&netobj->header, &netobj->http_service_binding);
	if (r != EFI_SUCCESS)
		goto failure_to_add_protocol;
	/*
	 * No harm on doing the following. If the PXE handle is present, the client could
	 * find it and try to get its IP address from it. In here the PXE handle is present
	 * but the PXE protocol is not yet implmenented, so we add this in the meantime.
	 */
	efi_net_get_addr((struct efi_ipv4_address *)&netobj->pxe_mode.station_ip,
			 (struct efi_ipv4_address *)&netobj->pxe_mode.subnet_mask, NULL);
#endif

	return EFI_SUCCESS;
failure_to_add_protocol:
	printf("ERROR: Failure to add protocol\n");
	return r;
out_of_resources:
	free(netobj);
	netobj = NULL;
	free(transmit_buffer);
	if (receive_buffer)
		for (i = 0; i < ETH_PACKETS_BATCH_RECV; i++)
			free(receive_buffer[i]);
	free(receive_buffer);
	free(receive_lengths);
	printf("ERROR: Out of memory\n");
	return EFI_OUT_OF_RESOURCES;
}

/**
 * efi_net_set_dp() - set device path of efi net device
 *
 * This gets called to update the device path when a new boot
 * file is downloaded
 *
 * @dev:	dev to set the device path from
 * @server:	remote server address
 * Return:	status code
 */
efi_status_t efi_net_set_dp(const char *dev, const char *server)
{
	efi_free_pool(net_dp);

	net_dp = NULL;
	if (!strcmp(dev, "Net"))
		net_dp = efi_dp_from_eth();
	else if (!strcmp(dev, "Http"))
		net_dp = efi_dp_from_http(server);

	if (!net_dp)
		return EFI_OUT_OF_RESOURCES;

	return EFI_SUCCESS;
}

/**
 * efi_net_get_dp() - get device path of efi net device
 *
 * Produce a copy of the current device path
 *
 * @dp:		copy of the current device path, or NULL on error
 */
void efi_net_get_dp(struct efi_device_path **dp)
{
	if (!dp)
		return;
	if (!net_dp)
		efi_net_set_dp("Net", NULL);
	if (net_dp)
		*dp = efi_dp_dup(net_dp);
}

/**
 * efi_net_get_addr() - get IP address information
 *
 * Copy the current IP address, mask, and gateway into the
 * efi_ipv4_address structs pointed to by ip, mask and gw,
 * respectively.
 *
 * @ip:		pointer to an efi_ipv4_address struct to
 *		be filled with the current IP address
 * @mask:	pointer to an efi_ipv4_address struct to
 *		be filled with the current network mask
 * @gw:		pointer to an efi_ipv4_address struct to be
 *		filled with the current network gateway
 */
void efi_net_get_addr(struct efi_ipv4_address *ip,
		      struct efi_ipv4_address *mask,
		      struct efi_ipv4_address *gw)
{
#ifdef CONFIG_NET_LWIP
	char ipstr[] = "ipaddr\0\0";
	char maskstr[] = "netmask\0\0";
	char gwstr[] = "gatewayip\0\0";
	int idx;
	struct in_addr tmp;
	char *env;

	idx = dev_seq(eth_get_dev());

	if (idx < 0 || idx > 99) {
		log_err("unexpected idx %d\n", idx);
		return;
	}

	if (idx) {
		sprintf(ipstr, "ipaddr%d", idx);
		sprintf(maskstr, "netmask%d", idx);
		sprintf(gwstr, "gatewayip%d", idx);
	}

	env = env_get(ipstr);
	if (env && ip) {
		tmp = string_to_ip(env);
		memcpy(ip, &tmp, sizeof(tmp));
	}

	env = env_get(maskstr);
	if (env && mask) {
		tmp = string_to_ip(env);
		memcpy(mask, &tmp, sizeof(tmp));
	}
	env = env_get(gwstr);
	if (env && gw) {
		tmp = string_to_ip(env);
		memcpy(gw, &tmp, sizeof(tmp));
	}
#else
	if (ip)
		memcpy(ip, &net_ip, sizeof(net_ip));
	if (mask)
		memcpy(mask, &net_netmask, sizeof(net_netmask));
#endif
}

/**
 * efi_net_set_addr() - set IP address information
 *
 * Set the current IP address, mask, and gateway to the
 * efi_ipv4_address structs pointed to by ip, mask and gw,
 * respectively.
 *
 * @ip:		pointer to new IP address
 * @mask:	pointer to new network mask to set
 * @gw:		pointer to new network gateway
 */
void efi_net_set_addr(struct efi_ipv4_address *ip,
		      struct efi_ipv4_address *mask,
		      struct efi_ipv4_address *gw)
{
#ifdef CONFIG_NET_LWIP
	char ipstr[] = "ipaddr\0\0";
	char maskstr[] = "netmask\0\0";
	char gwstr[] = "gatewayip\0\0";
	int idx;
	struct in_addr *addr;
	char tmp[46];

	idx = dev_seq(eth_get_dev());

	if (idx < 0 || idx > 99) {
		log_err("unexpected idx %d\n", idx);
		return;
	}

	if (idx) {
		sprintf(ipstr, "ipaddr%d", idx);
		sprintf(maskstr, "netmask%d", idx);
		sprintf(gwstr, "gatewayip%d", idx);
	}

	if (ip) {
		addr = (struct in_addr *)ip;
		ip_to_string(*addr, tmp);
		env_set(ipstr, tmp);
	}

	if (mask) {
		addr = (struct in_addr *)mask;
		ip_to_string(*addr, tmp);
		env_set(maskstr, tmp);
	}

	if (gw) {
		addr = (struct in_addr *)gw;
		ip_to_string(*addr, tmp);
		env_set(gwstr, tmp);
	}
#else
	if (ip)
		memcpy(&net_ip, ip, sizeof(*ip));
	if (mask)
		memcpy(&net_netmask, mask, sizeof(*mask));
#endif
}

/**
 * efi_net_set_buffer() - allocate a buffer of min 64K
 *
 * @buffer:	allocated buffer
 * @size:	desired buffer size
 * Return:	status code
 */
static efi_status_t efi_net_set_buffer(void **buffer, size_t size)
{
	efi_status_t ret = EFI_SUCCESS;

	if (size < SZ_64K)
		size = SZ_64K;

	*buffer = efi_alloc(size);
	if (!*buffer)
		ret = EFI_OUT_OF_RESOURCES;

	efi_wget_info.buffer_size = (ulong)size;

	return ret;
}

/**
 * efi_net_parse_headers() - parse HTTP headers
 *
 * Parses the raw buffer efi_wget_info.headers into an array headers
 * of efi structs http_headers. The array should be at least
 * MAX_HTTP_HEADERS long.
 *
 * @num_headers:	number of headers
 * @headers:		caller provided array of struct http_headers
 */
void efi_net_parse_headers(ulong *num_headers, struct http_header *headers)
{
	if (!num_headers || !headers)
		return;

	// Populate info with http headers.
	*num_headers = 0;
	const uchar *line_start = efi_wget_info.headers;
	const uchar *line_end;
	ulong count;
	struct http_header *current_header;
	const uchar *separator;
	size_t name_length, value_length;

	// Skip the first line (request or status line)
	line_end = strstr(line_start, "\r\n");

	if (line_end)
		line_start = line_end + 2;

	while ((line_end = strstr(line_start, "\r\n")) != NULL) {
		count = *num_headers;
		if (line_start == line_end || count >= MAX_HTTP_HEADERS)
			break;
		current_header = headers + count;
		separator = strchr(line_start, ':');
		if (separator) {
			name_length = separator - line_start;
			++separator;
			while (*separator == ' ')
				++separator;
			value_length = line_end - separator;
			if (name_length < MAX_HTTP_HEADER_NAME &&
			    value_length < MAX_HTTP_HEADER_VALUE) {
				strncpy(current_header->name, line_start, name_length);
				current_header->name[name_length] = '\0';
				strncpy(current_header->value, separator, value_length);
				current_header->value[value_length] = '\0';
				(*num_headers)++;
			}
		}
		line_start = line_end + 2;
	}
}

/**
 * efi_net_do_request() - issue an HTTP request using wget
 *
 * @url:		url
 * @method:		HTTP method
 * @buffer:		data buffer
 * @status_code:	HTTP status code
 * @file_size:		file size in bytes
 * @headers_buffer:	headers buffer
 * Return:		status code
 */
efi_status_t efi_net_do_request(u8 *url, enum efi_http_method method, void **buffer,
				u32 *status_code, ulong *file_size, char *headers_buffer)
{
	efi_status_t ret = EFI_SUCCESS;
	int wget_ret;
	static bool last_head;

	if (!buffer || !file_size)
		return EFI_ABORTED;

	efi_wget_info.method = (enum wget_http_method)method;
	efi_wget_info.headers = headers_buffer;

	switch (method) {
	case HTTP_METHOD_GET:
		ret = efi_net_set_buffer(buffer, last_head ? (size_t)efi_wget_info.hdr_cont_len : 0);
		if (ret != EFI_SUCCESS)
			goto out;
		wget_ret = wget_request((ulong)*buffer, url, &efi_wget_info);
		if ((ulong)efi_wget_info.hdr_cont_len > efi_wget_info.buffer_size) {
			// Try again with updated buffer size
			efi_free_pool(*buffer);
			ret = efi_net_set_buffer(buffer, (size_t)efi_wget_info.hdr_cont_len);
			if (ret != EFI_SUCCESS)
				goto out;
			if (wget_request((ulong)*buffer, url, &efi_wget_info)) {
				efi_free_pool(*buffer);
				ret = EFI_DEVICE_ERROR;
				goto out;
			}
		} else if (wget_ret) {
			efi_free_pool(*buffer);
			ret = EFI_DEVICE_ERROR;
			goto out;
		}
		// Pass the actual number of received bytes to the application
		*file_size = efi_wget_info.file_size;
		*status_code = efi_wget_info.status_code;
		last_head = false;
		break;
	case HTTP_METHOD_HEAD:
		ret = efi_net_set_buffer(buffer, 0);
		if (ret != EFI_SUCCESS)
			goto out;
		wget_request((ulong)*buffer, url, &efi_wget_info);
		*file_size = 0;
		*status_code = efi_wget_info.status_code;
		last_head = true;
		break;
	default:
		ret = EFI_UNSUPPORTED;
		break;
	}

out:
	return ret;
}
