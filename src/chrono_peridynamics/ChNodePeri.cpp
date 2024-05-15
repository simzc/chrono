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
// Authors: Alessandro Tasora, Radu Serban 
// =============================================================================


#include "chrono/physics/ChSystem.h"
#include "chrono_peridynamics/ChNodePeri.h"
#include "chrono/collision/bullet/ChCollisionModelBullet.h"


namespace chrono {

using namespace fea;
using namespace peridynamics;


// Register into the object factory, to enable run-time dynamic creation and persistence
//CH_FACTORY_REGISTER(ChNodePeri)


ChNodePeri::ChNodePeri()
    : volume(0.01), h_rad(0.2), coll_rad(0.0001) {

    SetMass(0.01);
}

ChNodePeri::ChNodePeri(const ChNodePeri& other) : ChNodeFEAxyz(other) {

    h_rad = other.h_rad;
    SetMass(other.GetMass());
    volume = other.volume;
    F_peridyn = other.F_peridyn;
    is_boundary = other.is_boundary;
    is_colliding = other.is_colliding;
    is_elastic = other.is_elastic;
    is_requiring_bonds = other.is_requiring_bonds;

    variables = other.variables;
}

ChNodePeri::~ChNodePeri() {
}

void ChNodePeri::SetHorizonRadius(double mr) {
    h_rad = mr;
    double aabb_rad = h_rad / 2;  // to avoid too many pairs: bounding boxes hemisizes will sum..  __.__--*--
    if (auto mshape = std::dynamic_pointer_cast<ChCollisionModelBullet>(GetCollisionModel()->GetShapeInstance(0).first))
        mshape->SetSphereRadius(coll_rad, std::max(0.0, aabb_rad - coll_rad));
}

void ChNodePeri::SetCollisionRadius(double mr) {
    coll_rad = mr;
    double aabb_rad = h_rad / 2;  // to avoid too many pairs: bounding boxes hemisizes will sum..  __.__--*--
    if (auto mshape = std::dynamic_pointer_cast<ChCollisionModelBullet>(GetCollisionModel()->GetShapeInstance(0).first))
        mshape->SetSphereRadius(coll_rad, std::max(0.0, aabb_rad - coll_rad));
}

void ChNodePeri::ContactForceLoadResidual_F(const ChVector3d& F,
    const ChVector3d& T,
    const ChVector3d& abs_point,
    ChVectorDynamic<>& R) {
    R.segment(NodeGetOffsetVelLevel(), 3) += F.eigen(); // from 
}

void ChNodePeri::ComputeJacobianForContactPart(const ChVector3d& abs_point,
    ChMatrix33<>& contact_plane,
    ChContactable_1vars::type_constraint_tuple& jacobian_tuple_N,
    ChContactable_1vars::type_constraint_tuple& jacobian_tuple_U,
    ChContactable_1vars::type_constraint_tuple& jacobian_tuple_V,
    bool second) {
    ChMatrix33<> Jx1 = contact_plane.transpose();
    if (!second)
        Jx1 *= -1;

    jacobian_tuple_N.Get_Cq().segment(0, 3) = Jx1.row(0);
    jacobian_tuple_U.Get_Cq().segment(0, 3) = Jx1.row(1);
    jacobian_tuple_V.Get_Cq().segment(0, 3) = Jx1.row(2);
}

} // end namespace chrono
