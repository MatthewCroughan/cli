/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>

#include <openthread/config.h>

#include <openthread/cli.h>
#include <openthread/diag.h>
#include <openthread/logging.h>
#include <openthread/platform/logging.h>

#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/tasklet.h"
#include "openthread/ip6.h"
#include "openthread/mqttsn.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "openthread-system.h"

#if defined(CONFIG_BT)
#include "ble.h"
#endif

#if defined(CONFIG_CLI_SAMPLE_LOW_POWER)
#include "low_power.h"
#endif

#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>

#define NETWORK_NAME "OTBR4444"
#define PANID 0x4444
#define EXTPANID {0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44}
#define DEFAULT_CHANNEL 15
#define MASTER_KEY {0x33, 0x33, 0x44, 0x44, 0x33, 0x33, 0x44, 0x44, 0x33, 0x33, 0x44, 0x44, 0x33, 0x33, 0x44, 0x44}

#define GATEWAY_MULTICAST_PORT 10000
#define GATEWAY_MULTICAST_ADDRESS "ff03::1"
#define GATEWAY_MULTICAST_RADIUS 8

#define CLIENT_PREFIX "tc-"
#define CLIENT_PORT 10000

#define TOPIC_PREFIX "sensors"

static const uint8_t sExpanId[] = EXTPANID;
static const uint8_t sMasterKey[] = MASTER_KEY;

#define PUBLISH_INTERVAL_MS 10000

void mqttsn_publish_handler(struct k_timer *dummy);
K_TIMER_DEFINE(mqttsn_publish_timer, mqttsn_publish_handler, NULL);

LOG_MODULE_REGISTER(cli_sample, CONFIG_OT_COMMAND_LINE_INTERFACE_LOG_LEVEL);

#define WELLCOME_TEXT \
	"\n\r"\
	"\n\r"\
	"OpenThread Command Line Interface is now running.\n\r" \
	"Use the 'ot' keyword to invoke OpenThread commands e.g. " \
	"'ot thread start.'\n\r" \
	"For the full commands list refer to the OpenThread CLI " \
	"documentation at:\n\r" \
	"https://github.com/openthread/openthread/blob/master/src/cli/README.md\n\r"

static void HandlePublished(otMqttsnReturnCode aCode, void* aContext)
{
    OT_UNUSED_VARIABLE(aCode);
    OT_UNUSED_VARIABLE(aContext);

    // Handle published
    otLogWarnPlat("Published");
}

otMqttsnTopic _aTopic;

static void HandleRegistered(otMqttsnReturnCode aCode, const otMqttsnTopic* aTopic, void* aContext)
{
    // Handle registered
    otInstance *instance = (otInstance *)aContext;
    if (aCode == kCodeAccepted)
    {
        otLogWarnPlat("HandleRegistered - OK");
        memcpy(&_aTopic, aTopic, sizeof(otMqttsnTopic));
    }
    else
    {
        otLogWarnPlat("HandleRegistered - Error");
    }
}

static void MqttsnConnect(otInstance *instance);

static void HandleConnected(otMqttsnReturnCode aCode, void* aContext)
{
    // Handle connected
    otInstance *instance = (otInstance *)aContext;
    if (aCode == kCodeAccepted)
    {
        otLogWarnPlat("HandleConnected -Accepted");

        // Get ID
        otExtAddress extAddress;
        otLinkGetFactoryAssignedIeeeEui64(instance, &extAddress);

        char data[128];
        sprintf(data, "%s/%02x%02x%02x%02x%02x%02x%02x%02x", TOPIC_PREFIX,
		extAddress.m8[0],
		extAddress.m8[1],
		extAddress.m8[2],
		extAddress.m8[3],
		extAddress.m8[4],
		extAddress.m8[5],
		extAddress.m8[6],
		extAddress.m8[7]
        );

        otLogWarnPlat("Registering Topic: %s", data);

        // Obtain target topic ID
        otMqttsnRegister(instance, data, HandleRegistered, (void *)instance);
    }
    else
    {
        otLogWarnPlat("HandleConnected - Error");
    }
}

static void HandleSearchGw(const otIp6Address* aAddress, uint8_t aGatewayId, void* aContext)
{
    OT_UNUSED_VARIABLE(aGatewayId);

    otLogWarnPlat("Got search gateway response");

    // Handle SEARCHGW response received
    // Connect to received address
    otInstance *instance = (otInstance *)aContext;
    otIp6Address address = *aAddress;
    // Set MQTT-SN client configuration settings
    otMqttsnConfig config;

    otExtAddress extAddress;
    otLinkGetFactoryAssignedIeeeEui64(instance, &extAddress);

    char data[256];
    sprintf(data, "%s-%02x%02x%02x%02x%02x%02x%02x%02x", CLIENT_PREFIX,
                extAddress.m8[0],
                extAddress.m8[1],
                extAddress.m8[2],
                extAddress.m8[3],
                extAddress.m8[4],
                extAddress.m8[5],
                extAddress.m8[6],
                extAddress.m8[7]
    );

    config.mClientId = data;
    config.mKeepAlive = 30;
    config.mCleanSession = true;
    config.mPort = GATEWAY_MULTICAST_PORT;
    config.mAddress = &address;
    config.mRetransmissionCount = 3;
    config.mRetransmissionTimeout = 10;

    // Register connected callback
    otMqttsnSetConnectedHandler(instance, HandleConnected, (void *)instance);
    // Connect to the MQTT broker (gateway)
    otMqttsnConnect(instance, &config);
}

static void SearchGateway(otInstance *instance)
{
    otIp6Address address;
    otIp6AddressFromString(GATEWAY_MULTICAST_ADDRESS, &address);

    otLogWarnPlat("Searching for gateway on %s", GATEWAY_MULTICAST_ADDRESS);

    otMqttsnSetSearchgwHandler(instance, HandleSearchGw, (void *)instance);
    // Send SEARCHGW multicast message
    otMqttsnSearchGateway(instance, &address, GATEWAY_MULTICAST_PORT, GATEWAY_MULTICAST_RADIUS);
}

static void StateChanged(otChangedFlags aFlags, void *aContext)
{
    otInstance *instance = (otInstance *)aContext;

     otLogWarnPlat("State Changed");

    // when thread role changed
    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        // If role changed to any of active roles then send SEARCHGW message
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER)
        {
            SearchGateway(instance);
        }
    }
}

int main(int aArgc, char *aArgv[])
{
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_shell_uart), zephyr_cdc_acm_uart)
	int ret;
	const struct device *dev;
	uint32_t dtr = 0U;

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return 0;
	}

	dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));
	if (dev == NULL) {
		LOG_ERR("Failed to find specific UART device");
		return 0;
	}

	LOG_INF("Waiting for host to be ready to communicate");

	/* Data Terminal Ready - check if host is ready to communicate */
	while (!dtr) {
		ret = uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
		if (ret) {
			LOG_ERR("Failed to get Data Terminal Ready line state: %d",
				ret);
			continue;
		}
		k_msleep(100);
	}

	/* Data Carrier Detect Modem - mark connection as established */
	(void)uart_line_ctrl_set(dev, UART_LINE_CTRL_DCD, 1);
	/* Data Set Ready - the NCP SoC is ready to communicate */
	(void)uart_line_ctrl_set(dev, UART_LINE_CTRL_DSR, 1);
#endif

	LOG_INF(WELLCOME_TEXT);

#if defined(CONFIG_BT)
	ble_enable();
#endif

#if defined(CONFIG_CLI_SAMPLE_LOW_POWER)
	low_power_enable();
#endif

	// New code
	otInstance *instance;
    otError error = OT_ERROR_NONE;
    otExtendedPanId extendedPanid;
    otNetworkKey masterKey;

    instance = openthread_get_default_instance();

	// Set default network settings
    // Set network name
    LOG_INF("Setting Network Name to %s", NETWORK_NAME);
    error = otThreadSetNetworkName(instance, NETWORK_NAME);
    // Set extended PANID
    memcpy(extendedPanid.m8, sExpanId, sizeof(sExpanId));
    LOG_INF("Setting PANID to 0x%04X", PANID);
    error = otThreadSetExtendedPanId(instance, &extendedPanid);
    // Set PANID
    LOG_INF("Setting Extended PANID");
    error = otLinkSetPanId(instance, PANID);
    // Set channel
    LOG_INF("Setting Channel to %d", DEFAULT_CHANNEL);
    error = otLinkSetChannel(instance, DEFAULT_CHANNEL);
    // Set masterkey
    LOG_INF("Setting Network Key");
    memcpy(masterKey.m8, sMasterKey, sizeof(sMasterKey));
    error = otThreadSetNetworkKey(instance, &masterKey);

    // Register notifier callback to receive thread role changed events
    error = otSetStateChangedCallback(instance, StateChanged, instance);

    // Start thread network
    otIp6SetSlaacEnabled(instance, true);
    error = otIp6SetEnabled(instance, true);
    error = otThreadSetEnabled(instance, true);

    // Start MQTT-SN client
    LOG_INF("Starting MQTT-SN on port %d", CLIENT_PORT);
    error = otMqttsnStart(instance, CLIENT_PORT);

    /* start one shot timer that expires after 10s */
    k_timer_start(&mqttsn_publish_timer, K_SECONDS(10), K_NO_WAIT);

    return 0;
}

void publish_work_handler(struct k_work *work)
{
	LOG_WRN("Publish Handler");

	otInstance *instance = openthread_get_default_instance();

    if(otMqttsnGetState(instance) == kStateDisconnected || otMqttsnGetState(instance)  == kStateLost)
    {
        LOG_INF("MQTT g/w disconnected or lost: %d", otMqttsnGetState(instance) );
        SearchGateway(instance);
    }
    else
    {
        static int count = 0;
        
        LOG_INF("Client state %d", otMqttsnGetState(instance));

        // Get ID
        otExtAddress extAddress;
        otLinkGetFactoryAssignedIeeeEui64(instance, &extAddress);

        // Publish message to the registered topic
        LOG_INF("Publishing...");
        const char* strdata = "{\"id\":%02x%02x%02x%02x%02x%02x%02x%02x, \"count\":%d, \"status\":%s, \"batt\":%d, \"lat\":1.234, \"lon\",5.678, \"height\":1.23, \"temp\":24.0}";
        char data[256];
        sprintf(data, strdata,
		    extAddress.m8[0],
		    extAddress.m8[1],
		    extAddress.m8[2],
		    extAddress.m8[3],
		    extAddress.m8[4],
		    extAddress.m8[5],
		    extAddress.m8[6],
		    extAddress.m8[7],
		    count++, "P1", 100);
        
        int32_t length = strlen(data);

        otError err = otMqttsnPublish(instance, (const uint8_t*)data, length, kQos1, false, &_aTopic,
            HandlePublished, NULL);

        LOG_INF("Publishing %d bytes rsp %d", length, err);
    }

    // Restart timer
    k_timer_start(&mqttsn_publish_timer, K_SECONDS(10), K_NO_WAIT);
}

K_WORK_DEFINE(publish_work, publish_work_handler);

void mqttsn_publish_handler(struct k_timer *dummy)
{
    k_work_submit(&publish_work);
}