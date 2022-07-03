// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2022 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Radu Serban
// =============================================================================
//
// Base class for an idler subsystem with a fixed distance tensioner.
// An idler consists of the idler wheel and a carrier body. The carrier body is
// connected to the chassis and the idler wheel to the carrier. A linear
// actuator connects the carrier body and a link body (the chassis or a
// supsension arm).
//
// The reference frame for a vehicle follows the ISO standard: Z-axis up, X-axis
// pointing forward, and Y-axis towards the left of the vehicle.
//
// =============================================================================

#include "chrono/assets/ChCylinderShape.h"
#include "chrono/assets/ChBoxShape.h"
#include "chrono/assets/ChPointPointShape.h"

#include "chrono_vehicle/tracked_vehicle/idler/ChDistanceIdler.h"
#include "chrono_vehicle/tracked_vehicle/ChTrackAssembly.h"

namespace chrono {
namespace vehicle {

// -----------------------------------------------------------------------------

class DistanceIdlerFunction : public ChFunction {
  public:
    DistanceIdlerFunction(double time, double init_val, double final_val)
        : m_time(time), m_init_val(init_val), m_final_val(final_val) {}
    DistanceIdlerFunction(const DistanceIdlerFunction& other)
        : m_time(other.m_time), m_init_val(other.m_init_val), m_final_val(other.m_final_val) {}

    virtual DistanceIdlerFunction* Clone() const override { return new DistanceIdlerFunction(*this); }

    virtual double Get_y(double x) const {
        if (x < m_time)
            return m_init_val + (m_final_val - m_init_val) * (x / m_time);
        return m_final_val;
    }

  private:
    double m_time;
    double m_init_val;
    double m_final_val;
};

// -----------------------------------------------------------------------------
ChDistanceIdler::ChDistanceIdler(const std::string& name) : ChIdler(name) {}

ChDistanceIdler::~ChDistanceIdler() {
    auto sys = m_carrier->GetSystem();
    if (sys) {
        sys->Remove(m_carrier);
        sys->Remove(m_revolute);
        sys->Remove(m_tensioner);
    }
}

void ChDistanceIdler::Initialize(std::shared_ptr<ChChassis> chassis,
                                 const ChVector<>& location,
                                 ChTrackAssembly* track) {
    // Express the idler reference frame in the absolute coordinate system
    ChFrame<> idler_to_abs(location);
    idler_to_abs.ConcatenatePreTransformation(chassis->GetBody()->GetFrame_REF_to_abs());

    // Transform all points and directions to absolute frame
    std::vector<ChVector<>> points(NUM_POINTS);

    for (int i = 0; i < NUM_POINTS; i++) {
        ChVector<> rel_pos = GetLocation(static_cast<PointId>(i));
        points[i] = idler_to_abs.TransformPointLocalToParent(rel_pos);
    }

    // Create and initialize the carrier body
    m_carrier = std::shared_ptr<ChBody>(chassis->GetSystem()->NewBody());
    m_carrier->SetNameString(m_name + "_carrier");
    m_carrier->SetPos(points[CARRIER]);
    m_carrier->SetRot(idler_to_abs.GetRot());
    m_carrier->SetMass(GetCarrierMass());
    m_carrier->SetInertiaXX(GetCarrierInertia());
    chassis->GetSystem()->AddBody(m_carrier);

    // Cache points for carrier visualization (expressed in the carrier frame)
    m_pC = m_carrier->TransformPointParentToLocal(points[CARRIER]);
    m_pW = m_carrier->TransformPointParentToLocal(points[CARRIER_WHEEL]);
    m_pR = m_carrier->TransformPointParentToLocal(points[CARRIER_CHASSIS]);
    m_pM = m_carrier->TransformPointParentToLocal(points[MOTOR_CARRIER]);

    // Create and initialize the revolute joint between carrier and chassis
    m_revolute = chrono_types::make_shared<ChLinkLockRevolute>();
    m_revolute->SetNameString(m_name + "_carrier_pin");
    m_revolute->Initialize(chassis->GetBody(), m_carrier,
                           ChCoordsys<>(points[CARRIER_CHASSIS], idler_to_abs.GetRot() * Q_from_AngY(CH_C_PI_2)));
    chassis->GetSystem()->AddLink(m_revolute);

    // Linear actuator function
    double init_dist = (points[MOTOR_ARM] - points[MOTOR_CARRIER]).Length();
    assert(init_dist < GetTensionerDistance());
    auto motfun = chrono_types::make_shared<DistanceIdlerFunction>(GetTensionerExtensionTime(), init_dist,
                                                                   GetTensionerDistance());

    // Create and initialize the tensioner motor element.
    // Connect the idler wheel carrier to the arm of the last suspension subsystem.
    // Attach a ramp function to extend the tensioner to desired distance.
    auto arm = track->GetTrackSuspensions().back()->GetCarrierBody();
    m_tensioner = chrono_types::make_shared<ChLinkMotorLinearPosition>();
    m_tensioner->SetNameString(m_name + "_tensioner");
    m_tensioner->SetMotionFunction(motfun);
    m_tensioner->SetGuideConstraint(ChLinkMotorLinear::GuideConstraint::FREE);
    m_tensioner->Initialize(arm, m_carrier, false, ChFrame<>(points[MOTOR_ARM]), ChFrame<>(points[MOTOR_CARRIER]));
    chassis->GetSystem()->AddLink(m_tensioner);

    // Invoke the base class implementation. This initializes the associated idler wheel.
    // Note: we must call this here, after the m_carrier body is created.
    ChIdler::Initialize(chassis, location, track);
}

void ChDistanceIdler::InitializeInertiaProperties() {
    m_mass = GetCarrierMass() + m_idler_wheel->GetMass();
}

void ChDistanceIdler::UpdateInertiaProperties() {
    m_parent->GetTransform().TransformLocalToParent(ChFrame<>(m_rel_loc, QUNIT), m_xform);

    // Calculate COM and inertia expressed in global frame
    utils::CompositeInertia composite;
    composite.AddComponent(m_carrier->GetFrame_COG_to_abs(), m_carrier->GetMass(), m_carrier->GetInertia());
    composite.AddComponent(m_idler_wheel->GetBody()->GetFrame_COG_to_abs(), m_idler_wheel->GetBody()->GetMass(),
                           m_idler_wheel->GetBody()->GetInertia());

    // Express COM and inertia in subsystem reference frame
    m_com.coord.pos = m_xform.TransformPointParentToLocal(composite.GetCOM());
    m_com.coord.rot = QUNIT;

    m_inertia = m_xform.GetA().transpose() * composite.GetInertia() * m_xform.GetA();
}

// -----------------------------------------------------------------------------
void ChDistanceIdler::AddVisualizationAssets(VisualizationType vis) {
    if (vis == VisualizationType::NONE)
        return;

    static const double threshold2 = 1e-6;
    double radius = GetCarrierVisRadius();

    if ((m_pW - m_pC).Length2() > threshold2) {
        auto cyl = chrono_types::make_shared<ChCylinderShape>();
        cyl->GetCylinderGeometry().p1 = m_pW;
        cyl->GetCylinderGeometry().p2 = m_pC;
        cyl->GetCylinderGeometry().rad = radius;
        m_carrier->AddVisualShape(cyl);
    }

    if ((m_pC - m_pR).Length2() > threshold2) {
        auto cyl = chrono_types::make_shared<ChCylinderShape>();
        cyl->GetCylinderGeometry().p1 = m_pC;
        cyl->GetCylinderGeometry().p2 = m_pR;
        cyl->GetCylinderGeometry().rad = radius;
        m_carrier->AddVisualShape(cyl);
    }

    auto box = chrono_types::make_shared<ChBoxShape>();
    box->GetBoxGeometry().Size = ChVector<>(3 * radius, radius, radius);
    m_carrier->AddVisualShape(box, ChFrame<>(m_pR));

    // Visualization of the tensioner spring (with default color)
    m_tensioner->AddVisualShape(chrono_types::make_shared<ChSegmentShape>());
}

void ChDistanceIdler::RemoveVisualizationAssets() {
    ChPart::RemoveVisualizationAssets(m_carrier);
    ChPart::RemoveVisualizationAssets(m_tensioner);
}

// -----------------------------------------------------------------------------
void ChDistanceIdler::LogConstraintViolations() {
    ChVectorDynamic<> C = m_revolute->GetConstraintViolation();
    GetLog() << "  Carrier-chassis revolute\n";
    GetLog() << "  " << C(0) << "  ";
    GetLog() << "  " << C(1) << "  ";
    GetLog() << "  " << C(2) << "  ";
    GetLog() << "  " << C(3) << "  ";
    GetLog() << "  " << C(4) << "\n";

    m_idler_wheel->LogConstraintViolations();
}

// -----------------------------------------------------------------------------
void ChDistanceIdler::ExportComponentList(rapidjson::Document& jsonDocument) const {
    ChPart::ExportComponentList(jsonDocument);

    std::vector<std::shared_ptr<ChBody>> bodies;
    bodies.push_back(m_carrier);
    ChPart::ExportBodyList(jsonDocument, bodies);

    std::vector<std::shared_ptr<ChLink>> joints;
    joints.push_back(m_revolute);
    joints.push_back(m_tensioner);
    ChPart::ExportJointList(jsonDocument, joints);
}

void ChDistanceIdler::Output(ChVehicleOutput& database) const {
    if (!m_output)
        return;

    std::vector<std::shared_ptr<ChBody>> bodies;
    bodies.push_back(m_carrier);
    database.WriteBodies(bodies);

    std::vector<std::shared_ptr<ChLink>> joints;
    joints.push_back(m_revolute);
    joints.push_back(m_tensioner);
    database.WriteJoints(joints);
}

}  // end namespace vehicle
}  // end namespace chrono
