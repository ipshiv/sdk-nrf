/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "bolt_lock_manager.h"
#include "led_widget.h"
#include "thread_util.h"

#include <platform/CHIPDeviceLayer.h>

#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include <app/clusters/ota-requestor/BDXDownloader.h>
#include <app/clusters/ota-requestor/OTARequestor.h>
#include <platform/GenericOTARequestorDriver.h>
#include <platform/nrfconnect/OTAImageProcessorImpl.h>
#endif

#include <dk_buttons_and_leds.h>
#include <logging/log.h>
#include <zephyr.h>

#include <algorithm>

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

LOG_MODULE_DECLARE(app);

namespace
{
constexpr size_t kAppEventQueueSize = 10;
constexpr uint32_t kFactoryResetTriggerTimeout = 3000;
constexpr uint32_t kFactoryResetCancelWindow = 3000;
constexpr EndpointId kLockEndpointId = 1;

K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
LEDWidget sStatusLED;
LEDWidget sLockLED;
LEDWidget sUnusedLED;
LEDWidget sUnusedLED_1;

bool sIsThreadProvisioned;
bool sIsThreadEnabled;
bool sHaveBLEConnections;

k_timer sFunctionTimer;

#ifdef CONFIG_CHIP_OTA_REQUESTOR
GenericOTARequestorDriver sOTARequestorDriver;
OTAImageProcessorImpl sOTAImageProcessor;
chip::BDXDownloader sBDXDownloader;
chip::OTARequestor sOTARequestor;
#endif
} /* namespace */

AppTask AppTask::sAppTask;

int AppTask::Init()
{
	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

	sStatusLED.Init(DK_LED1);
	sLockLED.Init(DK_LED2);
	sLockLED.Set(!BoltLockMgr().IsUnlocked());
	sUnusedLED.Init(DK_LED3);
	sUnusedLED_1.Init(DK_LED4);

	UpdateStatusLED();

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return ret;
	}

#ifdef CONFIG_MCUMGR_SMP_BT
	GetDFUOverSMP().Init(RequestSMPAdvertisingStart);
	GetDFUOverSMP().ConfirmNewImage();
#endif

	/* Initialize function timer */
	k_timer_init(&sFunctionTimer, &AppTask::TimerEventHandler, nullptr);
	k_timer_user_data_set(&sFunctionTimer, this);

	/* Initialize lock manager */
	BoltLockMgr().Init();

	/* Init ZCL Data Model and start server */
	chip::Server::GetInstance().Init();

	/* Initialize device attestation config */
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

	PlatformMgr().AddEventHandler(AppTask::ChipEventHandler, 0);

#ifdef CONFIG_CHIP_OTA_REQUESTOR
	sOTAImageProcessor.SetOTADownloader(&sBDXDownloader);
	sBDXDownloader.SetImageProcessorDelegate(&sOTAImageProcessor);
	sOTARequestorDriver.Init(&sOTARequestor, &sOTAImageProcessor);
	sOTARequestor.Init(&chip::Server::GetInstance(), &sOTARequestorDriver, &sBDXDownloader);
	chip::SetRequestorInstance(&sOTARequestor);
#endif

	return 0;
}

int AppTask::StartApp()
{
	int ret = Init();

	if (ret) {
		LOG_ERR("AppTask.Init() failed");
		return ret;
	}

	AppEvent event = {};

	while (true) {
		k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
		DispatchEvent(event);
	}
}

void AppTask::PostEvent(const AppEvent &event)
{
	if (k_msgq_put(&sAppEventQueue, &event, K_NO_WAIT)) {
		LOG_INF("Failed to post event to app task event queue");
	}
}

void AppTask::UpdateClusterState()
{
	/* write the new on/off value */
	EmberAfStatus status = Clusters::OnOff::Attributes::OnOff::Set(kLockEndpointId, !BoltLockMgr().IsUnlocked());

	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating on/off cluster failed: %x", status);
	}
}

#ifdef CONFIG_MCUMGR_SMP_BT
void AppTask::RequestSMPAdvertisingStart(void)
{
	sAppTask.PostEvent(AppEvent{ AppEvent::StartSMPAdvertising });
}
#endif

void AppTask::DispatchEvent(const AppEvent &event)
{
	switch (event.Type) {
	case AppEvent::Lock:
		LockActionHandler(BoltLockManager::Action::Lock, event.LockEvent.ChipInitiated);
		break;
	case AppEvent::Unlock:
		LockActionHandler(BoltLockManager::Action::Unlock, event.LockEvent.ChipInitiated);
		break;
	case AppEvent::Toggle:
		LockActionHandler(BoltLockMgr().IsUnlocked() ? BoltLockManager::Action::Lock :
								     BoltLockManager::Action::Unlock,
				  event.LockEvent.ChipInitiated);
		break;
	case AppEvent::CompleteLockAction:
		CompleteLockActionHandler();
		break;
	case AppEvent::FunctionPress:
		FunctionPressHandler();
		break;
	case AppEvent::FunctionRelease:
		FunctionReleaseHandler();
		break;
	case AppEvent::FunctionTimer:
		FunctionTimerEventHandler();
		break;
	case AppEvent::StartThread:
		StartThreadHandler();
		break;
	case AppEvent::StartBleAdvertising:
		StartBLEAdvertisingHandler();
		break;
	case AppEvent::UpdateLedState:
		event.UpdateLedStateEvent.LedWidget->UpdateState();
		break;
#ifdef CONFIG_MCUMGR_SMP_BT
	case AppEvent::StartSMPAdvertising:
		GetDFUOverSMP().StartBLEAdvertising();
		break;
#endif
	default:
		LOG_INF("Unknown event received");
		break;
	}
}

void AppTask::LockActionHandler(BoltLockManager::Action action, bool chipInitiated)
{
	if (BoltLockMgr().InitiateAction(action, chipInitiated)) {
		sLockLED.Blink(50, 50);
	}
}

void AppTask::CompleteLockActionHandler()
{
	bool chipInitiatedAction;

	if (!BoltLockMgr().CompleteCurrentAction(chipInitiatedAction)) {
		return;
	}

	sLockLED.Set(!BoltLockMgr().IsUnlocked());

	/* If the action wasn't initiated by CHIP, update CHIP clusters with the new lock state */
	if (!chipInitiatedAction) {
		UpdateClusterState();
	}
}

void AppTask::FunctionPressHandler()
{
	sAppTask.StartFunctionTimer(kFactoryResetTriggerTimeout);
	sAppTask.mFunction = TimerFunction::SoftwareUpdate;
}

void AppTask::FunctionReleaseHandler()
{
	if (sAppTask.mFunction == TimerFunction::SoftwareUpdate) {
		sAppTask.CancelFunctionTimer();
		sAppTask.mFunction = TimerFunction::NoneSelected;

#ifdef CONFIG_MCUMGR_SMP_BT
		GetDFUOverSMP().StartServer();
#else
		LOG_INF("Software update is disabled");
#endif

	} else if (sAppTask.mFunction == TimerFunction::FactoryReset) {
		sUnusedLED_1.Set(false);
		sUnusedLED.Set(false);

		/* Set lock status LED back to show state of lock. */
		sLockLED.Set(!BoltLockMgr().IsUnlocked());

		UpdateStatusLED();

		sAppTask.CancelFunctionTimer();
		sAppTask.mFunction = TimerFunction::NoneSelected;
		LOG_INF("Factory Reset has been Canceled");
	}
}

void AppTask::FunctionTimerEventHandler()
{
	if (sAppTask.mFunction == TimerFunction::SoftwareUpdate) {
		LOG_INF("Factory Reset Triggered. Release button within %ums to cancel.", kFactoryResetCancelWindow);
		sAppTask.StartFunctionTimer(kFactoryResetCancelWindow);
		sAppTask.mFunction = TimerFunction::FactoryReset;

#ifdef CONFIG_STATE_LEDS
		/* Turn off all LEDs before starting blink to make sure blink is co-ordinated. */
		sStatusLED.Set(false);
		sLockLED.Set(false);
		sUnusedLED_1.Set(false);
		sUnusedLED.Set(false);

		sStatusLED.Blink(500);
		sLockLED.Blink(500);
		sUnusedLED.Blink(500);
		sUnusedLED_1.Blink(500);
#endif
	} else if (sAppTask.mFunction == TimerFunction::FactoryReset) {
		sAppTask.mFunction = TimerFunction::NoneSelected;
		LOG_INF("Factory Reset triggered");
		ConfigurationMgr().InitiateFactoryReset();
	}
}

void AppTask::StartThreadHandler()
{
	if (chip::Server::GetInstance().AddTestCommissioning() != CHIP_NO_ERROR) {
		LOG_ERR("Failed to add test pairing");
	}

	if (!ConnectivityMgr().IsThreadProvisioned()) {
		StartDefaultThreadNetwork();
		LOG_INF("Device is not commissioned to a Thread network. Starting with the default configuration");
	} else {
		LOG_INF("Device is commissioned to a Thread network");
	}
}

void AppTask::StartBLEAdvertisingHandler()
{
	/* Don't allow on starting Matter service BLE advertising after Thread provisioning. */
	if (ConnectivityMgr().IsThreadProvisioned()) {
		LOG_INF("NFC Tag emulation and Matter service BLE advertising not started - device is commissioned to a Thread network.");
		return;
	}

	if (ConnectivityMgr().IsBLEAdvertisingEnabled()) {
		LOG_INF("BLE advertising is already enabled");
		return;
	}

	if (chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() !=
	    CHIP_NO_ERROR) {
		LOG_ERR("OpenBasicCommissioningWindow() failed");
	}
}

void AppTask::LEDStateUpdateHandler(LEDWidget &ledWidget)
{
	sAppTask.PostEvent(AppEvent{ AppEvent::UpdateLedState, &ledWidget });
}

void AppTask::UpdateStatusLED()
{
#ifdef CONFIG_STATE_LEDS
	/* Update the status LED.
	 *
	 * If thread and service provisioned, keep the LED On constantly.
	 *
	 * If the system has ble connection(s) uptill the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED On for a very short time. */
	if (sIsThreadProvisioned && sIsThreadEnabled) {
		sStatusLED.Set(true);
	} else if (sHaveBLEConnections) {
		sStatusLED.Blink(100, 100);
	} else {
		sStatusLED.Blink(50, 950);
	}
#endif
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
		if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started) {
			if (NFCMgr().IsTagEmulationStarted()) {
				LOG_INF("NFC Tag emulation is already started");
			} else {
				ShareQRCodeOverNFC(
					chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
			}
		} else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped) {
			NFCMgr().StopTagEmulation();
		}
#endif
		sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		UpdateStatusLED();
		break;
	case DeviceEventType::kThreadStateChange:
		sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
		sIsThreadEnabled = ConnectivityMgr().IsThreadEnabled();
		UpdateStatusLED();
		break;
	default:
		break;
	}
}

void AppTask::ButtonEventHandler(uint32_t buttonState, uint32_t hasChanged)
{
	if (DK_BTN1_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionPress });
	} else if (DK_BTN1_MSK & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionRelease });
	}

	if (DK_BTN2_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::Toggle, false });
	}

	if (DK_BTN3_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::StartThread });
	}

	if (DK_BTN4_MSK & buttonState & hasChanged) {
		GetAppTask().PostEvent(AppEvent{ AppEvent::StartBleAdvertising });
	}
}

void AppTask::CancelFunctionTimer()
{
	k_timer_stop(&sFunctionTimer);
}

void AppTask::StartFunctionTimer(uint32_t timeoutInMs)
{
	k_timer_start(&sFunctionTimer, K_MSEC(timeoutInMs), K_NO_WAIT);
}

void AppTask::TimerEventHandler(k_timer *timer)
{
	GetAppTask().PostEvent(AppEvent{ AppEvent::FunctionTimer });
}
