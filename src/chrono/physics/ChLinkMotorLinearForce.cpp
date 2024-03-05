// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Alessandro Tasora
// =============================================================================

#include "chrono/physics/ChLinkMotorLinearForce.h"

namespace chrono {

// Register into the object factory, to enable run-time dynamic creation and persistence
CH_FACTORY_REGISTER(ChLinkMotorLinearForce)

ChLinkMotorLinearForce::ChLinkMotorLinearForce() {
    this->c_z = false;
    SetupLinkMask();
    m_func = chrono_types::make_shared<ChFunctionConst>(0.0);
}

ChLinkMotorLinearForce::ChLinkMotorLinearForce(const ChLinkMotorLinearForce& other) : ChLinkMotorLinear(other) {}

ChLinkMotorLinearForce::~ChLinkMotorLinearForce() {}

void ChLinkMotorLinearForce::Update(double mytime, bool update_assets) {
    ChLinkMotorLinear::Update(mytime, update_assets);
}

void ChLinkMotorLinearForce::IntLoadResidual_F(const unsigned int off, ChVectorDynamic<>& R, const double c) {
    // compute instant force
    double mF = m_func->GetVal(this->GetChTime());

    ChFrame<> aframe1 = this->frame1 >> (*this->Body1);
    ChFrame<> aframe2 = this->frame2 >> (*this->Body2);
    ChVector3d m_abs_force = aframe2.GetRotMat() * ChVector3d(0, 0, mF);
    ChVector3d mbody_force;
    ChVector3d mbody_torque;

    if (Body2->Variables().IsActive()) {
        Body2->To_abs_forcetorque(m_abs_force,
                                  aframe1.GetPos(),            // absolute application point is always marker1
                                  false,                       // from abs. space
                                  mbody_force, mbody_torque);  // resulting force-torque, both in abs coords
        R.segment(Body2->Variables().GetOffset() + 0, 3) -= c * mbody_force.eigen();
        R.segment(Body2->Variables().GetOffset() + 3, 3) -=
            c * Body2->TransformDirectionParentToLocal(mbody_torque).eigen();
    }

    if (Body1->Variables().IsActive()) {
        Body1->To_abs_forcetorque(m_abs_force,
                                  aframe1.GetPos(),            // absolute application point is always marker1
                                  false,                       // from abs. space
                                  mbody_force, mbody_torque);  // resulting force-torque, both in abs coords
        R.segment(Body1->Variables().GetOffset() + 0, 3) += c * mbody_force.eigen();
        R.segment(Body1->Variables().GetOffset() + 3, 3) +=
            c * Body1->TransformDirectionParentToLocal(mbody_torque).eigen();
    }
}

void ChLinkMotorLinearForce::ConstraintsFbLoadForces(double factor) {
    // compute instant force
    double mF = m_func->GetVal(this->GetChTime());

    ChFrame<> aframe1 = this->frame1 >> (*this->Body1);
    ChFrame<> aframe2 = this->frame2 >> (*this->Body2);
    ChVector3d m_abs_force = aframe2.GetRotMat() * ChVector3d(0, 0, mF);
    ChVector3d mbody_force;
    ChVector3d mbody_torque;
    Body2->To_abs_forcetorque(m_abs_force,
                              aframe1.GetPos(),            // absolute application point is always marker1
                              false,                       // from abs. space
                              mbody_force, mbody_torque);  // resulting force-torque, both in abs coords
    Body2->Variables().Get_fb().segment(0, 3) -= factor * mbody_force.eigen();
    Body2->Variables().Get_fb().segment(3, 3) -= factor * Body2->TransformDirectionParentToLocal(mbody_torque).eigen();

    Body1->To_abs_forcetorque(m_abs_force,
                              aframe1.GetPos(),            // absolute application point is always marker1
                              false,                       // from abs. space
                              mbody_force, mbody_torque);  // resulting force-torque, both in abs coords
    Body1->Variables().Get_fb().segment(0, 3) += factor * mbody_force.eigen();
    Body1->Variables().Get_fb().segment(3, 3) += factor * Body1->TransformDirectionParentToLocal(mbody_torque).eigen();
}

void ChLinkMotorLinearForce::ArchiveOut(ChArchiveOut& archive_out) {
    // version number
    archive_out.VersionWrite<ChLinkMotorLinearForce>();

    // serialize parent class
    ChLinkMotorLinear::ArchiveOut(archive_out);

    // serialize all member data:
}

/// Method to allow de serialization of transient data from archives.
void ChLinkMotorLinearForce::ArchiveIn(ChArchiveIn& archive_in) {
    // version number
    /*int version =*/ archive_in.VersionRead<ChLinkMotorLinearForce>();

    // deserialize parent class
    ChLinkMotorLinear::ArchiveIn(archive_in);

    // deserialize all member data:
}

}  // end namespace chrono
