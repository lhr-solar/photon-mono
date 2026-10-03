#include "photon_ui/signal_map.hpp"

#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/mux.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace photon::ui {
namespace {

// DBC names from assets/dbc/carcan_telemcan.dbc.
// Columns: role, label, Message.Signal, unit, syntheticId, screens,
// tile, plot, hasWarnLow, hasWarnHigh, warnLow, warnHigh.
// Empty signal: derived in vital() (pack SOC from pack voltage, mph, slip, array power).
// pack_current is MC_BusMeasurement.MC_BusCurrent. There is no Main_Battery_Current.
// No rear wheel speed in this DBC. Suspension is SUS_Data1 on each corner.

constexpr ScalarDef kScalarDefs[] = {
    {"pack_soc", "Pack SOC", "", "%", kSignalSoc, MapScreen::Pit, true, false, true, false, 30.0, 0.0},
    {"pack_voltage", "Pack V", "BPS_Status.Main_Battery_Voltage", "V", 0, MapScreen::Pit, true, false, true,
     true, 90.0, 134.4},
    {"pack_current", "Pack I", "MC_BusMeasurement.MC_BusCurrent", "A", 0, MapScreen::Pit, true, false, false,
     false, 0.0, 0.0},
    {"pack_avg_temp", "Pack T", "BPS_Status.Main_Battery_Avg_Temperature", "C", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 45.0},
    {"bps_state", "BPS", "BPS_Status.BPS_Fault", "", 0, MapScreen::Pit, true, false, false, true, 0.0, 0.0},
    {"contactor_hv_plus", "HV+", "BPS_Status.HV_Plus_Contactor_State", "", 0, MapScreen::Pit, false, false,
     false, false, 0.0, 0.0},
    {"contactor_hv_minus", "HV-", "BPS_Status.HV_Minus_Contactor_State", "", 0, MapScreen::Pit, false, false,
     false, false, 0.0, 0.0},
    {"contactor_array", "Array", "BPS_Status.Array_Contactor_State", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"contactor_array_precharge", "Arr Pchg", "BPS_Status.Array_Precharge_Contactor_State", "", 0,
     MapScreen::Pit, false, false, false, false, 0.0, 0.0},
    {"contactor_motor", "Motor", "VCU_Status.Motor_Contactor_State", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"contactor_motor_precharge", "Mtr Pchg", "VCU_Status.Motor_Precharge_Contactor_State", "", 0,
     MapScreen::Pit, false, false, false, false, 0.0, 0.0},
    {"motor_temp", "Motor T", "MC_Motor_TempMeasurement.MC_MotorTemp", "C", kSignalTemp, MapScreen::Pit, true,
     false, false, true, 0.0, 75.0},
    {"inverter_temp", "Inverter T", "MC_Motor_TempMeasurement.MC_HeatsinkTemp", "C", 0, MapScreen::Pit, true,
     false, false, true, 0.0, 80.0},
    {"motor_rpm", "Motor RPM", "MC_VelocityMeasurement.MC_MotorVelocity", "rpm", kSignalRpm,
     MapScreen::Both | MapScreen::Scene3d, true, true, false, true, 0.0, 7500.0},
    {"vehicle_speed", "Speed", "MC_VelocityMeasurement.MC_VehicleVelocity", "m/s", kSignalSpeed,
     MapScreen::Both | MapScreen::Scene3d, true, false, false, true, 0.0, 15.0},
    {"throttle", "Throttle", "Pedal_Status.AccelPedal_Main_Pos", "%", 0, MapScreen::Both, true, false, false,
     true, 0.0, 100.0},
    {"brake", "Brake", "Pedal_Status.BrakePedal_Main_Pos", "%", 0, MapScreen::Pit | MapScreen::Scene3d, true,
     false, false, true, 0.0, 100.0},
    {"gear_forward", "F", "Driver_Input_Status.Gear_Forward", "", 0, MapScreen::Pit | MapScreen::Scene3d, false,
     false, false, false, 0.0, 0.0},
    {"gear_neutral", "N", "Driver_Input_Status.Gear_Neutral", "", 0, MapScreen::Pit | MapScreen::Scene3d, false,
     false, false, false, 0.0, 0.0},
    {"gear_reverse", "R", "Driver_Input_Status.Gear_Reverse", "", 0, MapScreen::Pit | MapScreen::Scene3d, false,
     false, false, false, 0.0, 0.0},
    {"regen_enable", "Regen", "Driver_Input_Status.Regen_Enable", "", 0, MapScreen::Pit | MapScreen::Scene3d,
     false, false, false, false, 0.0, 0.0},
    {"regen_activate", "Regen Act", "Driver_Input_Status.Regen_Activate", "", 0,
     MapScreen::Pit | MapScreen::Scene3d, false, false, false, false, 0.0, 0.0},
    {"cruise_enable", "Cruise", "Driver_Input_Status.Cruise_Enable", "", 0, MapScreen::Pit | MapScreen::Scene3d,
     false, false, false, false, 0.0, 0.0},
    {"blinker_left", "L", "Driver_Input_Status.Blinker_Left", "", 0, MapScreen::Pit | MapScreen::Scene3d, false,
     false, false, false, 0.0, 0.0},
    {"blinker_right", "R blink", "Driver_Input_Status.Blinker_Right", "", 0, MapScreen::Pit | MapScreen::Scene3d,
     false, false, false, false, 0.0, 0.0},
    {"hazard", "Haz", "Driver_Input_Status.Hazard_Pressed", "", 0, MapScreen::Pit | MapScreen::Scene3d, false,
     false, false, false, 0.0, 0.0},
    {"mppt_a_vin", "MPPT A Vin", "MPPT_A_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_a_iin", "MPPT A Iin", "MPPT_A_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_b_vin", "MPPT B Vin", "MPPT_B_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_b_iin", "MPPT B Iin", "MPPT_B_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_c_vin", "MPPT C Vin", "MPPT_C_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_c_iin", "MPPT C Iin", "MPPT_C_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"array_power", "Array P", "", "W", 0, MapScreen::Pit, true, false, false, false, 0.0, 0.0},
    {"supp_voltage", "Aux V", "Supp_Battery_Status.Supplemental_Battery_Voltage", "V", 0, MapScreen::Pit, false,
     false, true, false, 11.0, 0.0},
    {"supp_current", "Aux I", "Supp_Battery_Status.Supplemental_Battery_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"motor_ready", "Ready", "VCU_Status.Motor_Ready", "", 0, MapScreen::Pit, false, false, true, false, 1.0,
     0.0},
    {"ign_array", "Array", "Driver_Input_Status.Ignition_Array", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"ign_motor", "Motor", "Driver_Input_Status.Ignition_Motor", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"ign_off", "Off", "Driver_Input_Status.Ignition_Off", "", 0, MapScreen::Pit, false, false, false, false,
     0.0, 0.0},
    {"bps_charge_ok", "Charge OK", "BPS_Status.BPS_Charge_OK", "", 0, MapScreen::Pit, false, false, true, false,
     1.0, 0.0},
    {"bps_regen_ok", "Regen OK", "BPS_Status.BPS_Regen_OK", "", 0, MapScreen::Pit, false, false, true, false,
     1.0, 0.0},
    {"vcu_bps_fault", "VCU BPS", "VCU_Status.VCU_BPS_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 0.0},
    {"vcu_controls_fault", "VCU Ctrl", "VCU_Status.VCU_CONTROLS_FAULT_DETECTED", "", 0, MapScreen::Pit, false,
     false, false, true, 0.0, 0.0},
    {"vcu_mtr_fault", "VCU Mtr", "VCU_Status.VCU_MTR_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false, false,
     true, 0.0, 0.0},
    {"vcu_pedals_fault", "VCU Ped", "VCU_Status.VCU_PEDALS_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 0.0},
    {"vcu_steer_fault", "VCU Steer", "VCU_Status.VCU_STEERING_FAULT_DETECTED", "", 0, MapScreen::Pit, false,
     false, false, true, 0.0, 0.0},
    {"vcu_other_fault", "VCU Other", "VCU_Status.VCU_OTHER_FAULT", "", 0, MapScreen::Pit, false, false, false,
     true, 0.0, 0.0},
    {"mppt_a_fault", "MPPT A", "MPPT_A_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"mppt_b_fault", "MPPT B", "MPPT_B_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"mppt_c_fault", "MPPT C", "MPPT_C_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"steering_angle", "Steer", "LWS_Standard.LWS_Angle", "deg", 0,
     MapScreen::Dynamics | MapScreen::Scene3d | MapScreen::Dashboard, true, false, true, true, -540.0, 540.0},
    {"wheel_rpm_fl", "FL RPM", "WheelRPM_FL.RPM", "rpm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     false, true, 0.0, 1200.0},
    {"wheel_rpm_fr", "FR RPM", "WheelRPM_FR.RPM", "rpm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     false, true, 0.0, 1200.0},
    {"susp_fl", "Susp FL", "Suspension_FL.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"susp_fr", "Susp FR", "Suspension_FR.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"susp_rear", "Susp Rr", "Suspension_Back.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"gps_lat", "GPS Lat", "Lattitude_GPS.Latitude", "deg", 0, MapScreen::Scene3d, true, false, false, false,
     0.0, 0.0},
    {"gps_lon", "GPS Lon", "Longitude_GPS.Longitude", "deg", 0, MapScreen::Scene3d, true, false, false, false,
     0.0, 0.0},
    {"gps_speed", "GPS Spd", "Speed_GPS.Speed", "m/s", 0, MapScreen::Scene3d | MapScreen::Dashboard, true, false,
     false, true, 0.0, 40.0},
    {"gps_valid", "GPS", "Valid_GPS.Valid", "", 0, MapScreen::Scene3d, true, false, true, false, 1.0, 0.0},
    {"imu_ax", "Long G", "IMU_B_Accel.Accel_X", "g", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     true, true, -2.0, 2.0},
    {"imu_ay", "Lat G", "IMU_B_Accel.Accel_Y", "g", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true, true,
     true, -2.0, 2.0},
    {"imu_fr_ax", "Long G", "IMU_Accel_FR.Accel_X", "g", 0, MapScreen::Dynamics, false, false, true, true, -2.0,
     2.0},
    {"imu_fr_ay", "Lat G", "IMU_Accel_FR.Accel_Y", "g", 0, MapScreen::Dynamics, false, false, true, true, -2.0,
     2.0},
    {"imu_fl_ax", "Long G", "IMU_Accel_FL.Accel_X", "g", 0, MapScreen::Dynamics, false, false, true, true, -2.0,
     2.0},
    {"imu_fl_ay", "Lat G", "IMU_Accel_FL.Accel_Y", "g", 0, MapScreen::Dynamics, false, false, true, true, -2.0,
     2.0},
    {"imu_gx", "Gyro X", "IMU_Gyro_Back.Gyro_X", "dps", 0, MapScreen::Scene3d, false, false, false, false, 0.0,
     0.0},
    {"imu_gy", "Gyro Y", "IMU_Gyro_Back.Gyro_Y", "dps", 0, MapScreen::Scene3d, false, false, false, false, 0.0,
     0.0},
    {"yaw_rate", "Yaw", "IMU_Gyro_Back.Gyro_Z", "dps", 0, MapScreen::Dynamics, true, true, true, true, -90.0,
     90.0},
    {"vehicle_speed_mph", "Speed", "", "mph", 0, MapScreen::Both, true, false, false, true, 0.0, 70.0},
    {"slip_fl", "Slip FL", "", "%", 0, MapScreen::Dynamics, true, false, true, true, -20.0, 20.0},
    {"slip_fr", "Slip FR", "", "%", 0, MapScreen::Dynamics, true, false, true, true, -20.0, 20.0},
    {"lws_fault", "LWS", "LWS_Standard.LWS_Fault", "", 0, MapScreen::Dynamics | MapScreen::Dashboard, false, false,
     false, true, 0.0, 0.0},
    {"mc_bus_voltage", "Bus V", "MC_BusMeasurement.MC_BusVoltage", "V", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"drive_vel", "Drive Vel", "MC_DriveCommand.MC_MotorVelocitySetpoint", "rpm", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"drive_cur", "Drive Cur", "MC_DriveCommand.MC_MotorCurrentSetpoint", "%", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"power_cmd", "Power Cmd", "MC_PowerCommand.MC_MotorPowerSetpoint", "%", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"pre_motor", "Pre Motor", "VCU_Precharge_Voltages.VCU_Precharge_Motor_Voltage", "V", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"pre_batt", "Pre Batt", "VCU_Precharge_Voltages.VCU_Precharge_Battery_Voltage", "V", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"mc_limit_pwm", "Lim PWM", "MC_Status.MC_LIMIT_OutputVoltagePWM", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"mc_limit_i", "Lim I", "MC_Status.MC_LIMIT_MotorCurrent", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"mc_limit_vel", "Lim Vel", "MC_Status.MC_LIMIT_Velocity", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"mc_limit_bus_i", "Lim Bus I", "MC_Status.MC_LIMIT_BusCurrent", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"mc_limit_bus_v_hi", "Lim Bus Hi", "MC_Status.MC_LIMIT_BusVoltageUpper", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_limit_bus_v_lo", "Lim Bus Lo", "MC_Status.MC_LIMIT_BusVoltageLower", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_limit_temp", "Lim Temp", "MC_Status.MC_LIMIT_MotorTemp", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"mc_fault_hw_oc", "MC HW OC", "MC_Status.MC_FAULT_HardwareOverCurrent", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_sw_oc", "MC SW OC", "MC_Status.MC_FAULT_SoftwareOverCurrent", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_bus_ov", "MC Bus OV", "MC_Status.MC_FAULT_DcBusOverVoltage", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_hall", "MC Hall", "MC_Status.MC_FAULT_BadMotorPositionHallSeq", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_wd", "MC WD", "MC_Status.MC_FAULT_WatchdogCausedLastReset", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_cfg", "MC Cfg", "MC_Status.MC_FAULT_ConfigRead", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"mc_fault_15v", "MC 15V", "MC_Status.MC_FAULT_15vRailUnderVoltage", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"mc_fault_desat", "MC Desat", "MC_Status.MC_FAULT_DesaturationFault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_fault_overspeed", "MC Overspeed", "MC_Status.MC_FAULT_MotorOverSpeed", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"brake_p1", "Brake P1", "Brake_Pressure_1.Brake_Pressure", "psi", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"brake_p2", "Brake P2", "Brake_Pressure_2.Brake_Pressure", "psi", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"brake_p1_fault", "Brake P1 F", "Pedal_Status.Brake_Pressure_1_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"brake_p2_fault", "Brake P2 F", "Pedal_Status.Brake_Pressure_2_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"accel_redun", "Accel Red", "Pedal_Status.AccelPedal_Redundant_Pos", "%", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"brake_redun", "Brake Red", "Pedal_Status.BrakePedal_Redundant_Pos", "%", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"accel_main_fault", "Accel Main F", "Pedal_Status.AccelPedal_Main_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"accel_redun_fault", "Accel Red F", "Pedal_Status.AccelPedal_Redundant_Fault", "", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"brake_main_fault", "Brake Main F", "Pedal_Status.BrakePedal_Main_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"brake_redun_fault", "Brake Red F", "Pedal_Status.BrakePedal_Redundant_Fault", "", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"horn", "Horn", "Driver_Input_Status.Horn_Pressed", "", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"ptt", "PTT", "Driver_Input_Status.PushToTalk_Pressed", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"cruise_set", "Cruise Set", "Driver_Input_Status.Cruise_Set", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"vcu_fsm", "VCU FSM", "VCU_Status.VCU_FSM_State", "", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"vcu_pedals_wd", "Pedals WD", "VCU_Status.VCU_Pedals_Watchdog", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"vcu_driver_wd", "Driver WD", "VCU_Status.VCU_Driver_Input_Watchdog", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"vcu_regen_ok", "VCU Regen", "VCU_Status.VCU_Regen_OK", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"vcu_regen_active", "Regen Act", "VCU_Status.VCU_Regen_Active", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"mppt_a_vout", "MPPT A Vout", "MPPT_A_PowerMeasurements.MPPT_Output_Voltage", "V", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"mppt_a_iout", "MPPT A Iout", "MPPT_A_PowerMeasurements.MPPT_Output_Current", "A", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"mppt_a_heatsink", "MPPT A Hs", "MPPT_A_Status.MPPT_HeatsinkTemperature", "C", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mppt_a_ambient", "MPPT A Amb", "MPPT_A_Status.MPPT_AmbientTemperature", "C", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mppt_a_mode", "MPPT A Mode", "MPPT_A_Status.MPPT_Mode", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"coolant_1", "Coolant 1", "Coolant_Temperature.Coolant_Temperature_1", "C", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"coolant_2", "Coolant 2", "Coolant_Temperature.Coolant_Temperature_2", "C", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"flow_1", "Flow 1", "Pump_Status.FlowRate_1", "L/min", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"flow_2", "Flow 2", "Pump_Status.FlowRate_2", "L/min", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"pump_duty", "Pump", "Pump_Status.Pump_DutyCycle", "%", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"pump_fault", "Pump F", "Pump_Status.Pump_Fault", "", 0, MapScreen::Dashboard, false, false, false, false,
     0.0, 0.0},
    {"ltc_hvdcdc_sel", "HV DCDC", "LV_Carrier_Status.LTC4421_HVDCDC_Selected", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"ltc_hvdcdc_fault", "HV DCDC F", "LV_Carrier_Status.LTC4421_HVDCDC_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"ltc_hvdcdc_valid", "HV DCDC V", "LV_Carrier_Status.LTC4421_HVDCDC_Valid", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"ltc_supp_sel", "Supp Sel", "LV_Carrier_Status.LTC4421_SuppBatt_Selected", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"ltc_supp_fault", "Supp F", "LV_Carrier_Status.LTC4421_SuppBatt_Fault", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"ltc_supp_valid", "Supp Vld", "LV_Carrier_Status.LTC4421_SuppBatt_Valid", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"lv_en_supp", "LV Supp", "LV_Carrier_Status.LV_EN_SupplementalBattery", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"lv_en_psu", "LV PSU", "LV_Carrier_Status.LV_EN_PowerSupply", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"cam_backup", "Cam Rear", "Display_Status.Camera_Status_Backup", "", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"cam_left", "Cam L", "Display_Status.Camera_Status_Left", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"cam_right", "Cam R", "Display_Status.Camera_Status_Right", "", 0, MapScreen::Dashboard, false, false, false,
     false, 0.0, 0.0},
    {"controls_leader_fault", "Ctrl F", "Controls_Status.Controls_Leader_Fault", "", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"light_front", "Light F", "Controls_Status.LightingBoard_Front_Status", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"light_left", "Light L", "Controls_Status.LightingBoard_Left_Status", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"light_right", "Light R", "Controls_Status.LightingBoard_Right_Status", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"light_rear", "Light Rr", "Controls_Status.LightingBoard_Rear_Status", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"light_canopy", "Light C", "Controls_Status.LightingBoard_Canopy_Status", "", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_phase_b", "Phase B", "MC_PhaseCurrentMeasurement.MC_PhaseCurrentB", "A", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_phase_c", "Phase C", "MC_PhaseCurrentMeasurement.MC_PhaseCurrentC", "A", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"mc_bemf_q", "BEMF Q", "MC_BackEMFMeasurementPrediction.MC_BEMFq", "V", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"mc_bemf_d", "BEMF D", "MC_BackEMFMeasurementPrediction.MC_BEMFd", "V", 0, MapScreen::Dashboard, false, false,
     false, false, 0.0, 0.0},
    {"supp_charger", "Supp Chg", "Supp_Charger_Status.Supplemental_Charger_Status", "", 0, MapScreen::Dashboard,
     false, false, false, false, 0.0, 0.0},
    {"supp_vicor_v", "Vicor V", "Supp_Vicor_Stats.Supplemental_Vicor_Voltage", "V", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
    {"supp_vicor_i", "Vicor I", "Supp_Vicor_Stats.Supplemental_Vicor_Current", "A", 0, MapScreen::Dashboard, false,
     false, false, false, 0.0, 0.0},
};

constexpr MuxDef kMuxDefs[] = {
    {"cell_voltage", "BPS_Voltage_Aggregate_Arr.BPS_Tap_idx",
     "BPS_Voltage_Aggregate_Arr.BPS_Voltage_Tap_Data", 32},
    {"cell_temp", "BPS_Temperature_Aggregate_Arr.BPS_Tap_idx",
     "BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 32},
};

std::string catalogKey(const ctrl::CatalogSignal& sig) {
    if (sig.message.empty()) {
        return sig.name;
    }
    return sig.message + "." + sig.name;
}

std::size_t ringIndex(const SignalRing& ring, std::size_t logicalOldest) {
    if (ring.count < ring.cap) {
        return logicalOldest;
    }
    return (ring.plotOffset() + logicalOldest) % ring.cap;
}

// Rings are circular. Copy oldest-first before pairing the tap index with its value.
void fillFromMux(const SignalRing* idx, const SignalRing* val, std::vector<float>& out,
                 std::vector<double>* tapTs, int count, bool reset) {
    if (idx == nullptr || val == nullptr || idx->count == 0 || val->count == 0) {
        applyMuxSeries(nullptr, nullptr, 0, nullptr, nullptr, 0, out, tapTs, count, reset);
        return;
    }
    std::vector<double> idxT(idx->count);
    std::vector<double> idxV(idx->count);
    std::vector<double> valT(val->count);
    std::vector<double> valV(val->count);
    for (std::size_t i = 0; i < idx->count; ++i) {
        const std::size_t p = ringIndex(*idx, i);
        idxT[i] = idx->t[p];
        idxV[i] = idx->v[p];
    }
    for (std::size_t i = 0; i < val->count; ++i) {
        const std::size_t p = ringIndex(*val, i);
        valT[i] = val->t[p];
        valV[i] = val->v[p];
    }
    applyMuxSeries(idxT.data(), idxV.data(), idxT.size(), valT.data(), valV.data(), valT.size(), out, tapTs,
                   count, reset);
}

bool catalogRangeUsable(double mn, double mx) {
    return mx > mn && std::isfinite(mn) && std::isfinite(mx);
}

}  // namespace

SignalMap::SignalMap() {
    scalars_.reserve(sizeof(kScalarDefs) / sizeof(kScalarDefs[0]));
    for (const auto& def : kScalarDefs) {
        ResolvedScalar slot;
        slot.def = &def;
        slot.unit = def.unitHint;
        scalars_.push_back(std::move(slot));
    }
    muxes_.reserve(sizeof(kMuxDefs) / sizeof(kMuxDefs[0]));
    for (const auto& def : kMuxDefs) {
        ResolvedMux slot;
        slot.def = &def;
        slot.count = def.count;
        muxes_.push_back(slot);
    }
}

void SignalMap::reset() {
    for (auto& slot : scalars_) {
        slot.id = 0;
        slot.mapped = false;
        slot.unit = slot.def != nullptr ? slot.def->unitHint : "";
        slot.catalogMin = 0.0;
        slot.catalogMax = 0.0;
        slot.haveCatalogRange = false;
    }
    for (auto& slot : muxes_) {
        slot.indexId = 0;
        slot.valueId = 0;
        slot.mapped = false;
    }
    resolvedOnce_ = false;
}

void SignalMap::resolve(const std::vector<ctrl::CatalogSignal>& catalog, ClientArena& arena) {
    std::unordered_map<std::string, const ctrl::CatalogSignal*> byKey;
    byKey.reserve(catalog.size());
    for (const auto& sig : catalog) {
        byKey.emplace(catalogKey(sig), &sig);
    }

    int hits = 0;
    int misses = 0;
    for (auto& slot : scalars_) {
        slot.id = 0;
        slot.mapped = false;
        slot.unit = slot.def->unitHint;
        slot.catalogMin = 0.0;
        slot.catalogMax = 0.0;
        slot.haveCatalogRange = false;
        if (slot.def->signal[0] == '\0') {
            continue;
        }
        const auto it = byKey.find(slot.def->signal);
        if (it == byKey.end()) {
            arena.pushLog(std::string("signal_map miss ") + slot.def->role + " " + slot.def->signal);
            ++misses;
            continue;
        }
        slot.mapped = true;
        slot.id = it->second->id;
        if (!it->second->unit.empty()) {
            slot.unit = it->second->unit;
        }
        slot.catalogMin = it->second->min;
        slot.catalogMax = it->second->max;
        slot.haveCatalogRange = catalogRangeUsable(it->second->min, it->second->max);
        ++hits;
    }
    for (auto& slot : muxes_) {
        slot.indexId = 0;
        slot.valueId = 0;
        slot.mapped = false;
        const auto idx = byKey.find(slot.def->indexSignal);
        const auto val = byKey.find(slot.def->valueSignal);
        if (idx == byKey.end() || val == byKey.end()) {
            arena.pushLog(std::string("signal_map miss ") + slot.def->role + " mux");
            ++misses;
            continue;
        }
        slot.mapped = true;
        slot.indexId = idx->second->id;
        slot.valueId = val->second->id;
        ++hits;
    }
    resolvedOnce_ = true;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "signal_map resolved hits=%d misses=%d", hits, misses);
    arena.pushLog(buf);
}

void SignalMap::subscribe(Client& client, MapScreen screen) const {
    std::vector<std::uint32_t> ids;
    ids.reserve(scalars_.size() + muxes_.size() * 2);
    for (const auto& slot : scalars_) {
        if (slot.def == nullptr || !mapOn(slot.def->screens, screen)) {
            continue;
        }
        if (slot.mapped && slot.id != 0) {
            ids.push_back(slot.id);
        } else if (slot.def->syntheticId != 0) {
            ids.push_back(slot.def->syntheticId);
        }
    }
    if (mapOn(MapScreen::Pit, screen) || mapOn(MapScreen::Dashboard, screen)) {
        for (const auto& slot : muxes_) {
            if (!slot.mapped) {
                continue;
            }
            ids.push_back(slot.indexId);
            ids.push_back(slot.valueId);
        }
    }
    client.ensureSubscribed(ids);
}

void SignalMap::collectMappedKeys(std::vector<std::string>& keys, MapScreen screen) const {
    for (const auto& slot : scalars_) {
        if (!slot.mapped || slot.def == nullptr || slot.def->signal == nullptr || slot.def->signal[0] == '\0') {
            continue;
        }
        if (screen != MapScreen::None && !mapOn(slot.def->screens, screen)) {
            continue;
        }
        keys.emplace_back(slot.def->signal);
    }
}

void SignalMap::collectMappedIds(std::vector<std::uint32_t>& ids) const {
    for (const auto& slot : scalars_) {
        if (slot.mapped && slot.id != 0) {
            ids.push_back(slot.id);
        }
    }
    for (const auto& slot : muxes_) {
        if (!slot.mapped) {
            continue;
        }
        if (slot.indexId != 0) {
            ids.push_back(slot.indexId);
        }
        if (slot.valueId != 0) {
            ids.push_back(slot.valueId);
        }
    }
}

const ResolvedScalar* SignalMap::scalar(std::string_view role) const {
    for (const auto& slot : scalars_) {
        if (slot.def != nullptr && role == slot.def->role) {
            return &slot;
        }
    }
    return nullptr;
}

const ResolvedMux* SignalMap::mux(std::string_view role) const {
    for (const auto& slot : muxes_) {
        if (slot.def != nullptr && role == slot.def->role) {
            return &slot;
        }
    }
    return nullptr;
}

bool SignalMap::cellsMapped() const {
    const ResolvedMux* v = mux("cell_voltage");
    return v != nullptr && v->mapped;
}

bool SignalMap::valueAlarmed(const ResolvedScalar& slot, double value) {
    if (slot.def->hasWarnHigh && value > slot.def->warnHigh) {
        return true;
    }
    if (slot.def->hasWarnLow && value < slot.def->warnLow) {
        return true;
    }
    return false;
}

Vital SignalMap::syntheticVital(const ResolvedScalar& slot, const ClientArena& arena) const {
    Vital v;
    v.role = slot.def->role;
    v.label = slot.def->label;
    v.signal = slot.def->signal;
    v.unit = slot.def->unitHint;
    if (slot.def->syntheticId == 0) {
        return v;
    }
    if (const SignalDesc* desc = findSignal(slot.def->syntheticId)) {
        v.label = desc->label;
        v.unit = desc->unit;
        v.hasBar = desc->hasLow || desc->hasHigh;
        v.barMin = desc->hasLow ? desc->warnLow : 0.0;
        v.barMax = desc->hasHigh ? desc->warnHigh : (desc->hasLow ? 100.0 : 1.0);
        v.alarm = arena.has(desc->id) && signalAlarmed(*desc, arena.readout(desc->id));
    }
    v.id = slot.def->syntheticId;
    v.synthetic = true;
    v.have = arena.has(slot.def->syntheticId);
    v.value = arena.readout(slot.def->syntheticId);
    return v;
}

Vital SignalMap::vital(std::string_view role, const ClientArena& arena) const {
    const ResolvedScalar* slot = scalar(role);
    Vital v;
    if (slot == nullptr || slot->def == nullptr) {
        return v;
    }
    v.role = slot->def->role;
    v.label = slot->def->label;
    v.signal = slot->def->signal;
    v.unit = slot->unit.c_str();
    v.id = slot->id;

    if (role == "pack_soc") {
        const Vital packV = vital("pack_voltage", arena);
        if (packV.mapped && packV.have) {
            v.derived = true;
            v.have = true;
            v.value = estimatePackSoc(packV.value);
            v.unit = "%";
            v.hasBar = true;
            v.barMin = 0.0;
            v.barMax = 100.0;
            v.alarm = slot->def->hasWarnLow && v.value < slot->def->warnLow;
            return v;
        }
        return arena.haveSignalCatalog ? v : syntheticVital(*slot, arena);
    }
    if (role == "vehicle_speed_mph") {
        const Vital spd = vital("vehicle_speed", arena);
        v.unit = "mph";
        v.hasBar = true;
        v.barMin = 0.0;
        v.barMax = 70.0;
        v.derived = true;
        if (spd.mapped && spd.have && std::isfinite(spd.value)) {
            v.mapped = true;
            v.have = true;
            v.value = spd.value * kMpsToMph;
            v.alarm = slot->def->hasWarnHigh && v.value > slot->def->warnHigh;
        }
        return v;
    }
    if (role == "slip_fl" || role == "slip_fr") {
        const Vital rpm = vital(role == "slip_fl" ? "wheel_rpm_fl" : "wheel_rpm_fr", arena);
        const Vital spd = vital("vehicle_speed", arena);
        v.unit = "%";
        v.hasBar = true;
        v.barMin = -25.0;
        v.barMax = 25.0;
        v.derived = true;
        if (rpm.mapped && rpm.have && spd.mapped && spd.have && std::isfinite(rpm.value) &&
            std::isfinite(spd.value) && std::abs(spd.value) >= kMinGroundSpeedMps) {
            v.mapped = true;
            v.have = true;
            v.value = slipPercent(wheelSpeedMps(rpm.value), spd.value);
            v.alarm = std::abs(v.value) > 20.0;
        }
        return v;
    }
    if (role == "array_power") {
        double watts = 0.0;
        int pairs = 0;
        static constexpr const char* kVin[] = {"mppt_a_vin", "mppt_b_vin", "mppt_c_vin"};
        static constexpr const char* kIin[] = {"mppt_a_iin", "mppt_b_iin", "mppt_c_iin"};
        for (int i = 0; i < 3; ++i) {
            const Vital vin = vital(kVin[i], arena);
            const Vital iin = vital(kIin[i], arena);
            if (vin.mapped && iin.mapped && vin.have && iin.have) {
                watts += vin.value * iin.value;
                ++pairs;
            }
        }
        v.unit = "W";
        v.hasBar = true;
        v.barMin = 0.0;
        v.barMax = 1500.0;
        if (pairs > 0) {
            v.derived = true;
            v.have = true;
            v.value = watts;
            return v;
        }
        return v;
    }

    if (slot->mapped) {
        v.mapped = true;
        v.have = arena.has(slot->id);
        v.value = arena.readout(slot->id);
        v.alarm = v.have && valueAlarmed(*slot, v.value);
        if (slot->haveCatalogRange) {
            v.hasBar = true;
            v.barMin = slot->catalogMin;
            v.barMax = slot->catalogMax;
        } else if (slot->def->hasWarnLow || slot->def->hasWarnHigh) {
            v.hasBar = true;
            v.barMin = slot->def->hasWarnLow ? slot->def->warnLow : 0.0;
            v.barMax = slot->def->hasWarnHigh ? slot->def->warnHigh : (v.barMin + 1.0);
        }
        return v;
    }
    // Catalog is in and this name missed: leave the tile empty. Do not invent a value.
    if (arena.haveSignalCatalog || arena.catalogPinned()) {
        return v;
    }
    return syntheticVital(*slot, arena);
}

void SignalMap::fillCells(ClientArena& arena) const {
    const ResolvedMux* volt = mux("cell_voltage");
    const ResolvedMux* temp = mux("cell_temp");
    if (volt == nullptr || !volt->mapped || volt->count <= 0) {
        return;
    }
    // Keep the last pack. Zeroing unseen taps made cells blink off while the
    // next mux cycle / 60s L0 tile was still in flight.
    std::vector<float> voltages = arena.cells().voltage;
    std::vector<float> temps = arena.cells().temp;
    std::vector<double> tapTs = arena.cells().tapTs;
    fillFromMux(arena.ring(volt->indexId), arena.ring(volt->valueId), voltages, &tapTs, volt->count, false);
    if (temp != nullptr && temp->mapped) {
        fillFromMux(arena.ring(temp->indexId), arena.ring(temp->valueId), temps, nullptr, temp->count, false);
    }
    const auto cursor = arena.effectiveCursorMs();
    if (!arena.timelineIsLive() && cursor != 0 &&
        arena.tiles().sessionMaxMs() > arena.tiles().sessionMinMs()) {
        const auto toMs = cursor;
        const auto fromMs = toMs > 180000 ? toMs - 180000 : arena.tiles().sessionMinMs();
        const auto idxPlot = arena.tiles().gatherL0(volt->indexId, fromMs, toMs);
        const auto valPlot = arena.tiles().gatherL0(volt->valueId, fromMs, toMs);
        applyMuxSeries(idxPlot.t.data(), idxPlot.y.data(), idxPlot.t.size(), valPlot.t.data(),
                       valPlot.y.data(), valPlot.t.size(), voltages, &tapTs, volt->count, false);
        if (temp != nullptr && temp->mapped) {
            const auto tIdx = arena.tiles().gatherL0(temp->indexId, fromMs, toMs);
            const auto tVal = arena.tiles().gatherL0(temp->valueId, fromMs, toMs);
            applyMuxSeries(tIdx.t.data(), tIdx.y.data(), tIdx.t.size(), tVal.t.data(), tVal.y.data(),
                           tVal.t.size(), temps, nullptr, temp->count, false);
        }
    }
    if (temps.size() < voltages.size()) {
        temps.resize(voltages.size(), 0.0f);
    }
    arena.setMappedCells(std::move(voltages), std::move(temps), std::move(tapTs));
}

CellCallout SignalMap::cellCallout(const std::vector<float>& voltages) {
    CellCallout c;
    int seen = 0;
    for (int i = 0; i < static_cast<int>(voltages.size()); ++i) {
        const float v = voltages[static_cast<std::size_t>(i)];
        if (!std::isfinite(v) || v <= 0.05f) {
            continue;
        }
        if (seen == 0 || v < c.minV) {
            c.minV = v;
            c.minIndex = i;
        }
        if (seen == 0 || v > c.maxV) {
            c.maxV = v;
            c.maxIndex = i;
        }
        ++seen;
    }
    if (seen > 0) {
        c.deltaV = c.maxV - c.minV;
        c.valid = true;
    }
    return c;
}

double SignalMap::estimatePackSoc(double packVoltage) {
    const double ratio = (packVoltage - 83.2) / 51.2;
    if (!(ratio > 0.0)) {
        return 0.0;
    }
    double soc = 100.0 * std::pow(ratio, 0.55);
    if (soc < 0.0) {
        return 0.0;
    }
    if (soc > 100.0) {
        return 100.0;
    }
    return soc;
}

}  // namespace photon::ui
