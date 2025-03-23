/*
 ;    Project:       Open Vehicle Monitor System
 ;    Date:          1th October 2018
 ;
 ;    Changes:
 ;    1.0  Initial release
 ;
 ;    (C) 2018       Martin Graml
 ;    (C) 2019       Thomas Heuer
 ;
 ; Permission is hereby granted, free of charge, to any person obtaining a copy
 ; of this software and associated documentation files (the "Software"), to deal
 ; in the Software without restriction, including without limitation the rights
 ; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 ; copies of the Software, and to permit persons to whom the Software is
 ; furnished to do so, subject to the following conditions:
 ;
 ; The above copyright notice and this permission notice shall be included in
 ; all copies or substantial portions of the Software.
 ;
 ; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 ; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 ; THE SOFTWARE.
 ;
 ; Most of the CAN Messages are based on https://github.com/MyLab-odyssey/ED_BMSdiag
 ; http://ed.no-limit.de/wiki/index.php/Hauptseite
 */

#include "ovms_log.h"
static const char *TAG = "v-smarteq";

#define VERSION "1.0.0"

#include <stdio.h>
#include <string>
#include <iomanip>
#include "pcp.h"
#include "ovms_metrics.h"
#include "ovms_events.h"
#include "ovms_config.h"
#include "ovms_command.h"
#include "metrics_standard.h"
#include "ovms_notify.h"
#include "ovms_peripherals.h"
#include "vehicle_smarteq.h"
#include "ovms_time.h"
#include "ovms_plugins.h"
#include "buffered_shell.h"

static const OvmsPoller::poll_pid_t obdii_polls[] =
{
  // { tx, rx, type, pid, {OFF,AWAKE,ON,CHARGING}, bus, protocol }
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x07, {  0,300,3,3 }, 0, ISOTP_STD }, // rqBattState
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x04, {  0,300,300,300 }, 0, ISOTP_STD }, // rqBattTemperatures
//  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x41, {  0,300,300,60 }, 0, ISOTP_STD }, // rqBattVoltages_P1
//  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x42, {  0,300,300,60 }, 0, ISOTP_STD }, // rqBattVoltages_P2
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x200c, {  0,300,300,300 }, 0, ISOTP_STD }, // extern temp byte 2+3
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x2101, {  0,300,60,60 }, 0, ISOTP_STD }, // OCS Trip Distance km
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x2104, {  0,300,60,60 }, 0, ISOTP_STD }, // OCS Trip time s
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x0204, {  0,3600,3600,3600 }, 0, ISOTP_STD }, // maintenance data days
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x0203, {  0,3600,3600,3600 }, 0, ISOTP_STD }, // maintenance data usual km
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x0188, {  0,3600,3600,3600 }, 0, ISOTP_STD }, // maintenance level
  { 0x745, 0x765, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x81, {  0,3600,3600,3600 }, 0, ISOTP_STD }, // req.VIN
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x320c, {  0,300,60,60 }, 0, ISOTP_STD }, // rqHV_Energy
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x302A, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_State
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3495, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Load
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3024, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_volt_measure
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3025, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Amps
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3494, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Power
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x33BA, {  0,300,60,3 }, 0, ISOTP_STD }, // indicates ext power supply
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x339D, {  0,300,60,3 }, 0, ISOTP_STD }, // charging plug present
};

static const OvmsPoller::poll_pid_t slow_charger_polls[] =
{
  // { tx, rx, type, pid, {OFF,AWAKE,ON,CHARGING}, bus, protocol }
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x7303, {  0,0,0,3 }, 0, ISOTP_STD }, // rqChargerAC
};

static const OvmsPoller::poll_pid_t fast_charger_polls[] =
{
  // { tx, rx, type, pid, {OFF,AWAKE,ON,CHARGING}, bus, protocol }
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x503F, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph12_RMS_V
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x5041, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph23_RMS_V
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x5042, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph31_RMS_V
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x2001, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph1_RMS_A
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x503A, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph2_RMS_A
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x503B, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Ph3_RMS_A
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x504A, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Power
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x500E, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Power
  { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x5038, {  0,0,0,3 }, 0, ISOTP_STD }, // rqJB2AC_Power
};

/**
 * Constructor & destructor
 */

OvmsVehicleSmartEQ::OvmsVehicleSmartEQ() {
  ESP_LOGI(TAG, "Start Smart EQ vehicle module");

  m_booster_init = true;
  m_booster_start = false;
  m_booster_start_day = false;
  m_booster_ticker = 0;
  m_gps_off = false;
  m_gps_ticker = 0;
  m_12v_ticker = 0;
  m_v2_ticker = 0;
  m_v2_restart = false;

  m_charge_start = false;
  m_charge_finished = true;
  m_led_state = 4;
  m_cfg_cell_interval_drv = 0;
  m_cfg_cell_interval_chg = 0;

  m_network_type_ls = MyConfig.GetParamValue("xsq", "modem.net.type", "auto");
  m_indicator     = MyConfig.GetParamValueBool("xsq", "indicator", false);              //!< activate indicator e.g. 7 times or whtever
  m_ddt4all       = MyConfig.GetParamValueBool("xsq", "ddt4all", false);                //!< DDT4ALL mode

  // BMS configuration:
  BmsSetCellArrangementVoltage(96, 3);
  BmsSetCellArrangementTemperature(27, 1);
  BmsSetCellLimitsVoltage(2.0, 5.0);
  BmsSetCellLimitsTemperature(-39, 200);
  BmsSetCellDefaultThresholdsVoltage(0.020, 0.030);
  BmsSetCellDefaultThresholdsTemperature(2.0, 3.0);

  mt_bms_temps                  = new OvmsMetricVector<float>("xsq.v.bms.temps", SM_STALE_HIGH, Celcius);
  mt_bus_awake                  = MyMetrics.InitBool("xsq.v.bus.awake", SM_STALE_MIN, false);
  mt_use_at_reset               = MyMetrics.InitFloat("xsq.use.at.reset", SM_STALE_MID, 0, kWh);

  mt_ocs_duration               = MyMetrics.InitInt("xsq.ocs.duration", SM_STALE_MIN, 0, Minutes);
  mt_ocs_trip_km                = MyMetrics.InitFloat("xsq.ocs.trip.km", SM_STALE_MID, 0, Kilometers);
  mt_ocs_trip_time              = MyMetrics.InitString("xsq.ocs.trip.time", SM_STALE_MIN, 0, Other);
  mt_ocs_mt_day_prewarn         = MyMetrics.InitInt("xsq.ocs.mt.day.prewarn", SM_STALE_MID, 45, Other);
  mt_ocs_mt_day_usual           = MyMetrics.InitInt("xsq.ocs.mt.day.usual", SM_STALE_MID, 0, Other);
  mt_ocs_mt_km_usual            = MyMetrics.InitInt("xsq.ocs.mt.km.usual", SM_STALE_MID, 0, Kilometers);
  mt_ocs_mt_level               = MyMetrics.InitString("xsq.ocs.mt.level", SM_STALE_MID, "unknown", Other);

  mt_booster_on                 = MyMetrics.InitBool("xsq.booster.on", SM_STALE_MIN, false);
  mt_booster_weekly             = MyMetrics.InitBool("xsq.booster.weekly", SM_STALE_MIN, false);
  mt_booster_time               = MyMetrics.InitString("xsq.booster.time", SM_STALE_MIN, "0515", Other);
  mt_booster_h                  = MyMetrics.InitInt("xsq.booster.h", SM_STALE_MIN, 5, Other);
  mt_booster_m                  = MyMetrics.InitInt("xsq.booster.m", SM_STALE_MIN, 15, Other);
  mt_booster_ds                 = MyMetrics.InitInt("xsq.booster.ds", SM_STALE_MIN, 1, Other);
  mt_booster_de                 = MyMetrics.InitInt("xsq.booster.de", SM_STALE_MIN, 6, Other);
  mt_booster_1to3               = MyMetrics.InitInt("xsq.booster.1to3", SM_STALE_MIN, 0, Other);
  mt_booster_data               = MyMetrics.InitString("xsq.booster.data", SM_STALE_HIGH,"0,0,0,0,-1,-1,-1", Other);

  mt_pos_odometer_start         = MyMetrics.InitFloat("xsq.odometer.start", SM_STALE_MID, 0, Kilometers);
  mt_pos_odometer_start_total   = MyMetrics.InitFloat("xsq.odometer.start.total", SM_STALE_MID, 0, Kilometers);
  mt_pos_odometer_trip_total    = MyMetrics.InitFloat("xsq.odometer.trip.total", SM_STALE_MID, 0, Kilometers);

  mt_evc_hv_energy              = MyMetrics.InitFloat("xsq.evc.hv.energy", SM_STALE_MID, 0, kWh);
  mt_evc_LV_DCDC_amps           = MyMetrics.InitFloat("xsq.evc.lv.dcdc.amps", SM_STALE_MID, 0, Amps);
  mt_evc_LV_DCDC_load           = MyMetrics.InitFloat("xsq.evc.lv.dcdc.load", SM_STALE_MID, 0, Percentage);
  mt_evc_LV_DCDC_volt           = MyMetrics.InitFloat("xsq.evc.lv.dcdc.volt", SM_STALE_MID, 0, Volts);
  mt_evc_LV_DCDC_power          = MyMetrics.InitFloat("xsq.evc.lv.dcdc.power", SM_STALE_MID, 0, Watts);
  mt_evc_LV_DCDC_state          = MyMetrics.InitInt("xsq.evc.lv.dcdc.state", SM_STALE_MID, 0, Other);
  mt_evc_ext_power              = MyMetrics.InitBool("xsq.evc.ext.power", SM_STALE_MIN, false);
  mt_evc_plug_present           = MyMetrics.InitBool("xsq.evc.plug.present", SM_STALE_MIN, false);

  mt_bms_CV_Range_min           = MyMetrics.InitFloat("xsq.bms.cv.range.min", SM_STALE_MID, 0, Volts);
  mt_bms_CV_Range_max           = MyMetrics.InitFloat("xsq.bms.cv.range.max", SM_STALE_MID, 0, Volts);
  mt_bms_CV_Range_mean          = MyMetrics.InitFloat("xsq.bms.cv.range.mean", SM_STALE_MID, 0, Volts);
  mt_bms_BattLinkVoltage        = MyMetrics.InitFloat("xsq.bms.batt.link.voltage", SM_STALE_MID, 0, Volts);
  mt_bms_BattCV_Sum             = MyMetrics.InitFloat("xsq.bms.batt.cv.sum", SM_STALE_MID, 0, Volts);
  mt_bms_BattPower_voltage      = MyMetrics.InitFloat("xsq.bms.batt.voltage", SM_STALE_MID, 0, Volts);
  mt_bms_BattPower_current      = MyMetrics.InitFloat("xsq.bms.batt.current", SM_STALE_MID, 0, Amps);
  mt_bms_BattPower_power        = MyMetrics.InitFloat("xsq.bms.batt.power", SM_STALE_MID, 0, kW);
  mt_bms_HVcontactState         = MyMetrics.InitInt("xsq.bms.hv.contact.state", SM_STALE_MID, 0, Other);
  mt_bms_HV                     = MyMetrics.InitFloat("xsq.bms.hv", SM_STALE_MID, 0, Volts);
  mt_bms_EVmode                 = MyMetrics.InitInt("xsq.bms.ev.mode", SM_STALE_MID, 0, Other);
  mt_bms_LV                     = MyMetrics.InitFloat("xsq.bms.lv", SM_STALE_MID, 0, Volts);
  mt_bms_Amps                   = MyMetrics.InitFloat("xsq.bms.amps", SM_STALE_MID, 0, Amps);
  mt_bms_Amps2                  = MyMetrics.InitFloat("xsq.bms.amp2", SM_STALE_MID, 0, Amps);
  mt_bms_Power                  = MyMetrics.InitFloat("xsq.bms.power", SM_STALE_MID, 0, kW);

  mt_obl_fastchg                = MyMetrics.InitBool("xsq.obl.fastchg", SM_STALE_MIN, false);
  mt_obl_main_volts             = new OvmsMetricVector<float>("xsq.obl.volts", SM_STALE_HIGH, Volts);
  mt_obl_main_amps              = new OvmsMetricVector<float>("xsq.obl.amps", SM_STALE_HIGH, Amps);
  mt_obl_main_CHGpower          = new OvmsMetricVector<float>("xsq.obl.power", SM_STALE_HIGH, kW);
  mt_obl_main_freq              = MyMetrics.InitFloat("xsq.obl.freq", SM_STALE_MID, 0, Other);
  
  RegisterCanBus(1, CAN_MODE_ACTIVE, CAN_SPEED_500KBPS);

  MyConfig.RegisterParam("xsq", "Smart EQ", true, true);
  ConfigChanged(NULL);

  StdMetrics.ms_v_gen_current->SetValue(2);                // activate gen metrics to app transfer
  StdMetrics.ms_v_bat_12v_voltage_alert->SetValue(false);  // set 12V alert to false
  
  if (MyConfig.GetParamValue("password", "pin","0") == "0") {
    MyConfig.SetParamValueInt("password", "pin", 1234);           // set default pin
  }

  if (MyConfig.GetParamValue("xsq", "12v.charge","0") == "0") {
    MyConfig.SetParamValueBool("xsq", "12v.charge", true);
  }

  if (MyConfig.GetParamValue("xsq", "v2.check","0") == "0") {
    MyConfig.SetParamValueBool("xsq", "v2.check", false);
  }

  if (MyConfig.GetParamValue("xsq", "ddt4all","0") == "0") {
    MyConfig.SetParamValueBool("xsq", "ddt4all", false);
  }

  if(MyConfig.GetParamValue("xsq", "booster.system","0") == "0") {
    MyConfig.SetParamValueBool("xsq", "booster.system", true);
    StdMetrics.ms_v_gen_current->SetValue(3);                  // activate gen metrics to app transfer and in-app plugin <> fw switch                                   
  }
  
  if (mt_pos_odometer_trip_total->AsFloat(0) < 1.0f) {              // reset at boot
    ResetTotalCounters();
    ResetTripCounters();
  }
  ResetOldValues();                                                // removed old values from config usr/xsq
  TimeBasedClimateData();                                          // set default booster data from App values
  CommandWakeup();                                                 // wake up the car to get the first data

#ifdef CONFIG_OVMS_COMP_CELLULAR
  if(MyConfig.GetParamValue("xsq", "gps.onoff","0") == "0") {
    MyConfig.SetParamValueBool("xsq", "gps.onoff", true);
    MyConfig.SetParamValueInt("xsq", "gps.reactmin", 50);
  }
#endif
#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebInit();
#endif
}

OvmsVehicleSmartEQ::~OvmsVehicleSmartEQ() {
  ESP_LOGI(TAG, "Stop Smart EQ vehicle module");

#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebDeInit();
#endif
#ifdef CONFIG_OVMS_COMP_MAX7317
  if (m_enable_LED_state) {
    MyPeripherals->m_max7317->Output(9, 0);
    MyPeripherals->m_max7317->Output(8, 1);
    MyPeripherals->m_max7317->Output(7, 1);
  }
#endif
}

void OvmsVehicleSmartEQ::ObdModifyPoll() {
  PollSetPidList(m_can1, NULL);
  PollSetState(0);
  PollSetThrottling(0);
  PollSetResponseSeparationTime(20);

  // modify Poller..
  m_poll_vector.clear();
  // Add PIDs to poll list:
  m_poll_vector.insert(m_poll_vector.end(), obdii_polls, endof_array(obdii_polls));
  if (mt_obl_fastchg->AsBool()) {
    m_poll_vector.insert(m_poll_vector.end(), fast_charger_polls, endof_array(fast_charger_polls));
  } else {
    m_poll_vector.insert(m_poll_vector.end(), slow_charger_polls, endof_array(slow_charger_polls));
  }
  OvmsPoller::poll_pid_t p1 = { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x41, {  0,300,0,0 }, 0, ISOTP_STD };
  p1.polltime[2] = m_cfg_cell_interval_drv;
  p1.polltime[3] = m_cfg_cell_interval_chg;
  m_poll_vector.push_back(p1);
  
  OvmsPoller::poll_pid_t p2 = { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x42, {  0,300,0,0 }, 0, ISOTP_STD };
  p2.polltime[2] = m_cfg_cell_interval_drv;
  p2.polltime[3] = m_cfg_cell_interval_chg;
  m_poll_vector.push_back(p2);

  // Terminate poll list:
  m_poll_vector.push_back(POLL_LIST_END);
  ESP_LOGI(TAG, "Poll vector: size=%d cap=%d", m_poll_vector.size(), m_poll_vector.capacity());

  PollSetPidList(m_can1, m_poll_vector.data());
}

/**
 * ConfigChanged: reload single/all configuration variables (cfgupdate)
 */
void OvmsVehicleSmartEQ::ConfigChanged(OvmsConfigParam* param) {
  if (param && param->GetName() != "xsq")
    return;

  ESP_LOGI(TAG, "Smart EQ reload configuration");

  m_enable_write      = MyConfig.GetParamValueBool("xsq", "canwrite", false);
  m_enable_LED_state  = MyConfig.GetParamValueBool("xsq", "led", false);
  m_ios_tpms_fix      = MyConfig.GetParamValueBool("xsq", "ios_tpms_fix", false);
  m_reboot_time       = MyConfig.GetParamValueInt("xsq", "rebootnw", 30);
  m_resettrip         = MyConfig.GetParamValueBool("xsq", "resettrip", false);
  m_resettotal        = MyConfig.GetParamValueBool("xsq", "resettotal", false);

  m_TPMS_FL           = MyConfig.GetParamValueInt("xsq", "TPMS_FL", 0);
  m_TPMS_FR           = MyConfig.GetParamValueInt("xsq", "TPMS_FR", 1);
  m_TPMS_RL           = MyConfig.GetParamValueInt("xsq", "TPMS_RL", 2);
  m_TPMS_RR           = MyConfig.GetParamValueInt("xsq", "TPMS_RR", 3);
  
  m_ddt4all           = MyConfig.GetParamValueInt("xsq", "ddt4all", false);
  m_12v_charge        = MyConfig.GetParamValueBool("xsq", "12v.charge", true);
  m_v2_check          = MyConfig.GetParamValueBool("xsq", "v2.check", false);
  m_booster_system    = MyConfig.GetParamValueBool("xsq", "booster.system", true);
  m_gps_onoff         = MyConfig.GetParamValueBool("xsq", "gps.onoff", true);
  m_gps_reactmin      = MyConfig.GetParamValueInt("xsq", "gps.reactmin", 50);
  m_network_type      = MyConfig.GetParamValue("xsq", "modem.net.type", "auto");
  m_indicator         = MyConfig.GetParamValueBool("xsq", "indicator", false);              //!< activate indicator e.g. 7 times or whtever

  // make older System/Plugin Booster data compatible
  if(MyConfig.GetParamValue("xsq", "booster.data", "0,0,0,0,-1,-1,-1") != "0,0,0,0,-1,-1,-1") {
    mt_booster_data->SetValue(MyConfig.GetParamValue("xsq", "booster.data", "0,0,0,0,-1,-1,-1"));
    MyConfig.SetParamValue("xsq", "booster.data", "0,0,0,0,-1,-1,-1");
  }
  if(MyConfig.GetParamValue("usr", "b.data", "0,0,0,0,-1,-1,-1") != "0,0,0,0,-1,-1,-1") {
    mt_booster_data->SetValue(MyConfig.GetParamValue("usr", "b.data", "0,0,0,0,-1,-1,-1"));
    MyConfig.SetParamValue("usr", "b.data", "0,0,0,0,-1,-1,-1");
  }

#ifdef CONFIG_OVMS_COMP_MAX7317
  if (!m_enable_LED_state) {
    MyPeripherals->m_max7317->Output(9, 1);
    MyPeripherals->m_max7317->Output(8, 1);
    MyPeripherals->m_max7317->Output(7, 1);
  }
#endif
  int cell_interval_drv = MyConfig.GetParamValueInt("xsq", "cell_interval_drv", 60);
  int cell_interval_chg = MyConfig.GetParamValueInt("xsq", "cell_interval_chg", 60);

  bool do_modify_poll = (
    (cell_interval_drv != m_cfg_cell_interval_drv) ||
    (cell_interval_chg != m_cfg_cell_interval_chg));

  m_cfg_cell_interval_drv = cell_interval_drv;
  m_cfg_cell_interval_chg = cell_interval_chg;

  if (do_modify_poll) {
    ObdModifyPoll();
  }
  StdMetrics.ms_v_charge_limit_soc->SetValue((float) MyConfig.GetParamValueInt("xsq", "suffsoc", 0), Percentage );
  StdMetrics.ms_v_charge_limit_range->SetValue((float) MyConfig.GetParamValueInt("xsq", "suffrange", 0), Kilometers );
}

uint64_t OvmsVehicleSmartEQ::swap_uint64(uint64_t val) {
  val = ((val << 8) & 0xFF00FF00FF00FF00ull) | ((val >> 8) & 0x00FF00FF00FF00FFull);
  val = ((val << 16) & 0xFFFF0000FFFF0000ull) | ((val >> 16) & 0x0000FFFF0000FFFFull);
  return (val << 32) | (val >> 32);
}

void OvmsVehicleSmartEQ::IncomingFrameCan1(CAN_frame_t* p_frame) {
  uint8_t *data = p_frame->data.u8;
  uint64_t c = swap_uint64(p_frame->data.u64);
  
  static bool isCharging = false;
  static bool lastCharging = false;
  float _range_est;
  float _bat_temp;
  float _full_km;
  float _range_cac;
  float _soc;
  float _temp;
  int _duration_full;
  //char buf[10];

  if (m_candata_poll != 1 && StdMetrics.ms_v_bat_voltage->AsFloat(0, Volts) > 100) {
    ESP_LOGI(TAG,"Car has woken (CAN bus activity)");
    mt_bus_awake->SetValue(true);
    m_candata_poll = 1;
  }
  m_candata_timer = SQ_CANDATA_TIMEOUT;
  
  switch (p_frame->MsgID) {
    case 0x17e: //gear shift
    {
      switch(CAN_BYTE(6)) {
        case 0x00: // Parking
          StdMetrics.ms_v_env_gear->SetValue(0);
          StdMetrics.ms_v_gen_limit_soc->SetValue(1);
          break;
        case 0x10: // Rear
          StdMetrics.ms_v_env_gear->SetValue(-1);
          StdMetrics.ms_v_gen_limit_soc->SetValue(2);
          break;
        case 0x20: // Neutral
          StdMetrics.ms_v_env_gear->SetValue(1);
          StdMetrics.ms_v_gen_limit_soc->SetValue(3);
          break;
        case 0x70: // Drive
          StdMetrics.ms_v_env_gear->SetValue(2);
          StdMetrics.ms_v_gen_limit_soc->SetValue(4);
          break;
      }
      break;
    }
    case 0x350:
      StdMetrics.ms_v_env_locked->SetValue((CAN_BYTE(6) == 0x96));
      break;
    case 0x392:
      StdMetrics.ms_v_env_hvac->SetValue((CAN_BYTE(1) & 0x40) > 0);
      StdMetrics.ms_v_env_cabintemp->SetValue(CAN_BYTE(5) - 40.0f);
      break;
    case 0x42E: // HV Voltage
      _temp = ((c >> 13) & 0x7Fu) > 40.0f ? ((c >> 13) & 0x7Fu) - 40.0f : (40.0f - ((c >> 13) & 0x7Fu)) * -1.0f;
      if(_temp != 87) StdMetrics.ms_v_bat_temp->SetValue(_temp); // HVBatteryTemperature
      StdMetrics.ms_v_bat_voltage->SetValue((float) ((CAN_UINT(3)>>5)&0x3ff) / 2); // HV Voltage
      StdMetrics.ms_v_charge_climit->SetValue((c >> 20) & 0x3Fu); // MaxChargingNegotiatedCurrent
      break;
    case 0x4F8:
      StdMetrics.ms_v_env_handbrake->SetValue((CAN_BYTE(0) & 0x08) > 0);
      StdMetrics.ms_v_env_awake->SetValue((CAN_BYTE(0) & 0x40) > 0); // Ignition on
      break;
    case 0x5D7: // Speed, ODO
      StdMetrics.ms_v_pos_speed->SetValue((float) CAN_UINT(0) / 100.0f);
      StdMetrics.ms_v_pos_odometer->SetValue((float) (CAN_UINT32(2)>>4) / 100.0f);
      break;
    case 0x5de:
      StdMetrics.ms_v_env_headlights->SetValue((CAN_BYTE(0) & 0x04) > 0);
      StdMetrics.ms_v_door_fl->SetValue((CAN_BYTE(1) & 0x08) > 0);
      StdMetrics.ms_v_door_fr->SetValue((CAN_BYTE(1) & 0x02) > 0);
      StdMetrics.ms_v_door_rl->SetValue((CAN_BYTE(2) & 0x40) > 0);
      StdMetrics.ms_v_door_rr->SetValue((CAN_BYTE(2) & 0x10) > 0);
      StdMetrics.ms_v_door_trunk->SetValue((CAN_BYTE(7) & 0x10) > 0);
      break;
    case 0x646:
      mt_use_at_reset->SetValue(CAN_BYTE(1) * 0.1);
      if( MyConfig.GetParamValueBool("xsq", "bcvalue",  false)){
        StdMetrics.ms_v_gen_kwh_grid_total->SetValue(mt_use_at_reset->AsFloat()); // not the best idea at the moment
      } else {
        StdMetrics.ms_v_gen_kwh_grid_total->SetValue(0.0f);
      }
      break;
    case 0x654: // SOC(b)
      StdMetrics.ms_v_bat_soc->SetValue(CAN_BYTE(3));
      StdMetrics.ms_v_door_chargeport->SetValue((CAN_BYTE(0) & 0x20)); // ChargingPlugConnected
      _duration_full = (((c >> 22) & 0x3ffu) < 0x3ff) ? (c >> 22) & 0x3ffu : 0;
      mt_ocs_duration->SetValue((int)(_duration_full), Minutes);
      _range_est = ((c >> 12) & 0x3FFu); // VehicleAutonomy
      _bat_temp = StdMetrics.ms_v_bat_temp->AsFloat(0) - 20.0;
      _full_km = MyConfig.GetParamValueFloat("xsq", "full.km", 126.0);
      _range_cac = _full_km + (_bat_temp); // temperature compensation +/- range
      _soc = StdMetrics.ms_v_bat_soc->AsFloat();
      if ( _range_est != 1023.0 ) {
        StdMetrics.ms_v_bat_range_est->SetValue(_range_est); // VehicleAutonomy
        StdMetrics.ms_v_bat_range_full->SetValue((_range_est / _soc) * 100.0); // ToDo
        StdMetrics.ms_v_bat_range_ideal->SetValue((_range_cac * _soc) / 100.0); // ToDo  // try variable calculated range: est - (20 - (BATTtemp))
      }
      break;
    case 0x658: //
      StdMetrics.ms_v_bat_soh->SetValue(CAN_BYTE(4) & 0x7Fu); // SOH ?
      isCharging = (CAN_BYTE(5) & 0x20); // ChargeInProgress
      if (isCharging) { // STATE charge in progress
        //StdMetrics.ms_v_charge_inprogress->SetValue(isCharging);
      }
      if (isCharging != lastCharging) { // EVENT charge state changed
        if (isCharging) { // EVENT started charging
          // Set charging metrics
          StdMetrics.ms_v_charge_pilot->SetValue(true);
          StdMetrics.ms_v_charge_inprogress->SetValue(isCharging);
          StdMetrics.ms_v_charge_mode->SetValue("standard");
          StdMetrics.ms_v_charge_type->SetValue("type2");
          StdMetrics.ms_v_charge_state->SetValue("charging");
          StdMetrics.ms_v_charge_substate->SetValue("onrequest");
          StdMetrics.ms_v_charge_timestamp->SetValue(StdMetrics.ms_m_timeutc->AsInt());
        } else { // EVENT stopped charging
          StdMetrics.ms_v_charge_pilot->SetValue(false);
          StdMetrics.ms_v_charge_inprogress->SetValue(isCharging);
          StdMetrics.ms_v_charge_mode->SetValue("standard");
          StdMetrics.ms_v_charge_type->SetValue("type2");
          StdMetrics.ms_v_charge_duration_full->SetValue(0);
          StdMetrics.ms_v_charge_duration_soc->SetValue(0);
          StdMetrics.ms_v_charge_duration_range->SetValue(0);
          StdMetrics.ms_v_charge_power->SetValue(0);
          StdMetrics.ms_v_charge_timestamp->SetValue(StdMetrics.ms_m_timeutc->AsInt());
          if (StdMetrics.ms_v_bat_soc->AsInt() < 95) {
            // Assume the charge was interrupted
            ESP_LOGI(TAG,"Car charge session was interrupted");
            StdMetrics.ms_v_charge_state->SetValue("stopped");
            StdMetrics.ms_v_charge_substate->SetValue("interrupted");
          } else {
            // Assume the charge completed normally
            ESP_LOGI(TAG,"Car charge session completed");
            StdMetrics.ms_v_charge_state->SetValue("done");
            StdMetrics.ms_v_charge_substate->SetValue("onrequest");
          }
        }
      }
      lastCharging = isCharging;
      break;
    case 0x668:
      vehicle_smart_car_on((CAN_BYTE(0) & 0x40) > 0); // Drive Ready
      break;
    case 0x673:
      if (CAN_BYTE(2) != 0xff)
        StdMetrics.ms_v_tpms_pressure->SetElemValue(m_TPMS_RR, (float) CAN_BYTE(2)*3.1);
      if (CAN_BYTE(3) != 0xff)
        StdMetrics.ms_v_tpms_pressure->SetElemValue(m_TPMS_RL, (float) CAN_BYTE(3)*3.1);
      if (CAN_BYTE(4) != 0xff)
        StdMetrics.ms_v_tpms_pressure->SetElemValue(m_TPMS_FR, (float) CAN_BYTE(4)*3.1);
      if (CAN_BYTE(5) != 0xff)
        StdMetrics.ms_v_tpms_pressure->SetElemValue(m_TPMS_FL, (float) CAN_BYTE(5)*3.1);
      break;
    default:
      //ESP_LOGD(TAG, "IFC %03x 8 %02x %02x %02x %02x %02x %02x %02x %02x", p_frame->MsgID, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
      break;
  }
}

void OvmsVehicleSmartEQ::DisablePlugin(const char* plugin) {
  #ifdef CONFIG_OVMS_COMP_PLUGINS
      if (!ExecuteCommand("plugin disable " + std::string(plugin))) {
          ESP_LOGE(TAG, "Failed to disable plugin: %s", plugin);
          return;
      }
      if (!ExecuteCommand("vfs rm /store/plugins/" + std::string(plugin) + "/*.*")) {
          ESP_LOGE(TAG, "Failed to remove plugin files: %s", plugin);
          return;
      }
  #else
      ESP_LOGW(TAG, "Plugin system not enabled");
  #endif
}

bool OvmsVehicleSmartEQ::ExecuteCommand(const std::string& command) {
  
  std::string result = BufferedShell::ExecuteCommand(command, true);
  bool success = !result.empty();  // Consider command successful if output is not empty
  
  if (!success) {
      ESP_LOGE(TAG, "Failed to execute command: %s", command.c_str());
      return Fail;
  }

  return Success;
}

// temporary fix for old system/plugins data
void OvmsVehicleSmartEQ::ResetOldValues() {
  
  if(MyConfig.GetParamValueInt("xsq", "booster.ds",0) > 0) {
    ExecuteCommand("conf rm xsq booster.1to3");
    ExecuteCommand("conf rm xsq booster.de");
    ExecuteCommand("conf rm xsq booster.ds");
    ExecuteCommand("conf rm xsq booster.h");
    ExecuteCommand("conf rm xsq booster.m");
    ExecuteCommand("conf rm xsq booster.on");
    ExecuteCommand("conf rm xsq booster.time");
    ExecuteCommand("conf rm xsq booster.weekly");
    ExecuteCommand("conf rm xsq gps.off");    
  }

  if(MyConfig.GetParamValueBool("usr", "b.init", false)) {
    DisablePlugin("scheduled_booster");                            // set switch off schedule_booster plugin
  }
  if(MyConfig.GetParamValueInt("usr", "b.ticker",0) > 0) {
    ExecuteCommand("conf rm usr b.activated");
    ExecuteCommand("conf rm usr b.day_end");
    ExecuteCommand("conf rm usr b.day_start");
    ExecuteCommand("conf rm usr b.init");
    ExecuteCommand("conf rm usr b.ps_data");
    ExecuteCommand("conf rm usr b.ps_end_day");
    ExecuteCommand("conf rm usr b.ps_scheduled_boost");
    ExecuteCommand("conf rm usr b.ps_scheduled_boost_2");
    ExecuteCommand("conf rm usr b.ps_start_day");
    ExecuteCommand("conf rm usr b.scheduled");
    ExecuteCommand("conf rm usr b.scheduled_2");
    ExecuteCommand("conf rm usr b.ticker");
    ExecuteCommand("conf rm usr b.week");
  }

  if(MyConfig.GetParamValueBool("usr", "gps.init", false)) {
    DisablePlugin("gps_onoff");                                    // set switch off gps plugin
  }
  if(MyConfig.GetParamValueInt("usr", "gps.counter_value",0) > 0) {
    ExecuteCommand("conf rm usr gps.counter");
    ExecuteCommand("conf rm usr gps.counter_add");
    ExecuteCommand("conf rm usr gps.counter_value");
    ExecuteCommand("conf rm usr gps.init");
    ExecuteCommand("conf rm usr gps.on");
    ExecuteCommand("conf rm usr gps.parktime");
    ExecuteCommand("conf rm usr gps.power_switch");
    ExecuteCommand("conf rm usr gps.pubsub");
    ExecuteCommand("conf rm usr gps.ticker");
    ExecuteCommand("conf rm usr 12v.charging");
  }
  
  if(MyConfig.GetParamValueBool("usr", "12v.init", false)) {
    DisablePlugin("booster_12V");                                  // set switch off 12V charge plugin
  }
  if(MyConfig.GetParamValueInt("usr", "12v.counter",0) > 0) {
    ExecuteCommand("conf rm usr 12v.counter");
    ExecuteCommand("conf rm usr 12v.init");
    ExecuteCommand("conf rm usr 12v.ps_alert12v_off");
    ExecuteCommand("conf rm usr 12v.ps_alert12v_on");
    ExecuteCommand("conf rm usr 12v.ps_booster_2");
  }
}

void OvmsVehicleSmartEQ::ResetChargingValues() {
  m_charge_finished = false;
  StdMetrics.ms_v_charge_kwh->SetValue(0);
  //StdMetrics.ms_v_charge_kwh_grid->SetValue(0);
}

void OvmsVehicleSmartEQ::ResetTripCounters() {
  StdMetrics.ms_v_bat_energy_recd->SetValue(0);
  StdMetrics.ms_v_bat_energy_used->SetValue(0);
  mt_pos_odometer_start->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat());
  StdMetrics.ms_v_pos_trip->SetValue(0);
  StdMetrics.ms_v_charge_kwh_grid->SetValue(0);
}

void OvmsVehicleSmartEQ::ResetTotalCounters() {
  StdMetrics.ms_v_bat_energy_recd_total->SetValue(0);
  StdMetrics.ms_v_bat_energy_used_total->SetValue(0);
  mt_pos_odometer_trip_total->SetValue(0);
  mt_pos_odometer_start_total->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat());
  StdMetrics.ms_v_charge_kwh_grid_total->SetValue(0);
  MyConfig.SetParamValueBool("xsq", "resettotal", false);
}

// Task to check the time periodically
void OvmsVehicleSmartEQ::TimeCheckTask() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  // Check if the current time is the booster time
  if (timeinfo.tm_hour == mt_booster_h->AsInt() && timeinfo.tm_min == mt_booster_m->AsInt() && mt_booster_on->AsBool() && !StdMetrics.ms_v_env_hvac->AsBool()) {
    CommandHomelink(mt_booster_1to3->AsInt());
  }

  // Check if the current day is within the booster days range
  int current_day = timeinfo.tm_wday;
  
  if (current_day == mt_booster_ds->AsInt() && mt_booster_weekly->AsBool() && !m_booster_start_day) {
    m_booster_start_day = true;
    mt_booster_on->SetValue(true);
  }
  if (current_day == mt_booster_de->AsInt() && mt_booster_weekly->AsBool() && m_booster_start_day) {
    m_booster_start_day = false;
    mt_booster_on->SetValue(false);
  }
  if ((!mt_booster_weekly->AsBool() || !mt_booster_on->AsBool()) && m_booster_start_day) {
    m_booster_start_day = false;
  }
}

void OvmsVehicleSmartEQ::TimeBasedClimateData() {
  std::string _oldtime = mt_booster_time->AsString();
  std::string _newdata = mt_booster_data->AsString();
  std::vector<int> _data;
  std::stringstream _ss(_newdata);
  std::string _item, _booster_on, _booster_weekly, _booster_time;
  int _booster_ds, _booster_de, _booster_h, _booster_m;

  while (std::getline(_ss, _item, ',')) {
    _data.push_back(atoi(_item.c_str()));
  }

  if(_data[0]>0 || m_booster_init) {
    m_booster_init = false;
    mt_booster_data->SetValue("0,0,0,0,-1,-1,-1");              // reset the data
    _booster_on = _data[1] == 1 ? "yes" : "no";
    _booster_weekly = _data[2] == 1 ? "yes" : "no";
    if(_data[1]>0) { mt_booster_on->SetValue(_data[1] == 1);}
    if(_data[2]>0) { mt_booster_weekly->SetValue(_data[2] == 1);}
    if(_data[4]>-1) { _booster_ds = _data[4] > 6 ? 0 : _data[4]; mt_booster_ds->SetValue(_booster_ds);}
    if(_data[5]>-1) { _booster_de = _data[5] <= 5 ? _data[5]+1 : 0; mt_booster_de->SetValue(_booster_de);}
    if(_data[6]>-1) { mt_booster_1to3->SetValue(_data[6]);}

    if(_data[3]>0) { 
      _booster_h = (_data[3] / 100) % 24;                      // Extract hours and ensure 24h format
      _booster_m = _data[3] % 100;                             // Extract minutes
      if(mt_booster_m->AsInt() >= 60) {                        // Handle invalid minutes
          _booster_h = (_booster_h + _booster_m / 60) % 24;
          _booster_m = mt_booster_m->AsInt() % 60;
      }
      
      if (_data[3] >= 0 && _data[3] <= 2359) {
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << _data[3];
        mt_booster_time->SetValue(oss.str());
      } else {
          ESP_LOGE(TAG, "Invalid time value: %d", _data[3]);
      }
      mt_booster_h->SetValue(_booster_h);
      mt_booster_m->SetValue(_booster_m);
    } else {
      mt_booster_time->SetValue(_oldtime);
    }     
    
    // booster;no;no;0515;1;6;0
    char buf[50];
    sprintf(buf, "booster,%s,%s,%s,%d,%d,%d", _booster_on.c_str(), _booster_weekly.c_str(), mt_booster_time->AsString().c_str(), mt_booster_ds->AsInt(), mt_booster_de->AsInt(), mt_booster_1to3->AsInt());
    StdMetrics.ms_v_gen_mode->SetValue(std::string(buf));
    StdMetrics.ms_v_gen_current->SetValue(3);
  }
}

// check the 12V alert periodically and charge the 12V battery if needed
void OvmsVehicleSmartEQ::Check12vState() {
  static const float MIN_VOLTAGE = 10.0f;
  static const int ALERT_THRESHOLD_TICKS = 10;
  static const int HOMELINK_BUTTON = 2;
  
  float mref = StdMetrics.ms_v_bat_12v_voltage_ref->AsFloat();
  float dref = MyConfig.GetParamValueFloat("vehicle", "12v.ref", 12.6);
  bool alert_on = StdMetrics.ms_v_bat_12v_voltage_alert->AsBool();
  float volt = StdMetrics.ms_v_bat_12v_voltage->AsFloat();
  
  // Validate and update reference voltage
  if (mref > dref) {
      ESP_LOGI(TAG, "Adjusting 12V reference voltage from %.1fV to %.1fV", mref, dref);
      StdMetrics.ms_v_bat_12v_voltage_ref->SetValue(dref);
  }
  
  // Handle alert conditions
  if (alert_on && (volt > MIN_VOLTAGE)) {
      m_12v_ticker++;
      ESP_LOGI(TAG, "12V alert active for %d ticks, voltage: %.1fV", m_12v_ticker, volt);
      
      if (m_12v_ticker > ALERT_THRESHOLD_TICKS) {
          m_12v_ticker = 0;
          ESP_LOGI(TAG, "Initiating climate control due to 12V alert");
          CommandHomelink(HOMELINK_BUTTON);
      }
  } else if (m_12v_ticker > 0) {
      ESP_LOGI(TAG, "12V alert cleared, resetting ticker");
      m_12v_ticker = 0;
  }
}

// switch the GPS on/off depending on the parktime and the bus state
void OvmsVehicleSmartEQ::GPSOnOff() {
  static const int PARK_TIMEOUT_SECS = 600;  // 10 minutes
  static const int INITIAL_DELAY = 10;

  #ifdef CONFIG_OVMS_COMP_CELLULAR
    m_gps_ticker += 1;

    // Power saving: Turn off GPS
    bool should_turn_off = (StdMetrics.ms_v_env_parktime->AsInt() > PARK_TIMEOUT_SECS &&
                          !StdMetrics.ms_v_env_on->AsBool() &&
                          m_gps_onoff &&
                          !m_gps_off &&
                          (m_gps_ticker > INITIAL_DELAY));

    // Reactivation conditions
    bool should_turn_on = ((m_gps_ticker >= m_gps_reactmin) && m_gps_onoff && m_gps_off) ||
                        (mt_bus_awake->AsBool() && m_gps_off);

    if (should_turn_off) {
        ESP_LOGI(TAG, "Turning GPS off - vehicle parked and inactive");
        m_gps_off = true;
        m_gps_ticker = 0;
        if (MyPeripherals && MyPeripherals->m_cellular_modem) {
            MyPeripherals->m_cellular_modem->StopNMEA();
        }
    } else if (should_turn_on) {
        ESP_LOGI(TAG, "Turning GPS on - timer/bus wake condition");
        m_gps_off = false;
        m_gps_ticker = 0;
        if (MyPeripherals && MyPeripherals->m_cellular_modem) {
            MyPeripherals->m_cellular_modem->StartNMEA();
        }
    }
  #else
      ESP_LOGD(TAG, "GPS control disabled - cellular modem support not enabled");
  #endif // CONFIG_OVMS_COMP_CELLULAR
}

// check the Server V2 connection and reboot the network if needed
void OvmsVehicleSmartEQ::CheckV2State() {
  static const int DISCONNECT_THRESHOLD = 5;
  static const int RESTART_DELAY = 1;
  
  bool is_connected = StdMetrics.ms_s_v2_connected->AsBool();
  
  if (!is_connected) {
      m_v2_ticker++;
      
      if (m_v2_ticker > DISCONNECT_THRESHOLD && !m_v2_restart) {
          ESP_LOGI(TAG, "V2 server disconnected for %d ticks, initiating restart", m_v2_ticker);
          m_v2_restart = true;
          if (!ExecuteCommand("server v2 stop")) {
              ESP_LOGE(TAG, "Failed to stop V2 server");
              return;
          }
      }
      
      if (m_v2_ticker > (DISCONNECT_THRESHOLD + RESTART_DELAY) && m_v2_restart) {
          ESP_LOGI(TAG, "Restarting V2 server");
          m_v2_ticker = 0;
          m_v2_restart = false;
          if (!ExecuteCommand("server v2 start")) {
              ESP_LOGE(TAG, "Failed to start V2 server");
          }
      }
  } else if (m_v2_ticker > 0) {
      ESP_LOGI(TAG, "V2 server connection restored");
      m_v2_ticker = 0;
      m_v2_restart = false;
  }
}

// Cellular Modem Network type switch
void OvmsVehicleSmartEQ::ModemNetworkType() {
  if (m_network_type == "auto") {
      ExecuteCommand("cellular cmd AT+COPS=0");                     // set network to prefered Telekom.de LTE or the best available
  } 
  if (m_network_type == "gsm") {
      ExecuteCommand("cellular cmd AT+CNMP=2");                     // set network to GSM/3G/LTE
  }
  if (m_network_type == "lte") {
      ExecuteCommand("cellular cmd AT+CNMP=38");                    // set network to LTE only
  }
  m_network_type_ls = m_network_type;
}

/**
 * Update derived energy metrics while driving
 * Called once per second from Ticker1
 */
void OvmsVehicleSmartEQ::HandleEnergy() {
  float voltage  = StdMetrics.ms_v_bat_voltage->AsFloat(0, Volts);
  float current  = -StdMetrics.ms_v_bat_current->AsFloat(0, Amps);

  // Power (in kw) resulting from voltage and current
  float power = voltage * current / 1000.0;

  // Are we driving?
  if (power != 0.0 && StdMetrics.ms_v_env_on->AsBool()) {
    // Update energy used and recovered
    float energy = power / 3600.0;    // 1 second worth of energy in kwh's
    if (energy < 0.0f){
      StdMetrics.ms_v_bat_energy_used->SetValue( StdMetrics.ms_v_bat_energy_used->AsFloat() - energy);
      StdMetrics.ms_v_bat_energy_used_total->SetValue( StdMetrics.ms_v_bat_energy_used_total->AsFloat() - energy);
    }else{ // (energy > 0.0f)
      StdMetrics.ms_v_bat_energy_recd->SetValue( StdMetrics.ms_v_bat_energy_recd->AsFloat() + energy);
      StdMetrics.ms_v_bat_energy_recd_total->SetValue( StdMetrics.ms_v_bat_energy_recd_total->AsFloat() + energy);
    }
  }
}

void OvmsVehicleSmartEQ::HandleTripcounter(){
  if (mt_pos_odometer_start->AsFloat(0) == 0 && StdMetrics.ms_v_pos_odometer->AsFloat(0) > 0.0) {
    mt_pos_odometer_start->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat());
  }
  if (StdMetrics.ms_v_env_on->AsBool() && StdMetrics.ms_v_pos_odometer->AsFloat(0) > 0.0 && mt_pos_odometer_start->AsFloat(0) > 0.0) {
    StdMetrics.ms_v_pos_trip->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat(0) - mt_pos_odometer_start->AsFloat(0));
  }

  if (mt_pos_odometer_start_total->AsFloat(0) == 0 && StdMetrics.ms_v_pos_odometer->AsFloat(0) > 0.0) {
    mt_pos_odometer_start_total->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat());
  }
  if (StdMetrics.ms_v_env_on->AsBool() && StdMetrics.ms_v_pos_odometer->AsFloat(0) > 0.0 && mt_pos_odometer_start_total->AsFloat(0) > 0.0) {
    mt_pos_odometer_trip_total->SetValue(StdMetrics.ms_v_pos_odometer->AsFloat(0) - mt_pos_odometer_start_total->AsFloat(0));
  }
}

void OvmsVehicleSmartEQ::Handlev2Server(){
  // Handle v2Server connection
  if (StdMetrics.ms_s_v2_connected->AsBool()) {
    m_reboot_ticker = m_reboot_time * 60; // set reboot ticker
  }
  else if (m_reboot_ticker > 0 && --m_reboot_ticker == 0) {
    MyNetManager.RestartNetwork();
    m_reboot_ticker = m_reboot_time * 60;
  }
}

/**
 * Update derived metrics when charging
 * Called once per 10 seconds from Ticker10
 */
void OvmsVehicleSmartEQ::HandleCharging() {
  float limit_soc       = StdMetrics.ms_v_charge_limit_soc->AsFloat(0);
  float limit_range     = StdMetrics.ms_v_charge_limit_range->AsFloat(0, Kilometers);
  float max_range       = StdMetrics.ms_v_bat_range_full->AsFloat(0, Kilometers);
  float charge_current  = -StdMetrics.ms_v_bat_current->AsFloat(0, Amps);
  float charge_voltage  = StdMetrics.ms_v_bat_voltage->AsFloat(0, Volts);

  // Are we charging?
  if (!StdMetrics.ms_v_charge_pilot->AsBool()      ||
      !StdMetrics.ms_v_charge_inprogress->AsBool() ||
      (charge_current <= 0.0) ) {
    return;
  }

  // Check if we have what is needed to calculate energy and remaining minutes
  if (charge_voltage > 0 && charge_current > 0) {
    // Update energy taken
    // Value is reset to 0 when a new charging session starts...
    float power  = charge_voltage * charge_current / 1000.0f;     // power in kw
    float energy = power / 3600.0f * 1.0f;                         // 1 second worth of energy in kwh's
    StdMetrics.ms_v_charge_kwh->SetValue( StdMetrics.ms_v_charge_kwh->AsFloat() + energy);

    // Calculate remaining time to full charge
    if(StdMetrics.ms_v_charge_power->AsFloat() > 12.0f){
      StdMetrics.ms_v_charge_duration_full->SetValue(mt_ocs_duration->AsInt(), Minutes);
      ESP_LOGV(TAG, "Time remaining: %d mins to 100%% soc", mt_ocs_duration->AsInt());
    } else {
      float soc100 = 100.0f;
      int remaining_soc = calcMinutesRemaining(soc100, charge_voltage, charge_current);
      StdMetrics.ms_v_charge_duration_full->SetValue(remaining_soc, Minutes);
      ESP_LOGV(TAG, "Time remaining: %d mins to %0.0f%% soc", remaining_soc, soc100);
    }
    if (limit_soc > 0) {
      // if limit_soc is set, then calculate remaining time to limit_soc
      int minsremaining_soc = calcMinutesRemaining(limit_soc, charge_voltage, charge_current);

      StdMetrics.ms_v_charge_duration_soc->SetValue(minsremaining_soc, Minutes);
      ESP_LOGV(TAG, "Time remaining: %d mins to %0.0f%% soc", minsremaining_soc, limit_soc);
    }
    if (limit_range > 0 && max_range > 0.0f) {
      // if range limit is set, then compute required soc and then calculate remaining time to that soc
      float range_soc           = limit_range / max_range * 100.0f;
      int   minsremaining_range = calcMinutesRemaining(range_soc, charge_voltage, charge_current);

      StdMetrics.ms_v_charge_duration_range->SetValue(minsremaining_range, Minutes);
      ESP_LOGV(TAG, "Time remaining: %d mins for %0.0f km (%0.0f%% soc)", minsremaining_range, limit_range, range_soc);
    }
  }
}

void OvmsVehicleSmartEQ::HandleChargeport(){
  if (StdMetrics.ms_v_door_chargeport->AsBool() && !m_charge_start) {
    m_charge_start = true;
    if (m_charge_finished) ResetChargingValues();
    if (m_resettrip) ResetTripCounters();
    ESP_LOGD(TAG,"Charge Start");
  } 
  if (!StdMetrics.ms_v_door_chargeport->AsBool() && m_charge_start) {
    m_charge_start = false;
    m_charge_finished = true;
    StdMetrics.ms_v_charge_power->SetValue(0);
    ESP_LOGD(TAG,"Charge End");
  }
}

void OvmsVehicleSmartEQ::UpdateChargeMetrics() {
  int phasecnt = 0, i = 0, n = 0;
  float voltagesum = 0, ampsum = 0;
  
  for(i = 0; i < 3; i++) {
    if (mt_obl_main_volts->GetElemValue(i) > 120) {
      phasecnt++;
      n = i;
      voltagesum += mt_obl_main_volts->GetElemValue(i);
    }
  }
  if (phasecnt > 1) {
    voltagesum /= phasecnt;
  }
  StdMetrics.ms_v_charge_voltage->SetValue(voltagesum);

  if( phasecnt == 1 && mt_obl_fastchg->AsBool() ) {
    StdMetrics.ms_v_charge_current->SetValue(mt_obl_main_amps->GetElemValue(n));
    } else if( phasecnt == 3 && mt_obl_fastchg->AsBool() ) {
    for(i = 0; i < 3; i++) {
      if (mt_obl_main_amps->GetElemValue(i) > 0) {
        ampsum += mt_obl_main_amps->GetElemValue(i);
      }
    }
    StdMetrics.ms_v_charge_current->SetValue(ampsum/3);
  } else {
    for(i = 0; i < 3; i++) {
      if (mt_obl_main_amps->GetElemValue(i) > 0) {
        ampsum += mt_obl_main_amps->GetElemValue(i);
      }
    }
    StdMetrics.ms_v_charge_current->SetValue(ampsum);
  }

  StdMetrics.ms_v_charge_power->SetValue( mt_obl_main_CHGpower->GetElemValue(0) + mt_obl_main_CHGpower->GetElemValue(1) );
  float power = StdMetrics.ms_v_charge_power->AsFloat();
  float efficiency = (power == 0)
                     ? 0
                     : (StdMetrics.ms_v_bat_power->AsFloat() / power) * 100;
  StdMetrics.ms_v_charge_efficiency->SetValue(efficiency);
  ESP_LOGD(TAG, "SmartEQ_CHG_EFF_STD=%f", efficiency);
}

/**
 * Calculates minutes remaining before target is reached. Based on current charge speed.
 * TODO: Should be calculated based on actual charge curve. Maybe in a later version?
 */
int OvmsVehicleSmartEQ::calcMinutesRemaining(float target_soc, float charge_voltage, float charge_current) {
  float bat_soc = StdMetrics.ms_v_bat_soc->AsFloat(100);
  if (bat_soc > target_soc) {
    return 0;   // Done!
  }
  float remaining_wh    = DEFAULT_BATTERY_CAPACITY * (target_soc - bat_soc) / 100.0;
  float remaining_hours = remaining_wh / (charge_current * charge_voltage);
  float remaining_mins  = remaining_hours * 60.0;

  return MIN( 1440, (int)remaining_mins );
}

void OvmsVehicleSmartEQ::HandlePollState() {
  if ( StdMetrics.ms_v_charge_pilot->AsBool() && m_poll_state != 3 && m_enable_write ) {
    PollSetState(3);
    ESP_LOGI(TAG,"Pollstate Charging");
  }
  else if ( !StdMetrics.ms_v_charge_pilot->AsBool() && StdMetrics.ms_v_env_on->AsBool() && m_poll_state != 2 && m_enable_write ) {
    PollSetState(2);
    ESP_LOGI(TAG,"Pollstate Running");
  }
  else if ( !StdMetrics.ms_v_charge_pilot->AsBool() && !StdMetrics.ms_v_env_on->AsBool() && mt_bus_awake->AsBool() && m_poll_state != 1 && m_enable_write ) {
    PollSetState(1);
    ESP_LOGI(TAG,"Pollstate Awake");
  }
  else if ( !mt_bus_awake->AsBool() && m_poll_state != 0 ) {
    PollSetState(0);
    ESP_LOGI(TAG,"Pollstate Off");
  }
}

void OvmsVehicleSmartEQ::CalculateEfficiency() {
  // float consumption = 0;
  if (StdMetrics.ms_v_pos_speed->AsFloat() >= 5) {
    StdMetrics.ms_v_charge_kwh_grid->SetValue(((StdMetrics.ms_v_bat_energy_used->AsFloat() - StdMetrics.ms_v_bat_energy_recd->AsFloat()) / StdMetrics.ms_v_pos_trip->AsFloat()) * 100.0);
    StdMetrics.ms_v_charge_kwh_grid_total->SetValue(((StdMetrics.ms_v_bat_energy_used_total->AsFloat() - StdMetrics.ms_v_bat_energy_recd_total->AsFloat()) / mt_pos_odometer_trip_total->AsFloat()) * 100.0);
    StdMetrics.ms_v_bat_consumption->SetValue(mt_use_at_reset->AsFloat() * 10.0);
  }
}

void OvmsVehicleSmartEQ::OnlineState() {
#ifdef CONFIG_OVMS_COMP_MAX7317
  if (StdMetrics.ms_m_net_ip->AsBool()) {
    // connected:
    if (StdMetrics.ms_s_v2_connected->AsBool()) {
      if (m_led_state != 1) {
        MyPeripherals->m_max7317->Output(9, 1);
        MyPeripherals->m_max7317->Output(8, 0);
        MyPeripherals->m_max7317->Output(7, 1);
        m_led_state = 1;
        ESP_LOGI(TAG,"LED GREEN");
      }
    } else if (StdMetrics.ms_m_net_connected->AsBool()) {
      if (m_led_state != 2) {
        MyPeripherals->m_max7317->Output(9, 1);
        MyPeripherals->m_max7317->Output(8, 1);
        MyPeripherals->m_max7317->Output(7, 0);
        m_led_state = 2;
        ESP_LOGI(TAG,"LED BLUE");
      }
    } else {
      if (m_led_state != 3) {
        MyPeripherals->m_max7317->Output(9, 0);
        MyPeripherals->m_max7317->Output(8, 1);
        MyPeripherals->m_max7317->Output(7, 1);
        m_led_state = 3;
        ESP_LOGI(TAG,"LED RED");
      }
    }
  }
  else if (m_led_state != 0) {
    // not connected:
    MyPeripherals->m_max7317->Output(9, 1);
    MyPeripherals->m_max7317->Output(8, 1);
    MyPeripherals->m_max7317->Output(7, 1);
    m_led_state = 0;
    ESP_LOGI(TAG,"LED Off");
  }
#endif
}

void OvmsVehicleSmartEQ::vehicle_smart_car_on(bool isOn) {
  if (isOn && !StdMetrics.ms_v_env_on->AsBool()) {
    // Log once that car is being turned on
    ESP_LOGI(TAG,"CAR IS ON");
    //StdMetrics.ms_v_env_awake->SetValue(isOn);

    // Reset trip values
    if (!m_resettrip) {
      ResetTripCounters();
    }
    // Reset kWh/100km values
    if (m_resettotal) {
      ResetTotalCounters();
    }

    #ifdef CONFIG_OVMS_COMP_CELLULAR
      if (m_gps_off) {
        m_gps_off = false;
        m_gps_ticker = 0;
        MyPeripherals->m_cellular_modem->StartNMEA();
      }
      m_12v_ticker = 0;
      m_booster_ticker = 0;
    #endif
  }
  else if (!isOn && StdMetrics.ms_v_env_on->AsBool()) {
    // Log once that car is being turned off
    ESP_LOGI(TAG,"CAR IS OFF");
    //StdMetrics.ms_v_env_awake->SetValue(isOn);
  }

  // Always set this value to prevent it from going stale
  StdMetrics.ms_v_env_on->SetValue(isOn);
}

void OvmsVehicleSmartEQ::Ticker1(uint32_t ticker) {
  if (m_candata_timer > 0) {
    if (--m_candata_timer == 0) {
      // Car has gone to sleep
      ESP_LOGI(TAG,"Car has gone to sleep (CAN bus timeout)");
      mt_bus_awake->SetValue(false);
      m_candata_poll = 0;
      // PollSetState(0);
    }
  }

  // Booster start 2-3 times when Homelink 2 or 3
  if (m_booster_ticker >= 1 && !StdMetrics.ms_v_env_hvac->AsBool() && !m_booster_start) {
    CommandClimateControl(true);
  }
  if (m_booster_start && StdMetrics.ms_v_env_hvac->AsBool()) {
    m_booster_start = false;
    MyNotify.NotifyString("info", "hvac.enabled", "Booster on");
    if (m_booster_ticker >= 1) { 
      --m_booster_ticker;
      ESP_LOGI(TAG,"Booster ticker: %d", m_booster_ticker);
    }
  }

  HandleEnergy();
  HandleCharging();
  HandleTripcounter();
  Handlev2Server();
  HandleChargeport();

  if (m_enable_LED_state) OnlineState();

  if (ticker % 60 == 0) { // Every 60 seconds
    if(mt_booster_on->AsBool()) TimeCheckTask();
    if(m_12v_charge) Check12vState();
    if(m_gps_onoff) GPSOnOff();
    if(m_v2_check) CheckV2State();
  }
  if (ticker % 10 == 0) { // Every 10 seconds
    if(m_booster_system) TimeBasedClimateData();
    if(m_network_type != m_network_type_ls) ModemNetworkType();
  }
}

/**
 * PollerStateTicker: check for state changes
 *  This is called by VehicleTicker1() just before the next PollerSend().
 */
void OvmsVehicleSmartEQ::PollerStateTicker(canbus *bus) {
  bool car_online = mt_bus_awake->AsBool();
  bool lv_pwrstate = (StdMetrics.ms_v_bat_12v_voltage->AsFloat(0) > 12.8);
  
  // - base system is awake if we've got a fresh lv_pwrstate:
  StdMetrics.ms_v_env_aux12v->SetValue(car_online);

  // - charging / trickle charging 12V battery is active when lv_pwrstate is true:
  StdMetrics.ms_v_env_charging12v->SetValue(car_online && lv_pwrstate);
  
  HandlePollState();
}

// can can1 tx st 634 40 01 72 00
OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandClimateControl(bool enable) {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandClimateControl failed / no write access");
    return Fail;
  }
  if (StdMetrics.ms_v_env_hvac->AsBool()) {
    MyNotify.NotifyString("info", "hvac.enabled", "Booster already on");
    ESP_LOGI(TAG, "CommandClimateControl already on");
    return Success;
  }
  ESP_LOGI(TAG, "CommandClimateControl %s", enable ? "ON" : "OFF");

  OvmsVehicle::vehicle_command_t res;

  if (enable) {
    uint8_t data[4] = {0x40, 0x01, 0x00, 0x00};
    canbus *obd;
    obd = m_can1;

    res = CommandWakeup();
    if (res == Success) {
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      for (int i = 0; i < 10; i++) {
        obd->WriteStandard(0x634, 4, data);
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      m_booster_start = true;      
      res = Success;
    } else {
      res = Fail;
    }
  } else {
    res = NotImplemented;
  }

  // fallback to default implementation?
  if (res == NotImplemented) {
    res = OvmsVehicle::CommandClimateControl(enable);
  }
  return res;
}

OvmsVehicle::vehicle_command_t  OvmsVehicleSmartEQ::CommandCan(uint32_t txid,uint32_t rxid,bool enable) {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandCan failed / no write access");
    return Fail;
  }
  ESP_LOGI(TAG, "CommandCan");

  std::string request;
  std::string response;
  std::string reqstr = m_hl_canbyte;

  CommandWakeup2();
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  uint8_t protocol = ISOTP_STD;
  int timeout_ms = 500;

  request = hexdecode("10C0");
  PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  vTaskDelay(500 / portTICK_PERIOD_MS);
  request = hexdecode(reqstr);
  PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);

  if (enable) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    request = hexdecode("1103");  // key on/off
    PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  }
  return Success;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandHomelink(int button, int durationms) {
  // This is needed to enable climate control via Homelink for the iOS app
  ESP_LOGI(TAG, "CommandHomelink button=%d durationms=%d", button, durationms);
  OvmsVehicle::vehicle_command_t res = NotImplemented;

  if (StdMetrics.ms_v_bat_soc->AsInt() < 31) {
    ESP_LOGI(TAG, "Battery SOC is too low for climate control");
    return Fail;
  }

  switch (button)
  {
    case 0:
    {
      res =  CommandClimateControl(true);
      break;
    }
    case 1:
    {
      res = CommandClimateControl(true);
      m_booster_ticker = 2;
      break;
    }
    case 2:
    {
      res = CommandClimateControl(true);
      m_booster_ticker = 3;
      break;
    }
    default:
      res = OvmsVehicle::CommandHomelink(button, durationms);
      break;
  }

  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandWakeup() {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandWakeup failed: no write access!");
    return Fail;
  }

  OvmsVehicle::vehicle_command_t res;

  ESP_LOGI(TAG, "Send Wakeup Command");
  res = Fail;
  if(!mt_bus_awake->AsBool()) {
    uint8_t data[4] = {0x40, 0x00, 0x00, 0x00};
    canbus *obd;
    obd = m_can1;

    for (int i = 0; i < 20; i++) {
      obd->WriteStandard(0x634, 4, data);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      if (mt_bus_awake->AsBool()) {
        res = Success;
        ESP_LOGI(TAG, "Vehicle is now awake");
        break;
      }
    }
  } else {
    res = Success;
    ESP_LOGI(TAG, "Vehicle is awake");
  }

  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandWakeup2() {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandWakeup2 failed: no write access!");
    return Fail;
  }

  if(!mt_bus_awake->AsBool()) {
    ESP_LOGI(TAG, "Send Wakeup CommandWakeup2");
    uint8_t data[8] = {0xc1, 0x1b, 0x73, 0x57, 0x14, 0x70, 0x96, 0x85};
    canbus *obd;
    obd = m_can1;
    obd->WriteStandard(0x350, 8, data);
    ESP_LOGI(TAG, "Vehicle is awake");
    return Success;
  } else {
    ESP_LOGI(TAG, "Vehicle is awake");
    return Success;
  }
}

// lock: can can1 tx st 745 04 30 01 00 00
OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandLock(const char* pin) {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandLock failed / no write access");
    return Fail;
  }
  ESP_LOGI(TAG, "CommandLock");
  CommandWakeup();
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  
  uint32_t txid = 0x745, rxid = 0x765;
  uint8_t protocol = ISOTP_STD;
  int timeout_ms = 200;
  
  std::string request;
  std::string response;
  std::string reqstr = MyConfig.GetParamValue("xsq", "lock.byte", "30010000");
  
  request = hexdecode("10C0");
  int err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  
  request = hexdecode(reqstr);
  err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);

  if(m_indicator) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    std::string indstr = MyConfig.GetParamValue("xsq", "indicator", "30082002");
    request = hexdecode(indstr); // indicator light
    err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  }

  if (err == POLLSINGLE_TXFAILURE)
  {
    ESP_LOGD(TAG, "ERROR: transmission failure (CAN bus error)");
    return Fail;
  }
  else if (err < 0)
  {
    ESP_LOGD(TAG, "ERROR: timeout waiting for poller/response");
    return Fail;
  }
  else if (err)
  {
    ESP_LOGD(TAG, "ERROR: request failed with response error code %02X\n", err);
    return Fail;
  }

  StdMetrics.ms_v_env_locked->SetValue(true);
  return Success;
}

// unlock: can can1 tx st 745 04 30 01 00 01
OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandUnlock(const char* pin) {

  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandUnlock failed / no write access");
    return Fail;
  }
  ESP_LOGI(TAG, "CommandUnlock");
  CommandWakeup();
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  
  uint32_t txid = 0x745, rxid = 0x765;
  uint8_t protocol = ISOTP_STD;
  int timeout_ms = 200;
  std::string request;
  std::string response;
  std::string reqstr = MyConfig.GetParamValue("xsq", "unlock.byte", "30010001");
  
  request = hexdecode("10C0");
  int err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  
  request = hexdecode(reqstr);
  err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);

  if(m_indicator) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    std::string indstr = MyConfig.GetParamValue("xsq", "indicator", "30082002");
    request = hexdecode(indstr); // indicator light
    err = PollSingleRequest(m_can1, txid, rxid, request, response, timeout_ms, protocol);
  }

  if (err == POLLSINGLE_TXFAILURE)
  {
    ESP_LOGD(TAG, "ERROR: transmission failure (CAN bus error)");
    return Fail;
  }
  else if (err < 0)
  {
    ESP_LOGD(TAG, "ERROR: timeout waiting for poller/response");
    return Fail;
  }
  else if (err)
  {
    ESP_LOGD(TAG, "ERROR: request failed with response error code %02X\n", err);
    return Fail;
  }

  StdMetrics.ms_v_env_locked->SetValue(false);
  return Success;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandActivateValet(const char* pin) {
  return NotImplemented;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandDeactivateValet(const char* pin) {
  
  OvmsVehicle::vehicle_command_t res = Fail;
  int number = atoi(pin);
  ESP_LOGI(TAG, "DDT4all number=%d", number);
  if(!m_ddt4all && !m_enable_write && number > 5) {
    ESP_LOGE(TAG, "DDT4all failed / no write access");
    return Fail;
  }

  switch (number)
  {
    case 0:
    {
      // indicator 5x on
      m_hl_canbyte = "30082002";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 1:
    {
      // open trunk
      m_hl_canbyte = "300500";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 2:
    {
      res = Fail;
      break;
    }
    case 3:
    {
      res = Fail;
      break;
    }
    case 4:
    {
      #ifdef CONFIG_OVMS_COMP_WIFI
        if (MyPeripherals && MyPeripherals->m_esp32wifi) {
            MyPeripherals->m_esp32wifi->Restart();
            ESP_LOGI(TAG, "WiFi restart initiated");
            res = Success;
        } else {
            ESP_LOGE(TAG, "WiFi restart failed - WiFi not available");
            res = Fail;
        }
      #else
        ESP_LOGE(TAG, "WiFi support not enabled");
        res = NotImplemented;
      #endif
      break;
    }
    case 5:
    {
      #ifdef CONFIG_OVMS_COMP_CELLULAR
        if (MyPeripherals && MyPeripherals->m_cellular_modem) {
            MyPeripherals->m_cellular_modem->Restart();
            ESP_LOGI(TAG, "Cellular modem restart initiated");
            res = Success;
        } else {
            ESP_LOGE(TAG, "Cellular modem restart failed - modem not available");
            res = Fail;
        }
      #else
        ESP_LOGE(TAG, "Cellular support not enabled");
        res = NotImplemented;
      #endif
      break;
    }
    // CommandCan(txid, rxid, enable, request)
    case 6:
    {
      // BIPBIP_Lock false
      m_hl_canbyte = "3B1400";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 7:
    {
      // BIPBIP_Lock true
      m_hl_canbyte = "3B1480";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 8:
    {
      // REAR_WIPER_LINK false
      m_hl_canbyte = "3B5800";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 9:
    {
      // REAR_WIPER_LINK true
      m_hl_canbyte = "3B5880";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 10:
    {
      // RKE_Backdoor_open false
      m_hl_canbyte = "3B7800";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 11:
    {
      // RKE_Backdoor_open true
      m_hl_canbyte = "3B7880";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 12:
    {
      // Precond_by_key 00
      m_hl_canbyte = "3B7700";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 13:
    {
      // Precond_by_key 03
      m_hl_canbyte = "3B7703";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 14:
    {
      // ECOMODE_PRE_Restart false
      m_hl_canbyte = "3B7600";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 15:
    {
      // ECOMODE_PRE_Restart true
      m_hl_canbyte = "3B7680";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 16:
    {
      // Charging screen false
      m_hl_canbyte = "2E013D00";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 17:
    {
      // Charging screen true
      m_hl_canbyte = "2E013D01";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 26:
    {
      // AT_BeepInRPresent_CF false
      m_hl_canbyte = "2E014900";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 27:
    {
      // AT_BeepInRPresent_CF true
      m_hl_canbyte = "2E014980";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    
    case 28:
    {
      // EVStartupSoundInhibition_CF false
      m_hl_canbyte = "2E013501";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 29:
    {
      // EVStartupSoundInhibition_CF true
      m_hl_canbyte = "2E013500";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 30:
    {
      // indicator 5x on
      m_hl_canbyte = "30082002";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 31:
    {
      // open trunk
      m_hl_canbyte = "300500";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 32:
    {
      // key reminder false
      m_hl_canbyte = "3B5E00";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 33:
    {
      // key reminder true
      m_hl_canbyte = "3B5E80";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 34:
    {
      // long tempo display false
      m_hl_canbyte = "3B5700";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 35:
    {
      // long tempo display true
      m_hl_canbyte = "3B5780";
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 40:
    {
      // ClockDisplayed_CF not displayed
      m_hl_canbyte = "2E012100";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 41:
    {
      // ClockDisplayed_CF displayed managed
      m_hl_canbyte = "2E012101";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 42:
    {
      // ClockDisplayed_CF displayed not managed
      m_hl_canbyte = "2E012102";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 43:
    {
      // ClockDisplayed_CF not used (EQ Smart Connect)
      m_hl_canbyte = "2E012103";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 50:
    {
      // max AC current limitation configuration 20A only for slow charger!
      if (!mt_obl_fastchg->AsBool()) {
        m_hl_canbyte = "2E614150";
        CommandCan(0x719, 0x739, false);
        res = Success;
      } else {
        res = Fail;
      }
      break;
    }
    case 51:
    {
      // max AC current limitation configuration 32A only for slow charger!
      if (!mt_obl_fastchg->AsBool()) {
        m_hl_canbyte = "2E614180";
        CommandCan(0x719, 0x739, false);
        res = Success;
      } else {
        res = Fail;
      }
      break;
    }
    case 52:
    {
      // SBRLogic_CF Standard
      m_hl_canbyte = "2E018500";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 53:
    {
      // SBRLogic_CF US
      m_hl_canbyte = "2E018501";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 54:
    {
      // FrontSBRInhibition_CF false
      m_hl_canbyte = "2E010900";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 55:
    {
      // FrontSBRInhibition_CF true
      m_hl_canbyte = "2E010901";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 56:
    {
      // Speedmeter ring (Tacho) DayBacklightsPresent_CF false
      m_hl_canbyte = "2E011800";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 57:
    {
      // Speedmeter ring (Tacho) DayBacklightsPresent_CF true
      m_hl_canbyte = "2E011801";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 58:
    {
      // AdditionnalInstrumentPresent_CF false
      m_hl_canbyte = "2E018001";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 59:
    {
      // AdditionnalInstrumentPresent_CF true
      m_hl_canbyte = "2E018001";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    
    case 60:
    {
      // TPMSPresent_CF false
      m_hl_canbyte = "2E010E00";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 61:
    {
      // TPMSPresent_CF true
      m_hl_canbyte = "2E010E01";
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 100:
    {
      // ClearDiagnosticInformation.All
      m_hl_canbyte = "14FFFFFF";
      CommandCan(0x745, 0x765, true);
      res = Success;
      break;
    }
    case 719:
    {
      CommandCan(0x719, 0x739, false);
      res = Success;
      break;
    }
    case 743:
    {
      CommandCan(0x743, 0x763, true);
      res = Success;
      break;
    }
    case 745:
    {
      CommandCan(0x745, 0x765, false);
      res = Success;
      break;
    }
    case 746:
    {
      CommandCan(0x74d, 0x76d, false);
      res = Success;
      break;
    }
    default:
      res = Fail;
      break;
  }

  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandStat(int verbosity, OvmsWriter* writer) {

  bool chargeport_open = StdMetrics.ms_v_door_chargeport->AsBool();
  std::string charge_state = StdMetrics.ms_v_charge_state->AsString();
  if (chargeport_open && charge_state != "")
    {
    std::string charge_mode = StdMetrics.ms_v_charge_mode->AsString();
    bool show_details = !(charge_state == "done" || charge_state == "stopped");

    // Translate mode codes:
    if (charge_mode == "standard")
      charge_mode = "Standard";
    else if (charge_mode == "storage")
      charge_mode = "Storage";
    else if (charge_mode == "range")
      charge_mode = "Range";
    else if (charge_mode == "performance")
      charge_mode = "Performance";

    // Translate state codes:
    if (charge_state == "charging")
      charge_state = "Charging";
    else if (charge_state == "topoff")
      charge_state = "Topping off";
    else if (charge_state == "done")
      charge_state = "Charge Done";
    else if (charge_state == "preparing")
      charge_state = "Preparing";
    else if (charge_state == "heating")
      charge_state = "Charging, Heating";
    else if (charge_state == "stopped")
      charge_state = "Charge Stopped";
    else if (charge_state == "timerwait")
      charge_state = "Charge Stopped, Timer On";

    if (charge_mode != "")
      writer->printf("%s - ", charge_mode.c_str());
    writer->printf("%s\n", charge_state.c_str());

    if (show_details)
      {
      // Voltage & current:
      bool show_vc = (StdMetrics.ms_v_charge_voltage->AsFloat() > 0 || StdMetrics.ms_v_charge_current->AsFloat() > 0);
      if (show_vc)
        {
        writer->printf("%s/%s ",
          (char*) StdMetrics.ms_v_charge_voltage->AsUnitString("-", Native, 1).c_str(),
          (char*) StdMetrics.ms_v_charge_current->AsUnitString("-", Native, 1).c_str());
        }

      // Charge speed:
      if (StdMetrics.ms_v_bat_range_speed->IsDefined() && StdMetrics.ms_v_bat_range_speed->AsFloat() != 0)
        {
        writer->printf("%s\n", StdMetrics.ms_v_bat_range_speed->AsUnitString("-", ToUser, 1).c_str());
        }
      else if (show_vc)
        {
        writer->puts("");
        }

      // Estimated time(s) remaining:
      int duration_full = StdMetrics.ms_v_charge_duration_full->AsInt();
      if (duration_full > 0)
        writer->printf("Full: %d:%02dh\n", duration_full / 60, duration_full % 60);

      int duration_soc = StdMetrics.ms_v_charge_duration_soc->AsInt();
      if (duration_soc > 0)
        writer->printf("%s: %d:%02dh\n",
          (char*) StdMetrics.ms_v_charge_limit_soc->AsUnitString("SOC", ToUser, 0).c_str(),
          duration_soc / 60, duration_soc % 60);

      int duration_range = StdMetrics.ms_v_charge_duration_range->AsInt();
      if (duration_range > 0)
        writer->printf("%s: %d:%02dh\n",
          (char*) StdMetrics.ms_v_charge_limit_range->AsUnitString("Range", ToUser, 0).c_str(),
          duration_range / 60, duration_range % 60);
      }

    // Energy sums:
    if (StdMetrics.ms_v_charge_kwh_grid->IsDefined())
      {
      writer->printf("Drawn: %s\n",
        StdMetrics.ms_v_charge_kwh_grid->AsUnitString("-", ToUser, 1).c_str());
      }
    if (StdMetrics.ms_v_charge_kwh->IsDefined())
      {
      writer->printf("Charged: %s\n",
        StdMetrics.ms_v_charge_kwh->AsUnitString("-", ToUser, 1).c_str());
      }
    }
  else
    {
    writer->puts("Not charging");
    }

  writer->printf("SOC: %s\n", (char*) StdMetrics.ms_v_bat_soc->AsUnitString("-", ToUser, 1).c_str());

  if (StdMetrics.ms_v_bat_range_ideal->IsDefined())
    {
    const std::string& range_ideal = StdMetrics.ms_v_bat_range_ideal->AsUnitString("-", ToUser, 0);
    writer->printf("Ideal range: %s\n", range_ideal.c_str());
    }

  if (StdMetrics.ms_v_bat_range_est->IsDefined())
    {
    const std::string& range_est = StdMetrics.ms_v_bat_range_est->AsUnitString("-", ToUser, 0);
    writer->printf("Est. range: %s\n", range_est.c_str());
    }

  if (StdMetrics.ms_v_pos_odometer->IsDefined())
    {
    const std::string& odometer = StdMetrics.ms_v_pos_odometer->AsUnitString("-", ToUser, 1);
    writer->printf("ODO: %s\n", odometer.c_str());
    }

  if (StdMetrics.ms_v_bat_cac->IsDefined())
    {
    const std::string& cac = StdMetrics.ms_v_bat_cac->AsUnitString("-", ToUser, 1);
    writer->printf("CAC: %s\n", cac.c_str());
    }

  if (StdMetrics.ms_v_bat_soh->IsDefined())
    {
    const std::string& soh = StdMetrics.ms_v_bat_soh->AsUnitString("-", ToUser, 0);
    writer->printf("SOH: %s\n", soh.c_str());
    }

  if (mt_evc_hv_energy->IsDefined())
    {
    const std::string& hv_energy = mt_evc_hv_energy->AsUnitString("-", ToUser, 3);
    writer->printf("usable Energy: %s\n", hv_energy.c_str());
    }

  if (mt_use_at_reset->IsDefined())
    {
    const std::string& use_at_reset = mt_use_at_reset->AsUnitString("-", ToUser, 1);
    writer->printf("kWh/100km: %s\n", use_at_reset.c_str());
    }
  if (mt_ocs_trip_km->IsDefined())
    {
    const std::string& ocs_trip_km = mt_ocs_trip_km->AsUnitString("-", ToUser, 1);
    writer->printf("OCS Trip km: %s\n", ocs_trip_km.c_str());
    }
  if (mt_ocs_trip_time->IsDefined())
    {
    const std::string& ocs_trip_time = mt_ocs_trip_time->AsUnitString("-", ToUser, 1);
    writer->printf("OCS Trip time: %s\n", ocs_trip_time.c_str());
    }
  if (StdMetrics.ms_v_env_service_time->IsDefined())
    {
    time_t service_time = StdMetrics.ms_v_env_service_time->AsInt();
    char service_date[32];
    struct tm service_tm;
    localtime_r(&service_time, &service_tm);
    strftime(service_date, sizeof(service_date), "%d-%m-%Y", &service_tm);
    writer->printf("Maintenance Date: %s\n", service_date);
    }
  if (mt_ocs_mt_day_usual->IsDefined())
    {
    const std::string& service_at_days = mt_ocs_mt_day_usual->AsUnitString("-", ToUser, 1);
    writer->printf("Maintenance in days: %s\n", service_at_days.c_str());
    }
  if (mt_ocs_mt_km_usual->IsDefined())
    {
    const std::string& service_at_km = mt_ocs_mt_km_usual->AsUnitString("-", ToUser, 1);
    writer->printf("Maintenance in km: %s\n", service_at_km.c_str());
    }
  if (mt_ocs_mt_level->IsDefined())
    {
    const std::string& service_at_lvl = mt_ocs_mt_level->AsUnitString("-", ToUser, 1);
    writer->printf("Maintenance level: %s\n", service_at_lvl.c_str());
    }

  return Success;
}

/**
 * SetFeature: V2 compatibility config wrapper
 *  Note: V2 only supported integer values, V3 values may be text
 */
bool OvmsVehicleSmartEQ::SetFeature(int key, const char *value)
{
  switch (key)
  {
    case 1:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "led",  (bits& 1)!=0);
      return true;
    }
    case 2:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "ios_tpms_fix",  (bits& 1)!=0);
      return true;
    }
    case 3:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "resettrip",  (bits& 1)!=0);
      return true;
    }
    case 4:
    {
      int bits = atoi(value);
      char buf[10];
      sprintf(buf, "1,%d,0,0,-1,-1,-1", bits);
      mt_booster_data->SetValue(std::string(buf));
      return true;
    }
    case 5:
    {
      int bits = atoi(value);
      if(bits < 0) bits = 0;
      if(bits > 2359) bits = 0;
      
      char buf[4];
      snprintf(buf, sizeof(buf), "1,1,0,%04d,-1,-1,-1", bits);
      mt_booster_data->SetValue(std::string(buf));
      return true;
    }
    case 6:
    {
      int bits = atoi(value);
      if(bits < 0) bits = 0;
      if(bits > 2) bits = 2;
      char buf[4];
      sprintf(buf, "1,0,0,0,-1,-1,%d", bits);
      mt_booster_data->SetValue(std::string(buf));
      return true;
    }
    case 7:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "resettotal",  (bits& 1)!=0);
      return true;
    }
    // case 8 -> Vehicle.cpp GPS stream
    // case 9 -> Vehicle.cpp minsoc
    case 10:
    {
      MyConfig.SetParamValue("xsq", "suffsoc", value);
      return true;
    }
    case 11:
    {
      MyConfig.SetParamValue("xsq", "suffrange", value);
      return true;
    }
    case 12:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "bcvalue",  (bits& 1)!=0);
      return true;
    }
    case 13:
    {
      MyConfig.SetParamValue("xsq", "full.km", value);
      return true;
    }
    // case 14 -> Vehicle.cpp carbits
    case 15:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "canwrite",  (bits& 1)!=0);
      return true;
    }
    case 16:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "ddt4all",  (bits& 1)!=0);
      return true;
    }
    default:
      return OvmsVehicle::SetFeature(key, value);
  }
}

/**
 * GetFeature: V2 compatibility config wrapper
 *  Note: V2 only supported integer values, V3 values may be text
 */
const std::string OvmsVehicleSmartEQ::GetFeature(int key)
{
  switch (key)
  {
    case 1:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "led",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 2:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "ios_tpms_fix",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 3:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "resettrip",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 4:
      if(mt_booster_on->AsBool()){return std::string("1");}else{ return std::string("2");};
    case 5:
    {
      return mt_booster_time->AsString();
    }
    case 6:
    {
      int bits = mt_booster_1to3->AsInt();
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 7:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "resettotal",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    // case 8 -> Vehicle.cpp stream
    // case 9 -> Vehicle.cpp minsoc
    case 10:
      return MyConfig.GetParamValue("xsq", "suffsoc", STR(0));
    case 11:
      return MyConfig.GetParamValue("xsq", "suffrange", STR(0));
    case 12:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "bcvalue",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 13:
      return MyConfig.GetParamValue("xsq", "full.km", STR(126.0));
    // case 14 -> Vehicle.cpp carbits
    case 15:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "canwrite",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    case 16:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "ddt4all",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    default:
      return OvmsVehicle::GetFeature(key);
  }
}

class OvmsVehicleSmartEQInit {
  public:
  OvmsVehicleSmartEQInit();
} MyOvmsVehicleSmartEQInit __attribute__ ((init_priority (9000)));

OvmsVehicleSmartEQInit::OvmsVehicleSmartEQInit() {
  ESP_LOGI(TAG, "Registering Vehicle: SMART EQ (9000)");
  MyVehicleFactory.RegisterVehicle<OvmsVehicleSmartEQ>("SQ", "Smart ED/EQ 4.Gen");
}

