#include "../include.h"
#ifdef SERIAL_LINK_RECEIVER
#include "../datalayer/datalayer.h"
#include "../devboard/utils/events.h"
#include "SERIAL-LINK-RECEIVER-FROM-BATTERY.h"

#define INVERTER_SEND_NUM_VARIABLES 1
#define INVERTER_RECV_NUM_VARIABLES 16

#ifdef INVERTER_SEND_NUM_VARIABLES
const uint8_t sendingNumVariables = INVERTER_SEND_NUM_VARIABLES;
#else
const uint8_t sendingNumVariables = 0;
#endif

#ifdef TESTBENCH
// In the testbench environment, the receiver uses Serial3
#define SerialReceiver Serial3
#else
// In the production environment, the receiver uses Serial2
#define SerialReceiver Serial2
#endif

//                                          txid,rxid,  num_send,num_recv
SerialDataLink dataLinkReceive(SerialReceiver, 0, 0x01, sendingNumVariables,
                               INVERTER_RECV_NUM_VARIABLES);  // ...

static bool batteryFault = false;  // used locally - mainly to indicate Battery CAN failure

void __getData() {
  datalayer.battery.status.real_soc = (uint16_t)dataLinkReceive.getReceivedData(0);
  datalayer.battery.status.soh_pptt = (uint16_t)dataLinkReceive.getReceivedData(1);
  datalayer.battery.status.voltage_dV = (uint16_t)dataLinkReceive.getReceivedData(2);
  datalayer.battery.status.current_dA = (int16_t)dataLinkReceive.getReceivedData(3);
  datalayer.battery.info.total_capacity_Wh =
      (uint32_t)(dataLinkReceive.getReceivedData(4) * 10);  //add back missing decimal
  datalayer.battery.status.remaining_capacity_Wh =
      (uint32_t)(dataLinkReceive.getReceivedData(5) * 10);  //add back missing decimal
  datalayer.battery.status.max_discharge_power_W =
      (uint32_t)(dataLinkReceive.getReceivedData(6) * 10);  //add back missing decimal
  datalayer.battery.status.max_charge_power_W =
      (uint32_t)(dataLinkReceive.getReceivedData(7) * 10);  //add back missing decimal
  uint16_t _system_bms_status = (uint16_t)dataLinkReceive.getReceivedData(8);
  datalayer.battery.status.active_power_W =
      (uint32_t)(dataLinkReceive.getReceivedData(9) * 10);  //add back missing decimal
  datalayer.battery.status.temperature_min_dC = (int16_t)dataLinkReceive.getReceivedData(10);
  datalayer.battery.status.temperature_max_dC = (int16_t)dataLinkReceive.getReceivedData(11);
  datalayer.battery.status.cell_max_voltage_mV = (uint16_t)dataLinkReceive.getReceivedData(12);
  datalayer.battery.status.cell_min_voltage_mV = (uint16_t)dataLinkReceive.getReceivedData(13);
  datalayer.battery.info.chemistry = (battery_chemistry_enum)dataLinkReceive.getReceivedData(14);
  datalayer.system.status.battery_allows_contactor_closing = (bool)dataLinkReceive.getReceivedData(15);

  batteryFault = false;
  if (_system_bms_status == FAULT) {
    batteryFault = true;
    set_event(EVENT_SERIAL_TRANSMITTER_FAILURE, 0);
  }
}

void updateData() {
  // --- update with fresh data
  dataLinkReceive.updateData(0, datalayer.system.status.inverter_allows_contactor_closing);
  //dataLinkReceive.updateData(1,var2); // For future expansion,
  //dataLinkReceive.updateData(2,var3); // if inverter needs to send data to battery
}

/*
*  @ 9600bps, assume void manageSerialLinkReceiver()
*             is called every 1mS
*/

void manageSerialLinkReceiver() {

  static bool lasterror = false;
  static unsigned long last_minutesLost = 0;
  static unsigned long lastGood;
  static uint16_t lastGoodMaxCharge;
  static uint16_t lastGoodMaxDischarge;
  static bool initLink = false;
  static unsigned long reportTime = 0;
  static uint16_t reads = 0;
  static uint16_t errors = 0;
  unsigned long currentTime = millis();

  if (!initLink) {
    initLink = true;
    // sends variables every 5000mS even if no change
    dataLinkReceive.setUpdateInterval(5000);
#ifdef SERIALDATALINK_MUTEACK
    dataLinkReceive.muteACK(true);
#endif
  }
  dataLinkReceive.run();
  bool readError = dataLinkReceive.checkReadError(true);  // check for error & clear error flag

  if (readError) {
    logging.print(currentTime);
    logging.println(" - ERROR: SerialDataLink - Read Error");
    lasterror = true;
    errors++;
  }

  if (dataLinkReceive.checkNewData(true))  // true = clear Flag
  {
    __getData();
    reads++;
    lastGoodMaxCharge = datalayer.battery.status.max_charge_power_W;
    lastGoodMaxDischarge = datalayer.battery.status.max_discharge_power_W;
    //--- if BatteryFault then assume Data is stale
    if (!batteryFault)
      lastGood = currentTime;
    //bms_status = ACTIVE;  // just testing
    if (lasterror) {
      lasterror = false;
      logging.print(currentTime);
      logging.println(" - RECOVERY: SerialDataLink - Read GOOD");
    }
  }

  unsigned long minutesLost = (currentTime - lastGood) / 60000UL;
  if (minutesLost > 0 && lastGood > 0) {
    //   lose 25% each minute of data loss
    if (minutesLost < 4) {
      datalayer.battery.status.max_charge_power_W = (lastGoodMaxCharge * (4 - minutesLost)) / 4;
      datalayer.battery.status.max_discharge_power_W = (lastGoodMaxDischarge * (4 - minutesLost)) / 4;
      set_event(EVENT_SERIAL_RX_WARNING, minutesLost);
    } else {
      // Times Up -
      datalayer.battery.status.max_charge_power_W = 0;
      datalayer.battery.status.max_discharge_power_W = 0;
      set_event(EVENT_SERIAL_RX_FAILURE, uint8_t(min(minutesLost, 255uL)));
      //----- Throw Error
    }
    // report Lost data & Max charge / Discharge reductions
    if (minutesLost != last_minutesLost) {
      last_minutesLost = minutesLost;
      logging.print(currentTime);
      if (batteryFault) {
        logging.print("Battery Fault (minutes) : ");
      } else {
        logging.print(" - Minutes without data : ");
      }
      logging.print(minutesLost);
      logging.print(", max Charge = ");
      logging.print(datalayer.battery.status.max_charge_power_W);
      logging.print(", max Discharge = ");
      logging.println(datalayer.battery.status.max_discharge_power_W);
    }
  }

  if (currentTime - reportTime > 59999) {
    reportTime = currentTime;
    logging.print(currentTime);
    logging.print(" SerialDataLink-Receiver - NewData :");
    logging.print(reads);
    logging.print("   Errors : ");
    logging.println(errors);
    reads = 0;
    errors = 0;

// --- printUsefullData();
//logging.print("SOC = ");
//logging.println(SOC);
#ifdef DEBUG_LOG
    update_values_serial_link();
#endif
  }

  static unsigned long updateTime = 0;

#ifdef INVERTER_SEND_NUM_VARIABLES
  if (currentTime - updateTime > 100) {
    updateTime = currentTime;
    dataLinkReceive.run();
    bool sendError = dataLinkReceive.checkTransmissionError(true);  // check for error & clear error flag
    updateData();
  }
#endif
}

void update_values_serial_link() {
  logging.println("Values from battery: ");
  logging.print("SOC: ");
  logging.print(datalayer.battery.status.real_soc);
  logging.print(" SOH: ");
  logging.print(datalayer.battery.status.soh_pptt);
  logging.print(" Voltage: ");
  logging.print(datalayer.battery.status.voltage_dV);
  logging.print(" Current: ");
  logging.print(datalayer.battery.status.current_dA);
  logging.print(" Capacity: ");
  logging.print(datalayer.battery.info.total_capacity_Wh);
  logging.print(" Remain cap: ");
  logging.print(datalayer.battery.status.remaining_capacity_Wh);
  logging.print(" Max discharge W: ");
  logging.print(datalayer.battery.status.max_discharge_power_W);
  logging.print(" Max charge W: ");
  logging.print(datalayer.battery.status.max_charge_power_W);
  logging.print(" BMS status: ");
  logging.print(datalayer.battery.status.bms_status);
  logging.print(" Power: ");
  logging.print(datalayer.battery.status.active_power_W);
  logging.print(" Temp min: ");
  logging.print(datalayer.battery.status.temperature_min_dC);
  logging.print(" Temp max: ");
  logging.print(datalayer.battery.status.temperature_max_dC);
  logging.print(" Cell max: ");
  logging.print(datalayer.battery.status.cell_max_voltage_mV);
  logging.print(" Cell min: ");
  logging.print(datalayer.battery.status.cell_min_voltage_mV);
  logging.print(" LFP : ");
  logging.print(datalayer.battery.info.chemistry);
  logging.print(" Battery Allows Contactor Closing: ");
  logging.print(datalayer.system.status.battery_allows_contactor_closing);
  logging.print(" Inverter Allows Contactor Closing: ");
  logging.print(datalayer.system.status.inverter_allows_contactor_closing);

  logging.println("");
}

void setup_battery(void) {
  strncpy(datalayer.system.info.battery_protocol, "Serial link to another LilyGo board", 63);
  datalayer.system.info.battery_protocol[63] = '\0';
}
// Needed to make the compiler happy
void update_values_battery() {}
void transmit_can_battery() {}
void handle_incoming_can_frame_battery(CAN_frame rx_frame) {}

#endif
