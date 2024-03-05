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

#include "chrono/physics/ChLinkMotorLinearPosition.h"

namespace chrono {

// Register into the object factory, to enable run-time dynamic creation and persistence
CH_FACTORY_REGISTER(ChLinkMotorLinearPosition)

ChLinkMotorLinearPosition::ChLinkMotorLinearPosition() {
    // default motion function: ramp with initial value y(0) = 0 and slope dy/dt = 1
    m_func = chrono_types::make_shared<ChFunctionRamp>(0.0, 1.0);

    pos_offset = 0;
}

ChLinkMotorLinearPosition::ChLinkMotorLinearPosition(const ChLinkMotorLinearPosition& other)
    : ChLinkMotorLinear(other) {
    pos_offset = other.pos_offset;
}

ChLinkMotorLinearPosition::~ChLinkMotorLinearPosition() {}

void ChLinkMotorLinearPosition::Update(double mytime, bool update_assets) {
    // Inherit parent class:
    ChLinkMotorLinear::Update(mytime, update_assets);

    // Add the time-dependent term in residual C as
    //   C = d_error - d_setpoint - d_offset
    // with d_error = z_pos_1 - z_pos_2, and d_setpoint = z(t)

    C(m_actuated_idx) = this->mpos - m_func->GetVal(mytime) - this->pos_offset;
}

void ChLinkMotorLinearPosition::IntLoadConstraint_Ct(const unsigned int off_L, ChVectorDynamic<>& Qc, const double c) {
    double mCt = -m_func->GetDer(this->GetChTime());
    if (mask.Constr_N(m_actuated_idx).IsActive()) {
        Qc(off_L + m_actuated_idx) += c * mCt;
    }
}

void ChLinkMotorLinearPosition::ConstraintsBiLoad_Ct(double factor) {
    if (!this->IsActive())
        return;

    double mCt = -m_func->GetDer(this->GetChTime());
    if (mask.Constr_N(m_actuated_idx).IsActive()) {
        mask.Constr_N(m_actuated_idx).Set_b_i(mask.Constr_N(m_actuated_idx).Get_b_i() + factor * mCt);
    }
}

void ChLinkMotorLinearPosition::ArchiveOut(ChArchiveOut& archive_out) {
    // version number
    archive_out.VersionWrite<ChLinkMotorLinearPosition>();

    // serialize parent class
    ChLinkMotorLinear::ArchiveOut(archive_out);

    // serialize all member data:
    archive_out << CHNVP(pos_offset);
}

/// Method to allow de serialization of transient data from archives.
void ChLinkMotorLinearPosition::ArchiveIn(ChArchiveIn& archive_in) {
    // version number
    /*int version =*/ archive_in.VersionRead<ChLinkMotorLinearPosition>();

    // deserialize parent class
    ChLinkMotorLinear::ArchiveIn(archive_in);

    // deserialize all member data:
    archive_in >> CHNVP(pos_offset);
}

}  // end namespace chrono
