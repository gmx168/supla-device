/*
   Copyright (C) AC SOFTWARE SP. Z O.O

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

#ifndef SRC_SUPLA_NETWORK_HTML_BUTTON_TYPE_PARAMETERS_H_
#define SRC_SUPLA_NETWORK_HTML_BUTTON_TYPE_PARAMETERS_H_

#include <supla/network/html/select_input_parameter.h>

namespace Supla {

namespace Html {

class ButtonTypeParameters : public SelectInputParameter {
 public:
  explicit ButtonTypeParameters(int id, const char *labelPrefix = nullptr);

  void addMonostableOption();
  void addBistableOption();
  void addMotionSensorOption();
  void addCentralControlOption();

  // adds: monostable, bistable, and motion sensor
  void addDefualtOptions();
 private:
  char *labelPrefix = nullptr;
};

};  // namespace Html
};  // namespace Supla

#endif  // SRC_SUPLA_NETWORK_HTML_BUTTON_TYPE_PARAMETERS_H_
