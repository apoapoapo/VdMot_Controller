#include <knx.h>
#include "hardware.h"

HardwareTimer timer(TIM4);

#define COMM_DBG Serial6  // serial port for debugging

enum GroupObjectNames {
    /* Out */
    STATUS_VALVE_ACTIVE,
    STATUS_VALVE_PERCENT,
    TEMPERATURE_1WIRE_1,
    TEMPERATURE_1WIRE_2,
    TARGET_CURRENT,
    /* In */
    TARGET_OFFSET,
    TEMPERATURE_EXT,
    /* internal */
    NUM_OBJECTS_PER_VALVE
};

uint8_t get_object_index(uint8_t valve_number, GroupObjectNames obj_name) {
    return valve_number * NUM_OBJECTS_PER_VALVE + obj_name + 1;
}

GroupObjectNames get_object_name(uint8_t obj_index) {
    return (GroupObjectNames) (obj_index % NUM_OBJECTS_PER_VALVE);
}

uint8_t get_valve_number(uint8_t obj_index) {
    return obj_index / NUM_OBJECTS_PER_VALVE;
}

GroupObject& get_group_object(uint8_t valve_id, GroupObjectNames go_names) {
  return knx.getGroupObject(get_object_index(valve_id, go_names));
}

// #ifdef ACTUATOR_COUNT > 0
//   /* STATUS_VALVE_ACTIVE */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 0, GA_SUB_BASE + 0), KNX_DPT_1_011 /* DPT_State */, COM_OBJ_SENSOR),
//   /* STATUS_VALVE_PERCENT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 1, GA_SUB_BASE + 0), KNX_DPT_5_001 /* DPT_Scaling */, COM_OBJ_SENSOR),
//   /* TEMPERATURE_1WIRE_1 */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 2, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR), 
//   /* TEMPERATURE_1WIRE_2 */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 3, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR),
//   /* TARGET_CURRENT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 4, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR),
//   /* TARGET_OFFSET */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 6, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_LOGIC_IN),
//   /* TEMPERATURE_EXT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 7, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_LOGIC_IN),

// void knxEvents(byte index) {
//   COMM_DBG.print("KNX EVENT index: ");
//   COMM_DBG.print((int)index);
//   COMM_DBG.print(" ");
//   COMM_DBG.print(get_object_name(index));
//   COMM_DBG.flush();

//   e_KnxDeviceStatus ret;
//   switch (get_object_name(index))
//   {
//     case TARGET_OFFSET:
//       float target_offset;
//       ret = Knx.read(index, target_offset);
//       if (ret == KNX_DEVICE_OK) {
//         COMM_DBG.print("Got target offset for valve ");
//         COMM_DBG.print(get_valve_number(index));
//         COMM_DBG.print(": ");
//         COMM_DBG.println(target_offset);
//         COMM_DBG.flush();
//       }
//       break;
//     case TARGET_BASE:
//       float target_base;
//       ret = Knx.read(index, target_base);
//       if (ret == KNX_DEVICE_OK) {
//         COMM_DBG.print("Got target base for valve ");
//         COMM_DBG.print(get_valve_number(index));
//         COMM_DBG.print(": ");
//         COMM_DBG.println(target_base);
//         COMM_DBG.flush();
//       }
//       break;
//     case TEMPERATURE_EXT:
//       float value;
//       ret = Knx.read(index, value);
//       if (ret == KNX_DEVICE_OK) {
//         COMM_DBG.print("Got external temperature for valve ");
//         COMM_DBG.print(get_valve_number(index));
//         COMM_DBG.print(": ");
//         COMM_DBG.println(value);
//         COMM_DBG.flush();
//       }
//       break;
//     default:
//       break;
//   }
// }

void OnTimerInterrupt() {
  static int i = 1;
  static auto last_millis = millis();
  knx.loop();
  int divider = 5000;
  if (i % divider == 0) {
    auto now_millis = millis();
    COMM_DBG.print("KNX TIMER ");
    COMM_DBG.print(i);
    COMM_DBG.print(" millis (per timer): ");
    COMM_DBG.println(1.0 * (now_millis - last_millis) / divider);
    last_millis = now_millis;
    get_group_object(0, GroupObjectNames::TEMPERATURE_1WIRE_1).value((float) ((i / divider) % 100 * 2.55));
    COMM_DBG.flush(); 
  }
  i++;
}

void callback_target_offset(uint8_t valve_id, GroupObject& go) {
  COMM_DBG.print("Target offset: ");
  COMM_DBG.print(valve_id);
  COMM_DBG.print(" ");
  COMM_DBG.println((double)go.value());
}

void callback_target_offset_00(GroupObject& go) { callback_target_offset( 0, go); }
void callback_target_offset_01(GroupObject& go) { callback_target_offset( 1, go); }
void callback_target_offset_02(GroupObject& go) { callback_target_offset( 2, go); }
void callback_target_offset_03(GroupObject& go) { callback_target_offset( 3, go); }
void callback_target_offset_04(GroupObject& go) { callback_target_offset( 4, go); }
void callback_target_offset_05(GroupObject& go) { callback_target_offset( 5, go); }
void callback_target_offset_06(GroupObject& go) { callback_target_offset( 6, go); }
void callback_target_offset_07(GroupObject& go) { callback_target_offset( 7, go); }
void callback_target_offset_08(GroupObject& go) { callback_target_offset( 8, go); }
void callback_target_offset_09(GroupObject& go) { callback_target_offset( 9, go); }
void callback_target_offset_10(GroupObject& go) { callback_target_offset(10, go); }
void callback_target_offset_11(GroupObject& go) { callback_target_offset(11, go); }

void callback_temperature_ext(uint8_t valve_id, GroupObject& go) {
  COMM_DBG.print("Temperature ext: ");
  COMM_DBG.print(valve_id);
  COMM_DBG.print(" ");
  COMM_DBG.println((double)go.value());
}

void callback_temperature_ext_00(GroupObject& go) { callback_target_offset( 0, go); }
void callback_temperature_ext_01(GroupObject& go) { callback_target_offset( 1, go); }
void callback_temperature_ext_02(GroupObject& go) { callback_target_offset( 2, go); }
void callback_temperature_ext_03(GroupObject& go) { callback_target_offset( 3, go); }
void callback_temperature_ext_04(GroupObject& go) { callback_target_offset( 4, go); }
void callback_temperature_ext_05(GroupObject& go) { callback_target_offset( 5, go); }
void callback_temperature_ext_06(GroupObject& go) { callback_target_offset( 6, go); }
void callback_temperature_ext_07(GroupObject& go) { callback_target_offset( 7, go); }
void callback_temperature_ext_08(GroupObject& go) { callback_target_offset( 8, go); }
void callback_temperature_ext_09(GroupObject& go) { callback_target_offset( 9, go); }
void callback_temperature_ext_10(GroupObject& go) { callback_target_offset(10, go); }
void callback_temperature_ext_11(GroupObject& go) { callback_target_offset(11, go); }

void knx_setup() {
  COMM_DBG.println("KNX SETUP");
  knx.readMemory();
  if (knx.configured()) {
    // callback functions here
    // register callback for Target offset GO
    get_group_object( 0, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_00);
    get_group_object( 1, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_01);
    get_group_object( 2, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_02);
    get_group_object( 3, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_03);
    get_group_object( 4, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_04);
    get_group_object( 5, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_05);
    get_group_object( 6, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_06);
    get_group_object( 7, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_07);
    get_group_object( 8, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_08);
    get_group_object( 9, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_09);
    get_group_object(10, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_10);
    get_group_object(11, GroupObjectNames::TARGET_OFFSET).callback(callback_target_offset_11);

    get_group_object( 0, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_00);
    get_group_object( 1, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_01);
    get_group_object( 2, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_02);
    get_group_object( 3, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_03);
    get_group_object( 4, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_04);
    get_group_object( 5, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_05);
    get_group_object( 6, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_06);
    get_group_object( 7, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_07);
    get_group_object( 8, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_08);
    get_group_object( 9, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_09);
    get_group_object(10, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_10);
    get_group_object(11, GroupObjectNames::TEMPERATURE_EXT).callback(callback_temperature_ext_11);

//   /* STATUS_VALVE_ACTIVE */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 0, GA_SUB_BASE + 0), KNX_DPT_1_011 /* DPT_State */, COM_OBJ_SENSOR),
//   /* STATUS_VALVE_PERCENT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 1, GA_SUB_BASE + 0), KNX_DPT_5_001 /* DPT_Scaling */, COM_OBJ_SENSOR),
//   /* TEMPERATURE_1WIRE_1 */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 2, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR), 
//   /* TEMPERATURE_1WIRE_2 */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 3, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR),
//   /* TARGET_CURRENT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 4, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_SENSOR),
//   /* TARGET_BASE */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 5, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_LOGIC_IN),
//   /* TARGET_OFFSET */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 6, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_LOGIC_IN),
//   /* TEMPERATURE_EXT */ KnxComObject(G_ADDR(GA_MAIN, GA_MID_BASE + 7, GA_SUB_BASE + 0), KNX_DPT_9_001 /* DPT_Value_Temp */, COM_OBJ_LOGIC_IN),

    for (uint8_t valve_id = 0; valve_id < ACTUATOR_COUNT; ++valve_id) {
      get_group_object(valve_id, STATUS_VALVE_ACTIVE).dataPointType(DPT_State);
      get_group_object(valve_id, STATUS_VALVE_PERCENT).dataPointType(DPT_Scaling);
      get_group_object(valve_id, TEMPERATURE_1WIRE_1).dataPointType(DPT_Value_Temp);
      get_group_object(valve_id, TEMPERATURE_1WIRE_2).dataPointType(DPT_Value_Temp);
      get_group_object(valve_id, TARGET_CURRENT).dataPointType(DPT_Value_Temp);
      get_group_object(valve_id, TARGET_OFFSET).dataPointType(DPT_Value_Tempd);
      get_group_object(valve_id, TEMPERATURE_EXT).dataPointType(DPT_Value_Temp);

    }

    Serial6.print("Timeout: ");
    Serial6.println(knx.paramByte(0));
    Serial6.print("Zykl. senden: ");
    Serial6.println(knx.paramByte(1));
    Serial6.print("Min/Max senden: ");
    Serial6.println(knx.paramByte(2));
    Serial6.print("Aenderung senden: ");
    Serial6.println(knx.paramByte(3));
    Serial6.print("Abgleich: ");
    Serial6.println(knx.paramByte(4));
  }

  // KNX loop timer
  timer.pause();
  timer.setOverflow(500, MICROSEC_FORMAT);
  timer.attachInterrupt(OnTimerInterrupt);
  timer.refresh(); // Make register changes take effect
  timer.resume(); // Start
}

void knx_loop() {
  static int i = 1;
  static auto last_millis = millis();
  int divider = 500;
  if (i % divider == 0) {
    auto now_millis = millis();
    COMM_DBG.print("KNX LOOP ");
    COMM_DBG.print(i);
    COMM_DBG.print(" millis (per loop): ");
    COMM_DBG.println(1.0 * (now_millis - last_millis) / divider);
    last_millis = now_millis;
    COMM_DBG.flush(); 
  }
  i++;
}
