/*
 Copyright (C) AC SOFTWARE SP. Z O.O.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "sensor_parsed.h"

Supla::Sensor::SensorParsed::SensorParsed(Supla::Parser::Parser *parser) :
  parser(parser) {
  static int instanceCounter = 0;
  id = instanceCounter++;
}

void Supla::Sensor::SensorParsed::setMapping(const std::string &parameter,
    const std::string &key) {
  parameterToKey[parameter] = key;
  parser->addKey(key, -1); // ignore index
}

void Supla::Sensor::SensorParsed::setMapping(const std::string &parameter,
    const int index) {
  std::string key = parameter;
  key += "_";
  key += std::to_string(id);
  parameterToKey[parameter] = key;
  parser->addKey(key, index);
}

void Supla::Sensor::SensorParsed::setMultiplier(const std::string &parameter,
    double multiplier) {
  parameterMultiplier[parameter] = multiplier;
}

double Supla::Sensor::SensorParsed::getParameterValue(
    const std::string &parameter) {
  double multiplier = 1;
  if (parameterMultiplier.count(parameter)) {
    multiplier = parameterMultiplier[parameter];
  }
  return parser->getValue(parameterToKey[parameter]) * multiplier;
}

bool Supla::Sensor::SensorParsed::refreshParserSource() {
  if (parser && parser->refreshSource()) {
    return true;
  }
  return false;
}

bool Supla::Sensor::SensorParsed::isParameterConfigured(
    const std::string &parameter) {
  return parameterToKey.count(parameter) > 0;
}

void Supla::Sensor::SensorParsed::enableDebug() {
  if (parser) {
    parser->enableDebug();
  }
}
