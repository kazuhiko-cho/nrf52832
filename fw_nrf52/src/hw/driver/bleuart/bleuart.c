/*
 * bleuart.c
 *
 *  Created on: 2022. 5. 16.
 *      Author: kazuh
 */

#include "bleuart.h"
#include "led.h"
#include "qbuffer.h"

#include "app_timer.h"

#include "nrf_sdh.h"

#include "ble_nus.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"

#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"


#define APP_BLE_CONN_CFG_TAG            1                                           /**< A tag identifying the SoftDevice BLE configuration. */

#define DEVICE_NAME                     "Nordic_UART"                               /**< Name of device. Will be included in the advertising data. */
#define NUS_SERVICE_UUID_TYPE           BLE_UUID_TYPE_VENDOR_BEGIN                  /**< UUID type for the Nordic UART Service (vendor specific). */

#define APP_BLE_OBSERVER_PRIO           3                                           /**< Application's BLE observer priority. You shouldn't need to modify this value. */

#define APP_ADV_INTERVAL                64                                          /**< The advertising interval (in units of 0.625 ms. This value corresponds to 40 ms). */

#define APP_ADV_DURATION                18000                                       /**< The advertising duration (180 seconds) in units of 10 milliseconds. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(7.5, UNIT_1_25_MS)            /**< Minimum acceptable connection interval (20 ms), Connection interval uses 1.25 ms units. */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(75, UNIT_1_25_MS)             /**< Maximum acceptable connection interval (75 ms), Connection interval uses 1.25 ms units. */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds), Supervision Timeout uses 10 ms units. */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000)                       /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000)                      /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */



BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);                                   /**< BLE NUS service instance. */
NRF_BLE_GATT_DEF(m_gatt);                                                           /**< GATT module instance. */
NRF_BLE_QWR_DEF(m_qwr);                                                             /**< Context for the Queued Write module.*/
BLE_ADVERTISING_DEF(m_advertising);                                                 /**< Advertising module instance. */

static uint16_t   m_conn_handle          = BLE_CONN_HANDLE_INVALID;                 /**< Handle of the current connection. */
static uint16_t   m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3;            /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Nordic UART service module. */
static ble_uuid_t m_adv_uuids[]          =                                          /**< Universally unique service identifier. */
{
    {BLE_UUID_NUS_SERVICE, NUS_SERVICE_UUID_TYPE}
};


static bool is_init = false;
static bool is_connect = false;

static ret_code_t err_code_init = NRF_SUCCESS;

static qbuffer_t q_rx;
static qbuffer_t q_tx;

static uint8_t q_buf_rx[BLEUART_MAX_BUF_LEN];
static uint8_t q_buf_tx[BLEUART_MAX_BUF_LEN];

static bool is_q_rx_over = false;
static volatile bool is_ready_to_send = false;

static bool ble_stack_init(void);
static bool gap_params_init(void);
static bool gatt_init(void);
static bool services_init(void);
static bool advertising_init(void);
static bool conn_params_init(void);
static bool advertising_start(void);


static void handler_ble_evt(ble_evt_t const * p_ble_evt, void * p_context);
static void handler_gatt_evt(nrf_ble_gatt_t * p_gatt, nrf_ble_gatt_evt_t const * p_evt);
static void handler_nrf_qwr_error(uint32_t nrf_error);
static void handler_nus_data(ble_nus_evt_t * p_evt);
static void handler_on_adv_evt(ble_adv_evt_t ble_adv_evt);
static void handler_on_conn_params_evt(ble_conn_params_evt_t * p_evt);
static void handler_conn_params_error(uint32_t nrf_error);


bool bleUartInit(void)
{
  bool ret = true;



  qbufferCreate(&q_rx, q_buf_rx, BLEUART_MAX_BUF_LEN);
  qbufferCreate(&q_tx, q_buf_tx, BLEUART_MAX_BUF_LEN);

  ret &= ble_stack_init();
  ret &= gap_params_init();
  ret &= gatt_init();
  ret &= services_init();
  ret &= advertising_init();
  ret &= conn_params_init();
  ret &= advertising_start();

  is_init = ret;

  return ret;
}

bool bleUartIsInit(void)
{
  return is_init;
}

bool bleUartIsConnect(void)
{
  return is_connect;
}

uint32_t bleUartAvailable(void)
{
  return qbufferAvailable(&q_rx);
}

bool bleUartFlush(void)
{
  qbufferFlush(&q_rx);
  qbufferFlush(&q_tx);

  return true;
}

uint8_t  bleUartRead(void)
{
  uint8_t ret = 0;
  qbufferRead(&q_rx, &ret, 1);

  return ret;
}

uint32_t bleUartWrite(uint8_t *p_data, uint32_t length)
{
  uint32_t sent_len;
  uint16_t tx_len;
  uint32_t err_code;
  uint8_t *p_tx_buf;
  uint32_t pre_time;


  if(is_connect != true) return 0;


  sent_len = 0;

  while(sent_len < length)
  {
    p_tx_buf = &p_data[sent_len];
    tx_len = length;
    if(tx_len > m_ble_nus_max_data_len)
    {
      tx_len = m_ble_nus_max_data_len;
    }

    is_ready_to_send = false;
    err_code = ble_nus_data_send(&m_nus, p_tx_buf, &tx_len, m_conn_handle);
    if ((err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != NRF_ERROR_RESOURCES) &&
        (err_code != NRF_ERROR_NOT_FOUND))
    {
        break;
    }

    pre_time = millis();
    while(millis()-pre_time < 100)
    {
      if (is_ready_to_send == true)
      {
        break;
      }

      if(is_connect != true)
      {
        break;
      }
    }

    if (is_ready_to_send != true)
    {
      break;
    }
  }

  return sent_len;
}

bool ble_stack_init(void)
{
  err_code_init = nrf_sdh_enable_request();
  if (err_code_init != NRF_SUCCESS) return false;

  // Configure the BLE stack using the default settings.
  // Fetch the start address of the application RAM.
  uint32_t ram_start = 0;
  err_code_init = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
  if (err_code_init != NRF_SUCCESS) return false;

  // Enable BLE stack.
  err_code_init = nrf_sdh_ble_enable(&ram_start);
  if (err_code_init != NRF_SUCCESS) return false;

  // Register a handler for BLE events.
  NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, handler_ble_evt, NULL);

  return true;
}

bool gap_params_init(void)
{
  ble_gap_conn_params_t   gap_conn_params;
  ble_gap_conn_sec_mode_t sec_mode;


  if (err_code_init != NRF_SUCCESS) return false;


  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

  err_code_init = sd_ble_gap_device_name_set(&sec_mode,
                                        (const uint8_t *) DEVICE_NAME,
                                        strlen(DEVICE_NAME));
  if (err_code_init != NRF_SUCCESS) return false;

  memset(&gap_conn_params, 0, sizeof(gap_conn_params));

  gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
  gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
  gap_conn_params.slave_latency     = SLAVE_LATENCY;
  gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

  err_code_init = sd_ble_gap_ppcp_set(&gap_conn_params);
  if (err_code_init != NRF_SUCCESS) return false;

  return true;
}

bool gatt_init(void)
{
  if (err_code_init != NRF_SUCCESS) return false;

  err_code_init = nrf_ble_gatt_init(&m_gatt, handler_gatt_evt);
  if (err_code_init != NRF_SUCCESS) return false;

  err_code_init = nrf_ble_gatt_att_mtu_periph_set(&m_gatt, 64);
  if (err_code_init != NRF_SUCCESS) return false;

  return true;
}

bool services_init(void)
{
  ble_nus_init_t     nus_init;
  nrf_ble_qwr_init_t qwr_init = {0};

  if (err_code_init != NRF_SUCCESS) return false;

  // Initialize Queued Write Module.
  qwr_init.error_handler = handler_nrf_qwr_error;

  err_code_init = nrf_ble_qwr_init(&m_qwr, &qwr_init);
  if (err_code_init != NRF_SUCCESS) return false;

  // Initialize NUS.
  memset(&nus_init, 0, sizeof(nus_init));

  nus_init.data_handler = handler_nus_data;

  err_code_init = ble_nus_init(&m_nus, &nus_init);
  if (err_code_init != NRF_SUCCESS) return false;

  return true;
}

bool advertising_init(void)
{
  ble_advertising_init_t init;

  if (err_code_init != NRF_SUCCESS) return false;


  memset(&init, 0, sizeof(init));

  init.advdata.name_type          = BLE_ADVDATA_FULL_NAME;
  init.advdata.include_appearance = false;
  init.advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_LIMITED_DISC_MODE;

  init.srdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
  init.srdata.uuids_complete.p_uuids  = m_adv_uuids;

  init.config.ble_adv_fast_enabled  = true;
  init.config.ble_adv_fast_interval = APP_ADV_INTERVAL;
  init.config.ble_adv_fast_timeout  = APP_ADV_DURATION;
  init.evt_handler = handler_on_adv_evt;

  err_code_init = ble_advertising_init(&m_advertising, &init);
  if (err_code_init != NRF_SUCCESS) return false;

  ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);

  return true;
}

bool conn_params_init(void)
{
  ble_conn_params_init_t cp_init;

  if (err_code_init != NRF_SUCCESS) return false;


  memset(&cp_init, 0, sizeof(cp_init));

  cp_init.p_conn_params                  = NULL;
  cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
  cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
  cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
  cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
  cp_init.disconnect_on_fail             = false;
  cp_init.evt_handler                    = handler_on_conn_params_evt;
  cp_init.error_handler                  = handler_conn_params_error;

  err_code_init = ble_conn_params_init(&cp_init);
  if (err_code_init != NRF_SUCCESS) return false;

  return true;
}

bool advertising_start(void)
{
  if (err_code_init != NRF_SUCCESS) return false;

  err_code_init = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
  if (err_code_init != NRF_SUCCESS) return false;

  return true;
}

void handler_ble_evt(ble_evt_t const * p_ble_evt, void * p_context)
{
  uint32_t err_code = 0;

  switch (p_ble_evt->header.evt_id)
  {
    case BLE_GAP_EVT_CONNECTED:
      NRF_LOG_INFO("Connected");
      //err_code = bsp_indication_set(BSP_INDICATE_CONNECTED);
      APP_ERROR_CHECK(err_code);
      m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
      err_code = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
      APP_ERROR_CHECK(err_code);
      ledOn(_DEF_LED2);
      is_connect = true;
      break;

    case BLE_GAP_EVT_DISCONNECTED:
      NRF_LOG_INFO("Disconnected");
      // LED indication will be changed when advertising starts.
      m_conn_handle = BLE_CONN_HANDLE_INVALID;
      ledOff(_DEF_LED2);
      is_connect = false;
      break;

    case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
    {
      NRF_LOG_DEBUG("PHY update request.");
      ble_gap_phys_t const phys =
      {
          .rx_phys = BLE_GAP_PHY_AUTO,
          .tx_phys = BLE_GAP_PHY_AUTO,
      };
      err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
      APP_ERROR_CHECK(err_code);
    } break;

    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
      // Pairing not supported
      err_code = sd_ble_gap_sec_params_reply(m_conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
      APP_ERROR_CHECK(err_code);
      break;

    case BLE_GATTS_EVT_SYS_ATTR_MISSING:
      // No system attributes have been stored.
      err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
      APP_ERROR_CHECK(err_code);
      break;

    case BLE_GATTC_EVT_TIMEOUT:
      // Disconnect on GATT Client timeout event.
      err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                       BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
      APP_ERROR_CHECK(err_code);
      break;

    case BLE_GATTS_EVT_TIMEOUT:
      // Disconnect on GATT Server timeout event.
      err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                       BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
      APP_ERROR_CHECK(err_code);
      break;

    default:
      // No implementation needed.
      break;
  }
}

void handler_gatt_evt(nrf_ble_gatt_t * p_gatt, nrf_ble_gatt_evt_t const * p_evt)
{
  if ((m_conn_handle == p_evt->conn_handle) && (p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED))
  {
      m_ble_nus_max_data_len = p_evt->params.att_mtu_effective - OPCODE_LENGTH - HANDLE_LENGTH;
      NRF_LOG_INFO("Data len is set to 0x%X(%d)", m_ble_nus_max_data_len, m_ble_nus_max_data_len);
  }
  NRF_LOG_DEBUG("ATT MTU exchange completed. central 0x%x peripheral 0x%x",
                p_gatt->att_mtu_desired_central,
                p_gatt->att_mtu_desired_periph);
}

void handler_nrf_qwr_error(uint32_t nrf_error)
{
  APP_ERROR_HANDLER(nrf_error);
}

void handler_nus_data(ble_nus_evt_t * p_evt)
{
  bool ret;

  if (p_evt->type == BLE_NUS_EVT_RX_DATA)
  {
    ret = qbufferWrite(&q_rx, (uint8_t*)p_evt->params.rx_data.p_data, p_evt->params.rx_data.length);

    if(ret != true)
    {
      is_q_rx_over = true;
    }
  }

  if (p_evt->type == BLE_NUS_EVT_TX_RDY)
  {
    is_ready_to_send = true;
  }
}

void handler_on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
  uint32_t err_code = 0;

  switch (ble_adv_evt)
  {
    case BLE_ADV_EVT_FAST:
      //err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING);
      APP_ERROR_CHECK(err_code);
      break;
    case BLE_ADV_EVT_IDLE:
      //sleep_mode_enter();
      ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
      break;
    default:
      break;
  }
}

void handler_on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
  uint32_t err_code;

  if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
  {
    err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
    APP_ERROR_CHECK(err_code);
  }
}

void handler_conn_params_error(uint32_t nrf_error)
{
  APP_ERROR_HANDLER(nrf_error);
}


