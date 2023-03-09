/*
 * Copyright (C) AC SOFTWARE SP. Z O.O
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "hvac_base.h"

#include <stdint.h>
#include <string.h>
#include <supla/log_wrapper.h>
#include <supla/storage/config.h>
#include <supla/storage/storage.h>
#include <supla/time.h>
#include <supla/clock/clock.h>
#include <supla/actions.h>

#include "output_interface.h"

#define SUPLA_HVAC_DEFAULT_TEMP_MIN          1800  // 18.00 C
#define SUPLA_HVAC_DEFAULT_TEMP_MAX          2400  // 24.00 C
#define SUPLA_HVAC_MAX_DIFFERENTIAL_SETPOINT 5000  // 50.00 C

using Supla::Control::HvacBase;

HvacBase::HvacBase(Supla::Control::OutputInterface *primaryOutput,
                   Supla::Control::OutputInterface *secondaryOutput) {
  channel.setType(SUPLA_CHANNELTYPE_HVAC);
  channel.setFlag(SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE);
  channel.setFlag(SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE);

  addPrimaryOutput(primaryOutput);
  addSecondaryOutput(secondaryOutput);

  // default function is set in onInit based on supported modes or loaded from
  // config
}

HvacBase::~HvacBase() {
}

// void handleAction(int event, int action) override;
void HvacBase::handleAction(int event, int action) {
  (void)(event);
  switch (action) {
    case Supla::TURN_ON: {
      setTargetMode(SUPLA_HVAC_MODE_CMD_TURN_ON);
      break;
    }
    case Supla::TURN_OFF: {
      setTargetMode(SUPLA_HVAC_MODE_OFF, false);
      break;
    }
    case Supla::TOGGLE: {
      if (channel.getHvacMode() == SUPLA_HVAC_MODE_OFF) {
        setTargetMode(SUPLA_HVAC_MODE_CMD_TURN_ON);
      } else {
        setTargetMode(SUPLA_HVAC_MODE_OFF, false);
      }
    }
  }
}

bool HvacBase::iterateConnected() {
  auto result = Element::iterateConnected();

  if (!result) {
    SUPLA_LOG_DEBUG("HVAC send: IsOn %d, Mode %d, tMin %d, tMax %d, flags 0x%x",
                    channel.getHvacIsOn(),
                    channel.getHvacMode(),
                    channel.getHvacSetpointTemperatureMin(),
                    channel.getHvacSetpointTemperatureMax(),
                    channel.getHvacFlags());
  }

  if (result && !waitForChannelConfigAndIgnoreIt &&
      !waitForWeeklyScheduleAndIgnoreIt) {
    if (channelConfigChangedOffline == 1) {
      for (auto proto = Supla::Protocol::ProtocolLayer::first();
           proto != nullptr;
           proto = proto->next()) {
        if (proto->setChannelConfig(getChannelNumber(),
                                    channel.getDefaultFunction(),
                                    reinterpret_cast<void *>(&config),
                                    sizeof(TSD_ChannelConfig_HVAC),
                                    SUPLA_CONFIG_TYPE_DEFAULT)) {
          channelConfigChangedOffline = 2;
        }
      }
    }
    if (weeklyScheduleChangedOffline == 1) {
      for (auto proto = Supla::Protocol::ProtocolLayer::first();
           proto != nullptr;
           proto = proto->next()) {
        if (proto->setChannelConfig(getChannelNumber(),
                                    channel.getDefaultFunction(),
                                    reinterpret_cast<void *>(&weeklySchedule),
                                    sizeof(weeklySchedule),
                                    SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE)) {
          weeklyScheduleChangedOffline = 2;
        }
      }
    }
  }

  return result;
}

void HvacBase::onLoadConfig() {
  auto cfg = Supla::Storage::ConfigInstance();
  if (cfg) {
    char key[SUPLA_CONFIG_MAX_KEY_SIZE] = {};

    // Read last set channel function
    loadFunctionFromConfig();

    // Generic HVAC configuration
    generateKey(key, "hvac_cfg");
    if (cfg->getBlob(key,
                     reinterpret_cast<char *>(&config),
                     sizeof(TSD_ChannelConfig_HVAC))) {
      SUPLA_LOG_INFO("HVAC config loaded successfully");
    } else {
      SUPLA_LOG_INFO("HVAC config missing. Using SW defaults");
    }

    // Weekly schedule configuration
    generateKey(key, "hvac_weekly");
    if (cfg->getBlob(key,
                     reinterpret_cast<char *>(&weeklySchedule),
                     sizeof(TSD_ChannelConfig_WeeklySchedule))) {
      SUPLA_LOG_INFO("HVAC weekly schedule loaded successfully");
      isWeeklyScheduleConfigured = true;
    } else {
      SUPLA_LOG_INFO("HVAC weekly schedule missing. Using SW defaults");
      isWeeklyScheduleConfigured = false;
    }

    // load config changed offline flags
    generateKey(key, "cfg_chng");
    uint8_t flag = 0;
    cfg->getUInt8(key, &flag);
    SUPLA_LOG_INFO("HVAC config changed offline flag %d", flag);
    if (flag) {
      channelConfigChangedOffline = 1;
    } else {
      channelConfigChangedOffline = 0;
    }

    flag = 0;
    generateKey(key, "weekly_chng");
    cfg->getUInt8(key, &flag);
    SUPLA_LOG_INFO("HVAC weekly schedule config changed offline flag %d", flag);
    if (flag) {
      weeklyScheduleChangedOffline = 1;
    } else {
      weeklyScheduleChangedOffline = 0;
    }

  } else {
    SUPLA_LOG_ERROR("HVAC can't work without config storage");
  }
}

void HvacBase::onLoadState() {
  if (getChannelNumber() >= 0) {
    auto hvacValue = channel.getValueHvac();
    Supla::Storage::ReadState(reinterpret_cast<unsigned char *>(hvacValue),
                            sizeof(THVACValue));
    Supla::Storage::ReadState(
        reinterpret_cast<unsigned char *>(&lastWorkingMode),
        sizeof(lastWorkingMode));
  }
}

void HvacBase::onSaveState() {
  if (getChannelNumber() >= 0) {
    auto hvacValue = channel.getValueHvac();
    Supla::Storage::WriteState(
        reinterpret_cast<const unsigned char *>(hvacValue), sizeof(THVACValue));
    Supla::Storage::WriteState(
        reinterpret_cast<const unsigned char *>(&lastWorkingMode),
        sizeof(lastWorkingMode));
  }
}

void HvacBase::onInit() {
  // init default channel function when it wasn't read from config or
  // set by user
  if (channel.getDefaultFunction() == 0) {
    // set default to auto when both heat and cool are supported
    if (isHeatingSupported() && isCoolingSupported() && isAutoSupported()) {
      setAndSaveFunction(SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO);
    } else if (isHeatingSupported()) {
      setAndSaveFunction(SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT);
    } else if (isCoolingSupported()) {
      setAndSaveFunction(SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL);
    } else if (isDrySupported()) {
      setAndSaveFunction(SUPLA_CHANNELFNC_HVAC_DRYER);
    } else if (isFanSupported()) {
      setAndSaveFunction(SUPLA_CHANNELFNC_HVAC_FAN);
    }
  }
  if (getUsedAlgorithm() == 0 &&
      isAlgorithmValid(SUPLA_HVAC_ALGORITHM_ON_OFF)) {
    setUsedAlgorithm(SUPLA_HVAC_ALGORITHM_ON_OFF);
  }

  initDone = true;

  uint8_t mode = channel.getHvacMode();
  if (mode == SUPLA_HVAC_MODE_NOT_SET) {
    setTargetMode(SUPLA_HVAC_MODE_OFF, false);
    if (isModeSupported(SUPLA_HVAC_MODE_HEAT) &&
        !channel.isHvacFlagSetpointTemperatureMinSet()) {
      channel.setHvacSetpointTemperatureMin(SUPLA_HVAC_DEFAULT_TEMP_MIN);
    }
    if (isModeSupported(SUPLA_HVAC_MODE_COOL) &&
        !channel.isHvacFlagSetpointTemperatureMaxSet()) {
      channel.setHvacSetpointTemperatureMax(SUPLA_HVAC_DEFAULT_TEMP_MAX);
    }
    setOutput(0, true);
  }
}


void HvacBase::onRegistered(Supla::Protocol::SuplaSrpc *suplaSrpc) {
  SUPLA_LOG_DEBUG("HVAC onRegistered");
    SUPLA_LOG_DEBUG("HVAC send: IsOn %d, Mode %d, tMin %d, tMax %d, flags 0x%x",
                    channel.getHvacIsOn(),
                    channel.getHvacMode(),
                    channel.getHvacSetpointTemperatureMin(),
                    channel.getHvacSetpointTemperatureMax(),
                    channel.getHvacFlags());
  Supla::Element::onRegistered(suplaSrpc);
  if (channelConfigChangedOffline) {
    channelConfigChangedOffline = 1;
    waitForChannelConfigAndIgnoreIt = true;
  }
  if (weeklyScheduleChangedOffline) {
    weeklyScheduleChangedOffline = 1;
    waitForWeeklyScheduleAndIgnoreIt = true;
  }
}

void HvacBase::iterateAlways() {
  if (millis() - lastIterateTimestampMs < 1000) {
    return;
  }
  lastIterateTimestampMs = millis();

  if (lastConfigChangeTimestampMs &&
      millis() - lastConfigChangeTimestampMs < 5000) {
    return;
  }
  lastConfigChangeTimestampMs = 0;

  if (!isModeSupported(channel.getHvacMode())) {
    setTargetMode(SUPLA_HVAC_MODE_OFF);
  }

  auto t1 = getPrimaryTemp();
  auto t2 = getSecondaryTemp();

  if (!checkThermometersStatusForCurrentMode(t1, t2)) {
    setOutput(0, true);
    SUPLA_LOG_DEBUG("HVAC: check thermometers not valid");
    channel.setHvacFlagError(true);
    return;
  }

  if (channel.getDefaultFunction() ==
      SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL) {
    t1 -= t2;
    t2 = INT16_MIN;
  }

  if (channel.isHvacFlagWeeklySchedule()) {
    SUPLA_LOG_DEBUG("HVAC: check weekly schedule exit");
    processWeeklySchedule();
  }

  if (checkOverheatProtection(t1)) {
    SUPLA_LOG_DEBUG("HVAC: check overheat protection exit");
    channel.setHvacFlagError(false);
    return;
  }

  if (checkAntifreezeProtection(t1)) {
    SUPLA_LOG_DEBUG("HVAC: check antifreeze protection exit");
    channel.setHvacFlagError(false);
    return;
  }

  if (checkAuxProtection(t2)) {
    SUPLA_LOG_DEBUG("HVAC: check heater/cooler protection exit");
    channel.setHvacFlagError(false);
    return;
  }

  switch (channel.getHvacMode()) {
    case SUPLA_HVAC_MODE_AUTO: {
      int heatNewOutputValue =
          evaluateOutputValue(t1, getTemperatureSetpointMin());
      int coolNewOutputValue =
          evaluateOutputValue(t1, getTemperatureSetpointMax());
      if (heatNewOutputValue > 0) {
        setOutput(heatNewOutputValue, false);
      } else if (coolNewOutputValue < 0) {
        setOutput(coolNewOutputValue, false);
      } else {
        setOutput(0, false);
      }

      break;
    }
    case SUPLA_HVAC_MODE_HEAT: {
      int newOutputValue = evaluateOutputValue(t1, getTemperatureSetpointMin());
      if (newOutputValue < 0) {
        newOutputValue = 0;
      }
      setOutput(newOutputValue, false);
      break;
    }
    case SUPLA_HVAC_MODE_COOL: {
      int newOutputValue = evaluateOutputValue(t1, getTemperatureSetpointMax());
      if (newOutputValue > 0) {
        newOutputValue = 0;
      }
      setOutput(newOutputValue, false);
      break;
    }
    /*
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL: {
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_DRYER:
    case SUPLA_CHANNELFNC_HVAC_FAN: {
      // not implemented yet
      break;
    }
     */
    default: {
      break;
    }
  }
}

void HvacBase::setHeatingSupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_HEAT);
  } else {
    channel.setFuncList(channel.getFuncList() & ~SUPLA_HVAC_CAP_FLAG_MODE_HEAT);
  }
}

void HvacBase::setCoolingSupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_COOL);
  } else {
    channel.setFuncList(channel.getFuncList() & ~SUPLA_HVAC_CAP_FLAG_MODE_COOL);
  }
}

void HvacBase::setAutoSupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
    setHeatingSupported(true);
    setCoolingSupported(true);
  } else {
    channel.setFuncList(channel.getFuncList() & ~SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
  }
}

void HvacBase::setFanSupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_FAN);
  } else {
    channel.setFuncList(channel.getFuncList() & ~SUPLA_HVAC_CAP_FLAG_MODE_FAN);
  }
}

void HvacBase::setDrySupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_DRY);
  } else {
    channel.setFuncList(channel.getFuncList() & ~SUPLA_HVAC_CAP_FLAG_MODE_DRY);
  }
}

void HvacBase::setOnOffSupported(bool supported) {
  if (supported) {
    channel.setFuncList(channel.getFuncList() | SUPLA_HVAC_CAP_FLAG_MODE_ONOFF);
  } else {
    channel.setFuncList(channel.getFuncList() &
                        ~SUPLA_HVAC_CAP_FLAG_MODE_ONOFF);
  }
}

bool HvacBase::isOnOffSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_ONOFF;
}

bool HvacBase::isHeatingSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_HEAT;
}

bool HvacBase::isCoolingSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_COOL;
}

bool HvacBase::isAutoSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_AUTO;
}

bool HvacBase::isFanSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_FAN;
}

bool HvacBase::isDrySupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_MODE_DRY;
}

uint8_t HvacBase::handleChannelConfig(TSD_ChannelConfig *newConfig) {
  if (waitForChannelConfigAndIgnoreIt) {
    SUPLA_LOG_INFO("Ignoring config for channel %d", getChannelNumber());
    waitForChannelConfigAndIgnoreIt = false;
    return SUPLA_CONFIG_RESULT_TRUE;
  }

  if (newConfig == nullptr) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  if (newConfig->ConfigType != SUPLA_CONFIG_TYPE_DEFAULT) {
    return SUPLA_CONFIG_RESULT_TYPE_NOT_SUPPORTED;
  }

  auto channelFunction = newConfig->Func;
  if (!isFunctionSupported(channelFunction)) {
    return SUPLA_CONFIG_RESULT_FUNCTION_NOT_SUPPORTED;
  }

  if (newConfig->ConfigSize < sizeof(TSD_ChannelConfig_HVAC)) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  auto hvacConfig =
      reinterpret_cast<TSD_ChannelConfig_HVAC *>(newConfig->Config);

  if (!isConfigValid(hvacConfig)) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  TSD_ChannelConfig_HVAC configCopy;
  memcpy(&configCopy, &config, sizeof(TSD_ChannelConfig_HVAC));

  // Received config looks ok, so we apply it to channel
  changeFunction(channelFunction, false);

  // We don't use setters here, because they run validation againsted current
  // configuration, which may fail. However new config was already validated
  // so we assign them directly.
  config.MainThermometerChannelNo = hvacConfig->MainThermometerChannelNo;
  config.AuxThermometerChannelNo =
      hvacConfig->AuxThermometerChannelNo;
  config.AuxThermometerType = hvacConfig->AuxThermometerType;
  config.EnableAntiFreezeAndOverheatProtection =
      hvacConfig->EnableAntiFreezeAndOverheatProtection;
  config.UsedAlgorithm = hvacConfig->UsedAlgorithm;
  config.MinOnTimeS = hvacConfig->MinOnTimeS;
  config.MinOffTimeS = hvacConfig->MinOffTimeS;

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures, TEMPERATURE_ECO)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_ECO,
                           getTemperatureEco(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_COMFORT)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_COMFORT,
                           getTemperatureComfort(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures, TEMPERATURE_BOOST)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_BOOST,
                           getTemperatureBoost(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_FREEZE_PROTECTION)) {
    setTemperatureInStruct(
        &config.Temperatures,
        TEMPERATURE_FREEZE_PROTECTION,
        getTemperatureFreezeProtection(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_HEAT_PROTECTION)) {
    setTemperatureInStruct(
        &config.Temperatures,
        TEMPERATURE_HEAT_PROTECTION,
        getTemperatureHeatProtection(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_HISTERESIS)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_HISTERESIS,
                           getTemperatureHisteresis(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_BELOW_ALARM)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_BELOW_ALARM,
                           getTemperatureBelowAlarm(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_ABOVE_ALARM)) {
    setTemperatureInStruct(&config.Temperatures,
                           TEMPERATURE_ABOVE_ALARM,
                           getTemperatureAboveAlarm(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_AUX_MIN_SETPOINT)) {
    setTemperatureInStruct(
        &config.Temperatures,
        TEMPERATURE_AUX_MIN_SETPOINT,
        getTemperatureAuxMinSetpoint(&hvacConfig->Temperatures));
  }

  if (isTemperatureSetInStruct(&hvacConfig->Temperatures,
                               TEMPERATURE_AUX_MAX_SETPOINT)) {
    setTemperatureInStruct(
        &config.Temperatures,
        TEMPERATURE_AUX_MAX_SETPOINT,
        getTemperatureAuxMaxSetpoint(&hvacConfig->Temperatures));
  }

  if (memcmp(&config, &configCopy, sizeof(TSD_ChannelConfig_HVAC)) != 0) {
    saveConfig();
  }

  return SUPLA_CONFIG_RESULT_TRUE;
}

bool HvacBase::isConfigValid(TSD_ChannelConfig_HVAC *newConfig) const {
  if (newConfig == nullptr) {
    return false;
  }

  // main thermometer is mandatory and has to be set to a local thermometer
  if (!isChannelThermometer(newConfig->MainThermometerChannelNo)) {
    return false;
  }

  // heater cooler thermometer is optional, but if set, it has to be set to a
  // local thermometer
  if (newConfig->AuxThermometerType !=
      SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET) {
    if (!isChannelThermometer(newConfig->AuxThermometerChannelNo)) {
      return false;
    }
    if (newConfig->AuxThermometerChannelNo ==
        newConfig->MainThermometerChannelNo) {
      return false;
    }
  }

  switch (newConfig->AuxThermometerType) {
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET:
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_DISABLED:
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_FLOOR:
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_WATER:
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_GENERIC_COOLER:
    case SUPLA_HVAC_AUX_THERMOMETER_TYPE_GENERIC_HEATER:
      break;
    default:
      return false;
  }

  if (!isAlgorithmValid(newConfig->UsedAlgorithm)) {
    SUPLA_LOG_WARNING("HVAC: invalid algorithm %d", newConfig->UsedAlgorithm);
    return false;
  }

  if (!isMinOnOffTimeValid(newConfig->MinOnTimeS)) {
    SUPLA_LOG_WARNING("HVAC: invalid min on time %d", newConfig->MinOnTimeS);
    return false;
  }

  if (!isMinOnOffTimeValid(newConfig->MinOffTimeS)) {
    SUPLA_LOG_WARNING("HVAC: invalid min off time %d", newConfig->MinOffTimeS);
    return false;
  }

  if (!areTemperaturesValid(&newConfig->Temperatures)) {
    SUPLA_LOG_WARNING("HVAC: invalid temperatures");
    return false;
  }

  return true;
}

bool HvacBase::areTemperaturesValid(
    const THVACTemperatureCfg *temperatures) const {
  if (temperatures == nullptr) {
    return false;
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_FREEZE_PROTECTION)) {
    if (!isTemperatureFreezeProtectionValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid freeze protection temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_HEAT_PROTECTION)) {
    if (!isTemperatureHeatProtectionValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid heat protection temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_ECO)) {
    if (!isTemperatureEcoValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid eco temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_COMFORT)) {
    if (!isTemperatureComfortValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid comfort temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_BOOST)) {
    if (!isTemperatureBoostValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid boost temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_HISTERESIS)) {
    if (!isTemperatureHisteresisValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid histeresis value");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_BELOW_ALARM)) {
    if (!isTemperatureBelowAlarmValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid below alarm temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures, TEMPERATURE_ABOVE_ALARM)) {
    if (!isTemperatureAboveAlarmValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid above alarm temperature");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures,
                               TEMPERATURE_AUX_MIN_SETPOINT)) {
    if (!isTemperatureAuxMinSetpointValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid heater cooler min setpoint");
      return false;
    }
  }

  if (isTemperatureSetInStruct(temperatures,
                               TEMPERATURE_AUX_MAX_SETPOINT)) {
    if (!isTemperatureAuxMaxSetpointValid(temperatures)) {
      SUPLA_LOG_WARNING("HVAC: invalid heater cooler max setpoint");
      return false;
    }
  }

  return true;
}

bool HvacBase::isTemperatureSetInStruct(const THVACTemperatureCfg *temperatures,
                                        unsigned _supla_int_t index) {
  if (temperatures == nullptr) {
    return false;
  }

  if (index == 0) {
    return false;
  }

  return (temperatures->Index & index) == index;
}

_supla_int16_t HvacBase::getTemperatureFromStruct(
    const THVACTemperatureCfg *temperatures, unsigned _supla_int_t index) {
  if (!isTemperatureSetInStruct(temperatures, index)) {
    return SUPLA_TEMPERATURE_INVALID_INT16;
  }

  // convert index bit number to array index
  int arrayIndex = 0;
  for (; arrayIndex < 24; arrayIndex++) {
    if (index & (1 << arrayIndex)) {
      break;
    }
  }

  return temperatures->Temperature[arrayIndex];
}

bool HvacBase::isTemperatureInRoomConstrain(_supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  auto tMin =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MIN);
  auto tMax =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MAX);

  return temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureInAutoConstrain(_supla_int16_t tMin,
                                            _supla_int16_t tMax) const {
  auto offsetMin = getTemperatureAutoOffsetMin();
  auto offsetMax = getTemperatureAutoOffsetMax();
  return (tMax - tMin >= offsetMin) && (tMax - tMin <= offsetMax) &&
         isTemperatureInRoomConstrain(tMin) &&
         isTemperatureInRoomConstrain(tMax);
}

bool HvacBase::isTemperatureInAuxConstrain(
    _supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  auto tMin = getTemperatureFromStruct(&config.Temperatures,
                                       TEMPERATURE_AUX_MIN);
  auto tMax = getTemperatureFromStruct(&config.Temperatures,
                                       TEMPERATURE_AUX_MAX);

  return temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureFreezeProtectionValid(
    _supla_int16_t temperature) const {
  return isTemperatureInRoomConstrain(temperature);
}

bool HvacBase::isTemperatureFreezeProtectionValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t =
      getTemperatureFromStruct(temperatures, TEMPERATURE_FREEZE_PROTECTION);
  return isTemperatureFreezeProtectionValid(t);
}

bool HvacBase::isTemperatureHeatProtectionValid(
    _supla_int16_t temperature) const {
  return isTemperatureInRoomConstrain(temperature);
}

bool HvacBase::isTemperatureHeatProtectionValid(
    const THVACTemperatureCfg *temperatures) const {
  auto  t = getTemperatureFromStruct(temperatures, TEMPERATURE_HEAT_PROTECTION);
  return isTemperatureInRoomConstrain(t);
}

bool HvacBase::isTemperatureEcoValid(_supla_int16_t temperature) const {
  return isTemperatureInRoomConstrain(temperature);
}

bool HvacBase::isTemperatureEcoValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureFromStruct(temperatures, TEMPERATURE_ECO);
  return isTemperatureInRoomConstrain(t);
}

bool HvacBase::isTemperatureComfortValid(_supla_int16_t temperature) const {
  return isTemperatureInRoomConstrain(temperature);
}

bool HvacBase::isTemperatureComfortValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureFromStruct(temperatures, TEMPERATURE_COMFORT);
  return isTemperatureInRoomConstrain(t);
}

bool HvacBase::isTemperatureBoostValid(_supla_int16_t temperature) const {
  return isTemperatureInRoomConstrain(temperature);
}

bool HvacBase::isTemperatureBoostValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureFromStruct(temperatures, TEMPERATURE_BOOST);
  return isTemperatureInRoomConstrain(t);
}

bool HvacBase::isTemperatureHisteresisValid(_supla_int16_t hist) const {
  if (hist == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  auto histMin = getTemperatureFromStruct(&config.Temperatures,
                                          TEMPERATURE_HISTERESIS_MIN);
  auto histMax = getTemperatureFromStruct(&config.Temperatures,
                                          TEMPERATURE_HISTERESIS_MAX);

  return hist >= histMin && hist <= histMax;
}

bool HvacBase::isTemperatureHisteresisValid(
    const THVACTemperatureCfg *temperatures) const {
  auto hist = getTemperatureFromStruct(temperatures, TEMPERATURE_HISTERESIS);
  return isTemperatureHisteresisValid(hist);
}

bool HvacBase::isTemperatureAuxMinSetpointValid(
    _supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  auto tMin = getTemperatureAuxMin();
  auto tMax = getTemperatureAuxMax();

  auto tMaxSetpoint = getTemperatureAuxMaxSetpoint();

  return (temperature < tMaxSetpoint ||
          tMaxSetpoint == SUPLA_TEMPERATURE_INVALID_INT16) &&
         temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureAuxMinSetpointValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureAuxMinSetpoint(temperatures);
  if (isTemperatureSetInStruct(temperatures,
                               TEMPERATURE_AUX_MAX_SETPOINT)) {
    auto tMaxSetpoint = getTemperatureAuxMaxSetpoint(temperatures);
    if (t >= tMaxSetpoint) {
      return false;
    }
  }

  auto tMin = getTemperatureAuxMin();
  auto tMax = getTemperatureAuxMax();

  return t >= tMin && t <= tMax;
}

bool HvacBase::isTemperatureAuxMaxSetpointValid(
    _supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  // min and max values are taken from device config
  auto tMin = getTemperatureAuxMin();
  auto tMax = getTemperatureAuxMax();

  // current min setpoint is taken from current device config
  auto tMinSetpoint = getTemperatureAuxMinSetpoint();

  return (temperature > tMinSetpoint ||
          tMinSetpoint == SUPLA_TEMPERATURE_INVALID_INT16) &&
         temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureAuxMaxSetpointValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureAuxMaxSetpoint(temperatures);

  // min setpoint is taken from configuration in temperatures struct
  // i.e. new values send from server
  if (isTemperatureSetInStruct(temperatures,
                               TEMPERATURE_AUX_MIN_SETPOINT)) {
    auto tMinSetpoint = getTemperatureAuxMinSetpoint(temperatures);
    if (t <= tMinSetpoint) {
      return false;
    }
  }

  // min and max range is validated against device config (readonly values)
  auto tMin = getTemperatureAuxMin();
  auto tMax = getTemperatureAuxMax();

  return t >= tMin && t <= tMax;
}

bool HvacBase::isTemperatureBelowAlarmValid(_supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }

  auto tMin =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MIN);
  auto tMax =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MAX);

  return temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureBelowAlarmValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureFromStruct(temperatures, TEMPERATURE_BELOW_ALARM);
  return isTemperatureBelowAlarmValid(t);
}

bool HvacBase::isTemperatureAboveAlarmValid(_supla_int16_t temperature) const {
  if (temperature == SUPLA_TEMPERATURE_INVALID_INT16) {
    return false;
  }
  auto tMin =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MIN);
  auto tMax =
      getTemperatureFromStruct(&config.Temperatures, TEMPERATURE_ROOM_MAX);

  return temperature >= tMin && temperature <= tMax;
}

bool HvacBase::isTemperatureAboveAlarmValid(
    const THVACTemperatureCfg *temperatures) const {
  auto t = getTemperatureFromStruct(temperatures, TEMPERATURE_ABOVE_ALARM);
  return isTemperatureAboveAlarmValid(t);
}

bool HvacBase::isChannelThermometer(uint8_t channelNo) const {
  auto element = Supla::Element::getElementByChannelNumber(channelNo);
  if (element == nullptr) {
    SUPLA_LOG_WARNING("HVAC: thermometer not found for channel %d", channelNo);
    return false;
  }
  auto elementType = element->getChannel()->getChannelType();
  switch (elementType) {
    case SUPLA_CHANNELTYPE_HUMIDITYANDTEMPSENSOR:
    case SUPLA_CHANNELTYPE_THERMOMETER:
      break;
    default:
      SUPLA_LOG_WARNING("HVAC: thermometer channel %d has invalid type %d",
          channelNo, elementType);
      return false;
  }
  return true;
}

bool HvacBase::isAlgorithmValid(unsigned _supla_int16_t algorithm) const {
  return (config.AlgorithmCaps & algorithm) == algorithm;
}

uint8_t HvacBase::handleWeeklySchedule(TSD_ChannelConfig *newConfig) {
  if (waitForWeeklyScheduleAndIgnoreIt) {
    SUPLA_LOG_INFO("Ignoring weekly schedule for channel %d",
                   getChannelNumber());
    waitForWeeklyScheduleAndIgnoreIt = false;
    return SUPLA_CONFIG_RESULT_TRUE;
  }

  if (newConfig == nullptr) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  if (newConfig->ConfigSize < sizeof(TSD_ChannelConfig_WeeklySchedule)) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  auto newSchedule =
      reinterpret_cast<TSD_ChannelConfig_WeeklySchedule *>(newConfig->Config);

  if (!isWeeklyScheduleValid(newSchedule)) {
    return SUPLA_CONFIG_RESULT_DATA_ERROR;
  }

  if (!isWeeklyScheduleConfigured || memcmp(&weeklySchedule,
             newSchedule,
             sizeof(TSD_ChannelConfig_WeeklySchedule)) != 0) {
    memcpy(&weeklySchedule,
           newSchedule,
           sizeof(TSD_ChannelConfig_WeeklySchedule));
    saveWeeklySchedule();
  }

  return SUPLA_RESULT_TRUE;
}

bool HvacBase::isFunctionSupported(_supla_int_t channelFunction) const {
  switch (channelFunction) {
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT:
      return isHeatingSupported();
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL:
      return isCoolingSupported();
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO:
      return isAutoSupported();
    case SUPLA_CHANNELFNC_HVAC_FAN:
      return isFanSupported();
    case SUPLA_CHANNELFNC_HVAC_DRYER:
      return isDrySupported();
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL:
      return isDifferentialFunctionSupported();
  }
  return false;
}

void HvacBase::addAlgorithmCap(unsigned _supla_int16_t algorithm) {
  config.AlgorithmCaps |= algorithm;
}

void HvacBase::setTemperatureInStruct(THVACTemperatureCfg *temperatures,
                                      unsigned _supla_int_t index,
                                      _supla_int16_t temperature) {
  if (temperatures == nullptr) {
    return;
  }

  // convert index bit number to array index
  int arrayIndex = 0;
  for (; arrayIndex < 24; arrayIndex++) {
    if (index & (1 << arrayIndex)) {
      break;
    }
  }

  if (arrayIndex >= 24) {
    return;
  }

  temperatures->Index |= index;
  temperatures->Temperature[arrayIndex] = temperature;
}

void HvacBase::setTemperatureRoomMin(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_ROOM_MIN, temperature);
}

void HvacBase::setTemperatureRoomMax(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_ROOM_MAX, temperature);
}

void HvacBase::setTemperatureAuxMin(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_AUX_MIN, temperature);
}

void HvacBase::setTemperatureAuxMax(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_AUX_MAX, temperature);
}

void HvacBase::setTemperatureHisteresisMin(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_HISTERESIS_MIN, temperature);
}

void HvacBase::setTemperatureHisteresisMax(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_HISTERESIS_MAX, temperature);
}

void HvacBase::setTemperatureAutoOffsetMin(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_AUTO_OFFSET_MIN, temperature);
}

void HvacBase::setTemperatureAutoOffsetMax(_supla_int16_t temperature) {
  setTemperatureInStruct(
      &config.Temperatures, TEMPERATURE_AUTO_OFFSET_MAX, temperature);
}


bool HvacBase::setTemperatureFreezeProtection(_supla_int16_t temperature) {
  if (!isTemperatureFreezeProtectionValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureFreezeProtection()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_FREEZE_PROTECTION, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureHeatProtection(_supla_int16_t temperature) {
  if (!isTemperatureHeatProtectionValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureHeatProtection()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_HEAT_PROTECTION, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureEco(_supla_int16_t temperature) {
  if (!isTemperatureEcoValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureEco()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_ECO, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureComfort(_supla_int16_t temperature) {
  if (!isTemperatureComfortValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureComfort()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_COMFORT, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureBoost(_supla_int16_t temperature) {
  if (!isTemperatureBoostValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureBoost()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_BOOST, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureHisteresis(_supla_int16_t temperature) {
  if (!isTemperatureHisteresisValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureHisteresis()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_HISTERESIS, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureBelowAlarm(_supla_int16_t temperature) {
  if (!isTemperatureBelowAlarmValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureBelowAlarm()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_BELOW_ALARM, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureAboveAlarm(_supla_int16_t temperature) {
  if (!isTemperatureAboveAlarmValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureAboveAlarm()) {
    setTemperatureInStruct(
        &config.Temperatures, TEMPERATURE_ABOVE_ALARM, temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureAuxMinSetpoint(
    _supla_int16_t temperature) {
  if (!isTemperatureAuxMinSetpointValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureAuxMinSetpoint()) {
    setTemperatureInStruct(&config.Temperatures,
        TEMPERATURE_AUX_MIN_SETPOINT,
        temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

bool HvacBase::setTemperatureAuxMaxSetpoint(
    _supla_int16_t temperature) {
  if (!isTemperatureAuxMaxSetpointValid(temperature)) {
    return false;
  }
  if (temperature != getTemperatureAuxMaxSetpoint()) {
    setTemperatureInStruct(&config.Temperatures,
        TEMPERATURE_AUX_MAX_SETPOINT,
        temperature);
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
  return true;
}

_supla_int16_t HvacBase::getTemperatureRoomMin(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_ROOM_MIN);
}

_supla_int16_t HvacBase::getTemperatureRoomMax(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_ROOM_MAX);
}

_supla_int16_t HvacBase::getTemperatureAuxMin(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_AUX_MIN);
}

_supla_int16_t HvacBase::getTemperatureAuxMax(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_AUX_MAX);
}
_supla_int16_t HvacBase::getTemperatureHisteresisMin(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_HISTERESIS_MIN);
}
_supla_int16_t HvacBase::getTemperatureHisteresisMax(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_HISTERESIS_MAX);
}
_supla_int16_t HvacBase::getTemperatureAutoOffsetMin(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_AUTO_OFFSET_MIN);
}
_supla_int16_t HvacBase::getTemperatureAutoOffsetMax(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_AUTO_OFFSET_MAX);
}

_supla_int16_t HvacBase::getTemperatureRoomMin() const {
  return getTemperatureRoomMin(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureRoomMax() const {
  return getTemperatureRoomMax(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAuxMin() const {
  return getTemperatureAuxMin(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAuxMax() const {
  return getTemperatureAuxMax(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureHisteresisMin() const {
  return getTemperatureHisteresisMin(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureHisteresisMax() const {
  return getTemperatureHisteresisMax(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAutoOffsetMin() const {
  return getTemperatureAutoOffsetMin(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAutoOffsetMax() const {
  return getTemperatureAutoOffsetMax(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureFreezeProtection(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_FREEZE_PROTECTION);
}

_supla_int16_t HvacBase::getTemperatureHeatProtection(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_HEAT_PROTECTION);
}

_supla_int16_t HvacBase::getTemperatureEco(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_ECO);
}

_supla_int16_t HvacBase::getTemperatureComfort(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_COMFORT);
}

_supla_int16_t HvacBase::getTemperatureBoost(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_BOOST);
}

_supla_int16_t HvacBase::getTemperatureHisteresis(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_HISTERESIS);
}

_supla_int16_t HvacBase::getTemperatureBelowAlarm(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_BELOW_ALARM);
}

_supla_int16_t HvacBase::getTemperatureAboveAlarm(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures, TEMPERATURE_ABOVE_ALARM);
}

_supla_int16_t HvacBase::getTemperatureAuxMinSetpoint(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures,
                                  TEMPERATURE_AUX_MIN_SETPOINT);
}

_supla_int16_t HvacBase::getTemperatureAuxMaxSetpoint(
    const THVACTemperatureCfg *temperatures) const {
  return getTemperatureFromStruct(temperatures,
                                  TEMPERATURE_AUX_MAX_SETPOINT);
}

_supla_int16_t HvacBase::getTemperatureFreezeProtection() const {
  return getTemperatureFreezeProtection(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureHeatProtection() const {
  return getTemperatureHeatProtection(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureEco() const {
  return getTemperatureEco(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureComfort() const {
  return getTemperatureComfort(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureBoost() const {
  return getTemperatureBoost(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureHisteresis() const {
  return getTemperatureHisteresis(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureBelowAlarm() const {
  return getTemperatureBelowAlarm(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAboveAlarm() const {
  return getTemperatureAboveAlarm(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAuxMinSetpoint() const {
  return getTemperatureAuxMinSetpoint(&config.Temperatures);
}

_supla_int16_t HvacBase::getTemperatureAuxMaxSetpoint() const {
  return getTemperatureAuxMaxSetpoint(&config.Temperatures);
}

bool HvacBase::setUsedAlgorithm(unsigned _supla_int16_t newAlgorithm) {
  if (isAlgorithmValid(newAlgorithm)) {
    if (config.UsedAlgorithm != newAlgorithm) {
      config.UsedAlgorithm = newAlgorithm;
      if (initDone) {
        channelConfigChangedOffline = 1;
        saveConfig();
      }
    }
    return true;
  }
  return false;
}

unsigned _supla_int16_t HvacBase::getUsedAlgorithm() const {
  return config.UsedAlgorithm;
}

bool HvacBase::setMainThermometerChannelNo(uint8_t channelNo) {
  if (isChannelThermometer(channelNo)) {
    if (getAuxThermometerType() !=
        SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET) {
      if (channelNo == getAuxThermometerChannelNo()) {
        return false;
      }
    }
    if (config.MainThermometerChannelNo != channelNo) {
      config.MainThermometerChannelNo = channelNo;
      if (initDone) {
        channelConfigChangedOffline = 1;
        saveConfig();
      }
    }
    return true;
  }
  return false;
}

uint8_t HvacBase::getMainThermometerChannelNo() const {
  return config.MainThermometerChannelNo;
}

bool HvacBase::setAuxThermometerChannelNo(uint8_t channelNo) {
  if (isChannelThermometer(channelNo)) {
    if (getMainThermometerChannelNo() == channelNo) {
      return false;
    }
    if (config.AuxThermometerChannelNo != channelNo) {
      config.AuxThermometerChannelNo = channelNo;
      if (getAuxThermometerType() ==
          SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET) {
        setAuxThermometerType(
            SUPLA_HVAC_AUX_THERMOMETER_TYPE_DISABLED);
        if (initDone) {
          channelConfigChangedOffline = 1;
          saveConfig();
        }
      }
    }
    return true;
  }

  if (getChannelNumber() == channelNo) {
    if (config.AuxThermometerChannelNo != channelNo) {
      config.AuxThermometerChannelNo = channelNo;
      setAuxThermometerType(
          SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET);
      if (initDone) {
        channelConfigChangedOffline = 1;
        saveConfig();
      }
    }
    return true;
  }
  return false;
}

uint8_t HvacBase::getAuxThermometerChannelNo() const {
  return config.AuxThermometerChannelNo;
}

void HvacBase::setAuxThermometerType(uint8_t type) {
  if (config.AuxThermometerType != type) {
    config.AuxThermometerType = type;
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
}

uint8_t HvacBase::getAuxThermometerType() const {
  return config.AuxThermometerType;
}

void HvacBase::setAntiFreezeAndHeatProtectionEnabled(bool enabled) {
  if (config.EnableAntiFreezeAndOverheatProtection != enabled) {
    config.EnableAntiFreezeAndOverheatProtection = enabled;
    if (initDone) {
      channelConfigChangedOffline = 1;
      saveConfig();
    }
  }
}

bool HvacBase::isAntiFreezeAndHeatProtectionEnabled() const {
  auto func = channel.getDefaultFunction();
  switch (func) {
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO:
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT:
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL: {
      return config.EnableAntiFreezeAndOverheatProtection;
    }

    default: {
      return false;
    }
  }
}

bool HvacBase::isMinOnOffTimeValid(uint16_t seconds) const {
  return seconds <= 600;  // TODO(klew): is this range ok? from 1 s to 10 min
                          // 0 - disabled
}

bool HvacBase::setMinOnTimeS(uint16_t seconds) {
  if (isMinOnOffTimeValid(seconds)) {
    if (config.MinOnTimeS != seconds) {
      config.MinOnTimeS = seconds;
      if (initDone) {
        channelConfigChangedOffline = 1;
        saveConfig();
      }
    }
    return true;
  }
  return false;
}

uint16_t HvacBase::getMinOnTimeS() const {
  return config.MinOnTimeS;
}

bool HvacBase::setMinOffTimeS(uint16_t seconds) {
  if (isMinOnOffTimeValid(seconds)) {
    if (config.MinOffTimeS != seconds) {
      config.MinOffTimeS = seconds;
      if (initDone) {
        channelConfigChangedOffline = 1;
        saveConfig();
      }
    }
    return true;
  }
  return false;
}

uint16_t HvacBase::getMinOffTimeS() const {
  return config.MinOffTimeS;
}

void HvacBase::saveConfig() {
  auto cfg = Supla::Storage::ConfigInstance();
  lastConfigChangeTimestampMs = millis();
  if (cfg) {
    // Generic HVAC configuration
    char key[SUPLA_CONFIG_MAX_KEY_SIZE] = {};
    generateKey(key, "hvac_cfg");
    if (cfg->setBlob(key,
                     reinterpret_cast<char *>(&config),
                     sizeof(TSD_ChannelConfig_HVAC))) {
      SUPLA_LOG_INFO("HVAC config saved successfully");
    } else {
      SUPLA_LOG_INFO("HVAC failed to save config");
    }

    generateKey(key, "cfg_chng");
    if (channelConfigChangedOffline) {
     cfg->setUInt8(key, 1);
    } else {
      cfg->setUInt8(key, 0);
    }

    cfg->saveWithDelay(5000);
  }
}

void HvacBase::saveWeeklySchedule() {
  auto cfg = Supla::Storage::ConfigInstance();
  if (cfg) {
    char key[SUPLA_CONFIG_MAX_KEY_SIZE] = {};
    // Weekly schedule configuration
    generateKey(key, "hvac_weekly");
    if (cfg->setBlob(key,
                     reinterpret_cast<char *>(&weeklySchedule),
                     sizeof(TSD_ChannelConfig_WeeklySchedule))) {
      SUPLA_LOG_INFO("HVAC weekly schedule saved successfully");
    } else {
      SUPLA_LOG_INFO("HVAC failed to save weekly schedule");
    }

    generateKey(key, "weekly_chng");
    if (weeklyScheduleChangedOffline) {
      cfg->setUInt8(key, 1);
    } else {
      cfg->setUInt8(key, 0);
    }
    cfg->saveWithDelay(5000);
  }
  isWeeklyScheduleConfigured = true;
}

void HvacBase::handleSetChannelConfigResult(
    TSD_SetChannelConfigResult *result) {
  if (result == nullptr) {
    return;
  }

  bool success = (result->Result == SUPLA_CONFIG_RESULT_TRUE);

  switch (result->ConfigType) {
    case SUPLA_CONFIG_TYPE_DEFAULT: {
      SUPLA_LOG_INFO("HVAC set channel config %s (%d)",
                     success ? "succeeded" : "failed",
                     result->Result);
      clearChannelConfigChangedFlag();
      break;
    }
    case SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE: {
      SUPLA_LOG_INFO("HVAC set weekly schedule config %s (%d)",
                     success ? "succeeded" : "failed",
                     result->Result);
      clearWeeklyScheduleChangedFlag();
      break;
    }
    default:
      break;
  }
}

void HvacBase::clearChannelConfigChangedFlag() {
  if (channelConfigChangedOffline) {
    channelConfigChangedOffline = 0;
    auto cfg = Supla::Storage::ConfigInstance();
    if (cfg) {
      char key[SUPLA_CONFIG_MAX_KEY_SIZE] = {};
      generateKey(key, "cfg_chng");
      cfg->setUInt8(key, 0);
      cfg->saveWithDelay(1000);
    }
  }
}

void HvacBase::clearWeeklyScheduleChangedFlag() {
  if (weeklyScheduleChangedOffline) {
    weeklyScheduleChangedOffline = 0;
    auto cfg = Supla::Storage::ConfigInstance();
    if (cfg) {
      char key[SUPLA_CONFIG_MAX_KEY_SIZE] = {};
      generateKey(key, "weekly_chng");
      cfg->setUInt8(key, 0);
      cfg->saveWithDelay(1000);
    }
  }
}

bool HvacBase::isWeeklyScheduleValid(
    TSD_ChannelConfig_WeeklySchedule *newSchedule) const {
  bool programIsUsed[SUPLA_WEEKLY_SCHEDULE_PROGRAMS_MAX_SIZE] = {};

  // check if programs are valid
  for (int i = 0; i < SUPLA_WEEKLY_SCHEDULE_PROGRAMS_MAX_SIZE; i++) {
    if (!isProgramValid(newSchedule->Program[i])) {
      return false;
    }
    if (newSchedule->Program[i].Mode != SUPLA_HVAC_MODE_NOT_SET) {
      programIsUsed[i] = true;
    }
  }

  // check if only used programs are configured in the schedule
  for (int i = 0; i < SUPLA_WEEKLY_SCHEDULE_VALUES_SIZE; i++) {
    int programId = getWeeklyScheduleProgramId(newSchedule, i);
    if (programId != 0 && !programIsUsed[programId - 1]) {
      return false;
    }
  }

  return true;
}

int HvacBase::getWeeklyScheduleProgramId(
    const TSD_ChannelConfig_WeeklySchedule *schedule, int index) const {
  if (schedule == nullptr) {
    return -1;
  }
  if (index < 0 || index > SUPLA_WEEKLY_SCHEDULE_VALUES_SIZE) {
    return -1;
  }

  return (schedule->Value[index / 2] >> (index % 2 * 4)) & 0xF;
}

int HvacBase::getWeeklyScheduleProgramId(int index) const {
  if (!isWeeklyScheduleConfigured) {
    return -1;
  }
  return getWeeklyScheduleProgramId(&weeklySchedule, index);
}

int HvacBase::getWeeklyScheduleProgramId(enum DayOfWeek dayOfWeek,
                                         int hour,
                                         int quarter) const {
  return getWeeklyScheduleProgramId(calculateIndex(dayOfWeek, hour, quarter));
}

int HvacBase::calculateIndex(enum DayOfWeek dayOfWeek,
                             int hour,
                             int quarter) const {
  if (dayOfWeek < DayOfWeek_Sunday || dayOfWeek > DayOfWeek_Saturday) {
    return -1;
  }
  if (hour < 0 || hour > 23) {
    return -1;
  }
  if (quarter < 0 || quarter > 3) {
    return -1;
  }

  return (dayOfWeek * 24 + hour) * 4 + quarter;
}

bool HvacBase::isProgramValid(const TWeeklyScheduleProgram &program) const {
  if (program.Mode == SUPLA_HVAC_MODE_NOT_SET) {
    return true;
  }

  if (program.Mode != SUPLA_HVAC_MODE_COOL
      && program.Mode != SUPLA_HVAC_MODE_HEAT
      && program.Mode != SUPLA_HVAC_MODE_AUTO) {
    return false;
  }

  if (!isModeSupported(program.Mode)) {
    return false;
  }

  // check tempreatures
  switch (program.Mode) {
    case SUPLA_HVAC_MODE_HEAT: {
      return isSetpointMinTemperatureValid(program.SetpointTemperatureMin);
    }
    case SUPLA_HVAC_MODE_COOL: {
      return isTemperatureInRoomConstrain(program.SetpointTemperatureMax);
    }
    case SUPLA_HVAC_MODE_AUTO: {
      return isTemperatureInAutoConstrain(program.SetpointTemperatureMin,
                                          program.SetpointTemperatureMax);
    }
    default: {
      return false;
    }
  }

  return true;
}

bool HvacBase::isModeSupported(int mode) const {
  bool heatSupported = false;
  bool coolSupported = false;
  bool autoSupported = false;
  bool drySupported = false;
  bool fanSupported = isFanSupported();
  bool onOffSupported = isOnOffSupported();

  switch (channel.getDefaultFunction()) {
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT: {
      heatSupported = true;
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL: {
      coolSupported = true;
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO: {
      autoSupported = true;
      heatSupported = true;
      coolSupported = true;
      drySupported = isDrySupported();
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_DRYER: {
      drySupported = true;
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_FAN: {
      fanSupported = true;
      break;
    }
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL: {
      heatSupported = true;
    }
  }

  switch (mode) {
    case SUPLA_HVAC_MODE_HEAT: {
      return heatSupported;
    }
    case SUPLA_HVAC_MODE_COOL: {
      return coolSupported;
    }
    case SUPLA_HVAC_MODE_AUTO: {
      return autoSupported;
    }
    case SUPLA_HVAC_MODE_FAN_ONLY: {
      return fanSupported;
    }
    case SUPLA_HVAC_MODE_DRY: {
      return drySupported;
    }
    case SUPLA_HVAC_MODE_CMD_TURN_ON:
    case SUPLA_HVAC_MODE_OFF: {
      return onOffSupported;
    }
    case SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE: {
      return isWeeklyScheduleConfigured;
    }
  }

  return false;
}

bool HvacBase::setWeeklySchedule(int index, int programId) {
  if (index < 0 || index >= SUPLA_WEEKLY_SCHEDULE_VALUES_SIZE) {
    return false;
  }
  if (programId < 0 || programId > SUPLA_WEEKLY_SCHEDULE_PROGRAMS_MAX_SIZE) {
    return false;
  }

  if (programId > 0 &&
      (weeklySchedule.Program[programId - 1].Mode == SUPLA_HVAC_MODE_NOT_SET ||
      weeklySchedule.Program[programId - 1].Mode > SUPLA_HVAC_MODE_DRY)) {
    return false;
  }

  if (index % 2) {
    weeklySchedule.Value[index / 2] =
        (weeklySchedule.Value[index / 2] & 0x0F) | (programId << 4);
  } else {
    weeklySchedule.Value[index / 2] =
        (weeklySchedule.Value[index / 2] & 0xF0) | programId;
  }

  if (initDone) {
    weeklyScheduleChangedOffline = 1;
    saveWeeklySchedule();
  }

  isWeeklyScheduleConfigured = true;
  return true;
}

bool HvacBase::setWeeklySchedule(enum DayOfWeek dayOfWeek,
                                 int hour,
                                 int quarter,
                                 int programId) {
  return setWeeklySchedule(calculateIndex(dayOfWeek, hour, quarter), programId);
}

TWeeklyScheduleProgram HvacBase::getProgram(int programId) const {
  if (programId < 1 || programId > SUPLA_WEEKLY_SCHEDULE_PROGRAMS_MAX_SIZE) {
    TWeeklyScheduleProgram program = {};
    return program;
  }

  return weeklySchedule.Program[programId - 1];
}

bool HvacBase::setProgram(int programId,
                          unsigned char mode,
                          _supla_int16_t tMin,
                          _supla_int16_t tMax) {
  TWeeklyScheduleProgram program = {mode, {tMin}, {tMax}};

  if (!isProgramValid(program)) {
    return false;
  }

  weeklySchedule.Program[programId - 1].Mode = mode;
  weeklySchedule.Program[programId - 1].SetpointTemperatureMin = tMin;
  weeklySchedule.Program[programId - 1].SetpointTemperatureMax = tMax;

  if (initDone) {
    weeklyScheduleChangedOffline = 1;
    saveWeeklySchedule();
  }
  return true;
}

_supla_int16_t HvacBase::getPrimaryTemp() {
  return getTemperature(config.MainThermometerChannelNo);
}

_supla_int16_t HvacBase::getSecondaryTemp() {
  auto type = getAuxThermometerType();
  if (type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET &&
      type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_DISABLED) {
    return getTemperature(getAuxThermometerChannelNo());
  }

  return INT16_MIN;
  }

_supla_int16_t HvacBase::getTemperature(int channelNo) {
  if (channelNo >= 0 && channelNo != getChannelNumber()) {
    auto el = Supla::Element::getElementByChannelNumber(channelNo);

    if (!el) {
      return INT16_MIN;
    }

    double temp = el->getChannel()->getLastTemperature();
    if (temp <= TEMPERATURE_NOT_AVAILABLE) {
      return INT16_MIN;
    }
    temp *= 100;
    if (temp > INT16_MAX) {
      return INT16_MAX;
    }
    if (temp <= INT16_MIN) {
      return INT16_MIN + 1;
    }
    return temp;
  }
  return INT16_MIN;
}

void HvacBase::setOutput(int value, bool force) {
  if (primaryOutput == nullptr) {
    return;
  }

  if (lastValue == value) {
    return;
  }

  if (lastValue < -100) {
    lastValue = 0;
  }

  bool stateChanged = false;
  // make sure that min on/off time configuration is respected
  if (lastValue > 0 && value <= 0) {
    if (!force && millis() - lastOutputStateChangeTimestampMs <
        config.MinOnTimeS * 1000) {
      return;
    }
    // when output should change from heating to cooling, we add off step
    // between
    value = 0;
    stateChanged = true;
  }

  if (lastValue == 0 && value != 0) {
    if (!force && millis() - lastOutputStateChangeTimestampMs <
        config.MinOffTimeS * 1000) {
      return;
    }
    stateChanged = true;
  }

  if (lastValue < 0 && value >= 0) {
    if (!force && millis() - lastOutputStateChangeTimestampMs <
        config.MinOnTimeS * 1000) {
      return;
    }
    // when output should change from cooling to heating, we add off step
    // between
    value = 0;
    stateChanged = true;
  }

  if (stateChanged) {
    lastOutputStateChangeTimestampMs = millis();
  }
  lastValue = value;

  switch (channel.getDefaultFunction()) {
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT:
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL: {
      channel.setHvacFlagCooling(false);
      if (secondaryOutput) {
        secondaryOutput->setOutputValue(0);
      }

      if (value <= 0) {
        channel.setHvacFlagHeating(false);
        channel.setHvacIsOn(0);
        primaryOutput->setOutputValue(0);
      } else {
        channel.setHvacFlagHeating(true);
        if (primaryOutput->isOnOffOnly()) {
          value = 1;
        }
        channel.setHvacIsOn(value);
        primaryOutput->setOutputValue(value);
      }
      break;
    }

    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL: {
      channel.setHvacFlagHeating(false);
      auto output = primaryOutput;
      if (secondaryOutput) {
        output = secondaryOutput;
        primaryOutput->setOutputValue(0);
      }

      if (value >= 0) {
        channel.setHvacFlagCooling(false);
        channel.setHvacIsOn(0);
        output->setOutputValue(0);
      } else {
        channel.setHvacFlagCooling(true);
        if (primaryOutput->isOnOffOnly()) {
          value = 1;
        } else {
          value = -value;
        }
        channel.setHvacIsOn(value);
        output->setOutputValue(value);
      }
      break;
    }

    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO: {
      if (secondaryOutput == nullptr) {
        return;
      }

      if (value == 0) {
        channel.setHvacFlagCooling(false);
        channel.setHvacFlagHeating(false);
        channel.setHvacIsOn(0);
        primaryOutput->setOutputValue(0);
        secondaryOutput->setOutputValue(0);
      } else if (value >= 1) {
        channel.setHvacFlagCooling(false);
        channel.setHvacFlagHeating(true);
        secondaryOutput->setOutputValue(0);

        if (primaryOutput->isOnOffOnly()) {
          value = 1;
        }
        channel.setHvacIsOn(value);
        primaryOutput->setOutputValue(value);
      } else if (value <= -1) {
        channel.setHvacFlagCooling(true);
        channel.setHvacFlagHeating(false);
        primaryOutput->setOutputValue(0);

        if (secondaryOutput->isOnOffOnly()) {
          value = 1;
        } else {
          value = -value;
        }

        channel.setHvacIsOn(value);
        secondaryOutput->setOutputValue(value);
      }
      break;
    }
  }
}

void HvacBase::setTargetMode(int mode, bool keepScheduleOn) {
  SUPLA_LOG_DEBUG("HVAC: set target mode %d requested", mode);
  if (channel.getHvacMode() == mode) {
    if (!(mode == SUPLA_HVAC_MODE_OFF && !keepScheduleOn &&
          channel.isHvacFlagWeeklySchedule())) {
      return;
    }
  }

  if (isModeSupported(mode)) {
    if (mode == SUPLA_HVAC_MODE_OFF) {
      lastWorkingMode = channel.getHvacMode();
      if (!keepScheduleOn && channel.isHvacFlagWeeklySchedule()) {
        channel.setHvacFlagWeeklySchedule(false);
        lastWorkingMode = SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE;
      }
      channel.setHvacMode(mode);
      setOutput(0, true);
    } else if (mode == SUPLA_HVAC_MODE_CMD_TURN_ON) {
      if (channel.getHvacMode() == SUPLA_HVAC_MODE_OFF ||
          !isModeSupported(channel.getHvacMode())) {
        turnOn();
      }
      keepScheduleOn = true;
    } else if (mode == SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE) {
      keepScheduleOn = true;
      if (!turnOnWeeklySchedlue()) {
        return;
      }
    } else {
      channel.setHvacMode(mode);
    }

    if (!keepScheduleOn) {
      channel.setHvacFlagWeeklySchedule(false);
    }

    fixTemperatureSetpoints();

    lastConfigChangeTimestampMs = millis();
  }
  SUPLA_LOG_DEBUG("HVAC: mode %d", channel.getHvacMode());
}

bool HvacBase::checkAntifreezeProtection(_supla_int16_t t) {
  if (isAntiFreezeAndHeatProtectionEnabled()) {
    auto tFreeze = getTemperatureFreezeProtection();
    if (!isSensorTempValid(tFreeze)) {
      return false;
    }

    auto outputValue = evaluateOutputValue(t, tFreeze);
    if (outputValue > 0) {
      setOutput(outputValue, true);
      return true;
    }
  }
  return false;
}

bool HvacBase::checkOverheatProtection(_supla_int16_t t) {
  if (isAntiFreezeAndHeatProtectionEnabled()) {
    auto tOverheat = getTemperatureHeatProtection();
    if (!isSensorTempValid(tOverheat)) {
      return false;
    }

    auto outputValue = evaluateOutputValue(t, tOverheat);
    if (outputValue < 0) {
      setOutput(outputValue, true);
      return true;
    }
  }
  return false;
}

bool HvacBase::checkAuxProtection(_supla_int16_t t) {
  // heater/cooler protection is available in heat/cool/auto function
  auto func = channel.getDefaultFunction();
  switch (func) {
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT:
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL:
    case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO:
      break;

    default:
      return false;
  }

  auto type = getAuxThermometerType();

  if (type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET &&
      type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_DISABLED) {
    auto tAuxMin = getTemperatureAuxMinSetpoint();
    auto tAuxMax = getTemperatureAuxMaxSetpoint();
    if (isSensorTempValid(tAuxMin)) {
      auto outputValue = evaluateOutputValue(t, tAuxMin);
      if (outputValue > 0) {
        setOutput(outputValue, true);
        return true;
      }
    }

    if (isSensorTempValid(tAuxMax)) {
      auto outputValue = evaluateOutputValue(t, tAuxMax);
      if (outputValue < 0) {
        setOutput(outputValue, true);
        return true;
      }
    }
  }
  return false;
}

void HvacBase::copyFixedChannelConfigTo(HvacBase *hvac) {
  if (hvac == nullptr) {
    return;
  }
  hvac->addAlgorithmCap(config.AlgorithmCaps);
  hvac->setTemperatureRoomMin(getTemperatureRoomMin());
  hvac->setTemperatureRoomMax(getTemperatureRoomMax());
  hvac->setTemperatureAuxMin(getTemperatureAuxMin());
  hvac->setTemperatureAuxMax(getTemperatureAuxMax());
  hvac->setTemperatureHisteresisMin(getTemperatureHisteresisMin());
  hvac->setTemperatureHisteresisMax(getTemperatureHisteresisMax());
  hvac->setTemperatureAutoOffsetMin(getTemperatureAutoOffsetMin());
  hvac->setTemperatureAutoOffsetMax(getTemperatureAutoOffsetMax());
}

int HvacBase::handleNewValueFromServer(TSD_SuplaChannelNewValue *newValue) {
  auto hvacValue = reinterpret_cast<THVACValue *>(newValue);

  if (!isModeSupported(hvacValue->Mode)) {
    return 0;
  }

  int tMin = hvacValue->SetpointTemperatureMin;
  int tMax = hvacValue->SetpointTemperatureMax;

  if (Supla::Channel::isHvacFlagSetpointTemperatureMaxSet(hvacValue)) {
    if (!isTemperatureInRoomConstrain(tMax)) {
      return 0;
    }
  } else {
    tMax = getTemperatureSetpointMax();
  }

  if (Supla::Channel::isHvacFlagSetpointTemperatureMinSet(hvacValue)) {
    if (!isSetpointMinTemperatureValid(tMin)) {
      return 0;
    }
  } else {
    tMin = getTemperatureSetpointMin();
  }


  // auto constrain is verified only when auto mode is requested
  if (hvacValue->Mode == SUPLA_HVAC_MODE_AUTO) {
    if (!isTemperatureInAutoConstrain(tMin, tMax)) {
      return 0;
    }
  }

  uint8_t mode = hvacValue->Mode;
  setTargetMode(mode, false);

  if (mode != SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE) {
    setSetpointTemperaturesForCurrentMode(tMin, tMax);
  }

  // clear flag, so iterateAlawys method will apply new config instantly
  // instead of waiting few seconds
  lastConfigChangeTimestampMs = 0;

  return 1;
}

void HvacBase::setTemperatureSetpointMin(int tMin) {
  if (isSetpointMinTemperatureValid(tMin)) {
    channel.setHvacSetpointTemperatureMin(tMin);
    lastConfigChangeTimestampMs = millis();
  }
}

void HvacBase::setTemperatureSetpointMax(int tMax) {
  if (isTemperatureInRoomConstrain(tMax)) {
    channel.setHvacSetpointTemperatureMax(tMax);
    lastConfigChangeTimestampMs = millis();
  }
}

void HvacBase::clearTemperatureSetpointMin() {
  channel.clearHvacSetpointTemperatureMin();
  lastConfigChangeTimestampMs = millis();
}

void HvacBase::clearTemperatureSetpointMax() {
  channel.clearHvacSetpointTemperatureMax();
  lastConfigChangeTimestampMs = millis();
}

int HvacBase::getTemperatureSetpointMin() {
  if (channel.isHvacFlagSetpointTemperatureMinSet()) {
    return channel.getHvacSetpointTemperatureMin();
  }
  return INT16_MIN;
}

int HvacBase::getTemperatureSetpointMax() {
  if (channel.isHvacFlagSetpointTemperatureMaxSet()) {
    return channel.getHvacSetpointTemperatureMax();
  }
  return INT16_MIN;
}

void HvacBase::turnOn() {
  if (channel.isHvacFlagWeeklySchedule() ||
      lastWorkingMode == SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE) {
    turnOnWeeklySchedlue();
    return;
  }

  uint8_t mode = SUPLA_HVAC_MODE_OFF;
  if (!isModeSupported(lastWorkingMode)) {
    lastWorkingMode = SUPLA_HVAC_MODE_NOT_SET;
  }

  if (lastWorkingMode != SUPLA_HVAC_MODE_NOT_SET) {
    mode = lastWorkingMode;
  } else {
    switch (channel.getDefaultFunction()) {
      case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_HEAT: {
        mode = SUPLA_HVAC_MODE_HEAT;
        break;
      }
      case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_COOL: {
        mode = SUPLA_HVAC_MODE_COOL;
        break;
      }
      case SUPLA_CHANNELFNC_HVAC_THERMOSTAT_AUTO: {
        mode = SUPLA_HVAC_MODE_AUTO;
        break;
      }
    }
  }
  setTargetMode(mode, false);
}

bool HvacBase::turnOnWeeklySchedlue() {
  if (!isWeeklyScheduleConfigured) {
    return false;
  }

  channel.setHvacFlagWeeklySchedule(true);
  return processWeeklySchedule();
}

bool HvacBase::processWeeklySchedule() {
  if (!channel.isHvacFlagWeeklySchedule()) {
    return false;
  }

  if (!Supla::Clock::IsReady()) {
    setTargetMode(SUPLA_HVAC_MODE_OFF, true);
    channel.setHvacFlagClockError(true);
    return false;
  }

  channel.setHvacFlagClockError(false);
  int programId = getWeeklyScheduleProgramId(Supla::Clock::GetHvacDayOfWeek(),
      Supla::Clock::GetHour(), Supla::Clock::GetQuarter());

  if (programId == 0 || programId > SUPLA_WEEKLY_SCHEDULE_PROGRAMS_MAX_SIZE) {
    setTargetMode(SUPLA_HVAC_MODE_OFF, true);
    return true;
  }

  TWeeklyScheduleProgram program = getProgram(programId);

  setTargetMode(program.Mode, true);
  setSetpointTemperaturesForCurrentMode(program.SetpointTemperatureMin,
      program.SetpointTemperatureMax);
  return true;
}

void HvacBase::setSetpointTemperaturesForCurrentMode(int tMin, int tMax) {
      setTemperatureSetpointMin(tMin);
      setTemperatureSetpointMax(tMax);
      /*
  switch (channel.getHvacMode()) {
    case SUPLA_HVAC_MODE_HEAT: {
      setTemperatureSetpointMin(tMin);
      clearTemperatureSetpointMax();
      break;
    }
    case SUPLA_HVAC_MODE_COOL: {
      setTemperatureSetpointMax(tMax);
      clearTemperatureSetpointMin();
      break;
    }
    case SUPLA_HVAC_MODE_AUTO: {
      setTemperatureSetpointMin(tMin);
      setTemperatureSetpointMax(tMax);
      break;
    }
  }
  */
}

void HvacBase::addPrimaryOutput(Supla::Control::OutputInterface *output) {
  primaryOutput = output;

  if (primaryOutput == nullptr) {
    channel.removeFromFuncList(SUPLA_HVAC_CAP_FLAG_MODE_HEAT);
    channel.removeFromFuncList(SUPLA_HVAC_CAP_FLAG_MODE_COOL);
    channel.removeFromFuncList(SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
    channel.removeFromFuncList(SUPLA_HVAC_CAP_FLAG_MODE_ONOFF);
    return;
  }

  channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_MODE_HEAT);
  channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_MODE_COOL);
  channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_MODE_ONOFF);

  if (secondaryOutput != nullptr) {
    channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
  }
}

void HvacBase::addSecondaryOutput(Supla::Control::OutputInterface *output) {
  secondaryOutput = output;

  if (secondaryOutput == nullptr) {
    channel.removeFromFuncList(SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
    return;
  }

  if (primaryOutput != nullptr) {
    channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_MODE_AUTO);
  }
}

bool HvacBase::isSensorTempValid(_supla_int16_t temperature) const {
  return temperature > INT16_MIN;
}

bool HvacBase::checkThermometersStatusForCurrentMode(
    _supla_int16_t t1, _supla_int16_t t2) const {
  auto type = getAuxThermometerType();
  if (type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET &&
      type != SUPLA_HVAC_AUX_THERMOMETER_TYPE_DISABLED) {
    if (!isSensorTempValid(t1) || !isSensorTempValid(t2)) {
      return false;
    }
  }

  switch (channel.getHvacMode()) {
    case SUPLA_HVAC_MODE_HEAT:
    case SUPLA_HVAC_MODE_COOL:
    case SUPLA_HVAC_MODE_AUTO: {
      if (!isSensorTempValid(t1)) {
        return false;
      }
    }
  }

  if (channel.getDefaultFunction() ==
      SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL) {
    if (!isSensorTempValid(t1) || !isSensorTempValid(t2)) {
      return false;
    }
  }

  return true;
}

int HvacBase::evaluateOutputValue(_supla_int16_t tMeasured,
                                  _supla_int16_t tTarget) {
  if (!isSensorTempValid(tMeasured)) {
    SUPLA_LOG_DEBUG("HVAC: tMeasured not valid");
    channel.setHvacFlagError(true);
    return 0;
  }
  if (!isSensorTempValid(tTarget)) {
    SUPLA_LOG_DEBUG("HVAC: tTarget not valid");
    channel.setHvacFlagError(true);
    return 0;
  }
  if (getUsedAlgorithm() == SUPLA_HVAC_ALGORITHM_NOT_SET) {
    SUPLA_LOG_DEBUG("HVAC: algorithm not valid");
    channel.setHvacFlagError(true);
    return 0;
  }

  int output = lastValue;

  if (getUsedAlgorithm() == SUPLA_HVAC_ALGORITHM_ON_OFF) {
    auto histeresis = getTemperatureHisteresis();
    if (!isSensorTempValid(histeresis)) {
      SUPLA_LOG_DEBUG("HVAC: histeresis not valid");
      channel.setHvacFlagError(true);
      return 0;
    }
    histeresis >>= 1;

    // check if we should turn on heating
    if (lastValue <= 0) {
      if (tMeasured < tTarget - histeresis) {
        output = 100;
      }
    }

    // check if we should turn on cooling
    if (lastValue >= 0) {
      if (tMeasured > tTarget + histeresis) {
        output = -100;
      }
    }
  }
  channel.setHvacFlagError(false);
  return output;
}

void HvacBase::changeFunction(int newFunction, bool changedLocally) {
  auto currentFunction = channel.getDefaultFunction();
  if (currentFunction == newFunction) {
    return;
  }

  if (setAndSaveFunction(newFunction)) {
    lastConfigChangeTimestampMs = millis();
  }

  // if function was changed to or from "differential" then we reset setpoint
  // temperatures and mode to OFF. Also weekly schedule is cleared
  if (newFunction == SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL ||
      currentFunction == SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL) {
    channel.clearHvacSetpointTemperatureMax();
    channel.clearHvacSetpointTemperatureMin();
    channel.setHvacMode(SUPLA_HVAC_MODE_OFF);

    isWeeklyScheduleConfigured = false;
    memset(&weeklySchedule, 0, sizeof(weeklySchedule));
    saveWeeklySchedule();
  }

  if (changedLocally) {
    channelConfigChangedOffline = 1;
    saveConfig();
  }

  clearLastOutputValue();
}

bool HvacBase::isSetpointMinTemperatureValid(_supla_int16_t tMin) const {
  if (channel.getDefaultFunction() ==
      SUPLA_CHANNELFNC_HVAC_THERMOSTAT_DIFFERENTIAL) {
    return tMin >= -SUPLA_HVAC_MAX_DIFFERENTIAL_SETPOINT &&
           tMin <= SUPLA_HVAC_MAX_DIFFERENTIAL_SETPOINT;
  } else {
    return isTemperatureInRoomConstrain(tMin);
  }
}

void HvacBase::enableDifferentialFunctionSupport() {
  channel.addToFuncList(SUPLA_HVAC_CAP_FLAG_DIFFERENTIAL);
}

bool HvacBase::isDifferentialFunctionSupported() const {
  return channel.getFuncList() & SUPLA_HVAC_CAP_FLAG_DIFFERENTIAL;
}

void HvacBase::fixTemperatureSetpoints() {
  auto curTMin = getTemperatureSetpointMin();
  auto curTMax = getTemperatureSetpointMax();

  switch (channel.getHvacMode()) {
    case SUPLA_HVAC_MODE_HEAT: {
      if (!isSensorTempValid(curTMin)) {
        setTemperatureSetpointMin(SUPLA_HVAC_DEFAULT_TEMP_MIN);
      }
      break;
    }
    case SUPLA_HVAC_MODE_COOL: {
      if (!isSensorTempValid(curTMax)) {
        setTemperatureSetpointMax(SUPLA_HVAC_DEFAULT_TEMP_MAX);
      }
      break;
    }
    case SUPLA_HVAC_MODE_AUTO: {
      bool tMinValid = isSensorTempValid(curTMin);
      bool tMaxValid = isSensorTempValid(curTMax);

      if (tMinValid && tMaxValid) {
        if (!isTemperatureInAutoConstrain(curTMin, curTMax)) {
          auto offsetMin = getTemperatureAutoOffsetMin();
          auto offsetMax = getTemperatureAutoOffsetMax();
          if (curTMax - curTMin < offsetMin) {
            if (isTemperatureInRoomConstrain(curTMin + offsetMin)) {
              setTemperatureSetpointMax(curTMin + offsetMin);
            } else {
              setTemperatureSetpointMin(curTMax - offsetMin);
            }
          }
          if (curTMax - curTMin > offsetMax) {
            if (isTemperatureInRoomConstrain(curTMin + offsetMax)) {
              setTemperatureSetpointMax(curTMin + offsetMax);
            } else {
              setTemperatureSetpointMin(curTMax - offsetMax);
            }
          }
        } else if (tMinValid) {
          channel.setHvacSetpointTemperatureMin(curTMin);
        } else if (tMaxValid) {
          channel.setHvacSetpointTemperatureMax(curTMax);
        }
      }
      break;
    }
    default: {
      // other modes doesn't require temperature setpoints fix
      return;
    }
  }
}

void HvacBase::clearLastOutputValue() {
  lastValue = -1000;
  lastOutputStateChangeTimestampMs = 0;
}

