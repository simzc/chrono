#ifndef CH_TRACKED_VEHICLE_VISUAL_SYSTEM_VSG_H
#define CH_TRACKED_VEHICLE_VISUAL_SYSTEM_VSG_H

#include <string>

#include "chrono/physics/ChSystem.h"
#include "chrono/utils/ChUtilsChaseCamera.h"

#include "chrono_vsg/ChVisualSystemVSG.h"
#include "chrono_vehicle/utils/ChVehicleVisualSystemVSG.h"

#include "chrono_vehicle/ChApiVehicle.h"
#include "chrono_vehicle/ChVehicle.h"
#include "chrono_vehicle/tracked_vehicle/ChTrackedVehicle.h"
#include "chrono_vehicle/ChVehicleVisualSystem.h"
#include "chrono_vehicle/ChDriver.h"
#include "chrono_vehicle/ChConfigVehicle.h"

namespace chrono {
namespace vehicle {
class CH_VEHICLE_API ChTrackedVehicleVisualSystemVSG : public ChVehicleVisualSystemVSG {
  public:
    ChTrackedVehicleVisualSystemVSG();
    ~ChTrackedVehicleVisualSystemVSG(){};

    /// Attach a vehicle to this Irrlicht vehicle visualization system.
    virtual void AttachVehicle(vehicle::ChVehicle* vehicle) override;

  private:
    ChTrackedVehicle* m_tvehicle;
};
}  // namespace vehicle
}  // namespace chrono

#endif