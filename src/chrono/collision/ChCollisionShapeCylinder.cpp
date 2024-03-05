// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2023 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Radu Serban
// =============================================================================

#include "chrono/collision/ChCollisionShapeCylinder.h"

namespace chrono {

// Register into the object factory, to enable run-time dynamic creation and persistence
CH_FACTORY_REGISTER(ChCollisionShapeCylinder)
CH_UPCASTING(ChCollisionShapeCylinder, ChCollisionShape)

ChCollisionShapeCylinder::ChCollisionShapeCylinder() : ChCollisionShape(Type::CYLINDER) {}

ChCollisionShapeCylinder::ChCollisionShapeCylinder(std::shared_ptr<ChContactMaterial> material,
                                                   double radius,
                                                   double height)
    : ChCollisionShape(Type::CYLINDER, material) {
    gcylinder.r = radius;
    gcylinder.h = height;
}

ChCollisionShapeCylinder::ChCollisionShapeCylinder(std::shared_ptr<ChContactMaterial> material,
                                                   const ChCylinder& cyl)
    : ChCollisionShape(Type::CYLINDER, material), gcylinder(cyl) {}

void ChCollisionShapeCylinder::ArchiveOut(ChArchiveOut& archive_out) {
    // version number
    archive_out.VersionWrite<ChCollisionShapeCylinder>();
    // serialize parent class
    ChCollisionShape::ArchiveOut(archive_out);
    // serialize all member data:
    archive_out << CHNVP(gcylinder);
}

void ChCollisionShapeCylinder::ArchiveIn(ChArchiveIn& archive_in) {
    // version number
    /*int version =*/archive_in.VersionRead<ChCollisionShapeCylinder>();
    // deserialize parent class
    ChCollisionShape::ArchiveIn(archive_in);
    // stream in all member data:
    archive_in >> CHNVP(gcylinder);
}

}  // end namespace chrono
