/******************************************************************************
 * Copyright (C) 2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 ******************************************************************************/
/******************************************************************************
 *  Copyright (C) 2009-2016 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "btif_av"

#include "btif_av.h"

#include <base/logging.h>
#include <string.h>

#include <hardware/bluetooth.h>
#include <hardware/bt_av.h>
#include <hardware/bt_rc.h>

#include "audio_a2dp_hw/include/audio_a2dp_hw.h"
#include "bt_common.h"
#include "bt_utils.h"
#include "bta_api.h"
#include "btif_storage.h"
#include "btif_a2dp.h"
#include "btif_a2dp_audio_interface.h"
#include "btif_a2dp_control.h"
#include "btif_a2dp_sink.h"
#include "btif_av_co.h"
#include "btif_profile_queue.h"
#include "btif_util.h"
#include "btu.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"
#include "btif/include/btif_a2dp_source.h"
#include "device/include/interop.h"
#include "device/include/controller.h"

extern bool isDevUiReq;
bool isBitRateChange = false;
bool isBitsPerSampleChange = false;
static int reconfig_a2dp_param_id = 0;
static int reconfig_a2dp_param_val = 0;

/*****************************************************************************
 *  Constants & Macros
 *****************************************************************************/
#define BTIF_AV_SERVICE_NAME "Advanced Audio"
#define BTIF_AVK_SERVICE_NAME "Advanced Audio Sink"

#define BTIF_TIMEOUT_AV_OPEN_ON_RC_MS (2 * 1000)

/* Number of BTIF-AV control blocks */
/* Now supports Two AV connections. */
#define BTIF_AV_NUM_CB       2
#define HANDLE_TO_INDEX(x) ((x & BTA_AV_HNDL_MSK) - 1)
#define INVALID_INDEX        -1

typedef enum {
  BTIF_AV_STATE_IDLE = 0x0,
  BTIF_AV_STATE_OPENING,
  BTIF_AV_STATE_OPENED,
  BTIF_AV_STATE_STARTED,
  BTIF_AV_STATE_CLOSING
} btif_av_state_t;

/* Should not need dedicated suspend state as actual actions are no
   different than open state. Suspend flags are needed however to prevent
   media task from trying to restart stream during remote suspend or while
   we are in the process of a local suspend */

#define BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING 0x1
#define BTIF_AV_FLAG_REMOTE_SUSPEND 0x2
#define BTIF_AV_FLAG_PENDING_START 0x4
#define BTIF_AV_FLAG_PENDING_STOP 0x8
#define BTIF_AV_FLAG_PENDING_DISCONNECT 0x10
#define BTIF_TIMEOUT_AV_COLL_DETECTED_MS (2 * 1000)
#define BTIF_TIMEOUT_AV_COLL_DETECTED_MS_2 (5 * 1000)
#define BTIF_ERROR_SRV_AV_CP_NOT_SUPPORTED   705

/* Host role definitions */
#define HOST_ROLE_MASTER                   0x00
#define HOST_ROLE_SLAVE                    0x01
#define HOST_ROLE_UNKNOWN                  0xff

/* Default sink latency value while delay report is not supported by SNK */
#define BTIF_AV_DEFAULT_SINK_LATENCY 0
#define BTIF_AV_DEFAULT_MULTICAST_SINK_LATENCY 200

/* Param id for bitrate and bits per sample */
#define BITRATE_PARAM_ID 1
#define BITSPERSAMPLE_PARAM_ID 2

/*****************************************************************************
 *  Local type definitions
 *****************************************************************************/

typedef struct {
  tBTA_AV_HNDL bta_handle;
  RawAddress peer_bda;
  bool self_initiated_connection;
  btif_sm_handle_t sm_handle;
  uint8_t flags;
  tBTA_AV_EDR edr;
  uint8_t peer_sep; /* sep type of peer device */
  std::vector<btav_a2dp_codec_config_t> codec_priorities;
  bool edr_3mbps;
  bool dual_handoff;
  bool current_playing;
  btif_sm_state_t state;
  int service;
  bool is_slave;
  bool is_device_playing;
  bool reconfig_pending; /* flag used for codec switch */
  tBTA_AV_LATENCY sink_latency;
  bool remote_started;
  bool is_suspend_for_remote_start;
} btif_av_cb_t;

typedef struct {
  RawAddress* target_bda;
  uint16_t uuid;
} btif_av_connect_req_t;

typedef struct {
  int sample_rate;
  int channel_count;
  RawAddress peer_bd;
} btif_av_sink_config_req_t;

/*****************************************************************************
 *  Static variables
 *****************************************************************************/
static btav_source_callbacks_t* bt_av_src_callbacks = NULL;
static btav_sink_callbacks_t* bt_av_sink_callbacks = NULL;
static btif_av_cb_t btif_av_cb[BTIF_AV_NUM_CB] = {
    { 0, {{0}}, false, 0, 0, 0, 0, std::vector<btav_a2dp_codec_config_t>(), false,
    false, false, BTIF_AV_STATE_IDLE, BTA_A2DP_SOURCE_SERVICE_ID,
    false, false, false, 0, false, false},
    { 0, {{0}}, false, 0, 0, 0, 0, std::vector<btav_a2dp_codec_config_t>(), false,
    false, false, BTIF_AV_STATE_IDLE, BTA_A2DP_SOURCE_SERVICE_ID,
    false, false, false, 0, false, false},
};

static alarm_t* av_open_on_rc_timer = NULL;
static btif_sm_event_t idle_rc_event;
static tBTA_AV idle_rc_data;
int btif_max_av_clients = 1;
static bool enable_multicast = false;
static bool is_multicast_supported = false;
static bool multicast_disabled = false;
static RawAddress retry_bda;
static int conn_retry_count = 1;
static alarm_t *av_coll_detected_timer = NULL;
static bool isA2dpSink = false;
static bool codec_config_update_enabled = false;
bool is_codec_config_dump = false;

/*SPLITA2DP */
bool bt_split_a2dp_enabled = false;
bool reconfig_a2dp = false;
bool btif_a2dp_audio_if_init = false;
bool codec_cfg_change = false;
bool audio_start_awaited = false;
extern bool enc_update_in_progress;
extern bool is_block_hal_start;
/*SPLITA2DP */
/* both interface and media task needs to be ready to alloc incoming request */
#define CHECK_BTAV_INIT()                                                    \
  do {                                                                       \
    if (((bt_av_src_callbacks == NULL) && (bt_av_sink_callbacks == NULL)) || \
        (btif_av_cb[0].sm_handle == NULL)) {                                    \
      BTIF_TRACE_WARNING("%s: BTAV not initialized", __func__);              \
      return BT_STATUS_NOT_READY;                                            \
    }                                                                        \
  } while (0)

/* Helper macro to avoid code duplication in the state machine handlers */
#define CHECK_RC_EVENT(e, d)       \
  case BTA_AV_RC_BROWSE_OPEN_EVT:  \
  case BTA_AV_RC_CLOSE_EVT:        \
  case BTA_AV_RC_BROWSE_CLOSE_EVT: \
  case BTA_AV_REMOTE_CMD_EVT:      \
  case BTA_AV_VENDOR_CMD_EVT:      \
  case BTA_AV_META_MSG_EVT:        \
  case BTA_AV_RC_FEAT_EVT:         \
  case BTA_AV_REMOTE_RSP_EVT: {    \
    btif_rc_handler(e, d);         \
  } break;

void btif_av_flow_spec_cmd(int index, int bitrate);
static bool btif_av_state_idle_handler(btif_sm_event_t event, void* data, int idx);
static bool btif_av_state_opening_handler(btif_sm_event_t event, void* data, int idx);
static bool btif_av_state_opened_handler(btif_sm_event_t event, void* data, int idx);
static bool btif_av_state_started_handler(btif_sm_event_t event, void* data, int idx);
static bool btif_av_state_closing_handler(btif_sm_event_t event, void* data, int idx);

static bool btif_av_get_valid_idx(int idx);
int btif_av_idx_by_bdaddr(RawAddress *bd_addr);
static int btif_av_get_valid_idx_for_rc_events(RawAddress bd_addr, int rc_handle);
static int btif_get_conn_state_of_device(RawAddress address);
static bt_status_t connect_int(RawAddress *bd_addr, uint16_t uuid);
void btif_av_update_current_playing_device(int index);
static void btif_av_check_rc_connection_priority(void *p_data);
static bt_status_t connect_int(RawAddress* bd_addr, uint16_t uuid);
static bool btif_av_allow_codec_config_change(btav_a2dp_codec_index_t codec_type,
                                        btav_a2dp_codec_sample_rate_t sample_rate);
int btif_get_is_remote_started_idx();
static void btif_av_reset_remote_started_flag();

#ifdef AVK_BACKPORT
void btif_av_request_audio_focus(bool enable);
#endif

static const btif_sm_handler_t btif_av_state_handlers[] = {
    btif_av_state_idle_handler, btif_av_state_opening_handler,
    btif_av_state_opened_handler, btif_av_state_started_handler,
    btif_av_state_closing_handler};

static void btif_av_event_free_data(btif_sm_event_t event, void* p_data);

/*************************************************************************
 * Extern functions
 ************************************************************************/
extern void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV* p_data);
extern bool btif_rc_get_connected_peer(RawAddress* peer_addr);
extern uint8_t btif_rc_get_connected_peer_handle(const RawAddress& peer_addr);
extern void btif_rc_check_handle_pending_play(const RawAddress& peer_addr,
                                              bool bSendToApp);
extern void btif_rc_get_playing_device(RawAddress *address);
extern void btif_rc_clear_playing_state(bool play);
extern void btif_rc_clear_priority(RawAddress address);
extern void btif_rc_send_pause_command(RawAddress bda);
extern uint16_t btif_dm_get_br_edr_links();
extern uint16_t btif_dm_get_le_links();
extern uint16_t btif_hf_is_call_vr_idle();
extern void btif_a2dp_on_idle(int index);
extern fixed_queue_t* btu_general_alarm_queue;
extern void btif_media_send_reset_vendor_state();
extern tBTIF_A2DP_SOURCE_VSC btif_a2dp_src_vsc;
extern uint8_t* bta_av_co_get_peer_codec_info(uint8_t hdl);
extern void bta_av_vendor_offload_stop(void);
/*****************************************************************************
 * Local helper functions
 *****************************************************************************/
bool btif_av_is_connected_on_other_idx(int current_index);
bool btif_av_is_playing_on_other_idx(int current_index);
void btif_av_update_multicast_state(int index);
bool btif_av_get_ongoing_multicast();
tBTA_AV_HNDL btif_av_get_playing_device_hdl();
tBTA_AV_HNDL btif_av_get_av_hdl_from_idx(int idx);
int btif_av_get_other_connected_idx(int current_index);
void btif_av_reset_reconfig_flag();
bool btif_av_is_device_disconnecting();
tBTA_AV_HNDL btif_av_get_reconfig_dev_hndl();
void btif_av_reset_codec_reconfig_flag();
void btif_av_reinit_audio_interface();
bool btif_av_is_suspend_stop_pending_ack();

const char* dump_av_sm_state_name(btif_av_state_t state) {
  switch (state) {
    CASE_RETURN_STR(BTIF_AV_STATE_IDLE)
    CASE_RETURN_STR(BTIF_AV_STATE_OPENING)
    CASE_RETURN_STR(BTIF_AV_STATE_OPENED)
    CASE_RETURN_STR(BTIF_AV_STATE_STARTED)
    CASE_RETURN_STR(BTIF_AV_STATE_CLOSING)
    default:
      return "UNKNOWN_STATE";
  }
}

const char* dump_av_sm_event_name(btif_av_sm_event_t event) {
  switch ((int)event) {
    CASE_RETURN_STR(BTA_AV_ENABLE_EVT)
    CASE_RETURN_STR(BTA_AV_REGISTER_EVT)
    CASE_RETURN_STR(BTA_AV_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_START_EVT)
    CASE_RETURN_STR(BTA_AV_STOP_EVT)
    CASE_RETURN_STR(BTA_AV_PROTECT_REQ_EVT)
    CASE_RETURN_STR(BTA_AV_PROTECT_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_RC_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_RC_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_RC_BROWSE_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_RC_BROWSE_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_REMOTE_CMD_EVT)
    CASE_RETURN_STR(BTA_AV_REMOTE_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_VENDOR_CMD_EVT)
    CASE_RETURN_STR(BTA_AV_VENDOR_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_RECONFIG_EVT)
    CASE_RETURN_STR(BTA_AV_SUSPEND_EVT)
    CASE_RETURN_STR(BTA_AV_PENDING_EVT)
    CASE_RETURN_STR(BTA_AV_META_MSG_EVT)
    CASE_RETURN_STR(BTA_AV_REJECT_EVT)
    CASE_RETURN_STR(BTA_AV_RC_FEAT_EVT)
    CASE_RETURN_STR(BTA_AV_OFFLOAD_START_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_DELAY_REPORT_EVT)
    CASE_RETURN_STR(BTIF_SM_ENTER_EVT)
    CASE_RETURN_STR(BTIF_SM_EXIT_EVT)
    CASE_RETURN_STR(BTIF_AV_CONNECT_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_DISCONNECT_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_START_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_STOP_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SUSPEND_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SOURCE_CONFIG_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SOURCE_CONFIG_UPDATED_EVT)
    CASE_RETURN_STR(BTIF_AV_REMOTE_SUSPEND_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SINK_CONFIG_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_OFFLOAD_START_REQ_EVT)
    CASE_RETURN_STR(BTA_AV_OFFLOAD_STOP_RSP_EVT)
    CASE_RETURN_STR(BTIF_AV_SETUP_CODEC_REQ_EVT)
    default:
      return "UNKNOWN_EVENT";
  }
}

/****************************************************************************
 *  Local helper functions
 ****************************************************************************/
/*******************************************************************************
 *
 * Function         btif_initiate_av_open_timer_timeout
 *
 * Description      Timer to trigger AV open if the remote headset establishes
 *                  RC connection w/o AV connection. The timer is needed to IOP
 *                  with headsets that do establish AV after RC connection.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_initiate_av_open_timer_timeout(UNUSED_ATTR void* data) {
  RawAddress peer_addr;
  btif_av_connect_req_t connect_req;

  memset(&connect_req, 0, sizeof(btif_av_connect_req_t));
  /* is there at least one RC connection - There should be */
  if (btif_rc_get_connected_peer(&peer_addr)) {
    /* Check if this peer_addr is same as currently connected AV*/
    if (btif_get_conn_state_of_device(peer_addr) == BTIF_AV_STATE_OPENED) {
      BTIF_TRACE_DEBUG("AV is already connected");
    } else {
      uint8_t rc_handle;
      int index;
      /* Multicast: Check if AV slot is available for connection
       * If not available, AV got connected to different devices.
       * Disconnect this RC connection without AV connection.
       */
      rc_handle = btif_rc_get_connected_peer_handle(peer_addr);
      index = btif_av_get_valid_idx_for_rc_events(peer_addr, rc_handle);
      if (index >= btif_max_av_clients) {
          BTIF_TRACE_ERROR("%s No slot free for AV connection, back off",
                            __func__);
          return;
      }
      BTIF_TRACE_DEBUG("%s Issuing connect to the remote RC peer", __func__);
    /* In case of AVRCP connection request, we will initiate SRC connection */
    connect_req.target_bda = &peer_addr;
    if (bt_av_sink_callbacks != NULL)
      connect_req.uuid = UUID_SERVCLASS_AUDIO_SINK;
    else if (bt_av_src_callbacks != NULL)
      connect_req.uuid = UUID_SERVCLASS_AUDIO_SOURCE;
    btif_dispatch_sm_event(BTIF_AV_CONNECT_REQ_EVT, (char*)&connect_req,
                           sizeof(connect_req));
    }
  } else {
    BTIF_TRACE_ERROR("%s: No connected RC peers", __func__);
  }
}

/*****************************************************************************
 *  Static functions
 *****************************************************************************/

/*******************************************************************************
 *
 * Function         btif_report_connection_state
 *
 * Description      Updates the components via the callbacks about the
 *                  connection state of a2dp connection.
 *
 * Returns          None
 *
 ******************************************************************************/
static void btif_report_connection_state(btav_connection_state_t state,
                                         RawAddress* bd_addr) {
  if (bt_av_sink_callbacks != NULL) {
    HAL_CBACK(bt_av_sink_callbacks, connection_state_cb, state, bd_addr);
  } else if (bt_av_src_callbacks != NULL) {
    HAL_CBACK(bt_av_src_callbacks, connection_state_cb, state, bd_addr);
  }
}

/*******************************************************************************
 *
 * Function         btif_report_audio_state
 *
 * Description      Updates the components via the callbacks about the audio
 *                  state of a2dp connection. The state is updated when either
 *                  the remote ends starts streaming (started state) or whenever
 *                  it transitions out of started state (to opened or streaming)
 *                  state.
 *
 * Returns          None
 *
 ******************************************************************************/
static void btif_report_audio_state(btav_audio_state_t state,
                                    RawAddress* bd_addr) {
  if (bt_av_sink_callbacks != NULL) {
    HAL_CBACK(bt_av_sink_callbacks, audio_state_cb, state, bd_addr);
  } else if (bt_av_src_callbacks != NULL) {
    HAL_CBACK(bt_av_src_callbacks, audio_state_cb, state, bd_addr);
  }
}

void btif_av_peer_config_dump()
{
   int index = 0;
   RawAddress bd_addr;
   btif_sm_state_t av_state;
   index = btif_av_get_latest_playing_device_idx();
   BTIF_TRACE_DEBUG("Latest playing device index: %d", index);
   btif_get_latest_playing_device(&bd_addr);
   BTIF_TRACE_DEBUG("TARGET BD ADDRESS %s", bd_addr.ToString().c_str());
   if (index == btif_max_av_clients)
   {
       BTIF_TRACE_DEBUG("%s: AV Index invalid", __FUNCTION__);
       return;
   }
   av_state = btif_get_conn_state_of_device(bd_addr);
   BTIF_TRACE_DEBUG("%s: Av_state: %d", __FUNCTION__, av_state);
   BTIF_TRACE_DEBUG("%s: index: %d flags: 0x%x edr: 0x%x SHO: %d current_playing: %d",
                    __FUNCTION__, index, btif_av_cb[index].flags, btif_av_cb[index].edr,
                     btif_av_cb[index].dual_handoff, btif_av_cb[index].current_playing);
   BTIF_TRACE_DEBUG("%s: is_slave: %d is_device_palaying: %d",
          __FUNCTION__, btif_av_cb[index].is_slave, btif_av_cb[index].is_device_playing);
   A2dpCodecs* a2dp_codecs = bta_av_get_a2dp_codecs();
   if (a2dp_codecs != nullptr) {
       a2dp_codecs->debug_codec_dump(-1);
   }
}

static void btif_update_source_codec(void* p_data) {
  BTIF_TRACE_DEBUG("%s", __func__);

  // copy to avoid alignment problems
  btav_a2dp_codec_config_t req;
  memcpy(&req, p_data, sizeof(req));

  btif_a2dp_source_encoder_user_config_update_req(req);
}

static void btif_report_source_codec_state(UNUSED_ATTR void* p_data,
                                            RawAddress* bd_addr) {
  btav_a2dp_codec_config_t codec_config;
  std::vector<btav_a2dp_codec_config_t> codecs_local_capabilities;
  std::vector<btav_a2dp_codec_config_t> codecs_selectable_capabilities;

  A2dpCodecs* a2dp_codecs = bta_av_get_a2dp_codecs();
  if (a2dp_codecs == nullptr) return;
  if (!a2dp_codecs->getCodecConfigAndCapabilities(
          &codec_config, &codecs_local_capabilities,
          &codecs_selectable_capabilities)) {
    BTIF_TRACE_WARNING(
        "%s: error reporting audio source codec state: "
        "cannot get codec config and capabilities",
        __func__);
    return;
  }
  if (bt_av_src_callbacks != NULL) {
    BTIF_TRACE_DEBUG("%s codec config changed BDA:0x%02X%02X%02X%02X%02X%02X", __func__,
                   bd_addr->address[0], bd_addr->address[1], bd_addr->address[2],
                   bd_addr->address[3], bd_addr->address[4], bd_addr->address[5]);
    HAL_CBACK(bt_av_src_callbacks, audio_config_cb, codec_config,
              codecs_local_capabilities, codecs_selectable_capabilities, bd_addr);
  }
}


static void btif_av_collission_timer_timeout(UNUSED_ATTR void *data) {
  RawAddress *target_bda = &retry_bda;
  btif_sm_state_t av_state;
  RawAddress av_address;

  if(!btif_storage_is_device_bonded(target_bda)){
    BTIF_TRACE_IMP("btif_av_collission_timer_timeout: not bonded device ");
    return;
  }else{
    BTIF_TRACE_IMP("btif_av_collission_timer_timeout: bonded device ");
  }

  av_address = *target_bda;
  av_state = btif_get_conn_state_of_device(av_address);
  BTIF_TRACE_DEBUG("%s(): AV state: %d", __func__, av_state);
  BTIF_TRACE_DEBUG("TARGET BD ADDRESS %s", av_address.ToString().c_str());

  if (av_state == BTIF_AV_STATE_IDLE && conn_retry_count <= 1) {
    if (bt_av_src_callbacks != NULL) {
      BTIF_TRACE_DEBUG("%s Starting A2dp connection", __FUNCTION__);
      conn_retry_count++;
      btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, target_bda, connect_int);
    } else {
      BTIF_TRACE_DEBUG("%s Aborting A2dp connection retry", __FUNCTION__);
    }
  } else if (btif_rc_get_connected_peer_handle(av_address) == BTRC_HANDLE_NONE
            && conn_retry_count <= 1) {
    tBTA_AV_HNDL handle;
    int idx = btif_av_idx_by_bdaddr(&av_address);
    if (idx == btif_max_av_clients) {
      BTIF_TRACE_ERROR("%s: Invalid handle",__func__);
      handle = (tBTA_AV_HNDL)INVALID_INDEX;
    } else {
      handle = btif_av_cb[idx].bta_handle;
    }
    BTIF_TRACE_DEBUG("%s Starting Avrcp connection for handle: %d", __FUNCTION__, handle);
    if ((handle != (tBTA_AV_HNDL)INVALID_INDEX) && (bt_av_src_callbacks != NULL)) {
      BTA_AvOpenRc(handle);
      conn_retry_count++;
    } else {
      BTIF_TRACE_DEBUG("%s Aborting Avrcp connection retry", __FUNCTION__);
    }
  } else {
    if (conn_retry_count > 1) {
      conn_retry_count = 1;
      BTIF_TRACE_DEBUG("%s Connection Retry count exceeded", __FUNCTION__);
      return;
    }
    BTIF_TRACE_DEBUG("%s A2dp already connected", __FUNCTION__);
    BTIF_TRACE_DEBUG("%s Avrcp already connected on handle: %d", __FUNCTION__,
                   btif_rc_get_connected_peer_handle(av_address));
  }
}

static void btif_av_check_and_start_collission_timer(int index) {
  retry_bda = btif_av_cb[index].peer_bda;

  BTIF_TRACE_DEBUG("%s(), index: %d ", __func__, index);

  if (alarm_is_scheduled(av_coll_detected_timer)) {
    alarm_cancel(av_coll_detected_timer);
    BTIF_TRACE_DEBUG("btif_av_check_and_start_collission_timer:Deleting previously queued timer");
  }
  if (interop_match_addr(INTEROP_INCREASE_COLL_DETECT_TIMEOUT, &btif_av_cb[index].peer_bda))
  {
      /* Increase collision detected timeout */
      alarm_set_on_mloop(av_coll_detected_timer,
                 BTIF_TIMEOUT_AV_COLL_DETECTED_MS_2,
                 btif_av_collission_timer_timeout,
                 NULL);
   } else {
       alarm_set_on_mloop(av_coll_detected_timer,
                 BTIF_TIMEOUT_AV_COLL_DETECTED_MS,
                 btif_av_collission_timer_timeout,
                 NULL);
   }
}


/*****************************************************************************
 *
 * Function     btif_av_state_idle_handler
 *
 * Description  State managing disconnected AV link
 *
 * Returns      true if event was processed, false otherwise
 *
 ******************************************************************************/

static bool btif_av_state_idle_handler(btif_sm_event_t event, void* p_data, int index) {
  int i;
  char a2dp_role[255] = "false";
  BTIF_TRACE_IMP("%s event:%s flags %x on index %x", __func__,
                   dump_av_sm_event_name((btif_av_sm_event_t)event),
                   btif_av_cb[index].flags, index);

  switch (event) {
    case BTIF_SM_ENTER_EVT:
      /* clear the peer_bda */
      BTIF_TRACE_EVENT("IDLE state for index: %d", index);
      memset(&btif_av_cb[index].peer_bda, 0, sizeof(RawAddress)); //TODO
      btif_av_cb[index].flags = 0;
      btif_av_cb[index].edr_3mbps = false;
      btif_av_cb[index].edr = 0;
      btif_av_cb[index].current_playing = false;
      btif_av_cb[index].is_slave = false;
      btif_av_cb[index].is_device_playing = false;
      btif_av_cb[index].reconfig_pending = false;
      btif_av_cb[index].sink_latency = 0;
      btif_av_cb[index].remote_started = false;
      btif_av_cb[index].is_suspend_for_remote_start = false;
      for (int i = 0; i < btif_max_av_clients; i++)
        btif_av_cb[i].dual_handoff = false;
      osi_property_get("persist.service.bt.a2dp.sink", a2dp_role, "false");
      if (!strncmp("false", a2dp_role, 5)) {
        btif_av_cb[index].peer_sep = AVDT_TSEP_SNK;
        isA2dpSink = true;
      } else {
        btif_av_cb[index].peer_sep = AVDT_TSEP_SRC;
        isA2dpSink = false;
      }
      /* This API will be called twice at initialization
      ** Idle can be moved when device is disconnected too.
      ** Take care of other connected device here.*/
      if (!btif_av_is_connected()) {
        BTIF_TRACE_EVENT("reset A2dp states in IDLE ");
        bta_av_co_init(btif_av_cb[index].codec_priorities);
        btif_a2dp_on_idle(index);
      } else {
        //There is another AV connection, update current playin
        BTIF_TRACE_EVENT("reset A2dp states in IDLE ");
        btif_av_update_current_playing_device(index);
      }
      if (!btif_av_is_playing_on_other_idx(index) &&
           btif_av_is_split_a2dp_enabled()) {
        BTIF_TRACE_EVENT("reset Vendor flag A2DP state is IDLE");
        reconfig_a2dp = FALSE;
        btif_media_send_reset_vendor_state();
      }
      break;

    case BTIF_SM_EXIT_EVT:
      if (codec_config_update_enabled == false) {
        codec_config_update_enabled = true;
        BTIF_TRACE_IMP("%s: Codec config update enabled changed to true", __func__);
      }
      break;

    case BTA_AV_ENABLE_EVT:
      BTIF_TRACE_EVENT("AV is enabled now for index: %d", index);
      break;

    case BTA_AV_REGISTER_EVT:
      BTIF_TRACE_EVENT("The AV Handle:%d", ((tBTA_AV*)p_data)->registr.hndl);
      btif_av_cb[index].bta_handle = ((tBTA_AV*)p_data)->registr.hndl;
      break;

    case BTIF_AV_CONNECT_REQ_EVT: {
        btif_av_connect_req_t* connect_req_p = (btif_av_connect_req_t*)p_data;
        btif_av_cb[index].peer_bda = *connect_req_p->target_bda;
        BTA_AvOpen(btif_av_cb[index].peer_bda, btif_av_cb[index].bta_handle, true,
                   BTA_SEC_AUTHENTICATE, connect_req_p->uuid);

      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_OPENING);
      } break;

    case BTA_AV_PENDING_EVT:
    case BTA_AV_RC_OPEN_EVT:
      /* IOP_FIX: Jabra 620 only does RC open without AV open whenever it
       * connects. So
       * as per the AV WP, an AVRC connection cannot exist without an AV
       * connection. Therefore,
       * we initiate an AV connection if an RC_OPEN_EVT is received when we are
       * in AV_CLOSED state.
       * We initiate the AV connection after a small 3s timeout to avoid any
       * collisions from the
       * headsets, as some headsets initiate the AVRC connection first and then
       * immediately initiate the AV connection
       *
       * TODO: We may need to do this only on an AVRCP Play. FixMe
       */
      /* Check if connection allowed with this device
       * In Dual A2dp case, this event can come for both the headsets.
       * Reject second connection request as we are already checking
       * for device priority for first device and we cannot queue
       * incoming connections requests.
       */
      if (idle_rc_event != 0) {
        BTIF_TRACE_DEBUG("Processing another RC Event ");
        return false;
      }
      memcpy(&idle_rc_data, ((tBTA_AV*)p_data), sizeof(tBTA_AV));
      if (event == BTA_AV_RC_OPEN_EVT) {
        if (((tBTA_AV*)p_data)->rc_open.status == BTA_AV_SUCCESS) {
            btif_av_cb[index].peer_bda = ((tBTA_AV*)p_data)->rc_open.peer_addr;
        } else {
          idle_rc_event = 0;
          return true;
        }
      } else
        btif_av_cb[index].peer_bda = ((tBTA_AV*)p_data)->pend.bd_addr;

      // Only for AVDTP connection request move to opening state
      if (event == BTA_AV_PENDING_EVT)
        btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_OPENING);

      if (bt_av_src_callbacks != NULL) {
        BTIF_TRACE_DEBUG("Calling connection priority callback ");
        idle_rc_event = event;
        HAL_CBACK(bt_av_src_callbacks, connection_priority_cb,
           &(btif_av_cb[index].peer_bda));
        if (event == BTA_AV_PENDING_EVT)
          BTA_AvOpen(btif_av_cb[index].peer_bda,
                     btif_av_cb[index].bta_handle, true,
                     BTA_SEC_AUTHENTICATE, UUID_SERVCLASS_AUDIO_SOURCE);
      }
      if (bt_av_sink_callbacks != NULL) {
        if (event == BTA_AV_PENDING_EVT)
            BTA_AvOpen(btif_av_cb[index].peer_bda, btif_av_cb[index].bta_handle,
               true, BTA_SEC_AUTHENTICATE, UUID_SERVCLASS_AUDIO_SINK);
        else if (event == BTA_AV_RC_OPEN_EVT) {
          alarm_set_on_mloop(av_open_on_rc_timer,
          BTIF_TIMEOUT_AV_OPEN_ON_RC_MS,
          btif_initiate_av_open_timer_timeout, NULL);
          btif_rc_handler(event, (tBTA_AV *)p_data);
        }
      }
      break;

    case BTA_AV_RC_BROWSE_OPEN_EVT:
      BTIF_TRACE_DEBUG("%s: BTA_AV_RC_BROWSE_OPEN_EVT received", __func__);
      btif_rc_handler(event, (tBTA_AV*)p_data);
      break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT:
      btif_update_source_codec(p_data);
      break;

    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT:
    {
      RawAddress dummy_bdaddr = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
      btif_report_source_codec_state(NULL, &dummy_bdaddr);
    }
    break;

    /*
     * In case Signalling channel is not down
     * and remote started Streaming Procedure
     * we have to handle config and open event in
     * idle_state. We hit these scenarios while running
     * PTS test case for AVRCP Controller
     */
    case BTIF_AV_SINK_CONFIG_REQ_EVT: {
      btif_av_sink_config_req_t req;
      // copy to avoid alignment problems
      memcpy(&req, p_data, sizeof(req));

      BTIF_TRACE_WARNING(
          "%s: BTIF_AV_SINK_CONFIG_REQ_EVT sample_rate=%d "
          "channel_count=%d",
          __func__, req.sample_rate, req.channel_count);
      if (bt_av_sink_callbacks != NULL) {
        HAL_CBACK(bt_av_sink_callbacks, audio_config_cb, &(req.peer_bd),
                  req.sample_rate, req.channel_count);
      }
    } break;

    case BTA_AV_OPEN_EVT: {
      /* We get this event in Idle State if Signaling
       * channel is not closed, only Streaming channel was
       * closed earlier, and now only stream setup process is
       * initiated.
       */
      tBTA_AV *p_bta_data = (tBTA_AV*)p_data;
      btav_connection_state_t state;
      BTIF_TRACE_DEBUG("status:%d, edr 0x%x",p_bta_data->open.status,
                               p_bta_data->open.edr);

      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        state = BTAV_CONNECTION_STATE_CONNECTED;
        btif_av_cb[index].edr = p_bta_data->open.edr;
        if (p_bta_data->open.role == HOST_ROLE_SLAVE)
          btif_av_cb[index].is_slave = true;
        btif_av_cb[index].peer_sep = p_bta_data->open.sep;

        if (p_bta_data->open.edr & BTA_AV_EDR_3MBPS) {
          BTIF_TRACE_DEBUG("remote supports 3 mbps");
          btif_av_cb[index].edr_3mbps = true;
        }

        btif_av_cb[index].peer_bda = ((tBTA_AV*)p_data)->open.bd_addr;
      } else {
        BTIF_TRACE_WARNING("BTA_AV_OPEN_EVT::FAILED status: %d",
                             p_bta_data->open.status );
        state = BTAV_CONNECTION_STATE_DISCONNECTED;
      }

      /* change state to open based on the status */
      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        /* inform the application of the event */
        if (btif_av_is_split_a2dp_enabled() &&
          btif_a2dp_audio_if_init != true) {
          BTIF_TRACE_DEBUG("Got OPEN_EVT in IDLE state, init audio interface");
          btif_a2dp_audio_interface_init();
          btif_a2dp_audio_if_init = true;
        }
        btif_report_connection_state(state, &(btif_av_cb[index].peer_bda));
        btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_OPENED);
        /* BTIF AV State updated, now check
         * and update multicast state
         */
        btif_av_update_multicast_state(index);

        btif_av_cb[index].current_playing = TRUE;
        if (enable_multicast == FALSE)
        {
          for (i = 0; i < btif_max_av_clients; i++)
          {
            if (i != index)
            {
              btif_av_cb[i].current_playing = FALSE;
            }
          }
          if (btif_av_is_playing())
          {
              BTIF_TRACE_DEBUG("Trigger Dual A2dp Handoff on %d", index);
              btif_av_trigger_dual_handoff(TRUE, btif_av_cb[index].peer_bda.address);
          }
        }

        if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
          /* if queued PLAY command,  send it now */
          btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                   (p_bta_data->open.status == BTA_AV_SUCCESS));
        } else if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
          /* if queued PLAY command,  send it now */
          btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr, false);
          /* Bring up AVRCP connection too */
          BTA_AvOpenRc(btif_av_cb[index].bta_handle);
        }
      }

      btif_queue_advance();
    } break;

    case BTA_AV_REMOTE_CMD_EVT:
    case BTA_AV_VENDOR_CMD_EVT:
    case BTA_AV_META_MSG_EVT:
    case BTA_AV_RC_FEAT_EVT:
    case BTA_AV_REMOTE_RSP_EVT:
      btif_rc_handler(event, (tBTA_AV*)p_data);
      break;

    case BTA_AV_RC_CLOSE_EVT:
      BTIF_TRACE_DEBUG("%s: BTA_AV_RC_CLOSE_EVT: Stopping AV timer.", __func__);
      BTIF_TRACE_DEBUG("BTA_AV_RC_CLOSE_EVT: clear peer_bda.address index : %d", index);
      btif_av_cb[index].peer_bda =  RawAddress::kEmpty;
      BTIF_TRACE_DEBUG("BTA_AV_RC_CLOSE_EVT: Stopping AV timer.");
      alarm_cancel(av_open_on_rc_timer);
      btif_rc_handler(event, (tBTA_AV*)p_data);
      break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      BTIF_TRACE_ERROR(
          "%s: BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started IDLE",
          __func__);
      btif_a2dp_on_offload_started(BTA_AV_FAIL);
      break;

    default:
      BTIF_TRACE_WARNING("%s: unhandled event=%s", __func__,
                         dump_av_sm_event_name((btif_av_sm_event_t)event));
      return false;
  }

  return true;
}
/*****************************************************************************
 *
 * Function        btif_av_state_opening_handler
 *
 * Description     Intermediate state managing events during establishment
 *                 of avdtp channel
 *
 * Returns         true if event was processed, false otherwise
 *
 ******************************************************************************/

static bool btif_av_state_opening_handler(btif_sm_event_t event, void* p_data,
                                          int index) {
  int i;
  RawAddress* bt_addr = nullptr;
  BTIF_TRACE_IMP("%s event:%s flags %x on index %x", __func__,
                   dump_av_sm_event_name((btif_av_sm_event_t)event),
                   btif_av_cb[index].flags, index);

  switch (event) {
    case BTIF_SM_ENTER_EVT:
      /* inform the application that we are entering connecting state */
      if (bt_av_sink_callbacks != NULL)
        HAL_CBACK(bt_av_sink_callbacks, connection_state_cb,
                  BTAV_CONNECTION_STATE_CONNECTING, &(btif_av_cb[index].peer_bda));
      else if (bt_av_src_callbacks != NULL)
        HAL_CBACK(bt_av_src_callbacks, connection_state_cb,
                  BTAV_CONNECTION_STATE_CONNECTING, &(btif_av_cb[index].peer_bda));
      break;

    case BTIF_SM_EXIT_EVT:
      break;

    case BTA_AV_REJECT_EVT:
      BTIF_TRACE_WARNING("%s: Received BTA_AV_REJECT_EVT", __func__);
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                   &(btif_av_cb[index].peer_bda));
      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
      break;

    case BTA_AV_OPEN_EVT: {
      tBTA_AV* p_bta_data = (tBTA_AV*)p_data;
      btav_connection_state_t state;
      btif_sm_state_t av_state;
      BTIF_TRACE_DEBUG("status:%d, edr 0x%x, role: 0x%x", p_bta_data->open.status,
                       p_bta_data->open.edr, p_bta_data->open.role);

      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        state = BTAV_CONNECTION_STATE_CONNECTED;
        av_state = BTIF_AV_STATE_OPENED;
/* SPLITA2DP */
        if (btif_a2dp_audio_if_init != true) {
          if (btif_av_is_split_a2dp_enabled()) {
            BTIF_TRACE_DEBUG("Split a2dp enabled:initialize interface ");
            btif_a2dp_audio_if_init = true;
            btif_a2dp_audio_interface_init();
          } else {
            BTIF_TRACE_DEBUG("Split a2dp not enabled");
          }
        } else {
          BTIF_TRACE_DEBUG("audio interface is already initialized");
        }
/* SPLITA2DP */
        btif_av_cb[index].edr = p_bta_data->open.edr;
        if (p_bta_data->open.role == HOST_ROLE_SLAVE)
          btif_av_cb[index].is_slave = true;
        btif_av_cb[index].peer_sep = p_bta_data->open.sep;
        if (p_bta_data->open.edr & BTA_AV_EDR_3MBPS) {
          BTIF_TRACE_DEBUG("remote supports 3 mbps");
          btif_av_cb[index].edr_3mbps = true;
        }
      } else {
        BTIF_TRACE_WARNING("%s: BTA_AV_OPEN_EVT::FAILED status: %d", __func__,
                           p_bta_data->open.status);

        /* Multicast: Check if connected to AVRC only device
         * disconnect when Dual A2DP/Multicast is supported.
         */
        RawAddress peer_addr;
        uint8_t peer_handle = BTRC_HANDLE_NONE;
        if (btif_rc_get_connected_peer(&peer_addr) &&
            (btif_av_cb[index].peer_bda == peer_addr)) {
          /* Do not disconnect AVRCP connection if A2DP
           * connection failed due to SDP failure since remote
           * may not support A2DP. In such case we will keep
           * AVRCP only connection.
           */
          if (p_bta_data->open.status != BTA_AV_FAIL_SDP) {
            BTIF_TRACE_WARNING("%s: Disconnecting AVRCP: peer_addr=%s", __func__,
                             peer_addr.ToString().c_str());
            peer_handle = btif_rc_get_connected_peer_handle(peer_addr);
            if (peer_handle != BTRC_HANDLE_NONE) {
              BTA_AvCloseRc(peer_handle);
            }
          } else {
            BTIF_TRACE_WARNING("Keep AVRCP only connection");
          }
        }
        state = BTAV_CONNECTION_STATE_DISCONNECTED;
        av_state = BTIF_AV_STATE_IDLE;
      }
      if (p_bta_data->open.status != BTA_AV_SUCCESS &&
              p_bta_data->open.status != BTA_AV_FAIL_SDP) {
          btif_av_check_and_start_collission_timer(index);
      }
      /* inform the application of the event */
      btif_report_connection_state(state, &(btif_av_cb[index].peer_bda));
      /* change state to open/idle based on the status */
      btif_sm_change_state(btif_av_cb[index].sm_handle, av_state);
      /* Check if the other connected AV is playing. If YES, trigger DUAL Handoff */
      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        /* BTIF AV State updated, now check
         * and update multicast state
         */
        btif_av_update_multicast_state(index);

        /*This device should be now ready for all next playbacks*/
        btif_av_cb[index].current_playing = true;
        if (enable_multicast == false) {
          for (i = 0; i < btif_max_av_clients; i++)
            /* Other device is not current playing */
            if (i != index)
              btif_av_cb[i].current_playing = false;

          /* In A2dp Multicast, stack will take care of starting
           * the stream on newly connected A2dp device. If Handoff
           * is supported, trigger Handoff here. */
          if (btif_av_is_playing()) {
            BTIF_TRACE_DEBUG("Trigger Dual A2dp Handoff on %d", index);
            btif_av_trigger_dual_handoff(true, btif_av_cb[index].peer_bda);
          }
        }
        if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
          /* if queued PLAY command,  send it now */
          btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                      (p_bta_data->open.status == BTA_AV_SUCCESS));
        else if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
          /* if queued PLAY command,  send it now */
          btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr, false);
          /* Bring up AVRCP connection too */
          BTA_AvOpenRc(btif_av_cb[index].bta_handle);
        }
      }
        btif_queue_advance();
    } break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT:
      btif_update_source_codec(p_data);
      break;

    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT:
      if (p_data != NULL) {
          bt_addr = (RawAddress *)p_data;
      } else {
          BTIF_TRACE_DEBUG("%s: p_data not null", __func__);
          bt_addr = &btif_av_cb[index].peer_bda;
      }

      btif_report_source_codec_state(p_data, bt_addr);
      break;

    case BTIF_AV_SINK_CONFIG_REQ_EVT: {
      btif_av_sink_config_req_t req;
      // copy to avoid alignment problems
      memcpy(&req, p_data, sizeof(req));

      BTIF_TRACE_WARNING("BTIF_AV_SINK_CONFIG_REQ_EVT %d %d", req.sample_rate,
                         req.channel_count);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC &&
          bt_av_sink_callbacks != NULL) {
        HAL_CBACK(bt_av_sink_callbacks, audio_config_cb, &(btif_av_cb[index].peer_bda),
                  req.sample_rate, req.channel_count);
      }
    } break;

    case BTIF_AV_CONNECT_REQ_EVT: {
      // Check for device, if same device which moved to opening then ignore
      // callback
      if (memcmp(((btif_av_connect_req_t*)p_data)->target_bda,
                 &(btif_av_cb[index].peer_bda), sizeof(btif_av_cb[index].peer_bda)) == 0) {
        BTIF_TRACE_DEBUG(
            "%s: Same device moved to Opening state,ignore Connect Req",
            __func__);
      } else {
          btif_av_connect_req_t* connect_req_p = (btif_av_connect_req_t*)p_data;
          RawAddress& target_bda = *connect_req_p->target_bda;
        BTIF_TRACE_WARNING(
            "%s: device %s is already connecting, reject Connect request to %s",
            __func__, btif_av_cb[index].peer_bda.ToString().c_str(),
            target_bda.ToString().c_str());
        btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                     &target_bda);
      }
      // Ignore all connection request if we are already opening
      btif_queue_advance();
    } break;

    case BTA_AV_PENDING_EVT: {
      // Check for device, if same device which moved to opening then ignore
      // callback
      if (((tBTA_AV*)p_data)->pend.bd_addr == btif_av_cb[index].peer_bda) {
        BTIF_TRACE_DEBUG(
            "%s: Same device moved to Opening state,ignore Pending Req",
            __func__);
        break;
      } else {
          const RawAddress& bd_addr = ((tBTA_AV*)p_data)->pend.bd_addr;
        BTIF_TRACE_WARNING(
            "%s: device %s is already connecting, reject incoming request "
            "from %s",
            __func__, btif_av_cb[index].peer_bda.ToString().c_str(),
            bd_addr.ToString().c_str());
        BTA_AvDisconnect(bd_addr);
      }
    } break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      BTIF_TRACE_ERROR(
          "%s: BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started OPENING",
          __func__);
      btif_a2dp_on_offload_started(BTA_AV_FAIL);
      break;

    case BTA_AV_CLOSE_EVT:
      /* avdtp link is closed. Check if any other device is playing
       * and this is not the one
       */
      if (!btif_av_is_playing())
        btif_a2dp_on_stopped(NULL);
      /* inform the application that we are disconnected */
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
         &(btif_av_cb[index].peer_bda));
      btif_queue_advance();
      btif_av_check_and_start_collission_timer(index);
      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
       btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
           &(btif_av_cb[index].peer_bda));
       BTA_AvClose(btif_av_cb[index].bta_handle);
       btif_queue_advance();
       btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
       break;

    case BTA_AV_RC_OPEN_EVT:
       btif_rc_handler(event, (tBTA_AV*)p_data);;
       break;

    case BTA_AV_DELAY_REPORT_EVT:
      /* Initial delay report after avdtp stream configuration */
      BTIF_TRACE_DEBUG("%s : BTA_AV_DELAY_REPORT_EVT received", __func__);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        btif_av_cb[index].sink_latency = ((tBTA_AV*)p_data)->delay_rpt.sink_delay;
      }
      break;

      CHECK_RC_EVENT(event, (tBTA_AV*)p_data);

    default:
      BTIF_TRACE_WARNING("%s: unhandled event=%s", __func__,
                         dump_av_sm_event_name((btif_av_sm_event_t)event));
      return false;
  }
  return true;
}

/*****************************************************************************
 *
 * Function        btif_av_state_closing_handler
 *
 * Description     Intermediate state managing events during closing
 *                 of avdtp channel
 *
 * Returns         true if event was processed, false otherwise
 *
 ******************************************************************************/

static bool btif_av_state_closing_handler(btif_sm_event_t event, void* p_data, int index) {
  RawAddress *bt_addr = nullptr;

  BTIF_TRACE_IMP("%s event:%s flags %x and index %x", __func__,
                   dump_av_sm_event_name((btif_av_sm_event_t)event),
                   btif_av_cb[index].flags, index);

  switch (event) {
    case BTIF_SM_ENTER_EVT:
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        /* Multicast/Soft Hand-off:
         * If MC/SHO is enabled we need to keep/start playing on
         * other device.
         */
        if (btif_av_is_connected_on_other_idx(index)) {
          if (btif_av_is_playing()) {
              APPL_TRACE_DEBUG("Keep playing on other device");
          } else {
             if (btif_av_cb[index].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) {
                 APPL_TRACE_DEBUG("Not playing on other device: Stop media task as local suspend pending");
                 btif_a2dp_on_stopped(NULL);
             } else {
                APPL_TRACE_DEBUG("Not playing on other devie: Set Flush");
                btif_a2dp_source_set_tx_flush(true);
             }
          }
        } else {
          /* Single connections scenario:
           * Immediately stop transmission of frames
           * wait for audioflinger to stop a2dp
           */
          btif_a2dp_source_set_tx_flush(true);
        }
      }
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC)
        btif_a2dp_sink_set_rx_flush(true);
      break;

    case BTA_AV_STOP_EVT:
    case BTIF_AV_STOP_STREAM_REQ_EVT:
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        /* Dont stop in DUAL A2dp connections, as
         * UIPC will keep waiting for Audio CTRL channel
         * to get closed which is not required in Dual A2dp.
         * We will stop only when only single A2dp conn is present.*/
        if (btif_av_is_connected_on_other_idx(index)) {
          if (!btif_av_is_playing()) {
            APPL_TRACE_WARNING("Suspend the AV Data channel");
            //Stop media task
            btif_a2dp_on_stopped(NULL);
          }
        } else {
          /* immediately flush any pending tx frames while suspend is pending */
          APPL_TRACE_WARNING("Stop the AV Data channel");
          btif_a2dp_source_set_tx_flush(true);
          btif_a2dp_on_stopped(NULL);
        }
      }
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
        btif_a2dp_sink_set_rx_flush(true);
        btif_a2dp_on_stopped(NULL);
      }
      break;

    case BTIF_SM_EXIT_EVT:
      break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT:
      btif_update_source_codec(p_data);
      break;

    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT:
      if (p_data != NULL) {
          bt_addr = (RawAddress *)p_data;
      } else {
          BTIF_TRACE_DEBUG("%s: p_data not null", __func__);
          bt_addr = &btif_av_cb[index].peer_bda;
      }


      btif_report_source_codec_state(p_data, bt_addr);
      break;

    case BTA_AV_CLOSE_EVT:
      /* inform the application that we are disconnecting */
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                   &(btif_av_cb[index].peer_bda));

      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
      break;

    /* Handle the RC_CLOSE event for the cleanup */
    case BTA_AV_RC_CLOSE_EVT:
      btif_rc_handler(event, (tBTA_AV*)p_data);
      break;

    /* Handle the RC_BROWSE_CLOSE event for tetsing*/
    case BTA_AV_RC_BROWSE_CLOSE_EVT:
      btif_rc_handler(event, (tBTA_AV*)p_data);
      break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      BTIF_TRACE_ERROR(
          "%s: BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started Closing",
          __func__);
      btif_a2dp_on_offload_started(BTA_AV_FAIL);
      break;

    default:
      BTIF_TRACE_WARNING("%s: unhandled event=%s", __func__,
                         dump_av_sm_event_name((btif_av_sm_event_t)event));
      return false;
  }
  return true;
}

/*****************************************************************************
 *
 * Function     btif_av_state_opened_handler
 *
 * Description  Handles AV events while AVDTP is in OPEN state
 *
 * Returns      true if event was processed, false otherwise
 *
 ******************************************************************************/

static bool btif_av_state_opened_handler(btif_sm_event_t event, void* p_data,
                                         int index) {
  RawAddress * bt_addr = NULL;
  tBTA_AV* p_av = (tBTA_AV*)p_data;

  BTIF_TRACE_IMP("%s event:%s flags %x peer_sep %x and index %x", __func__,
                   dump_av_sm_event_name((btif_av_sm_event_t)event),
                   btif_av_cb[index].flags, btif_av_cb[index].peer_sep, index);

  if ((event == BTA_AV_REMOTE_CMD_EVT) &&
      (p_av->remote_cmd.rc_id == BTA_AV_RC_PLAY)) {
    if (btif_av_check_flag_remote_suspend(index)) {
      BTIF_TRACE_EVENT("%s: Resetting remote suspend flag on RC PLAY", __func__);
      btif_av_clear_remote_suspend_flag();
      if(btif_hf_is_call_vr_idle())
      {
        BTIF_TRACE_EVENT("%s: No active call, start stream", __func__);
        btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
      }
    }
  }

  switch (event) {
    case BTIF_SM_ENTER_EVT: {
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_STOP;
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
    } break;

    case BTIF_SM_EXIT_EVT: {
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
    } break;

    case BTIF_AV_START_STREAM_REQ_EVT: {
      /* update multicast state here if new device is connected
       * after A2dp connection. New A2dp device is connected
       * whlie playing */
      is_codec_config_dump = true;
      btif_av_update_multicast_state(index);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
        BTA_AvStart(btif_av_cb[index].bta_handle);
        btif_av_cb[index].flags |= BTIF_AV_FLAG_PENDING_START;
      } else {
        bt_status_t status = btif_a2dp_source_setup_codec(btif_av_cb[index].bta_handle);
        if (status == BT_STATUS_SUCCESS) {
          int idx = 0;
          BTA_AvStart(btif_av_cb[index].bta_handle);
          if (enable_multicast == true) {
            /* In A2dp Multicast, DUT initiated stream request
             * should be true for all connected A2dp devices
             */
            for (; idx < btif_max_av_clients; idx++)
              btif_av_cb[idx].flags |= BTIF_AV_FLAG_PENDING_START;
          } else
              btif_av_cb[index].flags |= BTIF_AV_FLAG_PENDING_START;
        } else {
          BTIF_TRACE_ERROR("## AV Disconnect## status : %x",status);
          BTA_AvDisconnect(btif_av_cb[index].peer_bda);
        }
      }
    } break;

    case BTA_AV_START_EVT: {
      BTIF_TRACE_DEBUG("BTA_AV_START_EVT status %d, suspending %d, init %d",
          p_av->start.status, p_av->start.suspending, p_av->start.initiator);
      BTIF_TRACE_DEBUG("BTA_AV_START_EVT role: %d", p_av->start.role);
      if (p_av->start.role == HOST_ROLE_SLAVE) {
        btif_av_cb[index].is_slave = true;
      } else {
        // update if we are master after role switch before start
        btif_av_cb[index].is_slave = false;
      }
      /* There can be role switch after device is connected,
       * hence check for role before starting multicast, and
       * disable if we are in slave role for any connection
       */
      btif_av_update_multicast_state(index);

      if ((p_av->start.status == BTA_SUCCESS) && (p_av->start.suspending == true))
        return true;

      /* if remote tries to start a2dp when call is in progress, suspend it right away */
      if ((!(btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START)) &&
            (!btif_hf_is_call_vr_idle())) {
        BTIF_TRACE_EVENT("%s: trigger suspend as call is in progress!!", __func__);
        btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
        btif_av_cb[index].remote_started = true;
        btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_STARTED);
        btif_dispatch_sm_event(BTIF_AV_REMOTE_SUSPEND_STREAM_REQ_EVT, NULL, 0);
        break;
      }

      /* if remote tries to start a2dp when DUT is a2dp source
       * then suspend. In case a2dp is sink and call is active
       * then disconnect the AVDTP channel
       */
      if (!(btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START)) {
        if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
          if (enable_multicast) {
            /* Stack will start the playback on newly connected
             * A2dp device, if the playback is already happening on
             * other connected device.*/
            if (btif_av_is_playing())
              /* when HS2 is connected during HS1 playing, stack directly
               * sends start event hence update encoder so that least L2CAP
               *  MTU is selected.*/
              BTIF_TRACE_DEBUG("%s: A2dp Multicast playback",
                            __func__);
            /* initiate suspend if start is initiate by remote and multicast
             * is enabled.
             * Avoid suspend if stream is started as quick suspend-start
             * creates IOT issue, seen with SBH50.
             */
            if (!p_av->start.initiator && !btif_av_is_playing()) {
              BTIF_TRACE_DEBUG("initiate suspend for remote start");
              btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
            }
          } else {
            if ((btif_av_cb[index].flags & BTIF_AV_FLAG_REMOTE_SUSPEND)) {
                BTIF_TRACE_DEBUG("%s: clear remote suspend flag on remote start",
                __func__);
                btif_av_cb[index].flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
            } else {
              BTIF_TRACE_DEBUG("%s: honor remote start",__func__);
              btif_av_cb[index].remote_started = true;
              btif_a2dp_honor_remote_start();
            }
          }
        }
      }

      /* remain in open state if status failed */
      /* Multicast-soft Handoff:
       * START failed, cleanup Handoff flag.
       */
      if (p_av->start.status != BTA_AV_SUCCESS)
      {
        int i;

        /* In case peer is A2DP SRC we do not want to ack commands on UIPC */
        if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
          if (btif_a2dp_on_started(&p_av->start,
              ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) != 0),
                btif_av_cb[index].bta_handle))
                  /* only clear pending flag after acknowledgement */
                  btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
          /* Clear dual handoff flag */
          for(i = 0; i < btif_max_av_clients; i++)
              btif_av_cb[i].dual_handoff = false;

          return false;
      }

#ifndef AVK_BACKPORT
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC)
          btif_a2dp_sink_set_rx_flush(false); /*  remove flush state, ready for streaming*/
#endif
      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_STARTED);
    } break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT: {
      btif_update_source_codec(p_data);
    } break;

    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT: {
      if (p_data != NULL) {
          bt_addr = (RawAddress *)p_data;
      } else {
          BTIF_TRACE_DEBUG("%s: p_data not null", __func__);
          bt_addr = &btif_av_cb[index].peer_bda;
      }

      btif_report_source_codec_state(p_data, bt_addr);
      if (btif_av_is_split_a2dp_enabled()) {
        if (codec_cfg_change) {
          codec_cfg_change = false;
          reconfig_a2dp = TRUE;
          HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                                       &(btif_av_cb[index].peer_bda), 0, 0);
        }
      }
    } break;

    case BTIF_AV_DISCONNECT_REQ_EVT: {
      BTA_AvClose(btif_av_cb[index].bta_handle);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
        BTA_AvCloseRc(btif_av_cb[index].bta_handle);
      }

/* SPLITA2DP */
      if (!btif_av_is_connected_on_other_idx(index)) {
        if (btif_av_is_split_a2dp_enabled()) {
          btif_a2dp_audio_if_init = false;
          btif_a2dp_audio_interface_deinit();
        }
      }
/* SPLITA2DP */
      /* inform the application that we are disconnecting */
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING,
                                   &(btif_av_cb[index].peer_bda));
    } break;

    case BTA_AV_CLOSE_EVT: {
      /* avdtp link is closed
       * Don't close the A2dp when Dual playback is happening
       */
      if (btif_av_is_connected_on_other_idx(index)) {
        APPL_TRACE_WARNING("Conn is closing,close AV data channel");
        if ((!btif_av_is_split_a2dp_enabled() ||
             (btif_av_is_split_a2dp_enabled() && !reconfig_a2dp)) &&
            ((btif_av_cb[index].flags & BTIF_AV_FLAG_REMOTE_SUSPEND) != 0) &&
            ((btif_av_cb[index].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) == 0)) {
          /* fake handoff state to switch streaming to other codec device */
          btif_av_cb[index].dual_handoff = true;
          if (btif_av_is_split_a2dp_enabled()) {
            BTIF_TRACE_DEBUG("%s: Notify framework to reconfigure",__func__);
            int idx = btif_av_get_other_connected_idx(index);
            if (idx != INVALID_INDEX) {
              if ((btif_av_cb[idx].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) == 0) {
                reconfig_a2dp = true;
                HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                                              &(btif_av_cb[idx].peer_bda), 0, 0);
              }
            }
          } else {
            BTIF_TRACE_DEBUG("%s: Start streaming on connected remote",__func__);
            btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
          }
        } else if (!btif_av_is_playing()) {
          APPL_TRACE_WARNING("Suspend the AV Data channel");
          /* ensure tx frames are immediately suspended */
          btif_a2dp_source_set_tx_flush(true);
          btif_a2dp_source_stop_audio_req();
        }
      } else {
        APPL_TRACE_WARNING("Stop the AV Data channel");
/* SPLITA2DP */
        if (btif_av_is_split_a2dp_enabled()) {
          if (btif_a2dp_audio_if_init) {
            btif_a2dp_audio_if_init = false;
            btif_a2dp_audio_interface_deinit();
          }
        }
/* SPLITA2DP */
        btif_a2dp_on_stopped(NULL);
      }

      /* inform the application that we are disconnected */
      btif_av_cb[index].flags |= BTIF_AV_FLAG_PENDING_DISCONNECT;
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                        &(btif_av_cb[index].peer_bda));
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_DISCONNECT;

      /* change state to idle, send acknowledgement if start is pending */
      if (btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) {
        btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
        btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
      }

      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
    } break;

    case BTA_AV_RECONFIG_EVT: {
      if ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) &&
          (p_av->reconfig.status == BTA_AV_SUCCESS)) {
        APPL_TRACE_WARNING("reconfig done BTA_AVstart()");
        BTA_AvStart(btif_av_cb[index].bta_handle);
      } else if (btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) {
        btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
        btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
      }
      if (btif_av_is_split_a2dp_enabled()) {
        HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                                          &(btif_av_cb[index].peer_bda), 0, 0);
      }
      btif_av_cb[index].reconfig_pending = false;
    } break;

    case BTIF_AV_CONNECT_REQ_EVT: {
      if (memcmp((RawAddress*)p_data, &(btif_av_cb[index].peer_bda),
                 sizeof(btif_av_cb[index].peer_bda)) == 0) {
        BTIF_TRACE_DEBUG("%s: Ignore BTIF_AV_CONNECT_REQ_EVT for same device",
                         __func__);
      } else {
          btif_av_connect_req_t* connect_req_p = (btif_av_connect_req_t*)p_data;
          RawAddress& target_bda = *connect_req_p->target_bda;
        BTIF_TRACE_WARNING(
            "%s: Moved to opened by Other incoming Connect request: "
            "target_bda=%s",
            __func__, target_bda.ToString().c_str());
        btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                     &target_bda);
      }
      btif_queue_advance();
    } break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT: {
      BTIF_TRACE_ERROR(
          "%s: BTIF_AV_OFFLOAD_START_REQ_EVT: Stream not Started Opened",
          __func__);
      btif_a2dp_on_offload_started(BTA_AV_FAIL);
    } break;

    case BTA_AV_OFFLOAD_START_RSP_EVT:
      APPL_TRACE_WARNING("Offload Start Rsp is unsupported in opened state, status = %d", p_av->status);
      if (btif_av_cb[index].flags & BTIF_AV_FLAG_REMOTE_SUSPEND) {
        if (p_av->status == BTA_AV_SUCCESS) {
          btif_a2dp_src_vsc.tx_started = TRUE;
          bta_av_vendor_offload_stop();
        }
        btif_a2dp_on_offload_started(BTA_AV_FAIL_UNSUPPORTED);
      }
      break;

    case BTA_AV_RC_OPEN_EVT: {
      btif_av_check_rc_connection_priority(p_data);
    }  break;

    case BTIF_AV_SETUP_CODEC_REQ_EVT:{
      uint8_t hdl = btif_av_get_av_hdl_from_idx(index);
      APPL_TRACE_DEBUG("%s: hdl = %d",__func__, hdl);
      if (hdl >= 0)
        btif_a2dp_source_setup_codec(hdl);
      }
      break;

    case BTA_AV_DELAY_REPORT_EVT:
      /* Initial delay report after avdtp stream configuration */
      BTIF_TRACE_DEBUG("%s : BTA_AV_DELAY_REPORT_EVT received", __func__);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        btif_av_cb[index].sink_latency = ((tBTA_AV*)p_data)->delay_rpt.sink_delay;
      }
      break;

      CHECK_RC_EVENT(event, (tBTA_AV*)p_data);

    default: {
      BTIF_TRACE_WARNING("%s : unhandled event:%s", __func__,
                          dump_av_sm_event_name((btif_av_sm_event_t)event));
      return false;
    }
  }
  return true;
}

/*****************************************************************************
 *
 * Function     btif_av_state_started_handler
 *
 * Description  Handles AV events while A2DP stream is started
 *
 * Returns      true if event was processed, false otherwise
 *
 ******************************************************************************/

static bool btif_av_state_started_handler(btif_sm_event_t event, void* p_data,
                                          int index) {
  tBTA_AV* p_av = (tBTA_AV*)p_data;
  RawAddress *bt_addr = nullptr;
  RawAddress playing_address = RawAddress::kEmpty;
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  int i;
  bool hal_suspend_pending = false;
  tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();
  bool remote_start_cancelled = false;
  BTIF_TRACE_IMP("%s event:%s flags %x  index =%d", __func__,
                   dump_av_sm_event_name((btif_av_sm_event_t)event),
                   btif_av_cb[index].flags, index);

  switch (event) {
    case BTIF_SM_ENTER_EVT:
      btif_rc_get_playing_device(&playing_address);
      if (!playing_address.IsEmpty() &&
          (playing_address == btif_av_cb[index].peer_bda) &&
          (btif_av_cb[index].flags == BTIF_AV_FLAG_PENDING_START)) {
          BTIF_TRACE_IMP("%s Clear play process flag",__func__);
          btif_rc_clear_playing_state(false);
      }
      /* Ack from entry point of started handler instead of open state to avoid
       * race condition
       */
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        if (!((!enable_multicast)&&((btif_av_cb[index].remote_started)
            && ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) == 0)))) {
          BTIF_TRACE_IMP("%s update media task on DUT initiated start for index =%d", __func__, index);
          if (btif_a2dp_on_started(&p_av->start,
              ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) != 0),
              btif_av_cb[index].bta_handle)) {
            /* only clear pending flag after acknowledgement */
            btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
          }
        } else {
          BTIF_TRACE_IMP("%s Do not update media task on remote start for index =%d", __func__, index);
        }
      }

      /* Already changed state to started, send acknowledgement
       * if start is pending
       */
      if (btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START) {
          if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
              btif_a2dp_on_started(NULL, true, btif_av_cb[index].bta_handle);
          btif_av_cb[index].flags &= ~BTIF_AV_FLAG_PENDING_START;
      }

      /* we are again in started state, clear any remote suspend flags */
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;

      if ((!enable_multicast)&&((btif_av_cb[index].remote_started)
          && (btif_av_is_playing_on_other_idx(index)))) {
        BTIF_TRACE_IMP("%s Do not update audio state change to app for index =%d", __func__, index);
      } else {
        /**
            * Report to components above that we have entered the streaming
            * stage, this should usually be followed by focus grant.
            * see update_audio_focus_state()
            */
        btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_av_cb[index].peer_bda));
      }
      btif_av_cb[index].is_device_playing = true;

#ifdef AVK_BACKPORT
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC)
        btif_av_request_audio_focus(true);
#endif
      // Clear Dual Handoff for all SCBs for DUT initiated Start.
      //If split a2dp is enabled, clear dual handoff flag in offload resp evt.
      if ((!btif_av_is_split_a2dp_enabled()) &&
                    (!(btif_av_cb[index].remote_started))) {
        for(i = 0; i < btif_max_av_clients; i++) {
          btif_av_cb[i].dual_handoff = false;
          // Other device is not current playing
          if (i != index)
            btif_av_cb[i].current_playing = false;
        }
        BTIF_TRACE_IMP("%s Setting device as current playing for index = %d",
                                        __func__, index);
        // This is latest device to play now
        btif_av_cb[index].current_playing = true;
      } else {
          BTIF_TRACE_IMP("%s Remote Start, Not updating current playing for index = %d",
                                          __func__, index);
      }
      break;

    case BTIF_SM_EXIT_EVT:
      break;

    case BTIF_AV_START_STREAM_REQ_EVT:
      /* we were remotely started, just ack back the local request */
      if (btif_av_cb[index].remote_started) btif_av_cb[index].remote_started = false;
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK) {
        uint8_t hdl = btif_av_get_av_hdl_from_idx(index);
        if (hdl >= 0) {
          bt_status_t status = btif_a2dp_source_setup_codec(hdl);
          BTIF_TRACE_DEBUG("%s: hdl = %u, status : %x, enc_update_in_progress = %d",
                                __func__, hdl, status, enc_update_in_progress);
          if (status == BT_STATUS_SUCCESS) {
            enc_update_in_progress = TRUE;
            btif_a2dp_on_started(NULL, true, btif_av_cb[index].bta_handle);
            btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_av_cb[index].peer_bda));
            btif_av_cb[index].is_device_playing = true;
            for (i = 0; i < btif_max_av_clients; i++)
            {
              //Other device is not current playing
              if (i != index)
                btif_av_cb[i].current_playing = false;
            }
            //This is latest device to play now
            btif_av_cb[index].current_playing = true;
          } else {
            BTIF_TRACE_ERROR("## AV Disconnect## status : %x",status);
            BTA_AvDisconnect(btif_av_cb[index].peer_bda.address);
          }
          btif_av_reset_reconfig_flag();
        }
      }
      break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT:
      btif_av_cb[index].reconfig_pending = true;
      btif_av_flow_spec_cmd(index, reconfig_a2dp_param_val);
      btif_update_source_codec(p_data);
      break;

    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT:
      if (p_data != NULL) {
          bt_addr = (RawAddress *)p_data;
      } else {
          BTIF_TRACE_DEBUG("%s: p_data not null", __func__);
          bt_addr = &btif_av_cb[index].peer_bda;
      }


      btif_report_source_codec_state(p_data, bt_addr);
      if (btif_av_is_split_a2dp_enabled()) {
        if (codec_cfg_change) {
          codec_cfg_change = false;
          reconfig_a2dp = TRUE;
          if (isBitRateChange || isBitsPerSampleChange) {
            HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, RECONFIG_A2DP_PARAM,
                                       &(btif_av_cb[index].peer_bda), reconfig_a2dp_param_id, reconfig_a2dp_param_val);
            isBitRateChange = false;
            isBitsPerSampleChange = false;
          }
        }
      }
      break;

    /* fixme -- use suspend = true always to work around issue with BTA AV */
    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
      /* set pending flag to ensure btif task is not trying to restart
       * stream while suspend is in progress.
       * Multicast: If streaming is happening on both devices, we need
       * to update flag for both connections as SUSPEND request will
       * be sent to only one stream as internally BTA takes care of
       * suspending both streams.
       */
      if (enable_multicast) {
        for(i = 0; i < btif_max_av_clients; i++) {
          state = btif_sm_get_state(btif_av_cb[i].sm_handle);
          if (state == BTIF_AV_STATE_STARTED)
            btif_av_cb[i].flags |= BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;
        }
      } else {
        btif_av_cb[index].flags |= BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;
      }
      /* if we were remotely suspended but suspend locally, local suspend
       * always overrides
       */
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;

      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
      {
          if (btif_av_is_connected_on_other_idx(index)) {
            if (!btif_av_is_playing_on_other_idx(index)) {
              APPL_TRACE_WARNING("Suspend the AV Data channel");
              //Flush and close media channel
              btif_a2dp_source_set_tx_flush(true);
            } else
                APPL_TRACE_WARNING("Not flushing as one link is already streaming");
          } else {
            /* immediately flush any pending tx frames while suspend is pending */
            APPL_TRACE_WARNING("Stop the AV Data channel");
            btif_a2dp_source_set_tx_flush(true);
          }
      }

      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC) {
        btif_a2dp_sink_set_rx_flush(true);
        btif_a2dp_on_stopped(NULL);
      }

      BTA_AvStop(true, btif_av_cb[index].bta_handle);
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
      // Now it is not the current playing
      btif_av_cb[index].current_playing = false;
      btif_av_update_current_playing_device(index);
      btif_rc_clear_priority(btif_av_cb[index].peer_bda);
      // request avdtp to close
      BTA_AvClose(btif_av_cb[index].bta_handle);
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SRC)
          BTA_AvCloseRc(btif_av_cb[index].bta_handle);

      if (btif_av_is_split_a2dp_enabled() &&
        btif_av_is_connected_on_other_idx(index)) {
        /*Fake handoff state to switch streaming to other coddeced
          device */
        btif_av_cb[index].dual_handoff = true;
      } else {
        if (btif_a2dp_audio_if_init) {
          btif_a2dp_audio_if_init = false;
          btif_a2dp_audio_interface_deinit();
        }
      }
      // inform the application that we are disconnecting
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTING, &(btif_av_cb[index].peer_bda));

      // wait in closing state until fully closed
      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_CLOSING);
      if (btif_av_cb[index].dual_handoff == true) {
        BTIF_TRACE_DEBUG("%s: Notify framework to reconfig",__func__);
        int idx = btif_av_get_other_connected_idx(index);
        /* Fix for below Klockwork Issue
         * Array 'btif_av_cb' of size 2 may use index value(s) -1 */
        if (idx != INVALID_INDEX) {
          reconfig_a2dp = TRUE;
          HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                                          &(btif_av_cb[idx].peer_bda), 0, 0);
          }
      }
      break;

    case BTA_AV_SUSPEND_EVT:
      BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT status %d, init %d, flag %d",
           p_av->suspend.status, p_av->suspend.initiator, btif_av_cb[index].flags);
      // Check if this suspend is due to DUAL_Handoff
      if ((btif_av_cb[index].dual_handoff) &&
          (p_av->suspend.status == BTA_AV_SUCCESS)) {
        if (!btif_av_is_split_a2dp_enabled()){
          uint8_t curr_hdl = btif_av_cb[index].bta_handle;
          uint8_t* cur_codec_cfg = NULL;
          uint8_t* old_codec_cfg = NULL;
          cur_codec_cfg = bta_av_co_get_peer_codec_info(curr_hdl);
          if (cur_codec_cfg != NULL) {
           BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT: Current codec = ");
           for (int i = 0; i < AVDT_CODEC_SIZE; i++)
             BTIF_TRACE_EVENT("%d ",cur_codec_cfg[i]);
          }
          int other_index = btif_av_get_other_connected_idx(index);
          if (other_index != INVALID_INDEX) {
            uint8_t other_hdl = btif_av_cb[other_index].bta_handle;
            old_codec_cfg = bta_av_co_get_peer_codec_info(other_hdl);
          }
          if (old_codec_cfg != NULL) {
           BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT: Old codec = ");
           for (int i = 0; i < AVDT_CODEC_SIZE; i++)
             BTIF_TRACE_EVENT("%d ",old_codec_cfg[i]);
          }
          if ((cur_codec_cfg != NULL) && (old_codec_cfg != NULL)) {
           if((A2DP_GetTrackBitsPerSample(cur_codec_cfg)==A2DP_GetTrackBitsPerSample(old_codec_cfg))
              && (A2DP_GetTrackSampleRate(cur_codec_cfg)==A2DP_GetTrackSampleRate(old_codec_cfg) &&
              (A2DP_GetTrackChannelCount(cur_codec_cfg)==A2DP_GetTrackChannelCount(old_codec_cfg))))
            {
              BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT: Dual handoff");
              btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
            } else {
              btif_dispatch_sm_event(BTIF_AV_SETUP_CODEC_REQ_EVT, NULL, 0);
              is_block_hal_start = true;
              btif_trigger_unblock_audio_start_recovery_timer();
              BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT: Wait for Audio Start as codec params differs");
            }
          } else {
            BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT: Dual handoff either codec NULL");
            btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
          }
        } else {
          BTIF_TRACE_EVENT("BTA_AV_SUSPEND_EVT:SplitA2DP Disallow stack start wait Audio to Start");
          audio_start_awaited = true;
        }
      } else {
        for (int i = 0; i < btif_max_av_clients; i++) {
          if (i != index && (btif_av_cb[i].dual_handoff == true))
            btif_av_cb[i].dual_handoff = false;
        }
      }
      if (btif_av_cb[index].remote_started) {
        if (btif_a2dp_source_is_remote_start()) {
          BTIF_TRACE_DEBUG("%s:cancel remote start timer",__func__);
          btif_a2dp_source_cancel_remote_start();
          /*
           * Remote sent avdtp start followed by avdtp suspend, setting
           * the flag not to update the play state to app
           */
          remote_start_cancelled = true;
        }
        btif_av_cb[index].remote_started = false;
      }
      else if (p_av->suspend.initiator != true) {
        /* remote suspend, notify HAL and await audioflinger to
         * suspend/stop stream
         * set remote suspend flag to block media task from restarting
         * stream only if we did not already initiate a local suspend
         * set remote suspend flag before suspending stream as in race conditions
         * when stream is suspended, but flag is things ge tossed up
         */
        BTIF_TRACE_EVENT("Clear before suspending");
        if ((btif_av_cb[index].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING) == 0)
          btif_av_cb[index].flags |= BTIF_AV_FLAG_REMOTE_SUSPEND;
        for (int i = 0; i < btif_max_av_clients; i++)
          if ((i != index) && btif_av_get_ongoing_multicast()) {
            multicast_disabled = true;
            btif_av_update_multicast_state(index);
            BTIF_TRACE_EVENT("Initiate suspend for other HS also");
            btif_sm_dispatch(btif_av_cb[i].sm_handle,
                    BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL);
          }
      }

      /* a2dp suspended, stop media task until resumed
       * Multicast: If streaming on other device, don't call onsuspended
       * as it unblocks the audio process and audio process may send
       * subsequent commands and create problem during the time where we
       * still did not receive response for SUSPEND sent to other device.
       * Keep the suspend failure handling untouched and handle
       * only success case to check and avoid calling onsuspended.
       */
      if (pending_cmd == A2DP_CTRL_CMD_SUSPEND ||
          pending_cmd == A2DP_CTRL_CMD_STOP) {
        BTIF_TRACE_DEBUG("HAL suspend/stop pending ack the suspend");
        hal_suspend_pending = true;
      }
      if ((p_av->suspend.status != BTA_AV_SUCCESS) ||
          hal_suspend_pending ||
          (!btif_av_is_playing_on_other_idx(index)) ||
          (btif_av_is_playing_on_other_idx(index) &&
          (btif_get_is_remote_started_idx() ==
          (btif_max_av_clients - (index +1))))) {
          BTIF_TRACE_DEBUG("Other device suspended/Remote started, ack the suspend");
          btif_a2dp_on_suspended(&p_av->suspend);
      } else if(btif_av_is_playing_on_other_idx(index)) {
        BTIF_TRACE_DEBUG("Other device not suspended, don't ack the suspend");
      }

      BTIF_TRACE_DEBUG("%s: local suspend flag: %d", __func__,
              btif_av_cb[index].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING);

      if ((!enable_multicast)&& btif_av_cb[index].is_suspend_for_remote_start
            && (btif_av_is_playing_on_other_idx(index)))
      {
        BTIF_TRACE_IMP("%s Don't update audio state change to app for idx =%d", __func__, index);
        btif_av_cb[index].is_device_playing = false;
        btif_av_cb[index].current_playing = false;
        btif_av_update_current_playing_device(index);
      } else if (!enable_multicast && remote_start_cancelled) {
          BTIF_TRACE_IMP("%s Don't update audio state as remote started and suspended", __func__);
          if (btif_av_cb[index].flags & BTIF_AV_FLAG_REMOTE_SUSPEND)
            btif_av_cb[index].flags &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
      }
      else
      {
        if (!((btif_av_cb[index].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING)
                                          || (p_av->suspend.initiator == true)))
        {
          btif_report_audio_state(BTAV_AUDIO_STATE_REMOTE_SUSPEND, &(btif_av_cb[index].peer_bda));
        }
        else
        {
          btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb[index].peer_bda));
        }
      }

      remote_start_cancelled = false;
      // if not successful, remain in current state
      if (p_av->suspend.status != BTA_AV_SUCCESS) {
        if (btif_av_cb[index].is_suspend_for_remote_start) {
          BTIF_TRACE_DEBUG("Suspend sent for remote start failed");
          btif_av_cb[index].is_suspend_for_remote_start = false;
          if (!btif_av_is_playing_on_other_idx(index) &&
                  (index == btif_av_get_latest_device_idx_to_start())) {
            BTIF_TRACE_DEBUG("other index not playing, setup codec");
            btif_dispatch_sm_event(BTIF_AV_SETUP_CODEC_REQ_EVT, NULL, 0);
          }
        }

        btif_av_cb[index].flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;

        if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
          // suspend failed, reset back tx flush state
          btif_a2dp_source_set_tx_flush(false);
        return false;
      }
      btif_av_cb[index].is_suspend_for_remote_start = false;
      btif_av_cb[index].is_device_playing = false;
      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_OPENED);

      // suspend completed and state changed, clear pending status
      btif_av_cb[index].flags &= ~BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING;
      break;

    case BTA_AV_STOP_EVT:
      btif_av_cb[index].flags |= BTIF_AV_FLAG_PENDING_STOP;
      if (!btif_av_cb[index].reconfig_pending)
        btif_av_cb[index].current_playing = false;
      BTIF_TRACE_DEBUG("Stop the AV Data channel");
      btif_a2dp_on_stopped(&p_av->suspend);
      btif_av_cb[index].is_device_playing = false;

      if ((!enable_multicast)&& btif_av_cb[index].is_suspend_for_remote_start
          && (btif_av_is_playing_on_other_idx(index))) {
        BTIF_TRACE_IMP("%s Do not update audio state change to app for index =%d", __FUNCTION__, index);
        btif_av_cb[index].is_device_playing = FALSE;
        btif_av_cb[index].current_playing = FALSE;
        btif_av_update_current_playing_device(index);
      } else {
        btif_report_audio_state(BTAV_AUDIO_STATE_STOPPED, &(btif_av_cb[index].peer_bda));
      }
      btif_av_cb[index].is_suspend_for_remote_start = FALSE;
      // if stop was successful, change state to open
      if (p_av->suspend.status == BTA_AV_SUCCESS)
        btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_OPENED);

      if (btif_av_is_split_a2dp_enabled() &&
        btif_av_is_connected_on_other_idx(index) && !reconfig_a2dp) {
        /*Fake handoff state to switch streaming to other coddeced
          device */
        btif_av_cb[index].dual_handoff = true;
        BTIF_TRACE_DEBUG("%s: Notify framework to reconfig",__func__);
        int idx = btif_av_get_other_connected_idx(index);
        /* Fix for below Klockwork Issue
         * Array 'btif_av_cb' of size 2 may use index value(s) -1 */
        if (idx != INVALID_INDEX) {
          reconfig_a2dp = true;
          HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                                          &(btif_av_cb[idx].peer_bda), 0, 0);
        }
      }
      break;

    case BTA_AV_CLOSE_EVT:
      btif_av_cb[index].flags |= BTIF_AV_FLAG_PENDING_STOP;

/* SPLITA2DP */
      if (!btif_av_is_connected_on_other_idx(index)) {
        if (btif_av_is_split_a2dp_enabled()) {
          btif_a2dp_audio_if_init = false;
          btif_a2dp_audio_interface_deinit();
        }
      }
/* SPLITA2DP */
      /* avdtp link is closed */
      btif_a2dp_on_stopped(NULL);

      /* inform the application that we are disconnected */
      btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
                                   &(btif_av_cb[index].peer_bda));

      btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
      break;

    case BTA_AV_RC_OPEN_EVT:
      btif_av_check_rc_connection_priority(p_data);
      break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      if (btif_av_cb[index].flags != BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING &&
          btif_av_cb[index].flags != BTIF_AV_FLAG_REMOTE_SUSPEND &&
          btif_av_cb[index].remote_started == false)
      {
          bool is_scrambling_enabled = btif_av_is_scrambling_enabled();

          BTIF_TRACE_WARNING("%s:is_scrambling_enabled %d",__func__,
                                    is_scrambling_enabled);

          BTA_AvOffloadStart(btif_av_cb[index].bta_handle, is_scrambling_enabled);
      }
      else if (btif_av_cb[index].remote_started)
      {
          BTIF_TRACE_ERROR("%s:remote started for this index, ignore req",__func__);
      }
      else
      {
          BTIF_TRACE_ERROR("%s:Suspend pending, cannot start offload",__func__);
          btif_a2dp_on_offload_started(BTA_AV_FAIL);
      }
      break;

    case BTA_AV_OFFLOAD_START_RSP_EVT:
      btif_a2dp_on_offload_started(p_av->status);

      for(i = 0; i < btif_max_av_clients; i++) {
        btif_av_cb[i].dual_handoff = false;
        // Other device is not current playing
        if (i != index)
          btif_av_cb[i].current_playing = false;
      }
      btif_av_cb[index].current_playing = true;
      break;

    case BTIF_AV_SETUP_CODEC_REQ_EVT: {
      uint8_t hdl = btif_av_get_av_hdl_from_idx(index);
      APPL_TRACE_DEBUG("%s: hdl = %d",__func__, hdl);
      if (hdl >= 0)
        btif_a2dp_source_setup_codec(hdl);
      }
      break;

    case BTA_AV_DELAY_REPORT_EVT:
      /* Todo Implement: Delay report during A2dp streaming
       * Update latency to A2dp StateMachine if need support
       * dynamic audio/video sync */
      break;

      CHECK_RC_EVENT(event, (tBTA_AV*)p_data);

    default:
      BTIF_TRACE_WARNING("%s: unhandled event=%s", __func__,
                         dump_av_sm_event_name((btif_av_sm_event_t)event));
      return false;
  }

  return true;
}

/*****************************************************************************
 *  Local event handlers
 *****************************************************************************/

static void btif_av_handle_event(uint16_t event, char* p_param) {
  int index = 0;
  tBTA_AV *p_bta_data = (tBTA_AV*)p_param;
  RawAddress *bt_addr = nullptr;
  uint8_t role;
  int uuid;

  switch (event) {
    case BTIF_AV_INIT_REQ_EVT:
      BTIF_TRACE_DEBUG("%s: BTIF_AV_INIT_REQ_EVT", __func__);
      if (btif_a2dp_source_startup())
        btif_a2dp_sink_on_init();
      break;

    case BTIF_AV_CLEANUP_REQ_EVT: // Clean up to be called on default index
      BTIF_TRACE_DEBUG("%s: BTIF_AV_CLEANUP_REQ_EVT", __func__);
      uuid = (int)*p_param;
      if (btif_a2dp_audio_if_init) {
        btif_a2dp_audio_if_init = false;
        btif_a2dp_audio_interface_deinit();
      }
      if (uuid == BTA_A2DP_SOURCE_SERVICE_ID) {
        if (bt_av_src_callbacks) {
          bt_av_src_callbacks = NULL;
          if (bt_av_sink_callbacks != NULL)
            break;
        }
      } else {
        if (bt_av_sink_callbacks) {
          bt_av_sink_callbacks = NULL;
          if (bt_av_src_callbacks != NULL)
            break;
        }
      }
      btif_a2dp_source_shutdown();
      btif_a2dp_sink_shutdown();
      return;

    case BTIF_AV_CONNECT_REQ_EVT:
      break;

    case BTIF_AV_SOURCE_CONFIG_REQ_EVT:
      //if (btif_av_get_num_connected_devices() > 1) {
        BTIF_TRACE_DEBUG("BTIF_AV_SOURCE_CONFIG_REQ_EVT more than one device connected");
        if (btif_av_stream_started_ready())
          index = btif_av_get_latest_playing_device_idx();
        else
          index = btif_av_get_latest_device_idx_to_start();
      //}
      break;
    case BTIF_AV_SOURCE_CONFIG_UPDATED_EVT:
      if (codec_config_update_enabled == false)
      {
        BTIF_TRACE_IMP("ignore updating BTIF_AV_SOURCE_CONFIG_UPDATED_EVT");
        return;
      }
      if (p_param != NULL) {
        bt_addr = (RawAddress *)p_param;
        index = btif_av_idx_by_bdaddr(bt_addr);
        if (index == btif_max_av_clients) {
            index = 0;
        }
      }
      BTIF_TRACE_IMP("Process BTIF_AV_SOURCE_CONFIG_UPDATED_EVT on idx = %d", index);
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
      // Bd address passed should help us in getting the handle
      bt_addr = (RawAddress *)p_param;
      index = btif_av_idx_by_bdaddr(bt_addr);
      break;

    case BTIF_AV_TRIGGER_HANDOFF_REQ_EVT:
            bt_addr = (RawAddress *)p_param;
            index = btif_av_idx_by_bdaddr(bt_addr);
            BTIF_TRACE_IMP("BTIF_AV_TRIGGER_HANDOFF_REQ_EVT on index %d", index);
            if (index >= 0 && index < btif_max_av_clients)
            {
                int i = 0;
                for(; i< btif_max_av_clients; i++)
                {
                    if (i == index)
                        btif_av_cb[i].current_playing = TRUE;
                    else
                        btif_av_cb[i].current_playing = FALSE;
                }
                for (i = 0; i< btif_max_av_clients; i++)
                {
                    BTIF_TRACE_IMP("current_playing on index %d, %d", i, btif_av_cb[i].current_playing);
                }
                /*RC play state is to be cleared to make sure the same when retained
                does not impact UI initiated play*/
	        btif_rc_clear_playing_state(FALSE);
                btif_av_trigger_dual_handoff(TRUE, btif_av_cb[index].peer_bda.address);
            }
            else
            {
                BTIF_TRACE_WARNING("Device is no longer connectied, device switch fails");
            }
            break;

    case BTIF_AV_START_STREAM_REQ_EVT:
      /* Get the last connected device on which START can be issued
       * Get the Dual A2dp Handoff Device first, if none is present,
       * go for lastest connected.
       * In A2dp Multicast, the index selected can be any of the
       * connected device. Stack will ensure to START the steaming
       * on both the devices. */
      index = btif_av_get_latest_device_idx_to_start();
      break;
    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
      // Should be handled by current STARTED
      index = btif_av_get_latest_playing_device_idx();
      if ((index < btif_max_av_clients) && (index == btif_get_is_remote_started_idx())) {
        if (btif_av_is_playing_on_other_idx(index)) {
            BTIF_TRACE_DEBUG("BTIF_AV_SUSPEND_STREAM_REQ_EVT:revising effective index");
            index = btif_max_av_clients - (index +1);
        } else {
            BTIF_TRACE_ERROR("%s: Postpone handling suspend/stop req @ index = %d",
                            __func__, index);
        }
        return;
      }
      break;
    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      {
        //AV handle passed by a2dp on started
        tBTA_AV_HNDL hdl = (tBTA_AV_HNDL)*p_param;
        index = HANDLE_TO_INDEX(hdl);
      }
      break;
    case BTIF_AV_REINIT_AUDIO_IF:
      btif_av_reinit_audio_interface();
      return;
    case BTIF_AV_REMOTE_SUSPEND_STREAM_REQ_EVT:
      index = btif_get_is_remote_started_idx();
      if (index >= btif_max_av_clients) {
        BTIF_TRACE_ERROR("%s: Invalid index for connection", __func__);
        return;
      }
      BTIF_TRACE_IMP("%s: Remote Started set @ index = %d", __func__, index);
      btif_av_cb[index].remote_started = false;
      btif_av_cb[index].is_suspend_for_remote_start = true;
#ifdef BTA_AV_SPLIT_A2DP_ENABLED
      if ((bt_split_a2dp_enabled) && (!btif_av_is_playing_on_other_idx(index))) {
        BTIF_TRACE_IMP("%s: Other index is not playing", __func__);
        bta_av_vendor_offload_stop();
      }
#endif
      event = BTIF_AV_SUSPEND_STREAM_REQ_EVT;
      break;
    case BTIF_AV_RESET_REMOTE_STARTED_FLAG_UPDATE_AUDIO_STATE_EVT:
      index = btif_get_is_remote_started_idx();
      if (btif_av_cb[index].peer_sep == AVDT_TSEP_SNK)
      {
        BTIF_TRACE_IMP("%s: on remote start clean up update audio started state for index %d", __func__, index);
        btif_report_audio_state(BTAV_AUDIO_STATE_STARTED, &(btif_av_cb[index].peer_bda));
      }
      btif_av_cb[index].is_device_playing = true;
      for (int i = 0; i < btif_max_av_clients; i++)
      {
          //Other device is not current playing
          if (i != index)
            btif_av_cb[i].current_playing = false;
      }
        //This is latest device to play now
      btif_av_cb[index].current_playing = true;
    case BTIF_AV_RESET_REMOTE_STARTED_FLAG_EVT:
      btif_av_reset_remote_started_flag();
      return;
      // Events from the stack, BTA
    case BTA_AV_ENABLE_EVT:
      index = 0;
      break;

    case BTA_AV_REGISTER_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->registr.hndl);
      break;

    case BTA_AV_OPEN_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->open.hndl);
      break;

    case BTA_AV_ROLE_CHANGED_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->role_changed.hndl);
      role = p_bta_data->role_changed.new_role;
      BTIF_TRACE_EVENT("Role change: 0x%x: new role: %s",
      p_bta_data->role_changed.hndl, (role == HOST_ROLE_SLAVE) ? "Slave" : "Master");
      if (index >= 0 && index < btif_max_av_clients) {
        btif_av_cb[index].is_slave = (role == HOST_ROLE_SLAVE) ? true : false;
        btif_av_update_multicast_state(index);
      } else {
        BTIF_TRACE_ERROR("%s: Invalid index for connection", __func__);
      }
      return;

    case BTA_AV_PENDING_EVT:
        /* In race conditions, outgoing and incoming connections
        * at same time check for BD address at index and if it
        * does not match then check for first avialable index.
        */
        index = HANDLE_TO_INDEX(p_bta_data->pend.hndl);
        if (index >= 0 && index < btif_max_av_clients &&
            (p_bta_data->pend.bd_addr == btif_av_cb[index].peer_bda))
       {
            BTIF_TRACE_EVENT("incomming connection at index %d", index);
       }
       else
       {
           RawAddress empty_addr = RawAddress::kEmpty;
           index = btif_av_idx_by_bdaddr(&empty_addr);
           if (index >= btif_max_av_clients)
           {
               BTIF_TRACE_ERROR("No free SCB available");
               BTA_AvDisconnect(p_bta_data->pend.bd_addr);
           }
           else
           {
               BTIF_TRACE_EVENT("updated index for connection %d", index);
           }
       }
       break;
    case BTA_AV_REJECT_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->reject.hndl);
      break;

    case BTA_AV_STOP_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->suspend.hndl);
      break;

    case BTA_AV_CLOSE_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->close.hndl);
      break;

    case BTA_AV_START_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->start.hndl);
      break;

    case BTA_AV_RECONFIG_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->reconfig.hndl);
      break;

    case BTA_AV_SUSPEND_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->suspend.hndl);
      break;

    case BTA_AV_OFFLOAD_START_RSP_EVT:
      index = btif_av_get_latest_playing_device_idx();
      if (index == btif_max_av_clients) {
        for (int i = 0; i < btif_max_av_clients; i++) {
          if (btif_av_check_flag_remote_suspend(i)) {
            index = i;
            break;
          }
        }
      }
      BTIF_TRACE_EVENT("index = %d, max connections = %d", index, btif_max_av_clients);
      break;

    case BTA_AV_OFFLOAD_STOP_RSP_EVT:
      if (p_bta_data->status == BTA_AV_SUCCESS) {
        btif_a2dp_src_vsc.tx_started = FALSE;
        btif_a2dp_src_vsc.tx_stop_initiated = FALSE;
        btif_media_send_reset_vendor_state();
      } else {
        btif_a2dp_src_vsc.tx_stop_initiated = FALSE;
      }
      break;
    case BTA_AV_DELAY_REPORT_EVT:
      index = HANDLE_TO_INDEX(p_bta_data->delay_rpt.hndl);
      break;

      /* Handle all RC events on default index. RC handling should take
       * care of the events. All events come with BD Address
       * Handled well in AV Opening, opened and started state
       * AV Idle handler needs to take care of this event properly.
       */
    case BTA_AV_RC_OPEN_EVT:
      index = btif_av_get_valid_idx_for_rc_events(p_bta_data->rc_open.peer_addr,
                p_bta_data->rc_open.rc_handle);
      break;

    case BTA_AV_RC_CLOSE_EVT:
      /* If there is no entry in the connection table
       * RC handler has to be called for cleanup.
       * Directly call the RC handler as we cannot
       * associate any AV handle to it.
       */
      index = btif_av_idx_by_bdaddr(&p_bta_data->rc_open.peer_addr);
      if (index == btif_max_av_clients)
        btif_rc_handler(event, (tBTA_AV*)p_bta_data);
      break;

    case BTA_AV_RC_COLL_DETECTED_EVT:
      index = btif_av_idx_by_bdaddr(&p_bta_data->rc_col_detected.peer_addr);
      break;

      /* Let the RC handler decide on these passthrough cmds
       * Use rc_handle to get the active AV device and use that mapping.
       */
    case BTA_AV_REMOTE_CMD_EVT:
    case BTA_AV_VENDOR_CMD_EVT:
    case BTA_AV_META_MSG_EVT:
    case BTA_AV_RC_FEAT_EVT:
      index = 0;
      BTIF_TRACE_EVENT("RC events: on index = %d", index);
      break;
    case BTIF_AV_SETUP_CODEC_REQ_EVT:
      index = btif_av_get_latest_device_idx_to_start();
      break;
  /* FALLTHROUGH */
  default:
    BTIF_TRACE_ERROR("Unhandled event = %d", event);
    break;
  }
  BTIF_TRACE_DEBUG("Handle the AV event = %x on index = %d", event, index);
  if (index >= 0 && index < btif_max_av_clients)
      btif_sm_dispatch(btif_av_cb[index].sm_handle, event, (void*)p_param);
  else
      BTIF_TRACE_ERROR("Unhandled Index = %d", index);
  btif_av_event_free_data(event, p_param);

}

void btif_av_reset_remote_started_flag()
{
    int i;
    BTIF_TRACE_DEBUG("btif_av_reset_remote_started_flag");
    for (i = 0; i < btif_max_av_clients; i++)
    {
        if (btif_av_cb[i].remote_started)
            btif_av_cb[i].remote_started = false;
    }
}

/*******************************************************************************
 *
 * Function         btif_av_get_valid_idx
 *
 * Description      Check the validity of the current index for the connection
 *
 * Returns          Boolean
 *
 ******************************************************************************/
static bool btif_av_get_valid_idx(int idx) {
  btif_sm_state_t state = btif_sm_get_state(btif_av_cb[idx].sm_handle);
  return ((state == BTIF_AV_STATE_OPENED) ||
          (state ==  BTIF_AV_STATE_STARTED) ||
          (state == BTIF_AV_STATE_OPENING));
}

/*******************************************************************************
 *
 * Function         btif_av_idx_by_bdaddr
 *
 * Description      Get the index corresponding to BD addr
 *
 * Returns          int
 *
 ******************************************************************************/
int btif_av_idx_by_bdaddr(RawAddress *bd_addr) {
  int i;
  for (i = 0; i < btif_max_av_clients; i++)
    if (*bd_addr == btif_av_cb[i].peer_bda)
      return i;
  return i;
}

bool btif_av_is_current_device(RawAddress address) {
  int index;

  index = btif_av_idx_by_bdaddr(&address);
  if ((index < btif_max_av_clients) && (btif_av_cb[index].current_playing)
      && (!btif_av_cb[index].remote_started))
    return true;
  return false;
}

/*******************************************************************************
 *
 * Function         btif_av_get_latest_device_idx_to_start
 *
 * Description      Get the index of the AV where streaming is to be started
 *
 * Returns          int
 *
 ******************************************************************************/
int btif_av_get_latest_device_idx_to_start() {
  int i, j;
  RawAddress playing_address = RawAddress::kEmpty;

  /* Get the device which sent PLAY command
   * If found, START on that index.
   */
  btif_rc_get_playing_device(&playing_address);
  BTIF_TRACE_DEBUG("%s:playing device address: %s", __func__,
                                 playing_address.ToString().c_str());
  if (!(playing_address.IsEmpty())) {
    /* Got some valid Playing device.
     * Get the AV index for this device.
     */
    i = btif_av_idx_by_bdaddr(&playing_address);
    BTIF_TRACE_DEBUG("%s: index i = %d", __func__, i);
    if (i == btif_max_av_clients)
      return btif_max_av_clients;
    BTIF_TRACE_EVENT("Got some valid Playing device; %d", i);
    btif_av_cb[i].current_playing = true;
    // Clear the Current playing device
    for (j = 0; j < btif_max_av_clients; j++) {
      if (j != i)
        btif_av_cb[j].current_playing = false;
    }
    // Clear the Play command in RC
    btif_rc_clear_playing_state(false);
    return i;
  }

  // No playing device, get the latest
  for (i = 0; i < btif_max_av_clients; i++)
    if (btif_av_cb[i].current_playing)
      break;
  if (i == btif_max_av_clients) {
    BTIF_TRACE_ERROR("Play on default opened device");
    for (i = 0; i < btif_max_av_clients; i++) {
      btif_sm_state_t state = BTIF_AV_STATE_IDLE;
      //i = 0; // play on default
      state = btif_sm_get_state(btif_av_cb[i].sm_handle);
      if (state == BTIF_AV_STATE_OPENED)
        break;
    }
  }
  if (i == btif_max_av_clients) {
    BTIF_TRACE_ERROR("No valid AV device found, play on default");
    i = 0;
  }
  return i;
}

/*******************************************************************************
 *
 * Function         btif_av_get_latest_playing_device_idx
 *
 * Description      Get the index of AV where streaming is happening
 *
 * Returns          int
 *
 ******************************************************************************/
int btif_av_get_latest_playing_device_idx() {
  int i;
  btif_sm_state_t state;
  for (i = 0; i < btif_max_av_clients; i++) {
    state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if (state == BTIF_AV_STATE_STARTED) {
      BTIF_TRACE_IMP("Latest playing device index %d", i);
      break;
    }
  }
  return i;
}

/*******************************************************************************
 *
 * Function         btif_get_is_remote_started_idx
 *
 * Description      Get the index of AV where remote Start is received
 *
 * Returns          int
 *
 ******************************************************************************/
int btif_get_is_remote_started_idx() {
  int i;
  for (i = 0; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].remote_started == true) {
      BTIF_TRACE_IMP("remote started set for device index %d", i);
      break;
    }
  }
  return i;
}

/*******************************************************************************
 *
 * Function         btif_av_is_playing
 *
 * Description      Is AV in streaming state
 *
 * Returns          bool
 *
 ******************************************************************************/
bool btif_av_is_playing() {
  int i;
  for (i = 0; i < btif_max_av_clients; i++) {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if (btif_av_cb[i].state == BTIF_AV_STATE_STARTED) {
      BTIF_TRACE_EVENT("btif_av_is_playing on index= %d", i);
      return true;
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         btif_get_conn_state_of_device
 *
 * Description      Returns the state of AV scb
 *
 * Returns          int
 *
 ******************************************************************************/
static int btif_get_conn_state_of_device(RawAddress address) {
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  int i;
  for (i = 0; i < btif_max_av_clients; i++) {
    if ( address == btif_av_cb[i].peer_bda ) {
      state = btif_sm_get_state(btif_av_cb[i].sm_handle);
      //BTIF_TRACE_EVENT("BD Found: %02X %02X %02X %02X %02X %02X :state: %s",
         // address[5], address[4], address[3],
         // address[2], address[1], address[0],
         // dump_av_sm_state_name((btif_av_state_t)state));
    }
  }
  return state;
}

/*******************************************************************************
 *
 * Function         btif_av_get_valid_idx_for_rc_events
 *
 * Description      gets th valid index for the RC event address
 *
 * Returns          int
 *
 ******************************************************************************/
static int btif_av_get_valid_idx_for_rc_events(RawAddress bd_addr, int rc_handle) {
  int index = 0;
  /* First try to find if it is first event in AV IF
   * both the handles would be in IDLE state, pick the first
   * If we get second RC event while processing the priority
   * for the first, reject the second connection.
   */

  /* Get the index from connected SCBs */
  index = btif_av_idx_by_bdaddr(&bd_addr);
  if (index == btif_max_av_clients) {
    /* None of the SCBS matched * Allocate free SCB, null address SCB*/
    RawAddress empty_addr = RawAddress::kEmpty;
    index = btif_av_idx_by_bdaddr(&empty_addr);
    BTIF_TRACE_EVENT("btif_av_get_valid_idx_for_rc_events is %d", index);
    if (index >= btif_max_av_clients) {
      BTIF_TRACE_EVENT("disconnect only AVRCP device rc_handle %d", rc_handle);
      BTA_AvCloseRc(rc_handle);
    }
  }
  return index;
}

/*******************************************************************************
 *
 * Function         btif_av_check_rc_connection_priority
 *
 * Description      Handles Priority callback for RC connections
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_av_check_rc_connection_priority(void *p_data) {
  RawAddress peer_bda;

  /*Check if it is for same AV device*/
  if (btif_av_is_device_connected(((tBTA_AV*)p_data)->rc_open.peer_addr)) {
    /*AV is connected */
    BTIF_TRACE_DEBUG("AV is connected, process RC connect event");
    btif_rc_handler(BTA_AV_RC_OPEN_EVT, (tBTA_AV*)p_data);
    return;
  }
  BTIF_TRACE_DEBUG("btif_av_check_rc_connection_priority");
  peer_bda = ((tBTA_AV*)p_data)->rc_open.peer_addr;

  if (idle_rc_event != 0) {
    BTIF_TRACE_DEBUG("Processing another RC Event ");
    return;
  }
  idle_rc_event = BTA_AV_RC_OPEN_EVT;
  memcpy(&idle_rc_data, ((tBTA_AV*)p_data), sizeof(tBTA_AV));
  if (((tBTA_AV*)p_data)->rc_open.status == BTA_AV_SUCCESS) {
        BTIF_TRACE_DEBUG("RC conn is success ");
    if (bt_av_src_callbacks != NULL) {
      BTIF_TRACE_DEBUG(" Check Device priority");
      HAL_CBACK(bt_av_src_callbacks, connection_priority_cb, &peer_bda);
    }
  } else {
    idle_rc_event = 0;
    memset(&idle_rc_data, 0, sizeof(tBTA_AV));
  }
  return;
}



void btif_av_event_deep_copy(uint16_t event, char* p_dest, char* p_src) {
  BTIF_TRACE_DEBUG("%s", __func__);
  tBTA_AV* av_src = (tBTA_AV*)p_src;
  tBTA_AV* av_dest = (tBTA_AV*)p_dest;

  // First copy the structure
  maybe_non_aligned_memcpy(av_dest, av_src, sizeof(*av_src));
  switch (event) {
    case BTA_AV_META_MSG_EVT:
      if (av_src->meta_msg.p_data && av_src->meta_msg.len) {
        av_dest->meta_msg.p_data = (uint8_t*)osi_calloc(av_src->meta_msg.len);
        memcpy(av_dest->meta_msg.p_data, av_src->meta_msg.p_data,
               av_src->meta_msg.len);
      }

      if (av_src->meta_msg.p_msg) {
        av_dest->meta_msg.p_msg = (tAVRC_MSG*)osi_calloc(sizeof(tAVRC_MSG));
        memcpy(av_dest->meta_msg.p_msg, av_src->meta_msg.p_msg,
               sizeof(tAVRC_MSG));

        tAVRC_MSG* p_msg_src = av_src->meta_msg.p_msg;
        tAVRC_MSG* p_msg_dest = av_dest->meta_msg.p_msg;

        if ((p_msg_src->hdr.opcode == AVRC_OP_VENDOR) &&
            (p_msg_src->vendor.p_vendor_data && p_msg_src->vendor.vendor_len)) {
          p_msg_dest->vendor.p_vendor_data =
              (uint8_t*)osi_calloc(p_msg_src->vendor.vendor_len);
          memcpy(p_msg_dest->vendor.p_vendor_data,
                 p_msg_src->vendor.p_vendor_data, p_msg_src->vendor.vendor_len);
        } else if (p_msg_src->hdr.opcode == AVRC_OP_BROWSE) {
          p_msg_dest->browse.p_browse_data = av_dest->meta_msg.p_data;
        }
      }
      break;

    default:
      break;
  }
}

static void btif_av_event_free_data(btif_sm_event_t event, void* p_data) {
  switch (event) {
    case BTA_AV_META_MSG_EVT: {
      tBTA_AV* av = (tBTA_AV*)p_data;
      if (av->meta_msg.p_data && av->meta_msg.len) {
        osi_free_and_reset((void**)&av->meta_msg.p_data);
      }

      if (av->meta_msg.p_msg) {
        if (av->meta_msg.p_msg->hdr.opcode == AVRC_OP_VENDOR) {
          osi_free(av->meta_msg.p_msg->vendor.p_vendor_data);
        }
        osi_free_and_reset((void**)&av->meta_msg.p_msg);
      }
    } break;

    default:
      break;
  }
}

static void bte_av_callback(tBTA_AV_EVT event, tBTA_AV* p_data) {
  btif_transfer_context(btif_av_handle_event, event, (char*)p_data,
                        sizeof(tBTA_AV), btif_av_event_deep_copy);
}

static void bte_av_sink_media_callback(tBTA_AV_EVT event,
                                       tBTA_AV_MEDIA* p_data) {
  int index = 0;

  switch (event) {
    case BTA_AV_SINK_MEDIA_DATA_EVT: {
      btif_sm_state_t state = btif_sm_get_state(btif_av_cb[index].sm_handle);
      if ((state == BTIF_AV_STATE_STARTED) || (state == BTIF_AV_STATE_OPENED)) {
        uint8_t queue_len = btif_a2dp_sink_enqueue_buf((BT_HDR*)p_data);
        BTIF_TRACE_DEBUG("%s: packets in sink queue %d", __func__, queue_len);
      }
      break;
    }
    case BTA_AV_SINK_MEDIA_CFG_EVT: {
      btif_av_sink_config_req_t config_req;

      /* send a command to BT Media Task */
      btif_a2dp_sink_update_decoder((uint8_t*)(p_data->avk_config.codec_info));
      /* Switch to BTIF context */
      config_req.sample_rate =
          A2DP_GetTrackSampleRate(p_data->avk_config.codec_info);
      if (config_req.sample_rate == -1) {
        APPL_TRACE_ERROR("%s: cannot get the track frequency", __func__);
        break;
      }
      config_req.channel_count =
          A2DP_GetTrackChannelCount(p_data->avk_config.codec_info);
      if (config_req.channel_count == -1) {
        APPL_TRACE_ERROR("%s: cannot get the channel count", __func__);
        break;
      }

      config_req.peer_bd = p_data->avk_config.bd_addr;
      btif_transfer_context(btif_av_handle_event, BTIF_AV_SINK_CONFIG_REQ_EVT,
                            (char*)&config_req, sizeof(config_req), NULL);
      break;
    }
    default:
      break;
  }
}

/*******************************************************************************
 *
 * Function         btif_av_init
 *
 * Description      Initializes btif AV if not already done
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/

bt_status_t btif_av_init(int service_id) {
  if (btif_av_cb[0].sm_handle == NULL) {
    alarm_free(av_open_on_rc_timer);
    av_open_on_rc_timer = alarm_new("btif_av.av_open_on_rc_timer");
    alarm_free(av_coll_detected_timer);
    av_coll_detected_timer = alarm_new("btif_av.av_coll_detected_timer");

    BTIF_TRACE_DEBUG("%s; service Id: %d", __func__, service_id);

    btif_av_cb[0].service = service_id;
    switch (service_id) {
      case BTA_A2DP_SOURCE_SERVICE_ID:
        if (btif_a2dp_source_media_task_is_running())
          return BT_STATUS_FAIL;  // Already running
        if (!btif_a2dp_source_startup())
          return BT_STATUS_FAIL;  // Already running
        break;
      case BTA_A2DP_SINK_SERVICE_ID:
        if (!btif_a2dp_sink_startup())
          return BT_STATUS_FAIL;  // Already running
        break;
      default:
        break;
    }

    /* Also initialize the AV state machine */
    for (int i = 0; i < btif_max_av_clients; i++)
      btif_av_cb[i].sm_handle = btif_sm_init(
        (const btif_sm_handler_t*)btif_av_state_handlers, BTIF_AV_STATE_IDLE,
        i);

    btif_transfer_context(btif_av_handle_event, BTIF_AV_INIT_REQ_EVT,
        (char*)&service_id, sizeof(int), NULL);

    btif_enable_service(service_id);
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         init_src
 *
 * Description      Initializes the AV interface for source mode
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t init_src(
    btav_source_callbacks_t* callbacks,
    std::vector<btav_a2dp_codec_config_t> codec_priorities,
    int max_a2dp_connections, int a2dp_multicast_state) {
  bt_status_t status = BT_STATUS_FAIL;
  BTIF_TRACE_EVENT("%s() with max conn = %d", __func__, max_a2dp_connections);

#ifdef ENABLE_SPLIT_A2DP
  char value[PROPERTY_VALUE_MAX] = {'\0};
  osi_property_get("persist.vendor.btstack.enable.splita2dp", value, "true");
  BTIF_TRACE_ERROR("split_a2dp_status = %s",value);
  bt_split_a2dp_enabled = (strcmp(value, "true") == 0);
  BTIF_TRACE_DEBUG("split_a2dp_status = %d",bt_split_a2dp_enabled);
  osi_property_get("persist.vendor.btstack.twsplus.defaultchannelmode", value, "mono");
  BTIF_TRACE_DEBUG("tws default channel mode = %s",value);
  tws_defaultmono_supported = (strcmp(value, "mono") == 0);
  BTIF_TRACE_DEBUG("default mono channel mode = %d",tws_defaultmono_supported);
#endif

  if (bt_av_sink_callbacks != NULL)
        // already did btif_av_init()
        status = BT_STATUS_SUCCESS;
  else {
    if (a2dp_multicast_state && !bt_split_a2dp_enabled)
      is_multicast_supported = true;
    btif_max_av_clients = max_a2dp_connections;
    for (int i = 0; i < btif_max_av_clients; i++)
      btif_av_cb[i].codec_priorities = codec_priorities;
    if (codec_config_update_enabled != false) {
        BTIF_TRACE_IMP("%s: Codec cfg update enabled changed to false", __func__);
        codec_config_update_enabled = false;
    }
    status = btif_av_init(BTA_A2DP_SOURCE_SERVICE_ID);
    if (status == BT_STATUS_SUCCESS) bt_av_src_callbacks = callbacks;
  }
  return status;
}

/*******************************************************************************
 *
 * Function         init_sink
 *
 * Description      Initializes the AV interface for sink mode
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/

static bt_status_t init_sink(btav_sink_callbacks_t* callbacks) {
  BTIF_TRACE_EVENT("%s", __func__);

  bt_status_t status = btif_av_init(BTA_A2DP_SINK_SERVICE_ID);
  if (status == BT_STATUS_SUCCESS) bt_av_sink_callbacks = callbacks;

  return status;
}

/*******************************************************************************
 *
 * Function         update_audio_focus_state
 *
 * Description      Updates the final focus state reported by components calling
 *                  this module.
 *
 * Returns          None
 *
 ******************************************************************************/
static void update_audio_focus_state(int state) {
  BTIF_TRACE_DEBUG("%s: state=%d", __func__, state);
  btif_a2dp_sink_set_focus_state_req((btif_a2dp_sink_focus_state_t)state);
}

/*******************************************************************************
 *
 * Function         update_audio_track_gain
 *
 * Description      Updates the track gain (used for ducking).
 *
 * Returns          None
 *
 ******************************************************************************/
static void update_audio_track_gain(float gain) {
  BTIF_TRACE_DEBUG("%s: gain=%f", __func__, gain);
  btif_a2dp_sink_set_audio_track_gain(gain);
}

/*******************************************************************************
 *
 * Function         btif_get_latest_playing_device
 *
 * Description      Get the index for the most recent source
 *
 * Returns          None
 *
 ******************************************************************************/
void btif_get_latest_playing_device(RawAddress *address) {
  int index;
  index = btif_av_get_latest_playing_device_idx();
  if (index < btif_max_av_clients)
        //copy bdaddrsss
    *address = btif_av_cb[index].peer_bda;
  else
    address = nullptr;
}

/*******************************************************************************
 *
 * Function         btif_av_is_device_connected
 *
 * Description      Check if the peer is connected(A2DP)
 *
 * Returns          true/false
 *
 ******************************************************************************/
bool btif_av_is_device_connected(RawAddress address) {
  btif_sm_state_t state = btif_get_conn_state_of_device(address);

  if ((state == BTIF_AV_STATE_OPENED) ||
      (state == BTIF_AV_STATE_STARTED))
    return true;
  else
    return false;
}

/*******************************************************************************
 *
 * Function         btif_av_trigger_dual_handoff
 *
 * Description      Trigger the DUAL HANDOFF. This function will trigger remote
 *                  suspend for currently playing device and then initiate START
 *                  on Handoff device whose address is passed as an argument.
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_av_trigger_dual_handoff(bool handoff, RawAddress address) {
  int index, next_idx, other_idx;
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  BTIF_TRACE_DEBUG("%s()", __func__);

  /* Get the current playing device */
  index = btif_av_get_latest_playing_device_idx();
  other_idx = btif_av_get_other_connected_idx(index);
  if (other_idx != INVALID_INDEX && other_idx != btif_max_av_clients) {
    state = btif_sm_get_state(btif_av_cb[other_idx].sm_handle);
  }
  if (index != btif_max_av_clients) {
    if ((btif_av_cb[index].remote_started) && (state == BTIF_AV_STATE_STARTED)
        && (index == btif_av_idx_by_bdaddr(&address))) {
      BTIF_TRACE_IMP("%s: SHO requested for remote STARTED index %d", __func__, index);
      BTIF_TRACE_IMP("other index %d in AV STARTED, suspend it", other_idx);
      index = other_idx;
    }

    btif_av_cb[index].dual_handoff = handoff; /*Initiate Handoff*/
    /*Initiate SUSPEND for this device*/
    next_idx = btif_av_get_other_connected_idx(index);
    if (next_idx != INVALID_INDEX && next_idx != btif_max_av_clients) {
      if (btif_av_cb[next_idx].remote_started) {
        btif_a2dp_source_cancel_remote_start();
        BTIF_TRACE_DEBUG("Reset remote start alarm on index = %d", next_idx);
        btif_av_cb[next_idx].remote_started = false;
      }
    }
    BTIF_TRACE_DEBUG("Initiate SUSPEND for this device on index = %d", index);
    btif_sm_dispatch(btif_av_cb[index].sm_handle, BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL);
  } else
    BTIF_TRACE_ERROR("Handoff on invalid index");

  if (btif_av_is_split_a2dp_enabled()) {
    next_idx = btif_av_get_other_connected_idx(index);
    /* Fix for below Klockwork Issue
    Array 'btif_av_cb' of size 2 may use index value(s) -1 */
    if (next_idx != INVALID_INDEX && next_idx != btif_max_av_clients) {
      reconfig_a2dp = true;
      HAL_CBACK(bt_av_src_callbacks, reconfig_a2dp_trigger_cb, SOFT_HANDOFF,
                              &(btif_av_cb[next_idx].peer_bda), 0, 0);
    }
  }
}

/*******************************************************************************
 *
 * Function         btif_av_trigger_suspend
 *
 * Description      Trigger suspend when multicast is ongoing for tuch tones
 *                  and new ACL is created.
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_av_trigger_suspend() {
  int index;
  /*Get the current playing device*/
  BTIF_TRACE_DEBUG("%s()", __func__);
  index = btif_av_get_latest_playing_device_idx();
  if (index <= btif_max_av_clients) {
    /* Initiate SUSPEND for this device */
    BTIF_TRACE_DEBUG("Initiate SUSPEND for this device on index = %d", index);
    btif_sm_dispatch(btif_av_cb[index].sm_handle, BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL);
  } else
    BTIF_TRACE_ERROR("suspend on invalid index");
}

/*******************************************************************************
 *
 * Function         connect_int
 *
 * Description      Establishes the AV signalling channel with the remote
 *                  headset
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t connect_int(RawAddress* bd_addr, uint16_t uuid) {
  btif_av_connect_req_t connect_req;
  int i;
  connect_req.target_bda = bd_addr;
  connect_req.uuid = uuid;
  BTIF_TRACE_EVENT("%s", __func__);

  if (!btif_storage_is_device_bonded(bd_addr))
  {
    BTIF_TRACE_WARNING("%s()## connect_int ## Device Not Bonded %s \n", __func__,
                      bd_addr->ToString().c_str());
    /* inform the application of the disconnection as the connection is not processed */
    btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, bd_addr);
    btif_queue_advance();
    return BT_STATUS_SUCCESS;
  }
  for (i = 0; i < btif_max_av_clients;) {
    if (btif_av_get_valid_idx(i)) {
      if (*bd_addr == btif_av_cb[i].peer_bda) {
        BTIF_TRACE_ERROR("Attempting connection for non idle device.. back off ");
        btif_queue_advance();
        return BT_STATUS_SUCCESS;
      }
      i++;
    } else
      break;
  }
  if (i == btif_max_av_clients) {
    uint8_t rc_handle;

    BTIF_TRACE_ERROR("%s: All indexes are full", __func__);

    btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, bd_addr);

    /* Multicast: Check if AV slot is available for connection
     * If not available, AV got connected to different devices.
     * Disconnect this RC connection without AV connection.
     */
    rc_handle = btif_rc_get_connected_peer_handle(bd_addr->address);
    if (rc_handle != BTRC_HANDLE_NONE)
      BTA_AvCloseRc(rc_handle);
    btif_queue_advance();
    return BT_STATUS_FAIL;
  }

  btif_sm_dispatch(btif_av_cb[i].sm_handle, BTIF_AV_CONNECT_REQ_EVT, (char*)&connect_req);

  return BT_STATUS_SUCCESS;
}

static bt_status_t src_connect_sink(RawAddress* bd_addr) {
  BTIF_TRACE_EVENT("%s", __func__);
  CHECK_BTAV_INIT();

  return btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, bd_addr, connect_int);
}

static bt_status_t sink_connect_src(RawAddress* bd_addr) {
  BTIF_TRACE_EVENT("%s", __func__);
  CHECK_BTAV_INIT();

  return btif_queue_connect(UUID_SERVCLASS_AUDIO_SINK, bd_addr, connect_int);
}

/*******************************************************************************
 *
 * Function         disconnect
 *
 * Description      Tears down the AV signalling channel with the remote headset
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t disconnect(RawAddress* bd_addr) {
  BTIF_TRACE_EVENT("%s", __func__);
  CHECK_BTAV_INIT();

  /* Switch to BTIF context */
  return btif_transfer_context(btif_av_handle_event, BTIF_AV_DISCONNECT_REQ_EVT,
                               (char*)bd_addr, sizeof(RawAddress), NULL);
}

static bt_status_t codec_config_src(
    std::vector<btav_a2dp_codec_config_t> codec_preferences) {
  BTIF_TRACE_EVENT("%s", __func__);
  CHECK_BTAV_INIT();

  isDevUiReq = false;
  for (auto cp : codec_preferences) {
    BTIF_TRACE_DEBUG(
        "%s: codec_type=%d codec_priority=%d "
        "sample_rate=0x%x bits_per_sample=0x%x "
        "channel_mode=0x%x codec_specific_1=%d "
        "codec_specific_2=%d codec_specific_3=%d "
        "codec_specific_4=%d",
        __func__, cp.codec_type, cp.codec_priority, cp.sample_rate,
        cp.bits_per_sample, cp.channel_mode, cp.codec_specific_1,
        cp.codec_specific_2, cp.codec_specific_3, cp.codec_specific_4);

          A2dpCodecConfig* current_codec = bta_av_get_a2dp_current_codec();
          if (current_codec != nullptr) {
            btav_a2dp_codec_config_t codec_config;
            codec_config = current_codec->getCodecConfig();
            isBitRateChange = false;
            isBitsPerSampleChange = false;
            if ((codec_config.codec_specific_1 != cp.codec_specific_1) &&
              (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC)) {
              switch (cp.codec_specific_1)
              {
              case 1000:
                if ((codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_44100) ||
                  (codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_88200))
                  reconfig_a2dp_param_val = 909000;
                else
                  reconfig_a2dp_param_val = 990000;
                break;
              case 1001:
                if ((codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_44100) ||
                  (codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_88200))
                  reconfig_a2dp_param_val = 606000;
                else
                  reconfig_a2dp_param_val = 660000;
                break;
              case 1002:
                if ((codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_44100) ||
                  (codec_config.sample_rate == BTAV_A2DP_CODEC_SAMPLE_RATE_88200))
                  reconfig_a2dp_param_val = 303000;
                else
                  reconfig_a2dp_param_val = 330000;
                break;
              case 1003:
                reconfig_a2dp_param_val = 0;
                break;
              }
              if (codec_config.codec_specific_1 != 0) {
                reconfig_a2dp_param_id = BITRATE_PARAM_ID;
                isBitRateChange = true;
              }
            } else if ((codec_config.bits_per_sample != cp.bits_per_sample) &&
                     (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC)) {
              switch (cp.bits_per_sample)
              {
                case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16:
                  reconfig_a2dp_param_val = 16;
                  break;
                case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24:
                  reconfig_a2dp_param_val = 24;
                  break;
                case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_32:
                  reconfig_a2dp_param_val = 32;
                  break;
                case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE:
                  break;
              }
              if ((cp.bits_per_sample != 0) && (codec_config.bits_per_sample != 0)) {
                reconfig_a2dp_param_id = BITSPERSAMPLE_PARAM_ID;
                isBitsPerSampleChange = true;
              }
            }
          }

          if (!btif_av_allow_codec_config_change(cp.codec_type,cp.sample_rate)) {
            int idx;
            if (btif_av_stream_started_ready())
              idx = btif_av_get_latest_playing_device_idx();
            else
              idx = btif_av_get_latest_device_idx_to_start();
            std::string addrstr = btif_av_cb[idx].peer_bda.ToString();
            const char* bt_addr = addrstr.c_str();
            btif_transfer_context(btif_av_handle_event, BTIF_AV_SOURCE_CONFIG_UPDATED_EVT,
                                  (char *)bt_addr,sizeof(RawAddress), NULL);
            return BT_STATUS_SUCCESS;
          }
          else
            codec_cfg_change = true;
    isDevUiReq = true;
    btif_transfer_context(btif_av_handle_event, BTIF_AV_SOURCE_CONFIG_REQ_EVT,
                          reinterpret_cast<char*>(&cp), sizeof(cp), NULL);
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         cleanup
 *
 * Description      Shuts down the AV interface and does the cleanup
 *
 * Returns          None
 *
 ******************************************************************************/
static void cleanup(int service_uuid) {
  BTIF_TRACE_EVENT("%s", __func__);

  btif_transfer_context(btif_av_handle_event, BTIF_AV_CLEANUP_REQ_EVT,
                        (char*)&service_uuid, sizeof(int), NULL);

  btif_disable_service(service_uuid);

  alarm_free(av_open_on_rc_timer);
  av_open_on_rc_timer = NULL;
}

static void cleanup_src(void) {
  BTIF_TRACE_EVENT("%s", __func__);

  btif_queue_cleanup(UUID_SERVCLASS_AUDIO_SOURCE);
  if (bt_av_src_callbacks) {
    bt_av_src_callbacks = NULL;
    if (bt_av_sink_callbacks == NULL) cleanup(BTA_A2DP_SOURCE_SERVICE_ID);
  }
}

static void cleanup_sink(void) {
  BTIF_TRACE_EVENT("%s", __func__);

  btif_queue_cleanup(UUID_SERVCLASS_AUDIO_SINK);
  if (bt_av_sink_callbacks) {
    bt_av_sink_callbacks = NULL;
    if (bt_av_src_callbacks == NULL) cleanup(BTA_A2DP_SINK_SERVICE_ID);
  }
}

/*******************************************************************************
 *
 * Function         allow_connection
 *
 * Description      Shuts down the AV interface and does the cleanup
 *
 * Returns          None
 *
 ******************************************************************************/
static void allow_connection(int is_valid, RawAddress *bd_addr)
{
  int index = 0;
  BTIF_TRACE_DEBUG(" %s() isValid is %d event %d", __func__,is_valid,idle_rc_event);
  switch (idle_rc_event) {
    case BTA_AV_RC_OPEN_EVT:
      if (is_valid) {
        BTIF_TRACE_DEBUG("allowconn for RC connection");
        alarm_set_on_mloop(av_open_on_rc_timer,
                           BTIF_TIMEOUT_AV_OPEN_ON_RC_MS,
                           btif_initiate_av_open_timer_timeout, NULL);
          btif_rc_handler(idle_rc_event, (tBTA_AV*)&idle_rc_data);
      } else {
        uint8_t rc_handle =  idle_rc_data.rc_open.rc_handle;
        BTA_AvCloseRc(rc_handle);
      }
      break;

   case BTA_AV_PENDING_EVT:
       index = btif_av_idx_by_bdaddr(bd_addr);
       if (index >= btif_max_av_clients) {
         BTIF_TRACE_DEBUG("Invalid index for device");
         break;
       }
       if (is_valid) {
       BTIF_TRACE_DEBUG("The connection is allowed for the device at index = %d", index);
       BTA_AvOpen(btif_av_cb[index].peer_bda, btif_av_cb[index].bta_handle,
                     true, BTA_SEC_AUTHENTICATE, UUID_SERVCLASS_AUDIO_SOURCE);
       } else {
         BTIF_TRACE_IMP("Reject incoming AV connection on Index %d", index);
         btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED,
             &(btif_av_cb[index].peer_bda));
         BTA_AvClose(btif_av_cb[index].bta_handle);
         btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
       }
   break;

   default:
     BTIF_TRACE_DEBUG("%s : unhandled event:%s", __FUNCTION__,
                              dump_av_sm_event_name((btif_av_sm_event_t)idle_rc_event));
  }
  idle_rc_event = 0;
  memset(&idle_rc_data, 0, sizeof(tBTA_AV));
}

static bt_status_t select_audio_device(RawAddress *bd_addr)
{
    BTIF_TRACE_EVENT("%s", __FUNCTION__);
    CHECK_BTAV_INIT();

    /* Switch to BTIF context */
    return btif_transfer_context(btif_av_handle_event,
            BTIF_AV_TRIGGER_HANDOFF_REQ_EVT,
            (char*)bd_addr, sizeof(RawAddress), NULL);
}

static const btav_source_interface_t bt_av_src_interface = {
    sizeof(btav_source_interface_t),
    init_src,
    src_connect_sink,
    disconnect,
    codec_config_src,
    cleanup_src,
    allow_connection,
    select_audio_device,
};

static const btav_sink_interface_t bt_av_sink_interface = {
    sizeof(btav_sink_interface_t),
    init_sink,
    sink_connect_src,
    disconnect,
    cleanup_sink,
    update_audio_focus_state,
    update_audio_track_gain,
};

/*******************************************************************************
 *
 * Function         btif_av_get_addr
 *
 * Description      Fetches current AV BD address
 *
 * Returns          BD address
 *
 ******************************************************************************/
RawAddress btif_av_get_addr(RawAddress address) {
  int i;
  RawAddress not_found = RawAddress::kEmpty;
  for (i = 0; i < btif_max_av_clients; i++)
    if (btif_av_cb[i].peer_bda == address)
      return btif_av_cb[i].peer_bda;
  return not_found;
}

/******************************************************************************
 *
 * Function         btif_av_get_peer_addr
 *
 * Description      Returns peer device address
 *
 * Returns          void. Updates the peer_bda argument
 *******************************************************************************/
void btif_av_get_peer_addr(RawAddress* peer_bda) {
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  for (int i = 0; i < btif_max_av_clients; i++) {
    state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if ((state == BTIF_AV_STATE_OPENED) ||
        (state == BTIF_AV_STATE_STARTED)) {
      BTIF_TRACE_DEBUG("btif_av_get_peer_addr: %u state: %d ",
              btif_av_cb[i].peer_bda, state);
      *peer_bda = btif_av_cb[i].peer_bda;
      if (state == BTIF_AV_STATE_STARTED)
        break;
    }
  }
}

/*******************************************************************************
 * Function         btif_av_is_sink_enabled
 *
 * Description      Checks if A2DP Sink is enabled or not
 *
 * Returns          true if A2DP Sink is enabled, false otherwise
 *
 ******************************************************************************/

bool btif_av_is_sink_enabled(void) {
  return (bt_av_sink_callbacks != NULL) ? true : false;
}

/*******************************************************************************
 *
 * Function         btif_av_stream_ready
 *
 * Description      Checks whether AV is ready for starting a stream
 *
 * Returns          None
 *
 ******************************************************************************/
bool btif_av_stream_ready(void) {
  int i;
  bool status = false;

  /* also make sure main adapter is enabled */
  if (btif_is_enabled() == 0) {
    BTIF_TRACE_EVENT("%s: main adapter not enabled", __func__);
    return false;
  }

  for (i = 0; i < btif_max_av_clients; i++) {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    BTIF_TRACE_DEBUG("%s : sm hdl %d, state %d, flags %x, handoff %d", __func__,
                   btif_av_cb[i].sm_handle, btif_av_cb[i].state,
                   btif_av_cb[i].flags, btif_av_cb[i].dual_handoff);
    /* Multicast:
     * If any of the stream is in pending suspend state when
     * we initiate start, it will result in inconsistent behavior
     * Check the pending SUSPEND flag and return failure
     * if suspend is in progress.
     */
    if (btif_av_cb[i].dual_handoff ||
        (btif_av_cb[i].flags & BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING)) {
      status = false;
      break;
    } else if (btif_av_cb[i].flags &
        (BTIF_AV_FLAG_REMOTE_SUSPEND|BTIF_AV_FLAG_PENDING_STOP|BTIF_AV_FLAG_PENDING_DISCONNECT)) {
      status = false;
      break;
    } else if (btif_av_cb[i].state == BTIF_AV_STATE_OPENED)
      status = true;
  }
  BTIF_TRACE_DEBUG("%s(): %d", __func__, status);
  return status;
}

/*******************************************************************************
 *
 * Function         btif_av_stream_started_ready
 *
 * Description      Checks whether AV ready for media start in streaming state
 *
 * Returns          bool
 *
 ******************************************************************************/

bool btif_av_stream_started_ready(void)
{
  int i;
  bool status = false;;

  for (i = 0; i < btif_max_av_clients; i++) {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if (btif_av_cb[i].dual_handoff) {
      BTIF_TRACE_ERROR("%s: Under Dual handoff ",__func__ );
      status = false;;
      break;
    } else if (btif_av_cb[i].flags &
               (BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING |
                BTIF_AV_FLAG_REMOTE_SUSPEND |
                BTIF_AV_FLAG_PENDING_STOP)) {
      status = false;;
      break;
    } else if (btif_av_cb[i].state == BTIF_AV_STATE_STARTED)
      status = true;
  }
  BTIF_TRACE_DEBUG("btif_av_stream_started_ready: %d", status);
  return status;
}

/*******************************************************************************
**
** Function         btif_av_is_start_ack_pending
**
** Description      Checks whether start command is sent but not acked by remote
**
** Returns          None
**
*******************************************************************************/

bool btif_av_is_start_ack_pending(void)
{
    int i;
    bool status = false;

    for (i = 0; i < btif_max_av_clients; i++)
    {
        if (btif_av_cb[i].flags & BTIF_AV_FLAG_PENDING_START)
        {
            status = true;
            break;
        }
    }
    BTIF_TRACE_DEBUG("btif_av_is_start_ack_pending: %d", status);
    return status;
}

/*******************************************************************************
**
** Function         btif_av_is_suspend_stop_pending_ack
**
** Description      Checks whether suspend/stop is pending ack when state is started
**
** Returns          None
**
*******************************************************************************/
bool btif_av_is_suspend_stop_pending_ack(void)
{
    int i;
    bool status = FALSE;

    for (i = 0; i < btif_max_av_clients; i++)
    {
        btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
        BTIF_TRACE_DEBUG("btif_av_stream_ready flags: %d, state: %d",
                                btif_av_cb[i].flags, btif_av_cb[i].state);
        if ((btif_av_cb[i].flags &
            (BTIF_AV_FLAG_LOCAL_SUSPEND_PENDING |
            BTIF_AV_FLAG_PENDING_STOP)) &&
            (btif_av_cb[i].state == BTIF_AV_STATE_STARTED)) {
          status = true;
          break;
        }
    }
    BTIF_TRACE_DEBUG("btif_av_is_stream_suspend_pending_ack: %d", status);
    return status;
}

/*******************************************************************************
 *
 * Function         btif_dispatch_sm_event
 *
 * Description      Send event to AV statemachine
 *
 * Returns          None
 *
 ******************************************************************************/

/* used to pass events to AV statemachine from other tasks */
void btif_dispatch_sm_event(btif_av_sm_event_t event, void* p_data, int len) {
  /* Switch to BTIF context */
  BTIF_TRACE_IMP("%s: event: %d, len: %d", __FUNCTION__, event, len);
  btif_transfer_context(btif_av_handle_event, event, (char*)p_data, len, NULL);
  BTIF_TRACE_IMP("%s: event %d sent", __FUNCTION__, event);
}

/*******************************************************************************
 *
 * Function         btif_av_execute_service
 *
 * Description      Initializes/Shuts down the service
 *
 * Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
 *
 ******************************************************************************/
bt_status_t btif_av_execute_service(bool b_enable) {
  int i;
  btif_sm_state_t state;
  bool delay_report_enabled = false;
  char value[PROPERTY_VALUE_MAX] = {'\0'};
  tBTA_AV_FEAT feat_delay_rpt = 0;

  BTIF_TRACE_DEBUG("%s(): enable: %d", __func__, b_enable);
  if (b_enable) {
    osi_property_get("persist.bt.a2dp.delay_report", value, "false");
    delay_report_enabled = (strcmp(value, "true") == 0);
    if (delay_report_enabled)
      feat_delay_rpt = BTA_AV_FEAT_DELAY_RPT;

    /* TODO: Removed BTA_SEC_AUTHORIZE since the Java/App does not
     * handle this request in order to allow incoming connections to succeed.
     * We need to put this back once support for this is added
     */
    /* Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
     * auto-suspend av streaming on AG events(SCO or Call). The suspend shall
     * be initiated by the app/audioflinger layers
     */
#if (AVRC_METADATA_INCLUDED == true)
    BTA_AvEnable(BTA_SEC_AUTHENTICATE,
      BTA_AV_FEAT_RCTG|BTA_AV_FEAT_METADATA|BTA_AV_FEAT_VENDOR|BTA_AV_FEAT_NO_SCO_SSPD
      |BTA_AV_FEAT_ACP_START|feat_delay_rpt
#if (AVRC_ADV_CTRL_INCLUDED == true)
      |BTA_AV_FEAT_RCCT
      |BTA_AV_FEAT_ADV_CTRL
      |BTA_AV_FEAT_BROWSE
#endif
      , bte_av_callback);
#else
    BTA_AvEnable(BTA_SEC_AUTHENTICATE, (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_NO_SCO_SSPD
                 |BTA_AV_FEAT_ACP_START | feat_delay_rpt), bte_av_callback);
#endif

    for (i = 0; i < btif_max_av_clients; i++) {
      BTIF_TRACE_DEBUG("%s: BTA_AvRegister : %d", __FUNCTION__, i);
      BTA_AvRegister(BTA_AV_CHNL_AUDIO, BTIF_AV_SERVICE_NAME, 0,
                     bte_av_sink_media_callback,
      UUID_SERVCLASS_AUDIO_SOURCE);
    }
    BTA_AvUpdateMaxAVClient(btif_max_av_clients);
  } else {
    if (btif_av_is_playing()) {
        BTIF_TRACE_DEBUG("Reset codec before BT ShutsDown");
        RawAddress dummy_bda = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
        btif_report_source_codec_state(NULL, &dummy_bda);
    }
    /* Also shut down the AV state machine */
    for (i = 0; i < btif_max_av_clients; i++ ) {
      if (btif_av_cb[i].sm_handle != NULL) {
        state = btif_sm_get_state(btif_av_cb[i].sm_handle);
        BTIF_TRACE_DEBUG("BT is shutting down, state=%d", state);
        if ((state == BTIF_AV_STATE_OPENING) || (state == BTIF_AV_STATE_OPENED) ||
            (state == BTIF_AV_STATE_STARTED)) {
          BTIF_TRACE_DEBUG("Moving State from opened/started to Idle due to BT ShutDown");
          if (btif_av_is_split_a2dp_enabled()) {
            btif_a2dp_audio_interface_deinit();
            btif_a2dp_audio_if_init = false;
          } else {
             tA2DP_CTRL_CMD pending_cmd = btif_a2dp_get_pending_command();
             BTIF_TRACE_DEBUG("%s: a2dp-ctrl-cmd : %s", __func__,
                                     audio_a2dp_hw_dump_ctrl_event(pending_cmd));
             if (pending_cmd) {
                 btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
             }
             btif_a2dp_source_on_stopped(NULL);
             btif_sm_change_state(btif_av_cb[i].sm_handle, BTIF_AV_STATE_IDLE);
             btif_queue_advance();

          }
        }
        btif_sm_shutdown(btif_av_cb[i].sm_handle);
        btif_av_cb[i].sm_handle = NULL;
      }
    }
    for (i = 0; i < btif_max_av_clients; i++)
      BTA_AvDeregister(btif_av_cb[i].bta_handle);
    BTA_AvDisable();
  }
  BTIF_TRACE_DEBUG("%s: enable: %d completed", __func__, b_enable);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_av_sink_execute_service
 *
 * Description      Initializes/Shuts down the service
 *
 * Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
 *
 ******************************************************************************/
bt_status_t btif_av_sink_execute_service(bool b_enable) {
  int i;
  BTIF_TRACE_DEBUG("%s(): enable: %d", __func__, b_enable);

  if (b_enable) {
    /* Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
     * auto-suspend av streaming on AG events(SCO or Call). The suspend shall
     * be initiated by the app/audioflinger layers
     */
    BTA_AvEnable(BTA_SEC_AUTHENTICATE, BTA_AV_FEAT_NO_SCO_SSPD|BTA_AV_FEAT_RCCT|
                                        BTA_AV_FEAT_METADATA|BTA_AV_FEAT_VENDOR|
                                        BTA_AV_FEAT_ADV_CTRL|BTA_AV_FEAT_RCTG,
                                        bte_av_callback);
    BTA_AvRegister(BTA_AV_CHNL_AUDIO, BTIF_AVK_SERVICE_NAME, 0,
                   bte_av_sink_media_callback, UUID_SERVCLASS_AUDIO_SINK);
  } else {
    /* Also shut down the AV state machine */
    for (i = 0; i < btif_max_av_clients; i++ ) {
      if (btif_av_cb[i].sm_handle != NULL) {
        BTIF_TRACE_DEBUG("%s(): shutting down AV SM", __func__);
        btif_sm_shutdown(btif_av_cb[i].sm_handle);
        btif_av_cb[i].sm_handle = NULL;
      }
    }
    BTA_AvDeregister(btif_av_cb[0].bta_handle);
    BTA_AvDisable();
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_av_get_src_interface
 *
 * Description      Get the AV callback interface for A2DP source profile
 *
 * Returns          btav_source_interface_t
 *
 ******************************************************************************/
const btav_source_interface_t* btif_av_get_src_interface(void) {
  BTIF_TRACE_EVENT("%s", __func__);
  return &bt_av_src_interface;
}

/*******************************************************************************
 *
 * Function         btif_av_get_sink_interface
 *
 * Description      Get the AV callback interface for A2DP sink profile
 *
 * Returns          btav_sink_interface_t
 *
 ******************************************************************************/
const btav_sink_interface_t* btif_av_get_sink_interface(void) {
  BTIF_TRACE_EVENT("%s", __func__);
  return &bt_av_sink_interface;
}

/*******************************************************************************
 *
 * Function         btif_av_is_connected
 *
 * Description      Checks if av has a connected sink
 *
 * Returns          bool
 *
 ******************************************************************************/
bool btif_av_is_connected(void) {
  int i;
  bool status = false;
  for (i = 0; i < btif_max_av_clients; i++) {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if ((btif_av_cb[i].state == BTIF_AV_STATE_OPENED) ||
        (btif_av_cb[i].state ==  BTIF_AV_STATE_STARTED)) {
      status = true;
      break;
    }
  }
  return status;
}

/*******************************************************************************
**
** Function         btif_av_is_connected_on_other_idx
**
** Description      Checks if any other AV SCB is connected
**
** Returns          bool
**
*******************************************************************************/
bool btif_av_is_connected_on_other_idx(int current_index) {
  //return true if other IDx is connected
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  int i;
  for (i = 0; i < btif_max_av_clients; i++) {
    if (i != current_index) {
      state = btif_sm_get_state(btif_av_cb[i].sm_handle);
      if ((state == BTIF_AV_STATE_OPENED) ||
          (state == BTIF_AV_STATE_STARTED))
        return true;
    }
  }
  return false;
}

/*******************************************************************************
**
** Function         btif_av_get_other_connected_idx
**
** Description      Checks if any AV SCB is connected other than the current
**                  index
**
** Returns          index
**
*******************************************************************************/
int btif_av_get_other_connected_idx(int current_index)
{
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  int i;
  // return true if other IDx is connected
  for (i = 0; i < btif_max_av_clients; i++) {
    if (i != current_index) {
      state = btif_sm_get_state(btif_av_cb[i].sm_handle);
      if ((state == BTIF_AV_STATE_OPENED) ||
          (state == BTIF_AV_STATE_STARTED))
        return i;
    }
  }
  return INVALID_INDEX;
}

/*******************************************************************************
**
** Function         btif_av_is_playing_on_other_idx
**
** Description      Checks if any other AV SCB is connected
**
** Returns          bool
**
*******************************************************************************/
bool btif_av_is_playing_on_other_idx(int current_index)
{
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  int i;
  // return true if other IDx is playing
  for (i = 0; i < btif_max_av_clients; i++) {
    if (i != current_index) {
      state = btif_sm_get_state(btif_av_cb[i].sm_handle);
      if (state == BTIF_AV_STATE_STARTED)
        return true;
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         btif_av_update_current_playing_device
 *
 * Description      Update the next connected device as playing
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_av_update_current_playing_device(int index) {
  int i;
  for (i = 0; i < btif_max_av_clients; i++)
    if (i != index)
      btif_av_cb[i].current_playing = true;
}

/*******************************************************************************
 *
 * Function         btif_av_get_current_playing_dev_idx
 *
 * Description      get current playing device index
 *
 * Returns          int
 *
 ******************************************************************************/
int btif_av_get_current_playing_dev_idx(void)
{
  int i;

  for (i = 0; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].current_playing == TRUE) {
      BTIF_TRACE_DEBUG("current playing on index = %d", i);
      return i;
    }
  }

  return 0;
}

/******************************************************************************
**
** Function        btif_av_get_sink_latency
**
** Description     get initial sink latency
**
** Returns         tBTA_AV_LATENCY
**
********************************************************************************/
tBTA_AV_LATENCY btif_av_get_sink_latency() {
  int index = 0;
  int i;
  tBTA_AV_LATENCY latency = 0;
  tBTA_AV_LATENCY sink_latency = 0;

  if (enable_multicast == false) {
    index = btif_av_get_current_playing_dev_idx();
    /* Set sink latency to be latency reported from sink if delay reporting
     * is enabled, or else set sink latency to be default sink latency value */
    if (btif_av_cb[index].sink_latency > 0)
      sink_latency = btif_av_cb[index].sink_latency;
    else
      sink_latency = BTIF_AV_DEFAULT_SINK_LATENCY;
  } else {
    BTIF_TRACE_DEBUG("%s, multicast enabled, calculate average sink latency", __func__);
    for (i = 0; i < btif_max_av_clients; i++) {
      if (btif_av_cb[i].sink_latency > 0)
        latency += btif_av_cb[i].sink_latency;
      else
        latency += BTIF_AV_DEFAULT_MULTICAST_SINK_LATENCY;
    }
    if (latency > 0)
      sink_latency = latency / btif_max_av_clients;
    else
      sink_latency = BTIF_AV_DEFAULT_MULTICAST_SINK_LATENCY;
  }

  BTIF_TRACE_DEBUG("%s, return sink latency: %d", __func__, sink_latency);
  return sink_latency;
}


/******************************************************************************
**
** Function        btif_av_is_scrambling_enabled
**
** Description     get scrambling is enabled from bluetooth.
**
** Returns         bool
**
********************************************************************************/
bool btif_av_is_scrambling_enabled() {
  uint8_t no_of_freqs = 0;
  uint8_t *freqs = NULL;
  char value[PROPERTY_VALUE_MAX] = {'\0'};
  btav_a2dp_codec_config_t codec_config;
  std::vector<btav_a2dp_codec_config_t> codecs_local_capabilities;
  std::vector<btav_a2dp_codec_config_t> codecs_selectable_capabilities;
  A2dpCodecs* a2dp_codecs = bta_av_get_a2dp_codecs();
  if (a2dp_codecs == nullptr) return false;

  osi_property_get("persist.vendor.bt.splita2dp.44_1_war", value, "false");

  if(strcmp(value, "true")) {
    BTIF_TRACE_WARNING(
        "persist.vendor.bt.splita2dp.44_1_war is not set");
    return false;
  }

  if (!a2dp_codecs->getCodecConfigAndCapabilities(
          &codec_config, &codecs_local_capabilities,
          &codecs_selectable_capabilities)) {
    BTIF_TRACE_WARNING(
        "btif_av_is_scrambling_enabled failed: "
        "cannot get codec config and capabilities");
    return false;
  }
  freqs = controller_get_interface()->get_scrambling_supported_freqs(&no_of_freqs);
  if(no_of_freqs == 0) {
    BTIF_TRACE_WARNING(
        "BT controller doesn't support scrambling");
    return false;
  }

  if (freqs != NULL) {
    for ( uint8_t i = 0; i < no_of_freqs; i++) {
      if (freqs[i] ==  ( uint8_t ) codec_config.sample_rate ) {
         return true;
      }
    }
  }
  return false;
}

uint8_t btif_av_get_peer_sep(int index) {
  if (isA2dpSink == true)
    return AVDT_TSEP_SNK;
  else
    return AVDT_TSEP_SRC;
}

/*******************************************************************************
 *
 * Function         btif_av_is_peer_edr
 *
 * Description      Check if the connected a2dp device supports
 *                  EDR or not. Only when connected this function
 *                  will accurately provide a true capability of
 *                  remote peer. If not connected it will always be false.
 *
 * Returns          true if remote device is capable of EDR
 *
 ******************************************************************************/
bool btif_av_is_peer_edr(void) {
  btif_sm_state_t state;
  bool peer_edr = false;

  ASSERTC(btif_av_is_connected(), "No active a2dp connection", 0);

  /* If any of the remote in streaming state is BR
   * return false to ensure proper configuration
   * is used. Ideally, since multicast is not supported
   * if any of the connected device is BR device,
   * we should not see both devices in START state.
   */
  for (int index = 0; index < btif_max_av_clients; index ++) {
    state = btif_sm_get_state(btif_av_cb[index].sm_handle);
    if ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START)
            || btif_av_cb[index].current_playing) {
      if (btif_av_cb[index].edr)
        peer_edr = true;
      else
        return false;
    }
  }
  return peer_edr;
}

/*******************************************************************************
 *
 * Function         btif_av_any_br_peer
 *
 * Description      Check if the any of connected devices is BR device.
 *
 * Returns          true if connected to any BR device, false otherwise.
 *
 ******************************************************************************/
bool btif_av_any_br_peer(void) {
  btif_sm_state_t state;
  for (int index = 0; index < btif_max_av_clients; index ++) {
    state = btif_sm_get_state(btif_av_cb[index].sm_handle);
    if (state >= BTIF_AV_STATE_OPENED) {
      if (!btif_av_cb[index].edr) {
        BTIF_TRACE_WARNING("%s : Connected to BR device :", __FUNCTION__);
        return true;
      }
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         btif_av_peer_supports_3mbps
 *
 * Description      check if the connected a2dp device supports
 *                  3mbps edr. Only when connected this function
 *                  will accurately provide a true capability of
 *                  remote peer. If not connected it will always be false.
 *
 * Returns          true if remote device is EDR and supports 3mbps
 *
 ******************************************************************************/
bool btif_av_peer_supports_3mbps(void) {
  btif_sm_state_t state;
  ASSERTC(btif_av_is_connected(), "No active a2dp connection", 0);

  for (int index = 0; index < btif_max_av_clients; index ++) {
    state = btif_sm_get_state(btif_av_cb[index].sm_handle);
    if ((btif_av_cb[index].flags & BTIF_AV_FLAG_PENDING_START)
         || btif_av_cb[index].current_playing)
      if (btif_av_cb[index].edr_3mbps)
        return true;
  }
  return false;
}

/******************************************************************************
 *
 * Function        btif_av_clear_remote_suspend_flag
 *
 * Description     Clears btif_av_cd.flags if BTIF_AV_FLAG_REMOTE_SUSPEND is set
 *
 * Returns          void
 *****************************************************************************/
void btif_av_clear_remote_suspend_flag(void) {
  int i;
  for (i = 0; i < btif_max_av_clients; i++) {
    BTIF_TRACE_DEBUG("%s(): flag :%x", __func__, btif_av_cb[i].flags);
    btif_av_cb[i].flags  &= ~BTIF_AV_FLAG_REMOTE_SUSPEND;
  }
}

/*******************************************************************************
 *
 * Function         btif_av_move_idle
 *
 * Description      Opening state is intermediate state. It cannot handle
 *                  incoming/outgoing connect/disconnect requests.When ACL
 *                  is disconnected and we are in opening state then move back
 *                  to idle state which is proper to handle connections.
 *
 * Returns          Void
 *
 ******************************************************************************/
void btif_av_move_idle(RawAddress bd_addr) {
  int index =0;
  /* inform the application that ACL is disconnected and move to idle state */
  index = btif_av_idx_by_bdaddr(&bd_addr);
  if (index == btif_max_av_clients) {
    BTIF_TRACE_IMP("btif_av_move_idle: Already in IDLE");
    return;
  }
  btif_sm_state_t state = btif_sm_get_state(btif_av_cb[index].sm_handle);
  BTIF_TRACE_IMP("ACL Disconnected state %d  is same device %d",state,
           memcmp (&bd_addr, &(btif_av_cb[index].peer_bda), sizeof(bd_addr)));
  if (state == BTIF_AV_STATE_OPENING &&
      (memcmp (&bd_addr, &(btif_av_cb[index].peer_bda), sizeof(bd_addr)) == 0)) {
    BTIF_TRACE_IMP("Moving BTIF State from Opening to Idle due to ACL disconnect");
    btif_report_connection_state(BTAV_CONNECTION_STATE_DISCONNECTED, &(btif_av_cb[index].peer_bda));
    BTA_AvClose(btif_av_cb[index].bta_handle);
    btif_av_check_and_start_collission_timer(index);
    btif_sm_change_state(btif_av_cb[index].sm_handle, BTIF_AV_STATE_IDLE);
    btif_queue_advance();
  }
}

/******************************************************************************
 *
 * Function        btif_av_get_num_playing_devices
 *
 * Description     Return number of A2dp playing devices
 *
 * Returns         int
 *****************************************************************************/
uint16_t btif_av_get_num_playing_devices(void) {
  uint16_t i;
  uint16_t playing_devices = 0;
  for (i = 0; i < btif_max_av_clients; i++) {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if (btif_av_cb[i].state ==  BTIF_AV_STATE_STARTED)
        playing_devices++;
  }
  BTIF_TRACE_DEBUG("AV devices playing: %d", playing_devices);

  return playing_devices;
}
/*******************************************************************************
 *
 * Function        btif_av_get_num_connected_devices
 *
 * Description     Return number of A2dp connected devices
 *
 * Returns         int
 *****************************************************************************/
uint16_t btif_av_get_num_connected_devices(void) {
  uint16_t i;
  uint16_t connected_devies = 0;
  for (i = 0; i < btif_max_av_clients; i++)
  {
    btif_av_cb[i].state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if ((btif_av_cb[i].state == BTIF_AV_STATE_OPENED) ||
        (btif_av_cb[i].state ==  BTIF_AV_STATE_STARTED))
      connected_devies++;
  }
  BTIF_TRACE_DEBUG("AV Connection count: %d", connected_devies);

  return connected_devies;
}

/******************************************************************************
**
** Function         btif_av_get_av_hdl_from_idx
**
** Description      Returns bta handle from the device index
**
** Returns         BTA HANDLE
********************************************************************************/
tBTA_AV_HNDL btif_av_get_av_hdl_from_idx(int idx)
{
    if (idx == btif_max_av_clients)
    {
        BTIF_TRACE_ERROR("%s: Invalid handle",__func__);
        return -1;
    }
    return btif_av_cb[idx].bta_handle;
}

/******************************************************************************
 *
 * Function        btif_av_update_multicast_state
 *
 * Description     Enable Multicast only if below conditions are satisfied
 *                 1. Connected to only 2 EDR HS.
 *                 2. Connected to both HS as master.
 *                 3. Connected to 2 EDR HS and one BLE device
 *                 Multicast will fall back to soft handsoff in below conditions
 *                 1. Number of ACL links is more than 2,like connected to HID
 *                    initiating connection for HS1 and HS2.
 *                 2. Connected to BR and EDR HS.
 *                 3. Connected to more then 1 BLE device
 *
 * Returns         void
*****************************************************************************/
void btif_av_update_multicast_state(int index) {
  uint16_t num_connected_br_edr_devices = 0;
  uint16_t num_connected_le_devices = 0;
  uint16_t num_av_connected = 0;
  uint16_t i = 0;
  bool is_slave = false;
  bool is_br_hs_connected = false;
  bool prev_multicast_state = enable_multicast;

  if (!is_multicast_supported) {
    BTIF_TRACE_DEBUG("%s() Multicast is Disabled", __func__);
    return;
  }

  if (multicast_disabled == true) {
    multicast_disabled = false;
    enable_multicast = false;
    BTA_AvEnableMultiCast(false, btif_av_cb[index].bta_handle);
    return;
  }

  BTIF_TRACE_DEBUG("%s() Multicast previous state : %s", __func__,
    enable_multicast ? "Enabled" : "Disabled" );

  num_connected_br_edr_devices = btif_dm_get_br_edr_links();
  num_connected_le_devices = btif_dm_get_le_links();
  num_av_connected = btif_av_get_num_connected_devices();
  is_br_hs_connected = btif_av_any_br_peer();

  for (i = 0; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].is_slave == true) {
      BTIF_TRACE_WARNING("Conected as slave");
      is_slave = true;
      break;
    }
  }

  if ((num_av_connected <= 2) && (is_br_hs_connected != true) &&
      (is_slave == false) && ((num_connected_br_edr_devices <= 2) &&
      (num_connected_le_devices <= 1)))
    enable_multicast = true;
  else
    enable_multicast = false;

  BTIF_TRACE_DEBUG("%s() Multicast current state : %s", __func__,
        enable_multicast ? "Enabled" : "Disabled" );

  if (prev_multicast_state != enable_multicast) {
    BTA_AvEnableMultiCast(enable_multicast, btif_av_cb[index].bta_handle);
    HAL_CBACK(bt_av_src_callbacks, multicast_state_cb, enable_multicast);
  }
}

/******************************************************************************
**
** Function        btif_av_get_multicast_state
**
** Description     Returns true if multicast is enabled else false
**
** Returns         bool
******************************************************************************/
bool btif_av_get_multicast_state() {
  return enable_multicast;
}

/******************************************************************************
**
** Function        btif_av_get_ongoing_multicast
**
** Description     Returns true if multicast is ongoing
**
** Returns         bool
******************************************************************************/
bool btif_av_get_ongoing_multicast() {
  int i = 0, j = 0;
  if (!is_multicast_supported) {
    BTIF_TRACE_DEBUG("Multicast is Disabled");
    return false;
  }
  for (i = 0; i < btif_max_av_clients; i++)
    if (btif_av_cb[i].is_device_playing)
      j++;

  if (j == btif_max_av_clients)
    return true;
  else
    return false;
}

/******************************************************************************
**
** Function        btif_av_is_multicast_supported
**
** Description     Returns true if multicast is supported
**
** Returns         bool
******************************************************************************/
bool btif_av_is_multicast_supported() {
  return is_multicast_supported;
}

bool btif_av_check_flag_remote_suspend(int index) {
  BTIF_TRACE_ERROR("%s(): index = %d",__func__,index);
  if (btif_av_cb[index].flags & BTIF_AV_FLAG_REMOTE_SUSPEND) {
    BTIF_TRACE_DEBUG("remote suspend flag set on index = %d",index);
    return true;
  } else {
    BTIF_TRACE_DEBUG("remote suspend flag not set on index = %d",index);
    return false;
  }
}

/* SPLITA2DP*/
/*******************************************************************************
 *
 * Function         btif_av_is_split_a2dp_enabled
 *
 * Description      Check if split a2dp is enabled.
 *
 * Returns          TRUE if split a2dp is enabled, FALSE otherwise
 *
 ******************************************************************************/
bool btif_av_is_split_a2dp_enabled() {
  BTIF_TRACE_DEBUG("btif_av_is_split_a2dp_enabled:%d",bt_split_a2dp_enabled);
  return bt_split_a2dp_enabled;
}
/******************************************************************************
**
** Function         btif_av_is_under_handoff
**
** Description     check if AV state is under handoff
**
** Returns         TRUE if handoff is triggered, FALSE otherwise
********************************************************************************/
bool btif_av_is_under_handoff() {
  int i;
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;

  BTIF_TRACE_DEBUG("btif_av_is_under_handoff");

  for (i = 0; i < btif_max_av_clients; i++) {
    state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    if (btif_av_cb[i].dual_handoff &&
      (state == BTIF_AV_STATE_STARTED || state == BTIF_AV_STATE_OPENED)) {
      /* If a2dp reconfigure is triggered when playing device disconnect is
       * initiated locally then return false, otherwise wait till the suspend cfm
       * is received from the remote.
       */
       BTIF_TRACE_DEBUG("AV is under handoff");
       return true;
    }
  }
  return false;
}

bool btif_av_is_handoff_set() {
  for (int i = 0; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].dual_handoff) {
      BTIF_TRACE_DEBUG("%s: AV is under handoff for idx = %d",__func__, i);
      return true;
    }
  }
  return false;
}

bool btif_av_is_device_disconnecting() {
  int i;
  btif_sm_state_t state = BTIF_AV_STATE_IDLE;
  BTIF_TRACE_DEBUG("btif_av_is_device_disconnecting");
  for (i = 0; i < btif_max_av_clients; i++) {
    state = btif_sm_get_state(btif_av_cb[i].sm_handle);
    BTIF_TRACE_DEBUG("%s: state = %d",__func__,state);
    if ((btif_av_cb[i].dual_handoff &&
      state == BTIF_AV_STATE_CLOSING)) {
      BTIF_TRACE_DEBUG("Device disconnecting");
      return true;
    }
  }
  return false;
}
void btif_av_reset_reconfig_flag() {
  int i;
  BTIF_TRACE_DEBUG("%s",__func__);
  reconfig_a2dp = FALSE;
  for (i = 0; i < btif_max_av_clients; i++)
  {
    if (btif_av_cb[i].dual_handoff) {
      BTIF_TRACE_DEBUG("%s resetting dual handoff flag for index %d",__func__, i);
      btif_av_cb[i].dual_handoff = FALSE;
    }
  }
}

bool btif_av_allow_codec_config_change(btav_a2dp_codec_index_t codec_type,
          btav_a2dp_codec_sample_rate_t sample_rate) {
  BTIF_TRACE_DEBUG("%s",__func__);
  /* Only 48khz sampling rate is supported in Split A2dp mode for other codecs, disregard
   * codec switch request for sample rate change.
   * LDAC Supports all sampling rates and switch request will be honored
  */

  if (codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC) {
      return true;
  }
  if (sample_rate > 0 && sample_rate != BTAV_A2DP_CODEC_SAMPLE_RATE_48000) {
      BTIF_TRACE_DEBUG("config not supported codec_type = %d, sample_rate = %d",
                        codec_type, sample_rate)
      return false; //Only 48k is supported in split mode
  }
  return true;
}

/******************************************************************************
**
** Function         btif_av_get_reconfig_dev_hndl
**
** Description     Retrieve the bta handle for the device codec switch is
**                 triggered in the middle of streaming.
**
** Returns         BTA HANDLE if codec switch triggered during streaming
********************************************************************************/
tBTA_AV_HNDL btif_av_get_reconfig_dev_hndl() {
  int i = 0;
  BTIF_TRACE_DEBUG("%s",__func__);
  for (i = 0 ; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].reconfig_pending)
        return btif_av_cb[i].bta_handle;
  }
  return 0;
}

/******************************************************************************
**
** Function        btif_av_reset_codec_reconfig_flag
**
** Description     Reset codec reconfig flag if avdtp reconfig is not triggered
**                 for user provided codec config
**
** Returns         void
********************************************************************************/
void btif_av_reset_codec_reconfig_flag() {
  int i;
  BTIF_TRACE_DEBUG("%s",__func__);
  for (i = 0; i < btif_max_av_clients; i++) {
    if (btif_av_cb[i].reconfig_pending)
     btif_av_cb[i].reconfig_pending = false;
  }
}

/******************************************************************************
**
** Function        btif_av_reinit_audio_interface
**
** Description     Reinit audio interface,this function is called when BT
**                 audio hal server is died
**
** Returns         void
********************************************************************************/
void btif_av_reinit_audio_interface() {
  BTIF_TRACE_DEBUG(LOG_TAG,"btif_av_reint_audio_interface");
  btif_a2dp_audio_interface_init();
  btif_a2dp_audio_if_init = true;
}

void btif_av_flow_spec_cmd(int index, int bitrate) {
  tBT_FLOW_SPEC flow_spec;
  memset(&flow_spec, 0x00, sizeof(flow_spec));
  flow_spec.flow_direction = 0x00;     /* flow direction - out going */
  flow_spec.service_type = 0x02;       /* Guaranteed */
  flow_spec.token_rate = 0x00;         /* bytes/second - no token rate is specified*/
  flow_spec.token_bucket_size = 0x00;  /* bytes - no token bucket is needed*/
  flow_spec.latency = 0xFFFFFFFF;      /* microseconds - default value */
  flow_spec.peak_bandwidth = bitrate/8;/*bytes per second */
  BTM_FlowSpec (btif_av_cb[index].peer_bda, &flow_spec, NULL);
  APPL_TRACE_DEBUG("%s peak_bandwidth %d",__func__, flow_spec.peak_bandwidth);
}
/*SPLITA2DP*/
