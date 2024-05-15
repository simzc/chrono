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

#ifndef CHMATTERPERISPRINGS_H
#define CHMATTERPERISPRINGS_H


#include "chrono_peridynamics/ChApiPeridynamics.h"
#include "chrono_peridynamics/ChMatterPeridynamics.h"


namespace chrono {
namespace peridynamics {

class ChPeridynamics;

/// @addtogroup chrono_peridynamics
/// @{


//------------------------------------------------------------------------------------
// ChMatterPeriSprings    -  for didactical purpose


/// The simplest peridynamic material: a bond-based material based on a 
/// network of springs, each with the same stiffness k regardless of length, etc. 
/// Just for didactical purposes - do not use it for serious applications.
/// Also use a damping coefficient r. 

class ChApiPeridynamics ChMatterPeriSprings : public ChMatterPeri<> {
public:
    double k = 100;
    double r = 10;

    ChMatterPeriSprings() {};

    // Implement the function that adds the peridynamics force to each node, as a 
    // summation of all the effects of neighbouring nodes.
    virtual void ComputeForces() {
        // loop on bounds
        for (auto& bound : this->bounds) {
            ChMatterDataPerBound& mbound = bound.second;
            ChVector3d old_vdist = mbound.nodeB->GetX0() - mbound.nodeA->GetX0();
            ChVector3d     vdist = mbound.nodeB->GetPos() - mbound.nodeA->GetPos();
            ChVector3d     vdir = vdist.GetNormalized();
            double         vel = Vdot(vdir, mbound.nodeB->GetPosDt() - mbound.nodeA->GetPosDt());
            ChVector3d force_val = (vdist.Length() - old_vdist.Length()) * this->k  + vel * this->r;
            mbound.nodeB->F_peridyn += -vdir * force_val;
            mbound.nodeA->F_peridyn += vdir * force_val;
        }
    };
};



//------------------------------------------------------------------------------------
// ChMatterPeriSpringsBreakable   -  for didactical purpose



class  ChApiPeridynamics ChMatterDataPerBoundBreakable : public ChMatterDataPerBound { 
public: 
    bool broken = false;
};

class ChApiPeridynamics ChMatterPeriSpringsBreakable : public ChMatterPeri<ChMatterDataPerNode, ChMatterDataPerBoundBreakable> {
public:
    double k = 100;
    double r = 10;
    double max_stretch = 0.08;

    ChMatterPeriSpringsBreakable() {};

    // Implement the function that adds the peridynamics force to each node, as a 
    // summation of all the effects of neighbouring nodes.
    virtual void ComputeForces() {
        // loop on bounds
        for (auto& bound : this->bounds) {
            ChMatterDataPerBoundBreakable& mbound = bound.second;
            if (!mbound.broken) {
                ChVector3d old_vdist = mbound.nodeB->GetX0() - mbound.nodeA->GetX0();
                ChVector3d     vdist = mbound.nodeB->GetPos() - mbound.nodeA->GetPos();
                ChVector3d     vdir = vdist.GetNormalized();
                double         vel = Vdot(vdir, mbound.nodeB->GetPosDt() - mbound.nodeA->GetPosDt());
                ChVector3d force_val = (vdist.Length() - old_vdist.Length()) * this->k + vel * this->r;
                mbound.nodeB->F_peridyn += -vdir * force_val / mbound.nodeB->volume; //divide by volumes because F_peridyn are force _densities_
                mbound.nodeA->F_peridyn += vdir * force_val / mbound.nodeA->volume;

                double stretch = (vdist.Length() - old_vdist.Length()) / old_vdist.Length();
                if (stretch > max_stretch) {
                    mbound.broken = true;
                    // the following will propagate the fracture geometry so that broken parts can collide
                    mbound.nodeA->is_boundary = true; 
                    mbound.nodeB->is_boundary = true;
                }
            }
            else {
                if ((mbound.nodeB->GetPos() - mbound.nodeA->GetPos()).Length() > mbound.nodeA->GetHorizonRadius())
                    bounds.erase(bound.first);
            }

        }
    }
};



class /*ChApiPeridynamics*/ ChVisualPeriSpringsBreakable : public ChGlyphs {
public:
    ChVisualPeriSpringsBreakable(std::shared_ptr<ChMatterPeriSpringsBreakable> amatter) : mmatter(amatter) { is_mutable = true; };
    virtual ~ChVisualPeriSpringsBreakable() {}

    // set true if you want those attributes to be attached to the glyps 
    // (ex for postprocessing in falsecolor or with vectors with the Blender addon)
    bool attach_velocity = 0;
    bool attach_acceleration = 0;

protected:
    virtual void Update(ChPhysicsItem* updater, const ChFrame<>& frame) {
        if (!mmatter)
            return;
        this->Reserve(mmatter->GetNnodes());
        unsigned int i = 0;
        for (const auto& anode : mmatter->GetMapOfNodes()) {
            this->SetGlyphPoint(i, anode.second.node->GetPos());
            ++i;
        }
        if (attach_velocity) {
            ChPropertyVector myprop;
            myprop.name = "velocity";
            this->AddProperty(myprop);
            auto mdata = &((ChPropertyVector*)(m_properties.back()))->data;
            mdata->resize(mmatter->GetNnodes());
            i = 0;
            for (const auto& anode : mmatter->GetMapOfNodes()) {
                mdata->at(i) = anode.second.node->GetPosDt();
                ++i;
            }
        }
        if (attach_acceleration) {
            ChPropertyVector myprop;
            myprop.name = "acceleration";
            this->AddProperty(myprop);
            auto mdata = &((ChPropertyVector*)(m_properties.back()))->data;
            mdata->resize(mmatter->GetNnodes());
            i = 0;
            for (const auto& anode : mmatter->GetMapOfNodes()) {
                mdata->at(i) = anode.second.node->GetPosDt2();
                ++i;
            }
        }

    }

    std::shared_ptr<ChMatterPeriSpringsBreakable> mmatter;
};


class /*ChApiPeridynamics*/ ChVisualPeriSpringsBreakableBounds : public ChGlyphs {
public:
    ChVisualPeriSpringsBreakableBounds(std::shared_ptr<ChMatterPeriSpringsBreakable> amatter) : mmatter(amatter) { is_mutable = true; };
    virtual ~ChVisualPeriSpringsBreakableBounds() {}

    bool draw_broken = true;
    bool draw_unbroken = false;

protected:
    virtual void Update(ChPhysicsItem* updater, const ChFrame<>& frame) {
        if (!mmatter)
            return;
        this->Reserve(mmatter->GetNbounds());
        unsigned int i = 0;
        for (const auto& anode : mmatter->GetMapOfBounds()) {
            if (anode.second.broken && draw_broken)
                this->SetGlyphVector(i, anode.second.nodeA->GetPos(), anode.second.nodeB->GetPos()-anode.second.nodeA->GetPos(),ChColor(1,0,0));
            if (!anode.second.broken && draw_unbroken)
                this->SetGlyphVector(i, anode.second.nodeA->GetPos(), anode.second.nodeB->GetPos() - anode.second.nodeA->GetPos(),ChColor(0, 0, 1));
            ++i;
        }
    }

    std::shared_ptr<ChMatterPeriSpringsBreakable> mmatter;
};


/// @} chrono_peridynamics

}  // end namespace peridynamics
}  // end namespace chrono

#endif
