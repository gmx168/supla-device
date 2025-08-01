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

#ifndef SRC_SUPLA_CONTROL_BUTTON_AGGREGATOR_H_
#define SRC_SUPLA_CONTROL_BUTTON_AGGREGATOR_H_

#include <supla/control/button.h>
#include <stdint.h>

#define BUTTON_AGGREGATOR_MAX_BUTTONS 10

namespace Supla {
namespace Control {

class ButtonAggregator : public Supla::Control::Button {
 public:
  ButtonAggregator();
  virtual ~ButtonAggregator();

  void onTimer() override;
  void handleAction(int event, int action) override;
  bool addButton(Supla::Control::Button* button);

 protected:
  Supla::Control::Button* buttons[BUTTON_AGGREGATOR_MAX_BUTTONS] = {};
  int pressCount = 0;
  int buttonCount = 0;
  bool stateChanged = false;
  uint32_t allPressedTimestamp = 0;
};

}  // namespace Control
}  // namespace Supla

#endif  // SRC_SUPLA_CONTROL_BUTTON_AGGREGATOR_H_
