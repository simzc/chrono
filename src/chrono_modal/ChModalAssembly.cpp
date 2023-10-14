﻿// =============================================================================
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

#include "chrono_modal/ChModalAssembly.h"
#include "chrono/physics/ChSystem.h"
#include "chrono/fea/ChNodeFEAxyz.h"
#include "chrono/fea/ChNodeFEAxyzrot.h"

namespace chrono {

using namespace fea;
using namespace collision;
using namespace geometry;

namespace modal {

// Register into the object factory, to enable run-time dynamic creation and persistence
CH_FACTORY_REGISTER(ChModalAssembly)

ChModalAssembly::ChModalAssembly()
    : modal_variables(nullptr), n_modes_coords_w(0), is_modal(false), internal_nodes_update(true) {}

ChModalAssembly::ChModalAssembly(const ChModalAssembly& other) : ChAssembly(other) {
    is_modal = other.is_modal;
    modal_q = other.modal_q;
    modal_q_dt = other.modal_q_dt;
    modal_q_dtdt = other.modal_q_dtdt;
    custom_F_modal = other.custom_F_modal;
    internal_nodes_update = other.internal_nodes_update;
    m_custom_F_modal_callback = other.m_custom_F_modal_callback;
    m_custom_F_full_callback = other.m_custom_F_full_callback;

    //// TODO:  deep copy of the object lists (internal_bodylist, internal_linklist, internal_meshlist,
    /// internal_otherphysicslist)
}

ChModalAssembly::~ChModalAssembly() {
    RemoveAllInternalBodies();
    RemoveAllInternalLinks();
    RemoveAllInternalMeshes();
    RemoveAllInternalOtherPhysicsItems();
    if (modal_variables)
        delete modal_variables;
}

ChModalAssembly& ChModalAssembly::operator=(ChModalAssembly other) {
    ChModalAssembly tmp(other);
    swap(*this, other);
    return *this;
}

// Note: implement this as a friend function (instead of a member function swap(ChModalAssembly& other)) so that other
// classes that have a ChModalAssembly member (currently only ChSystem) could use it, the same way we use std::swap
// here.
void swap(ChModalAssembly& first, ChModalAssembly& second) {
    using std::swap;
    // swap(first.nbodies, second.nbodies);
    // ***TODO***
}

void ChModalAssembly::Clear() {
    ChAssembly::Clear();  // parent

    RemoveAllInternalBodies();
    RemoveAllInternalLinks();
    RemoveAllInternalMeshes();
    RemoveAllInternalOtherPhysicsItems();

    if (modal_variables)
        delete modal_variables;
}

// Assembly a sparse matrix by bordering square H with rectangular Cq.
//    HCQ = [ H  Cq' ]
//          [ Cq  0  ]
void util_sparse_assembly_2x2symm(
    Eigen::SparseMatrix<double, Eigen::ColMajor, int>& HCQ,  ///< resulting square sparse matrix (column major)
    const ChSparseMatrix& H,                                 ///< square sparse H matrix, n_v x n_v
    const ChSparseMatrix& Cq)                                ///< rectangular  sparse Cq  n_c x n_v
{
    int n_v = H.rows();
    int n_c = Cq.rows();
    HCQ.resize(n_v + n_c, n_v + n_c);
    HCQ.reserve(H.nonZeros() + 2 * Cq.nonZeros());
    HCQ.setZero();

    for (int k = 0; k < H.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H, k); it; ++it) {
            HCQ.insert(it.row(), it.col()) = it.value();
        }

    for (int k = 0; k < Cq.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(Cq, k); it; ++it) {
            HCQ.insert(it.row() + n_v, it.col()) = it.value();  // insert Cq
            HCQ.insert(it.col(), it.row() + n_v) = it.value();  // insert Cq'
        }

    // This seems necessary in Release mode
    HCQ.makeCompressed();

    //***NOTE***
    // for some reason the HCQ matrix created via .insert() or .elementRef() or triplet insert, is
    // corrupt in Release mode, not in Debug mode. However, when doing a loop like the one below,
    // it repairs it.
    // ***TODO*** avoid this bad hack and find the cause of the release/debug difference.
    /*
    for (int k = 0; k < HCQ.rows(); ++k) {
        for (int j = 0; j < HCQ.cols(); ++j) {
            auto foo = HCQ.coeffRef(k, j);
            //GetLog() << HCQ.coeffRef(k,j) << " ";
        }
    }
    */
}

//---------------------------------------------------------------------------------------

void ChModalAssembly::SwitchModalReductionON_backup(ChSparseMatrix& full_M,
                                                    ChSparseMatrix& full_K,
                                                    ChSparseMatrix& full_Cq,
                                                    const ChModalSolveUndamped& n_modes_settings,
                                                    const ChModalDamping& damping_model) {
    if (is_modal)
        return;

    // 1) compute eigenvalue and eigenvectors
    this->ComputeModesExternalData(full_M, full_K, full_Cq, n_modes_settings);

    // 2) fetch initial x0 state of assembly, full not reduced
    int bou_int_coords = this->n_boundary_coords + this->n_internal_coords;
    int bou_int_coords_w = this->n_boundary_coords_w + this->n_internal_coords_w;
    double fooT;
    ChStateDelta assembly_v0;
    full_assembly_x_old.setZero(bou_int_coords, nullptr);
    assembly_v0.setZero(bou_int_coords_w, nullptr);
    this->IntStateGather(0, full_assembly_x_old, 0, assembly_v0, fooT);

    // 3) bound ChVariables etc. to the modal coordinates, resize matrices, set as modal mode
    this->SetModalMode(true);
    this->SetupModalData(this->modes_V.cols());

    // 4) do the Herting reduction as in Sonneville, 2021

    ChSparseMatrix K_II = full_K.block(this->n_boundary_coords_w, this->n_boundary_coords_w, this->n_internal_coords_w,
                                       this->n_internal_coords_w);
    ChSparseMatrix K_IB =
        full_K.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

    ChSparseMatrix M_II = full_M.block(this->n_boundary_coords_w, this->n_boundary_coords_w, this->n_internal_coords_w,
                                       this->n_internal_coords_w);
    ChSparseMatrix M_IB =
        full_M.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

    ChSparseMatrix Cq_B = full_Cq.block(0, 0, full_Cq.rows(), this->n_boundary_coords_w);
    ChSparseMatrix Cq_I = full_Cq.block(0, this->n_boundary_coords_w, full_Cq.rows(), this->n_internal_coords_w);

    ChMatrixDynamic<> V_B = this->modes_V.block(0, 0, this->n_boundary_coords_w, this->n_modes_coords_w).real();
    ChMatrixDynamic<> V_I =
        this->modes_V.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_modes_coords_w).real();

    // K_IIc = [ K_II   Cq_I' ]
    //         [ Cq_I     0   ]

    Eigen::SparseMatrix<double> K_IIc;
    util_sparse_assembly_2x2symm(K_IIc, K_II, Cq_I);
    K_IIc.makeCompressed();

    // Matrix of static modes (constrained, so use K_IIc instead of K_II,
    // the original unconstrained Herting reduction is Psi_S = - K_II^{-1} * K_IB )
    //
    // {Psi_S; foo} = - K_IIc^{-1} * {K_IB ; Cq_B}

    ChMatrixDynamic<> Psi_S(this->n_internal_coords_w, this->n_boundary_coords_w);

    // avoid computing K_IIc^{-1}, effectively do n times a linear solve:
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.analyzePattern(K_IIc);
    solver.factorize(K_IIc);
    for (int i = 0; i < K_IB.cols(); ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + full_Cq.rows());
        if (Cq_B.rows())
            rhs << K_IB.col(i).toDense(), Cq_B.col(i).toDense();
        else
            rhs << K_IB.col(i).toDense();
        ChVectorDynamic<> x = solver.solve(rhs);
        Psi_S.block(0, i, this->n_internal_coords_w, 1) = -x.head(this->n_internal_coords_w);
    }

    // Matrix of dynamic modes (V_B and V_I already computed as constrained eigenmodes,
    // but use K_IIc instead of K_II anyway, to reuse K_IIc already factored before)
    //
    // {Psi_D; foo} = - K_IIc^{-1} * {(M_IB * V_B + M_II * V_I) ; 0}

    ChMatrixDynamic<> Psi_D(this->n_internal_coords_w, this->n_modes_coords_w);

    for (int i = 0; i < this->n_modes_coords_w; ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + full_Cq.rows());
        rhs << (M_IB * V_B + M_II * V_I).col(i), Eigen::VectorXd::Zero(full_Cq.rows());
        ChVectorDynamic<> x = solver.solve(rhs);
        Psi_D.block(0, i, this->n_internal_coords_w, 1) = -x.head(this->n_internal_coords_w);
    }

    // Psi = [ I     0    ]
    //       [Psi_S  Psi_D]
    Psi.setZero(this->n_boundary_coords_w + this->n_internal_coords_w,
                this->n_boundary_coords_w + this->n_modes_coords_w);
    //***TODO*** maybe prefer sparse Psi matrix, especially for upper blocks...

    Psi << Eigen::MatrixXd::Identity(n_boundary_coords_w, n_boundary_coords_w),
        Eigen::MatrixXd::Zero(n_boundary_coords_w, n_modes_coords_w), Psi_S, Psi_D;

    this->modal_M = Psi.transpose() * full_M * Psi;
    this->modal_K = Psi.transpose() * full_K * Psi;

    // Reset to zero all the atomic masses of the boundary nodes because now their mass is represented by  this->modal_M
    // NOTE! this should be made more generic and future-proof by implementing a virtual method ex. RemoveMass() in all
    // ChPhysicsItem
    for (auto& body : bodylist) {
        body->SetMass(0);
        body->SetInertia(VNULL);
    }
    for (auto& item : this->meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node))
                    xyz->SetMass(0);
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    xyzrot->SetMass(0);
                    xyzrot->GetInertia().setZero();
                }
            }
        }
    }

    // Modal reduction of R damping matrix: compute using user-provided damping model
    this->modal_R.setZero(modal_M.rows(), modal_M.cols());
    damping_model.ComputeR(*this, this->modal_M, this->modal_K, Psi, this->modal_R);

    // Invalidate results of the initial eigenvalue analysis because now the DOFs are different after reduction,
    // to avoid that one could be tempted to plot those eigenmodes, which now are not exactly the ones of the reduced
    // assembly.
    this->modes_assembly_x0.resize(0);
    this->modes_damping_ratio.resize(0);
    this->modes_eig.resize(0);
    this->modes_freq.resize(0);
    this->modes_V.resize(0, 0);

    // Debug dump data. ***TODO*** remove
    if (true) {
        ChStreamOutAsciiFile fileP("dump_modal_Psi.dat");
        fileP.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(Psi, fileP);
        ChStreamOutAsciiFile fileM("dump_modal_M.dat");
        fileM.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_M, fileM);
        ChStreamOutAsciiFile fileK("dump_modal_K.dat");
        fileK.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_K, fileK);
        ChStreamOutAsciiFile fileR("dump_modal_R.dat");
        fileR.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_R, fileR);
    }
}

void ChModalAssembly::SwitchModalReductionON(ChSparseMatrix& full_M,
                                             ChSparseMatrix& full_K,
                                             ChSparseMatrix& full_Cq,
                                             const ChModalSolveUndamped& n_modes_settings,
                                             const ChModalDamping& damping_model) {
    if (is_modal)
        return;

    GetLog() << " * run in line:\t" << __LINE__ << "\n";
    this->SetupInitial();
    this->Setup();
    this->Update();

    // Steps of modal reduction

    // 1-
    //  to calculate the position of the mass center of the subsystem,
    //  then to determine the selection matrix S,
    //  then to determine the floating frame F as ChFrameMoving().
    //  note: both pos/vel of F are used in the modal method.

    // 2-
    //  to find a way to retrieve the pos/vel/acc of boundary and internal nodes: B, I
    //  then to determine the transformation matrices: P_B1, P_B2, P_I1, P_I2

    // 3-
    //  transform the full system matrices in the original mixed basis to be in the local frame of F,
    //  where P_B2, P_I2 will be used.
    //  the system matrices: full_M_loc, full_K_loc, full_R_loc, full_Cq_loc will be obtained.

    // 4-
    //  perform the modal reduction procedure in the local frame of F,
    //  the system matrices: M_red, K_red, R_red, Cq_red will be obtained.
    //  transformation matrices Psi, Psi_S, Psi_D will be obtained.
    //  todo: verify whether the equation K_IB*P_B1+K_II*P_I1==0 is true for the rigid-body mode shapes: \Phi_r = [P_B1;
    //  P_I1].
    //
    //

    // 2) fetch the initial state of assembly, full not reduced, as an initialization
    double fooT;
    ChStateDelta full_assembly_v0;
    full_assembly_x_old.setZero(this->ncoords, nullptr);
    full_assembly_v0.setZero(this->ncoords_w, nullptr);
    this->IntStateGather(0, full_assembly_x_old, 0, full_assembly_v0, fooT);

    this->ComputeMassCenter();

    this->CpmputeSelectionMatrix();

    this->UpdateFloatingFrameOfReference();

    // fetch the initial floating frame of reference F at the initial configuration
    this->floating_frame_F0 = this->floating_frame_F;

    this->ComputeLocalFullKRMmatrix();

    // 1) compute eigenvalue and eigenvectors
    this->ComputeModesExternalData(full_M_loc, full_K_loc, full_Cq_loc, n_modes_settings);

    // 3) bound ChVariables etc. to the modal coordinates, resize matrices, set as modal mode
    this->SetModalMode(true);
    this->SetupModalData(this->modes_V.cols());

    this->UpdateTransformationMatrix();

    // 4) do the Herting reduction as in Sonneville, 2021
    DoModalReduction(damping_model);
    GetLog() << " * run in line:\t" << __LINE__ << "\n";

    // compute the modal K R M matrices
    ComputeInertialKRMmatrix();  // inertial M K R
    ComputeStiffnessMatrix();    // material stiffness and geometrical stiffness
    ComputeDampingMatrix();      // material damping
    ComputeModalKRMmatrix();

    GetLog() << "run in line:\t" << __LINE__ << "\n";
    GetLog() << "**** the new implemented modal reduction is done...\n";

    // Debug dump data. ***TODO*** remove
    if (true) {
        ChStreamOutAsciiFile filePsi("dump_modal_Psi.dat");
        filePsi.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(Psi, filePsi);
        ChStreamOutAsciiFile fileM("dump_modal_M.dat");
        fileM.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_M, fileM);
        ChStreamOutAsciiFile fileK("dump_modal_K.dat");
        fileK.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_K, fileK);
        ChStreamOutAsciiFile fileR("dump_modal_R.dat");
        fileR.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_R, fileR);
        ChStreamOutAsciiFile fileCq("dump_modal_Cq.dat");
        fileCq.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_Cq, fileCq);

        ChStreamOutAsciiFile fileM_red("dump_reduced_M.dat");
        fileM_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->M_red, fileM_red);
        ChStreamOutAsciiFile fileK_red("dump_reduced_K.dat");
        fileK_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->K_red, fileK_red);
        ChStreamOutAsciiFile fileR_red("dump_reduced_R.dat");
        fileR_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->R_red, fileR_red);
        ChStreamOutAsciiFile fileCq_red("dump_reduced_Cq.dat");
        fileCq_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->Cq_red, fileCq_red);
    }
}

void ChModalAssembly::SwitchModalReductionON(const ChModalSolveUndamped& n_modes_settings,
                                             const ChModalDamping& damping_model) {
    if (is_modal)
        return;

    // 1) fetch the full (not reduced) mass and stiffness
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    // 2) compute modal reduction from full_M, full_K
    // this->SwitchModalReductionON_backup(full_M, full_K, full_Cq, n_modes_settings, damping_model);
    this->SwitchModalReductionON(full_M, full_K, full_Cq, n_modes_settings, damping_model);
}

void ChModalAssembly::ComputeMassCenter() {
    // Build a temporary mesh to collect all nodes and elements in the modal assembly because it happens
    // that the boundary nodes are added in the boundary 'meshlist' whereas their associated elements might
    // be in the 'internal_meshlist', leading to a mess in the mass computation.
    auto mmesh_bou_int = chrono_types::make_shared<ChMesh>();
    // collect boundary mesh
    for (auto& item : meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes())
                mmesh_bou_int->AddNode(node);
            for (auto& ele : mesh->GetElements())
                mmesh_bou_int->AddElement(ele);
        }
    }
    // collect internal mesh
    for (auto& item : internal_meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes())
                mmesh_bou_int->AddNode(node);
            for (auto& ele : mesh->GetElements())
                mmesh_bou_int->AddElement(ele);
        }
    }

    double mass_total = 0;
    ChVector<> mass_weighted_radius(0);

    // for boundary bodies
    for (auto& body : bodylist) {
        if (body->IsActive()) {
            mass_total += body->GetMass();
            mass_weighted_radius += body->GetMass() * body->GetPos();
        }
    }
    // for internal bodies
    for (auto& body : internal_bodylist) {
        if (body->IsActive()) {
            mass_total += body->GetMass();
            mass_weighted_radius += body->GetMass() * body->GetPos();
        }
    }

    // compute the mass properties of the mesh
    double mmesh_mass = 0;
    ChVector<> mmesh_com(0);
    ChMatrix33<> mmesh_inertia(0);
    mmesh_bou_int->ComputeMassProperties(mmesh_mass, mmesh_com, mmesh_inertia);
    mass_total += mmesh_mass;
    mass_weighted_radius += mmesh_mass * mmesh_com;

    if (mass_total)
        this->com_x = mass_weighted_radius / mass_total;
    else
        // located at the position of the first boundary body/node of subassembly
        this->com_x = full_assembly_x_old.segment(0, 3);
}

void ChModalAssembly::CpmputeSelectionMatrix() {
    unsigned int n_bou = n_boundary_bodies;
    for (auto& item : meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            n_bou += mesh->GetNnodes();
        }
    }

    // it is expected the below is true.
    // unsigned int n_bou = n_boundary_coords_w/6;

    ChMatrixDynamic<> pos_bou;
    pos_bou.setZero(3, n_bou);
    unsigned int icol = 0;

    // for boundary bodies and nodes
    for (auto& body : bodylist) {
        if (body->IsActive()) {
            pos_bou.col(icol) = body->GetPos().eigen();
            icol++;
        }
    }
    for (auto& item : meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node)) {
                    // SHOULD NOT HIT HERE since boundary nodes should have 6 DOFs to be able to link with outside
                    pos_bou.col(icol) = xyz->GetPos().eigen();
                    icol++;
                }
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    pos_bou.col(icol) = xyzrot->GetPos().eigen();
                    icol++;
                }
            }
        }
    }

    ChMatrixDynamic<> A;
    A.setZero(n_bou + 1, n_bou);
    A.topRows(n_bou) = pos_bou.transpose() * pos_bou;
    A.bottomRows(1).setOnes();

    ChVectorDynamic<> v;
    v.setZero(n_bou + 1);
    v.head(n_bou) = pos_bou.transpose() * com_x.eigen();
    v.tail(1).setOnes();

    // The below algorithm is equivalent but whether it is better (more robust) than previous one?
    bool test_second_method = false;
    if (test_second_method) {
        ChMatrixDynamic<> diff_bou;
        diff_bou.setZero(3, n_bou);
        for (int i = 0; i < n_bou; i++) {
            diff_bou.col(i) = pos_bou.col(i) - com_x.eigen();
        }

        A.topRows(n_bou) = diff_bou.transpose() * diff_bou;
        A.bottomRows(1).setOnes();

        v.setZero(n_bou + 1);
        v.tail(1).setOnes();
    }

    // The floating frame of reference F is placed approximately at the mass center of the subsystem.
    // The position of the mass center is determined from both boundary and internal bodies/nodes,
    // but the coefficient vector 'alpha' here is evaluated from only boundary bodies/nodes.
    ChVectorDynamic<> alpha;
    alpha.setZero(n_bou);
    alpha = A.colPivHouseholderQr().solve(v);

    this->S.setZero(6, 6 * n_bou);
    for (int i = 0; i < n_bou; i++) {
        S.block(0, 6 * i, 3, 3).diagonal().setConstant(alpha(i));      // translation part
        S.block(3, 6 * i + 3, 3, 3).diagonal().setConstant(alpha(i));  // rotation part
        // S.block(0, 6 * i, 6, 6).diagonal().setConstant(alpha(i));
    }
}

void ChModalAssembly::UpdateFloatingFrameOfReference() {
    ChVectorDynamic<> pos_bou;
    pos_bou.setZero(S.cols());
    ChVectorDynamic<> vel_bou;
    vel_bou.setZero(S.cols());
    ChVectorDynamic<> acc_bou;
    acc_bou.setZero(S.cols());

    unsigned int i_bou = 0;

    // todo:
    // it is better (faster, safer) to retrieve pos_bou,vel_bou,acc_bou
    // from the integrator via IntStateGather(), IntStateGatherAcceleration()
    // for the sake of computation efficiency.
    //
    // for boundary bodies and nodes
    for (auto& body : bodylist) {
        if (body->IsActive()) {
            pos_bou.segment(6 * i_bou, 3) = body->GetPos().eigen();
            pos_bou.segment(6 * i_bou + 3, 3) = body->GetRot().Q_to_Rotv().eigen();
            vel_bou.segment(6 * i_bou, 3) = body->GetPos_dt().eigen();
            vel_bou.segment(6 * i_bou + 3, 3) = body->GetWvel_loc().eigen();
            acc_bou.segment(6 * i_bou, 3) = body->GetPos_dtdt().eigen();
            acc_bou.segment(6 * i_bou + 3, 3) = body->GetWacc_loc().eigen();
            i_bou++;
        }
    }
    for (auto& item : meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node)) {
                    // SHOULD NOT HIT HERE since boundary nodes should have 6 DOFs to be able to link with outside
                    pos_bou.segment(6 * i_bou, 3) = xyz->GetPos().eigen();
                    vel_bou.segment(6 * i_bou, 3) = xyz->GetPos_dt().eigen();
                    acc_bou.segment(6 * i_bou, 3) = xyz->GetPos_dtdt().eigen();
                    i_bou++;
                }
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    pos_bou.segment(6 * i_bou, 3) = xyzrot->GetPos().eigen();
                    pos_bou.segment(6 * i_bou + 3, 3) = xyzrot->GetRot().Q_to_Rotv().eigen();
                    vel_bou.segment(6 * i_bou, 3) = xyzrot->GetPos_dt().eigen();
                    vel_bou.segment(6 * i_bou + 3, 3) = xyzrot->GetWvel_loc().eigen();
                    acc_bou.segment(6 * i_bou, 3) = xyzrot->GetPos_dtdt().eigen();
                    acc_bou.segment(6 * i_bou + 3, 3) = xyzrot->GetWacc_loc().eigen();
                    i_bou++;
                }
            }
        }
    }

    ChVectorDynamic<> pos_F(6);
    pos_F = S * pos_bou;
    floating_frame_F.SetPos(pos_F.head(3));
    ChQuaternion<> rot_F;
    rot_F.Q_from_Rotv(pos_F.tail(3));
    floating_frame_F.SetRot(rot_F);

    ChVectorDynamic<> vel_F(6);
    vel_F = S * vel_bou;
    floating_frame_F.SetPos_dt(vel_F.head(3));
    floating_frame_F.SetWvel_loc(vel_F.tail(3));

    ChVectorDynamic<> acc_F(6);
    acc_F = S * acc_bou;
    floating_frame_F.SetPos_dtdt(acc_F.head(3));
    floating_frame_F.SetWacc_loc(acc_F.tail(3));

    R_F = floating_frame_F.GetA();
    wloc_F = floating_frame_F.GetWvel_loc();
}

void ChModalAssembly::UpdateTransformationMatrix() {
    // update P_B1, P_B2, P_I1, P_I2, P_W, Y
    GetLog() << " * run in line:\t" << __LINE__ << "\n";

    //  for boudnary bodies and nodes
    P_B1.setZero(n_boundary_coords_w, 6);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_B1.block(6 * i_bou, 0, 3, 3) = ChMatrix33<>(1.0);
        P_B1.block(6 * i_bou, 3, 3, 3) =
            -ChStarMatrix33<>(ChVector<>(full_assembly_x_old.segment(7 * i_bou, 3)) - floating_frame_F.GetPos()) * R_F;
        // todo:boundary nodes must have 4 rotational DOFs from quaternion parametrization
        ChQuaternion<> quat_bou = full_assembly_x_old.segment(7 * i_bou + 3, 4);
        ChMatrix33<> R_B(quat_bou);
        P_B1.block(6 * i_bou + 3, 3, 3, 3) = R_B.transpose() * R_F;
        i_bou++;
    }

    P_B2.setIdentity(n_boundary_coords_w, n_boundary_coords_w);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_B2.block(6 * i_bou, 6 * i_bou, 3, 3) = R_F;
        i_bou++;
    }

    // for internal bodies and nodes
    P_I1.setZero(n_internal_coords_w, 6);
    for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
        P_I1.block(6 * i_int, 0, 3, 3) = ChMatrix33<>(1.0);
        P_I1.block(6 * i_int, 3, 3, 3) =
            -ChStarMatrix33<>(ChVector<>(full_assembly_x_old.segment(n_boundary_coords + 7 * i_int, 3)) -
                              floating_frame_F.GetPos()) *
            R_F;
        // todo:internal nodes must have 4 rotational DOFs from quaternion parametrization
        ChQuaternion<> quat_int = full_assembly_x_old.segment(n_boundary_coords + 7 * i_int + 3, 4);
        ChMatrix33<> R_I(quat_int);
        P_I1.block(6 * i_int + 3, 3, 3, 3) = R_I.transpose() * R_F;
        i_int++;
    }

    P_I2.setIdentity(n_internal_coords_w, n_internal_coords_w);
    for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
        P_I2.block(6 * i_int, 6 * i_int, 3, 3) = R_F;
        i_int++;
    }

    P_W.setIdentity(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    P_W.topLeftCorner(n_boundary_coords_w, n_boundary_coords_w) = P_B2;

    ChMatrixDynamic<> I_BB;
    I_BB.setIdentity(n_boundary_coords_w, n_boundary_coords_w);
    Y.setIdentity(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Y.topLeftCorner(n_boundary_coords_w, n_boundary_coords_w) = P_B2.transpose() * (I_BB - P_B1 * S);
}

void util_sparse_assembly_MKRloc(ChSparseMatrix& MKRloc,  ///< resulting square sparse matrix (column major)
                                 const ChSparseMatrix& H_BB,
                                 const ChSparseMatrix& H_BI,
                                 const ChSparseMatrix& H_IB,
                                 const ChSparseMatrix& H_II) {
    int r_B = H_BB.rows();
    int c_B = H_BB.cols();
    int r_I = H_II.rows();
    int c_I = H_II.cols();
    MKRloc.resize(r_B + r_I, c_B + c_I);
    MKRloc.reserve(H_BB.nonZeros() + H_BI.nonZeros() + H_IB.nonZeros() + H_II.nonZeros());
    MKRloc.setZero();

    for (int k = 0; k < H_BB.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H_BB, k); it; ++it) {
            MKRloc.insert(it.row(), it.col()) = it.value();
        }

    for (int k = 0; k < H_BI.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H_BI, k); it; ++it) {
            MKRloc.insert(it.row(), it.col() + c_B) = it.value();
        }

    for (int k = 0; k < H_IB.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H_IB, k); it; ++it) {
            MKRloc.insert(it.row() + r_B, it.col()) = it.value();
        }

    for (int k = 0; k < H_II.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H_II, k); it; ++it) {
            MKRloc.insert(it.row() + r_B, it.col() + c_B) = it.value();
        }

    // This seems necessary in Release mode
    MKRloc.makeCompressed();
}

void ChModalAssembly::ComputeLocalFullKRMmatrix() {
    // 1) fetch the full (not reduced) mass and stiffness
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_R;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyDampingMatrix(&full_R);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    // todo: to fill the sparse P_BI in a more straightforward and efficient way
    ChMatrixDynamic<> P_BI;
    P_BI.setIdentity(n_boundary_coords_w + n_internal_coords_w, n_boundary_coords_w + n_internal_coords_w);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_BI.block(6 * i_bou, 6 * i_bou, 3, 3) = R_F;
        i_bou++;
    }
    for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
        P_BI.block(n_boundary_coords_w + 6 * i_int, n_boundary_coords_w + 6 * i_int, 3, 3) = R_F;
        i_int++;
    }
    ChSparseMatrix P_BI_sp = P_BI.sparseView();

    full_M_loc = P_BI_sp.transpose() * full_M * P_BI_sp;
    full_K_loc = P_BI_sp.transpose() * full_K * P_BI_sp;
    full_R_loc = P_BI_sp.transpose() * full_R * P_BI_sp;
    full_Cq_loc = full_Cq * P_BI_sp;

    full_M_loc.makeCompressed();
    full_K_loc.makeCompressed();
    full_R_loc.makeCompressed();
    full_Cq_loc.makeCompressed();

    //// stiffness matrix in origial mixed basis (pos: absolute frame; rot: local frame)
    // ChSparseMatrix K_BB = full_K.block(0, 0, this->n_boundary_coords_w, this->n_boundary_coords_w);
    // ChSparseMatrix K_II = full_K.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
    // this->n_internal_coords_w,
    //                                    this->n_internal_coords_w);
    // ChSparseMatrix K_BI =
    //     full_K.block(0, this->n_boundary_coords_w, this->n_boundary_coords_w, this->n_internal_coords_w);
    // ChSparseMatrix K_IB =
    //     full_K.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

    //// mass matrix in origial mixed basis (pos: absolute frame; rot: local frame)
    // ChSparseMatrix M_BB = full_M.block(0, 0, this->n_boundary_coords_w, this->n_boundary_coords_w);
    // ChSparseMatrix M_II = full_M.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
    // this->n_internal_coords_w,
    //                                    this->n_internal_coords_w);
    // ChSparseMatrix M_BI =
    //     full_M.block(0, this->n_boundary_coords_w, this->n_boundary_coords_w, this->n_internal_coords_w);
    // ChSparseMatrix M_IB =
    //     full_M.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

    //// damping matrix in origial mixed basis (pos: absolute frame; rot: local frame)
    // ChSparseMatrix R_BB = full_R.block(0, 0, this->n_boundary_coords_w, this->n_boundary_coords_w);
    // ChSparseMatrix R_II = full_R.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
    // this->n_internal_coords_w,
    //                                    this->n_internal_coords_w);
    // ChSparseMatrix R_BI =
    //     full_R.block(0, this->n_boundary_coords_w, this->n_boundary_coords_w, this->n_internal_coords_w);
    // ChSparseMatrix R_IB =
    //     full_R.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

    //// constraint matrix in origial mixed basis (pos: absolute frame; rot: local frame)
    //// ChSparseMatrix Cq_B = full_Cq.block(0, 0, full_Cq.rows(), this->n_boundary_coords_w);
    //// ChSparseMatrix Cq_I = full_Cq.block(0, this->n_boundary_coords_w, full_Cq.rows(), this->n_internal_coords_w);
    // ChSparseMatrix Cq_BB = full_Cq.block(0, 0, this->n_boundary_doc_w, this->n_boundary_coords_w);
    // ChSparseMatrix Cq_II = full_Cq.block(this->n_boundary_doc_w, this->n_boundary_coords_w, this->n_internal_doc_w,
    //                                      this->n_internal_coords_w);
    // ChSparseMatrix Cq_BI =
    //     full_Cq.block(0, this->n_boundary_coords_w, this->n_boundary_doc_w, this->n_internal_coords_w);
    // ChSparseMatrix Cq_IB = full_Cq.block(this->n_boundary_doc_w, 0, this->n_internal_doc_w,
    // this->n_boundary_coords_w);

    //// Compute P_B2,P_I2 in the current (typically initial undeformed) configuration
    // P_B2.setIdentity(n_boundary_coords_w, n_boundary_coords_w);
    // for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
    //     P_B2.block(6 * i_bou, 6 * i_bou, 3, 3) = R_F;
    //     i_bou++;
    // }
    // P_I2.setIdentity(n_internal_coords_w, n_internal_coords_w);
    // for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
    //     P_I2.block(6 * i_int, 6 * i_int, 3, 3) = R_F;
    //     i_int++;
    // }

    //// transformation matrix from original mixed basis to local basis in the floating frame F
    // ChSparseMatrix P_B2_sp = this->P_B2.sparseView();
    // ChSparseMatrix P_I2_sp = this->P_I2.sparseView();

    //// mass matrix in local basis of the floating frame F
    // ChSparseMatrix M_BB_loc = P_B2_sp.transpose() * M_BB * P_B2_sp;
    // ChSparseMatrix M_BI_loc = P_B2_sp.transpose() * M_BI * P_I2_sp;
    // ChSparseMatrix M_IB_loc = P_I2_sp.transpose() * M_IB * P_B2_sp;
    // ChSparseMatrix M_II_loc = P_I2_sp.transpose() * M_II * P_I2_sp;
    // util_sparse_assembly_MKRloc(full_M_loc, M_BB_loc, M_BI_loc, M_IB_loc, M_II_loc);
    // full_M_loc.makeCompressed();

    //// stiffness matrix in local basis of the floating frame F
    // ChSparseMatrix K_BB_loc = P_B2_sp.transpose() * K_BB * P_B2_sp;
    // ChSparseMatrix K_BI_loc = P_B2_sp.transpose() * K_BI * P_I2_sp;
    // ChSparseMatrix K_IB_loc = P_I2_sp.transpose() * K_IB * P_B2_sp;
    // ChSparseMatrix K_II_loc = P_I2_sp.transpose() * K_II * P_I2_sp;
    // util_sparse_assembly_MKRloc(full_K_loc, K_BB_loc, K_BI_loc, K_IB_loc, K_II_loc);
    // full_K_loc.makeCompressed();

    //// damping matrix in local basis of the floating frame F
    // ChSparseMatrix R_BB_loc = P_B2_sp.transpose() * R_BB * P_B2_sp;
    // ChSparseMatrix R_BI_loc = P_B2_sp.transpose() * R_BI * P_I2_sp;
    // ChSparseMatrix R_IB_loc = P_I2_sp.transpose() * R_IB * P_B2_sp;
    // ChSparseMatrix R_II_loc = P_I2_sp.transpose() * R_II * P_I2_sp;
    // util_sparse_assembly_MKRloc(full_R_loc, R_BB_loc, R_BI_loc, R_IB_loc, R_II_loc);
    // full_R_loc.makeCompressed();

    //// constraint matrix in local basis of the floating frame F
    // ChSparseMatrix Cq_BB_loc = Cq_BB * P_B2_sp;
    // ChSparseMatrix Cq_II_loc = Cq_II * P_I2_sp;
    // ChSparseMatrix Cq_BI_loc = Cq_BI * P_I2_sp;
    // ChSparseMatrix Cq_IB_loc = Cq_IB * P_B2_sp;
    // util_sparse_assembly_MKRloc(full_Cq_loc, Cq_BB_loc, Cq_BI_loc, Cq_IB_loc, Cq_II_loc);
    // full_Cq_loc.makeCompressed();
}

void ChModalAssembly::DoModalReduction(const ChModalDamping& damping_model) {
    // 1) compute eigenvalue and eigenvectors of the full subsystem.
    // It is calculated in the local floating frame of reference F, thus there must be six rigid-body modes.
    // It is expected that the eigenvalues of the six rigid-body modes are zero, but
    // maybe nonzero if the geometrical stiffness matrix Kg is involved, we also have the opportunity
    // to consider the inertial damping and inertial stiffness matrices Ri,Ki respectively.
    //
    // ChModalSolveUndamped n_modes_settings(n_modes_coords_w);
    // n_modes_settings.Solve(full_M_loc, full_K_loc, full_Cq_loc, this->modes_V, this->modes_eig, this->modes_freq);
    // this->modes_damping_ratio.setZero(this->modes_freq.rows());

    ChMatrixDynamic<> V_B = this->modes_V.block(0, 0, this->n_boundary_coords_w, this->n_modes_coords_w).real();
    ChMatrixDynamic<> V_I =
        this->modes_V.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_modes_coords_w).real();

    // K_IIc = [  K_II   Cq_II' ]
    //         [ Cq_II     0    ]
    ChSparseMatrix K_II_loc = full_K_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                               this->n_internal_coords_w, this->n_internal_coords_w);
    ChSparseMatrix Cq_II_loc = full_Cq_loc.block(this->n_boundary_doc_w, this->n_boundary_coords_w,
                                                 this->n_internal_doc_w, this->n_internal_coords_w);
    Eigen::SparseMatrix<double> K_IIc_loc;
    util_sparse_assembly_2x2symm(K_IIc_loc, K_II_loc, Cq_II_loc);
    K_IIc_loc.makeCompressed();

    // Matrix of static modes (constrained, so use K_IIc instead of K_II,
    // the original unconstrained Herting reduction is Psi_S = - K_II^{-1} * K_IB
    //
    // Psi_S_C = {Psi_S; Psi_S_LambdaI} = - K_IIc^{-1} * {K_IB ; Cq_IB}
    ChSparseMatrix Cq_IB_loc =
        full_Cq_loc.block(this->n_boundary_doc_w, 0, this->n_internal_doc_w, this->n_boundary_coords_w);
    Psi_S.setZero(this->n_internal_coords_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_LambdaI(this->n_internal_doc_w, this->n_boundary_coords_w);

    // avoid computing K_IIc^{-1}, effectively do n times a linear solve:
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.analyzePattern(K_IIc_loc);
    solver.factorize(K_IIc_loc);
    ChSparseMatrix K_IB_loc =
        full_K_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);
    for (int i = 0; i < this->n_boundary_coords_w; ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
        if (this->n_internal_doc_w)
            rhs << K_IB_loc.col(i).toDense(), Cq_IB_loc.col(i).toDense();
        else
            rhs << K_IB_loc.col(i).toDense();

        ChVectorDynamic<> x = solver.solve(rhs);

        Psi_S.col(i) = -x.head(this->n_internal_coords_w);
        Psi_S_C.col(i) = -x;
        if (this->n_internal_doc_w)
            Psi_S_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
    }

    // Matrix of dynamic modes (V_B and V_I already computed as constrained eigenmodes,
    // but use K_IIc instead of K_II anyway, to reuse K_IIc already factored before)
    //
    // Psi_D_C = {Psi_D; Psi_D_LambdaI} = - K_IIc^{-1} * {(M_IB * V_B + M_II * V_I) ; 0}
    Psi_D.setZero(this->n_internal_coords_w, this->n_modes_coords_w);
    ChMatrixDynamic<> Psi_D_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_modes_coords_w);
    ChMatrixDynamic<> Psi_D_LambdaI(this->n_internal_doc_w, this->n_modes_coords_w);

    ChSparseMatrix M_II_loc = full_M_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                               this->n_internal_coords_w, this->n_internal_coords_w);
    ChSparseMatrix M_IB_loc =
        full_M_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> rhs_top = M_IB_loc * V_B + M_II_loc * V_I;
    for (int i = 0; i < this->n_modes_coords_w; ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
        if (this->n_internal_doc_w)
            rhs << rhs_top.col(i), Eigen::VectorXd::Zero(this->n_internal_doc_w);
        else
            rhs << rhs_top.col(i);

        ChVectorDynamic<> x = solver.solve(rhs);

        Psi_D.col(i) = -x.head(this->n_internal_coords_w);
        Psi_D_C.col(i) = -x;
        if (this->n_internal_doc_w)
            Psi_D_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
    }

    // Psi = [ I     0    ]
    //       [Psi_S  Psi_D]
    Psi.setZero(this->n_boundary_coords_w + this->n_internal_coords_w,
                this->n_boundary_coords_w + this->n_modes_coords_w);
    //***TODO*** maybe prefer sparse Psi matrix, especially for upper blocks...

    Psi << Eigen::MatrixXd::Identity(n_boundary_coords_w, n_boundary_coords_w),
        Eigen::MatrixXd::Zero(n_boundary_coords_w, n_modes_coords_w), Psi_S, Psi_D;

    // Modal reduction of the M K matrices.
    // The tangent mass and stiffness matrices consists of:
    // Linear mass matrix
    // Linear material stiffness matrix, geometrical nonlinear stiffness matrix, inertial stiffness matrix
    // Linear structural damping matrix, inertial damping matrix (gyroscopic matrix, might affect the numerical
    // stability)
    this->M_red = Psi.transpose() * full_M_loc * Psi;
    this->K_red = Psi.transpose() * full_K_loc * Psi;

    // Maybe also have a reduced Cq matrix......
    ChSparseMatrix Cq_B_loc = full_Cq_loc.topRows(this->n_boundary_doc_w);
    this->Cq_red = Cq_B_loc * Psi;

    // Initialize the reduced damping matrix
    this->R_red.setZero(this->M_red.rows(), this->M_red.cols());  // default R=0 , zero damping

    // Reset to zero all the atomic masses of the boundary nodes because now their mass is represented by  this->modal_M
    // NOTE! this should be made more generic and future-proof by implementing a virtual method ex. RemoveMass() in all
    // ChPhysicsItem
    for (auto& body : bodylist) {
        body->SetMass(0);
        body->SetInertia(VNULL);
    }
    for (auto& item : this->meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node))
                    xyz->SetMass(0);
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    xyzrot->SetMass(0);
                    xyzrot->GetInertia().setZero();
                }
            }
        }
    }

    // Modal reduction of R damping matrix: compute using user-provided damping model.
    // todo: maybe the Cq_red is necessary for specifying the suitable modal damping ratios.
    // ChModalDampingNone damping_model;
    damping_model.ComputeR(*this, this->M_red, this->K_red, Psi, this->R_red);
    R_red.setZero(); // set zero for test temporarily

    // Invalidate results of the initial eigenvalue analysis because now the DOFs are different after reduction,
    // to avoid that one could be tempted to plot those eigenmodes, which now are not exactly the ones of the reduced
    // assembly.
    // this->modes_assembly_x0.resize(0);
    this->modes_damping_ratio.resize(0);
    this->modes_eig.resize(0);
    this->modes_freq.resize(0);
    this->modes_V.resize(0, 0);
}

void ChModalAssembly::ComputeInertialKRMmatrix() {
    // fetch the state snapshot (modal reduced)
    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;
    double fooT;
    ChState x_mod;
    ChStateDelta v_mod;
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    ChStateDelta a_mod;
    a_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGatherAcceleration(0, a_mod);

    // update matrices
    V.setZero();
    O_B.setZero();
    O_F.setZero();
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        V.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(R_F.transpose() * v_mod.segment(6 * i_bou, 3));
        O_B.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) = ChStarMatrix33<>(v_mod.segment(6 * i_bou + 3, 3));
        O_F.block(6 * i_bou, 6 * i_bou, 3, 3) = ChStarMatrix33<>(wloc_F);
    }

    // update matrices
    V_acc.setZero();
    V_rmom.setZero();
    O_thetamom.setZero();
    V_F1.setZero();
    V_F2.setZero();
    V_F3.setZero();
    ChVectorDynamic<> momen = M_red * (P_W.transpose() * v_mod);
    ChVectorDynamic<> centr = M_red * (P_W.transpose() * a_mod);
    ChVectorDynamic<> momen_F = O_F * momen;
    ChVectorDynamic<> coriolis = M_red * (V * (U * v_mod));
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        V_acc.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(R_F.transpose() * a_mod.segment(6 * i_bou, 3));
        V_rmom.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(momen.segment(6 * i_bou, 3));
        O_thetamom.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) = ChStarMatrix33<>(momen.segment(6 * i_bou + 3, 3));
        V_F1.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(centr.segment(6 * i_bou, 3));
        V_F2.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(momen_F.segment(6 * i_bou, 3));
        V_F3.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(coriolis.segment(6 * i_bou, 3));
    }

    // inertial mass matrix
    M_sup = P_W * M_red * P_W.transpose();

    // inertial damping matrix
    ChMatrixDynamic<> Ri_1 = P_W * (M_red * V - V_rmom) * U;
    Ri_sup = P_W * (O_F * M_red - M_red * O_F) * P_W.transpose() + Ri_1 - Ri_1.transpose() +
             O_B * M_red * P_W.transpose() - O_thetamom;

    // inertial stiffness matrix
    Ki_sup = P_W * (O_F * M_red - M_red * O_F) * V * U - U.transpose() * V.transpose() * M_red * V * U +
             O_B * M_red * V * U - P_W * (V_F1 + V_F2 + V_F3) * U + P_W * M_red * V_acc * U +
             U.transpose() * V_rmom.transpose() * V * U;

    // quadratic velocity term
    ChMatrixDynamic<> mat_F = P_W * O_F * M_red * P_W.transpose();
    ChMatrixDynamic<> mat_B = O_B * M_red * P_W.transpose();
    ChMatrixDynamic<> mat_M = P_W * M_red * V * U;
    g_quad = (mat_F + mat_B + mat_M - mat_M.transpose()) * v_mod;
}

void ChModalAssembly::ComputeStiffnessMatrix() {
    GetLog() << "run in line:\t" << __LINE__ << "\n";

    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    ChVectorDynamic<> displ_loc;
    displ_loc.setZero(n_boundary_coords_w + n_modes_coords_w);
    displ_loc.tail(n_modes_coords_w) = this->modal_q;
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
        ChVector<> r_BF0 = floating_frame_F0.GetA().transpose() *
                           (modes_assembly_x0.segment(7 * i_bou, 3) - floating_frame_F0.GetPos().eigen());
        displ_loc.segment(6 * i_bou, 3) = (R_F.transpose() * (r_B - floating_frame_F.GetPos()) - r_BF0).eigen();

        ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
        ChMatrix33<> R_B(quat_bou);
        displ_loc.segment(6 * i_bou + 3, 3) =
            (quat_bou.Q_to_Rotv() - R_B.transpose() * (R_F * floating_frame_F.GetRot().Q_to_Rotv().eigen())).eigen();
    }

    // local internal forces of reduced superelement
    g_loc = K_red * displ_loc;

    // material stiffness matrix of reduced superelement
    Km_sup = Y.transpose() * K_red * Y;

    ChMatrix33<> Xi_F1;
    ChMatrix33<> Xi_F2;
    ChMatrix33<> Xi_F3;
    ChMatrixDynamic<> Xi_F;
    ChMatrixDynamic<> Xi_H;
    ChMatrixDynamic<> Xi_V;
    Xi_F1.setZero();
    Xi_F2.setZero();
    Xi_F3.setZero();
    Xi_F.setZero(6, 6);
    Xi_H.setZero(6, n_boundary_coords_w);
    Xi_V.setZero(n_boundary_coords_w, 6);

    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        ChVector<> F_loc = g_loc.segment(6 * i_bou, 3);
        ChVector<> M_loc = g_loc.segment(6 * i_bou + 3, 3);
        ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
        ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
        ChMatrix33<> R_B(quat_bou);
        Xi_F1 += R_F * ChStarMatrix33<>(F_loc);
        Xi_F3 += ChStarMatrix33<>(F_loc) * R_F.transpose() * ChStarMatrix33<>(r_B - floating_frame_F.GetPos()) * R_F -
                 ChStarMatrix33<>(R_F.transpose() * (R_B * M_loc));
        Xi_H.block(3, 6 * i_bou, 3, 3) = ChStarMatrix33<>(F_loc) * R_F.transpose();
        Xi_H.block(3, 6 * i_bou + 3, 3, 3) = R_F.transpose() * (R_B * ChStarMatrix33<>(M_loc));
        Xi_V.block(6 * i_bou, 3, 3, 3) = -R_F * ChStarMatrix33<>(F_loc);
    }
    Xi_F2 = Xi_F1.transpose();
    Xi_F << ChMatrix33<>(0), Xi_F1, Xi_F2, Xi_F3;

    // geometrical stiffness matrix of reduced superelement
    Kg_sup.setZero();
    Kg_sup.topLeftCorner(n_boundary_coords_w, n_boundary_coords_w) =
        S.transpose() * Xi_F * S + S.transpose() * Xi_H + Xi_V * S;
}

void ChModalAssembly::ComputeDampingMatrix() {
    // material damping matrix of reduced superelement.
    // neglect the time derivative term dY_dt in the damping model.
    Rm_sup = Y.transpose() * R_red * Y;
}

void ChModalAssembly::ComputeModalKRMmatrix() {
    this->modal_M = M_sup;
    this->modal_K = Km_sup + Kg_sup + Ki_sup;
    this->modal_R = Rm_sup + Ri_sup;
    this->modal_Cq = Cq_red * P_W.transpose();

    GetLog() << "run in line:\t" << __LINE__ << "\n";
    GetLog() << "modal_M.norm:\t" << modal_M.norm() << "\n";
    GetLog() << "modal_K.norm:\t" << modal_K.norm() << "\n";
    GetLog() << "modal_R.norm:\t" << modal_R.norm() << "\n";
    GetLog() << "modal_Cq.norm:\t" << modal_Cq.norm() << "\n";
}

void ChModalAssembly::SetupModalData(int nmodes_reduction) {
    this->n_modes_coords_w = nmodes_reduction;
    this->Setup();

    // Initialize matrices
    P_B1.setZero(n_boundary_coords_w, 6);
    P_B2.setZero(n_boundary_coords_w, n_boundary_coords_w);
    P_I1.setZero(n_internal_coords_w, 6);
    P_I2.setZero(n_internal_coords_w, n_internal_coords_w);
    P_W.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Y.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);

    O_B.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    V.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);
    O_F.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    V_acc.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);
    V_rmom.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);
    O_thetamom.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    V_F1.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);
    V_F2.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);
    V_F3.setZero(n_boundary_coords_w + n_modes_coords_w, 6 + n_modes_coords_w);

    M_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    K_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    R_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Cq_red.setZero(n_boundary_doc_w, n_boundary_coords_w + n_modes_coords_w);

    Km_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Kg_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Rm_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    M_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Ri_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Ki_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);

    // Extend the selection matrix S to U for the following computation.
    U.setZero(6 + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    U.topLeftCorner(6, n_boundary_coords_w) = S;
    U.bottomRightCorner(n_modes_coords_w, n_modes_coords_w).setIdentity();

    if (!modal_variables || (modal_variables->Get_ndof() != this->n_modes_coords_w)) {
        // Initialize ChVariable object used for modal variables
        if (modal_variables)
            delete modal_variables;
        modal_variables = new ChVariablesGenericDiagonalMass(this->n_modes_coords_w);
        modal_variables->GetMassDiagonal()
            .setZero();  // diag. mass not needed, the mass will be defined via this->modal_Hblock

        // Initialize the modal_Hblock, which is a ChKblockGeneric referencing all ChVariable items:
        std::vector<ChVariables*> mvars;
        // - for BOUNDARY variables: trick to collect all ChVariable references..
        ChSystemDescriptor temporary_descriptor;
        for (auto& body : bodylist)
            body->InjectVariables(temporary_descriptor);
        for (auto& link : linklist)
            link->InjectVariables(temporary_descriptor);
        for (auto& mesh : meshlist)
            mesh->InjectVariables(temporary_descriptor);
        for (auto& item : otherphysicslist)
            item->InjectVariables(temporary_descriptor);
        mvars = temporary_descriptor.GetVariablesList();
        // - for the MODAL variables:
        mvars.push_back(this->modal_variables);

        // NOTE! Purge the not active variables, so that there is a  1-to-1 mapping
        // between the assembly's matrices this->modal_M, modal_K, modal_R and the modal_Hblock->Get_K() block.
        // In fact the ChKblockGeneric modal_Hblock could also handle the not active vars, but the modal_M, K etc are
        // computed for the active-only variables for simplicity in the Herting transformation.
        std::vector<ChVariables*> mvars_active;
        for (auto mvar : mvars) {
            if (mvar->IsActive())
                mvars_active.push_back(mvar);
        }

        this->modal_Hblock.SetVariables(mvars_active);

        // Initialize vectors to be used with modal coordinates:
        this->modal_q.setZero(this->n_modes_coords_w);
        this->modal_q_dt.setZero(this->n_modes_coords_w);
        this->modal_q_dtdt.setZero(this->n_modes_coords_w);
        this->custom_F_modal.setZero(this->n_modes_coords_w);
        this->custom_F_full.setZero(this->n_boundary_coords_w + this->n_internal_coords_w);
    }
}

bool ChModalAssembly::ComputeModes(const ChModalSolveUndamped& n_modes_settings) {
    m_timer_matrix_assembly.start();
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    m_timer_matrix_assembly.stop();

    // SOLVE EIGENVALUE
    this->ComputeModesExternalData(full_M, full_K, full_Cq, n_modes_settings);

    return true;
}

bool ChModalAssembly::ComputeModesExternalData(ChSparseMatrix& full_M,
                                               ChSparseMatrix& full_K,
                                               ChSparseMatrix& full_Cq,
                                               const ChModalSolveUndamped& n_modes_settings) {
    m_timer_setup.start();
    this->SetupInitial();
    this->Setup();
    this->Update();

    // fetch the state snapshot for this analysis
    double fooT;
    ChStateDelta modes_assembly_v0;
    modes_assembly_x0.setZero(this->ncoords, nullptr);
    modes_assembly_v0.setZero(this->ncoords_w, nullptr);
    this->IntStateGather(0, modes_assembly_x0, 0, modes_assembly_v0, fooT);

    // cannot use more modes than n. of tot coords, if so, clamp
    // int nmodes_clamped = ChMin(nmodes, this->ncoords_w);

    this->Setup();

    assert(full_M.rows() == this->ncoords_w);
    assert(full_K.rows() == this->ncoords_w);
    assert(full_Cq.cols() == this->ncoords_w);

    m_timer_setup.stop();

    // SOLVE EIGENVALUE
    // for undamped system (use generalized constrained eigen solver)
    // - Must work with large dimension and sparse matrices only
    // - Must work also in free-free cases, with 6 rigid body modes at 0 frequency.
    m_timer_modal_solver_call.start();
    n_modes_settings.Solve(full_M, full_K, full_Cq, this->modes_V, this->modes_eig, this->modes_freq);
    m_timer_modal_solver_call.stop();

    m_timer_setup.start();

    this->modes_damping_ratio.setZero(this->modes_freq.rows());

    this->Setup();

    m_timer_setup.stop();

    return true;
}

bool ChModalAssembly::ComputeModesDamped(const ChModalSolveDamped& n_modes_settings) {
    m_timer_setup.start();

    this->SetupInitial();
    this->Setup();
    this->Update();

    // fetch the state snapshot of this analysis
    double fooT;
    ChStateDelta modes_assembly_v0;
    modes_assembly_x0.setZero(this->ncoords, nullptr);
    modes_assembly_v0.setZero(this->ncoords_w, nullptr);
    this->IntStateGather(0, modes_assembly_x0, 0, modes_assembly_v0, fooT);

    this->Setup();

    m_timer_setup.stop();

    m_timer_matrix_assembly.start();

    ChSparseMatrix full_M;
    ChSparseMatrix full_R;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyDampingMatrix(&full_R);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    m_timer_matrix_assembly.stop();

    // SOLVE QUADRATIC EIGENVALUE
    // for damped system (use quadratic constrained eigen solver)
    // - Must work with large dimension and sparse matrices only
    // - Must work also in free-free cases, with 6 rigid body modes at 0 frequency.
    m_timer_modal_solver_call.start();
    n_modes_settings.Solve(full_M, full_R, full_K, full_Cq, this->modes_V, this->modes_eig, this->modes_freq,
                           this->modes_damping_ratio);
    m_timer_modal_solver_call.stop();

    m_timer_setup.start();
    this->Setup();
    m_timer_setup.stop();

    return true;
}

void ChModalAssembly::SetFullStateWithModeOverlay(int n_mode, double phase, double amplitude) {
    if (n_mode >= this->modes_V.cols()) {
        this->Update();
        throw ChException("Error: mode " + std::to_string(n_mode) + " is beyond the " +
                          std::to_string(this->modes_V.cols()) + " computed eigenvectors.");
    }

    if (this->modes_V.rows() != this->ncoords_w) {
        this->Update();
        return;
    }

    double fooT = 0;
    ChState assembly_x_new;
    ChStateDelta assembly_v;
    ChStateDelta assembly_Dx_loc;
    ChStateDelta assembly_Dx;

    assembly_x_new.setZero(this->ncoords, nullptr);
    assembly_v.setZero(this->ncoords_w, nullptr);
    assembly_Dx_loc.setZero(this->ncoords_w, nullptr);
    assembly_Dx.setZero(this->ncoords_w, nullptr);

    // pick the nth eigenvector in local reference F
    assembly_Dx_loc = sin(phase) * amplitude * this->modes_V.col(n_mode).real() +
                      cos(phase) * amplitude * this->modes_V.col(n_mode).imag();

    // transform the above local increment in F to the original mixed basis,
    // then it can be accumulated to modes_assembly_x0 to update the position.
    for (int i = 0; i < ncoords_w / 6; ++i) {
        assembly_Dx.segment(6 * i, 3) = R_F * assembly_Dx_loc.segment(6 * i, 3);    // translation
        assembly_Dx.segment(6 * i + 3, 3) = assembly_Dx_loc.segment(6 * i + 3, 3);  // rotation
    }

    this->IntStateIncrement(0, assembly_x_new, this->modes_assembly_x0, 0,
                            assembly_Dx);  // x += amplitude * eigenvector

    this->IntStateScatter(0, assembly_x_new, 0, assembly_v, fooT, true);

    this->Update();
}

void ChModalAssembly::SetInternalStateWithModes(bool full_update) {
    if (!this->is_modal)
        return;

    int bou_int_coords = this->n_boundary_coords + this->n_internal_coords;
    int bou_int_coords_w = this->n_boundary_coords_w + this->n_internal_coords_w;
    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    if (this->Psi.rows() != bou_int_coords_w || this->Psi.cols() != bou_mod_coords_w)
        return;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    // the old state snapshot (modal reduced)
    ChState x0_mod;  // =[qB_old; 0]
    x0_mod.setZero(bou_mod_coords, nullptr);
    x0_mod.segment(0, this->n_boundary_coords) = this->full_assembly_x_old.segment(0, this->n_boundary_coords);

    ChStateDelta assembly_Dx_reduced;  // = [delta_qB; delta_eta]. Note: delta_qB=qB-qB_old, delta_eta=eta-0
    assembly_Dx_reduced.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGetIncrement(0, x_mod, x0_mod, 0, assembly_Dx_reduced);

    // ChStateDelta Dx_local; // =[delta_qB^bar; delta_eta]
    // Dx_local.setZero(bou_mod_coords_w, nullptr);
    // Dx_local = assembly_Dx_reduced;
    // Dx_local.segment(0, n_boundary_coords_w) =
    //     Y.topLeftCorner(n_boundary_coords_w, n_boundary_coords_w) * assembly_Dx_reduced.segment(0,
    //     n_boundary_coords_w);

    //// recover the increment of full state (for both boundary and internal nodes)
    // ChStateDelta assembly_Dx_local; // =[delta_qB^bar; delta_qI^bar]
    // assembly_Dx_local.setZero(bou_int_coords_w, nullptr);
    // assembly_Dx_local = this->Psi * Dx_local;

    ChStateDelta assembly_Dx;  // =[delta_qB; delta_qI]
    assembly_Dx.setZero(bou_int_coords_w, nullptr);
    assembly_Dx.segment(0, n_boundary_coords_w) = assembly_Dx_reduced.segment(0, n_boundary_coords_w);
    assembly_Dx.segment(n_boundary_coords_w, n_internal_coords_w) =
        P_I2 * Psi_S * P_B2.transpose() * assembly_Dx_reduced.segment(0, n_boundary_coords_w) +
        P_I2 * Psi_D * assembly_Dx_reduced.segment(n_boundary_coords_w, n_modes_coords_w);

    ChStateDelta assembly_v;  // =[qB_dt; qI_dt]
    assembly_v.setZero(bou_int_coords_w, nullptr);
    assembly_v.segment(0, n_boundary_coords_w) = v_mod.segment(0, n_boundary_coords_w);
    assembly_v.segment(n_boundary_coords_w, n_internal_coords_w) =
        P_I2 * Psi_S * P_B2.transpose() * v_mod.segment(0, n_boundary_coords_w) +
        P_I2 * Psi_D * v_mod.segment(n_boundary_coords_w, n_modes_coords_w);

    // check: K_IB*P_B1+K_II*P_I1==0. Should be valid, otherwise the modal method is totally wrong!
    bool rigidbody_mode_test = true;
    if (rigidbody_mode_test) {
        ChSparseMatrix K_II_loc = full_K_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                                   this->n_internal_coords_w, this->n_internal_coords_w);
        ChSparseMatrix Cq_II_loc = full_Cq_loc.block(this->n_boundary_doc_w, this->n_boundary_coords_w,
                                                     this->n_internal_doc_w, this->n_internal_coords_w);
        Eigen::SparseMatrix<double> K_IIc_loc;
        util_sparse_assembly_2x2symm(K_IIc_loc, K_II_loc, Cq_II_loc);
        K_IIc_loc.makeCompressed();

        ChSparseMatrix K_IB_loc =
            full_K_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);

        ChSparseMatrix P_B1_sp = P_B1.sparseView();
        ChSparseMatrix P_I1_sp = P_I1.sparseView();

        ChSparseMatrix check = K_IB_loc * P_B1_sp + K_II_loc * P_I1_sp;

        GetLog() << "run in line:\t" << __LINE__ << "\n";
        GetLog() << "check: K_IB*P_B1+K_II*P_I1==0?\t" << check.norm() << "\n";
    }

    bool needs_temporary_bou_int = this->is_modal;
    if (needs_temporary_bou_int)
        this->is_modal = false;

    ChState assembly_x_new;  // =[qB_new; qI_new]
    assembly_x_new.setZero(bou_int_coords, nullptr);
    this->IntStateIncrement(0, assembly_x_new, this->full_assembly_x_old, 0, assembly_Dx);

    // scatter to internal nodes only and update them
    unsigned int displ_x = 0 - this->offset_x;
    unsigned int displ_v = 0 - this->offset_w;
    double T = this->GetChTime();
    for (auto& body : internal_bodylist) {
        if (body->IsActive())
            body->IntStateScatter(displ_x + body->GetOffset_x(), assembly_x_new, displ_v + body->GetOffset_w(),
                                  assembly_v, T, full_update);
        else
            body->Update(T, full_update);
    }
    for (auto& mesh : internal_meshlist) {
        mesh->IntStateScatter(displ_x + mesh->GetOffset_x(), assembly_x_new, displ_v + mesh->GetOffset_w(), assembly_v,
                              T, full_update);
    }
    for (auto& link : internal_linklist) {
        if (link->IsActive())
            link->IntStateScatter(displ_x + link->GetOffset_x(), assembly_x_new, displ_v + link->GetOffset_w(),
                                  assembly_v, T, full_update);
        else
            link->Update(T, full_update);
    }
    for (auto& item : internal_otherphysicslist) {
        item->IntStateScatter(displ_x + item->GetOffset_x(), assembly_x_new, displ_v + item->GetOffset_w(), assembly_v,
                              T, full_update);
    }

    if (needs_temporary_bou_int)
        this->is_modal = true;

    // store the full state for the computation in next time step
    full_assembly_x_old = assembly_x_new;
}

void ChModalAssembly::SetFullStateReset() {
    if (this->modes_assembly_x0.rows() != this->ncoords)
        return;

    double fooT = 0;
    ChStateDelta assembly_v;

    assembly_v.setZero(this->ncoords_w, nullptr);

    this->IntStateScatter(0, this->modes_assembly_x0, 0, assembly_v, fooT, true);

    this->Update();
}

void ChModalAssembly::SetInternalNodesUpdate(bool mflag) {
    this->internal_nodes_update = mflag;
}

//---------------------------------------------------------------------------------------

// Note: removing items from the assembly incurs linear time cost

void ChModalAssembly::AddInternalBody(std::shared_ptr<ChBody> body) {
    assert(std::find(std::begin(internal_bodylist), std::end(internal_bodylist), body) == internal_bodylist.end());
    assert(body->GetSystem() == nullptr);  // should remove from other system before adding here

    // set system and also add collision models to system
    body->SetSystem(system);
    internal_bodylist.push_back(body);

    ////system->is_initialized = false;  // Not needed, unless/until ChBody::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalBody(std::shared_ptr<ChBody> body) {
    auto itr = std::find(std::begin(internal_bodylist), std::end(internal_bodylist), body);
    assert(itr != internal_bodylist.end());

    internal_bodylist.erase(itr);
    body->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalLink(std::shared_ptr<ChLinkBase> link) {
    assert(std::find(std::begin(internal_linklist), std::end(internal_linklist), link) == internal_linklist.end());

    link->SetSystem(system);
    internal_linklist.push_back(link);

    ////system->is_initialized = false;  // Not needed, unless/until ChLink::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalLink(std::shared_ptr<ChLinkBase> link) {
    auto itr = std::find(std::begin(internal_linklist), std::end(internal_linklist), link);
    assert(itr != internal_linklist.end());

    internal_linklist.erase(itr);
    link->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalMesh(std::shared_ptr<fea::ChMesh> mesh) {
    assert(std::find(std::begin(internal_meshlist), std::end(internal_meshlist), mesh) == internal_meshlist.end());

    mesh->SetSystem(system);
    internal_meshlist.push_back(mesh);

    system->is_initialized = false;
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalMesh(std::shared_ptr<fea::ChMesh> mesh) {
    auto itr = std::find(std::begin(internal_meshlist), std::end(internal_meshlist), mesh);
    assert(itr != internal_meshlist.end());

    internal_meshlist.erase(itr);
    mesh->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalOtherPhysicsItem(std::shared_ptr<ChPhysicsItem> item) {
    assert(!std::dynamic_pointer_cast<ChBody>(item));
    assert(!std::dynamic_pointer_cast<ChLinkBase>(item));
    assert(!std::dynamic_pointer_cast<ChMesh>(item));
    assert(std::find(std::begin(internal_otherphysicslist), std::end(internal_otherphysicslist), item) ==
           internal_otherphysicslist.end());
    // assert(item->GetSystem()==nullptr); // should remove from other system before adding here

    // set system and also add collision models to system
    item->SetSystem(system);
    internal_otherphysicslist.push_back(item);

    ////system->is_initialized = false;  // Not needed, unless/until ChPhysicsItem::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalOtherPhysicsItem(std::shared_ptr<ChPhysicsItem> item) {
    auto itr = std::find(std::begin(internal_otherphysicslist), std::end(internal_otherphysicslist), item);
    assert(itr != internal_otherphysicslist.end());

    internal_otherphysicslist.erase(itr);
    item->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternal(std::shared_ptr<ChPhysicsItem> item) {
    if (auto body = std::dynamic_pointer_cast<ChBody>(item)) {
        AddInternalBody(body);
        return;
    }

    if (auto link = std::dynamic_pointer_cast<ChLinkBase>(item)) {
        AddInternalLink(link);
        return;
    }

    if (auto mesh = std::dynamic_pointer_cast<fea::ChMesh>(item)) {
        AddInternalMesh(mesh);
        return;
    }

    AddInternalOtherPhysicsItem(item);
}

void ChModalAssembly::RemoveInternal(std::shared_ptr<ChPhysicsItem> item) {
    if (auto body = std::dynamic_pointer_cast<ChBody>(item)) {
        RemoveInternalBody(body);
        return;
    }

    if (auto link = std::dynamic_pointer_cast<ChLinkBase>(item)) {
        RemoveInternalLink(link);
        return;
    }

    if (auto mesh = std::dynamic_pointer_cast<fea::ChMesh>(item)) {
        RemoveInternalMesh(mesh);
        return;
    }

    RemoveInternalOtherPhysicsItem(item);
}

void ChModalAssembly::RemoveAllInternalBodies() {
    for (auto& body : internal_bodylist) {
        body->SetSystem(nullptr);
    }
    internal_bodylist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalLinks() {
    for (auto& link : internal_linklist) {
        link->SetSystem(nullptr);
    }
    internal_linklist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalMeshes() {
    for (auto& mesh : internal_meshlist) {
        mesh->SetSystem(nullptr);
    }
    internal_meshlist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalOtherPhysicsItems() {
    for (auto& item : internal_otherphysicslist) {
        item->SetSystem(nullptr);
    }
    internal_otherphysicslist.clear();

    if (system)
        system->is_updated = false;
}

// -----------------------------------------------------------------------------

void ChModalAssembly::GetSubassemblyMassMatrix(ChSparseMatrix* M) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the M part only
    KRMmatricesLoad(0, 0, 1.0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(1.0);

    // Fill system-level M matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, M, nullptr, nullptr, nullptr, nullptr, false, false);
    // M->makeCompressed();
}

void ChModalAssembly::GetSubassemblyStiffnessMatrix(ChSparseMatrix* K) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the K part only
    this->KRMmatricesLoad(1.0, 0, 0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(0.0);

    // Fill system-level K matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, K, nullptr, nullptr, nullptr, nullptr, false, false);
    // K->makeCompressed();
}

void ChModalAssembly::GetSubassemblyDampingMatrix(ChSparseMatrix* R) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the R part only
    this->KRMmatricesLoad(0, 1.0, 0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(0.0);

    // Fill system-level R matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, R, nullptr, nullptr, nullptr, nullptr, false, false);
    // R->makeCompressed();
}

void ChModalAssembly::GetSubassemblyConstraintJacobianMatrix(ChSparseMatrix* Cq) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all jacobian matrices
    this->ConstraintsLoadJacobians();

    // Fill system-level R matrix
    temp_descriptor.ConvertToMatrixForm(Cq, nullptr, nullptr, nullptr, nullptr, nullptr, false, false);
    // Cq->makeCompressed();
}

void ChModalAssembly::DumpSubassemblyMatrices(bool save_M, bool save_K, bool save_R, bool save_Cq, const char* path) {
    char filename[300];
    const char* numformat = "%.12g";

    if (save_M) {
        ChSparseMatrix mM;
        this->GetSubassemblyMassMatrix(&mM);
        sprintf(filename, "%s%s", path, "_M.dat");
        ChStreamOutAsciiFile file_M(filename);
        file_M.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mM, file_M);
    }
    if (save_K) {
        ChSparseMatrix mK;
        this->GetSubassemblyStiffnessMatrix(&mK);
        sprintf(filename, "%s%s", path, "_K.dat");
        ChStreamOutAsciiFile file_K(filename);
        file_K.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mK, file_K);
    }
    if (save_R) {
        ChSparseMatrix mR;
        this->GetSubassemblyDampingMatrix(&mR);
        sprintf(filename, "%s%s", path, "_R.dat");
        ChStreamOutAsciiFile file_R(filename);
        file_R.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mR, file_R);
    }
    if (save_Cq) {
        ChSparseMatrix mCq;
        this->GetSubassemblyConstraintJacobianMatrix(&mCq);
        sprintf(filename, "%s%s", path, "_Cq.dat");
        ChStreamOutAsciiFile file_Cq(filename);
        file_Cq.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mCq, file_Cq);
    }
}

// -----------------------------------------------------------------------------

void ChModalAssembly::SetSystem(ChSystem* m_system) {
    ChAssembly::SetSystem(m_system);  // parent

    for (auto& body : internal_bodylist) {
        body->SetSystem(m_system);
    }
    for (auto& link : internal_linklist) {
        link->SetSystem(m_system);
    }
    for (auto& mesh : internal_meshlist) {
        mesh->SetSystem(m_system);
    }
    for (auto& item : internal_otherphysicslist) {
        item->SetSystem(m_system);
    }
}

void ChModalAssembly::SyncCollisionModels() {
    ChAssembly::SyncCollisionModels();  // parent

    for (auto& body : internal_bodylist) {
        body->SyncCollisionModels();
    }
    for (auto& link : internal_linklist) {
        link->SyncCollisionModels();
    }
    for (auto& mesh : internal_meshlist) {
        mesh->SyncCollisionModels();
    }
    for (auto& item : internal_otherphysicslist) {
        item->SyncCollisionModels();
    }
}

// -----------------------------------------------------------------------------
// UPDATING ROUTINES

void ChModalAssembly::SetupInitial() {
    ChAssembly::SetupInitial();  // parent

    for (int ip = 0; ip < internal_bodylist.size(); ++ip) {
        internal_bodylist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_linklist.size(); ++ip) {
        internal_linklist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_meshlist.size(); ++ip) {
        internal_meshlist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_otherphysicslist.size(); ++ip) {
        internal_otherphysicslist[ip]->SetupInitial();
    }
}

// Count all bodies, links, meshes, and other physics items.
// Set counters (DOF, num constraints, etc) and offsets.
void ChModalAssembly::Setup() {
    ChAssembly::Setup();  // parent

    n_boundary_bodies = nbodies;
    n_boundary_links = nlinks;
    n_boundary_meshes = nmeshes;
    n_boundary_physicsitems = nphysicsitems;
    n_boundary_coords = ncoords;
    n_boundary_coords_w = ncoords_w;
    n_boundary_doc = ndoc;
    n_boundary_doc_w = ndoc_w;
    n_boundary_doc_w_C = ndoc_w_C;
    n_boundary_doc_w_D = ndoc_w_D;
    n_boundary_sysvars = nsysvars;
    n_boundary_sysvars_w = nsysvars_w;
    n_boundary_dof = ndof;

    n_internal_bodies = 0;
    n_internal_links = 0;
    n_internal_meshes = 0;
    n_internal_physicsitems = 0;
    n_internal_coords = 0;
    n_internal_coords_w = 0;
    n_internal_doc = 0;
    n_internal_doc_w = 0;
    n_internal_doc_w_C = 0;
    n_internal_doc_w_D = 0;

    // For the "internal" items:
    //

    for (auto& body : internal_bodylist) {
        if (body->GetBodyFixed()) {
            // throw ChException("Cannot use a fixed body as internal");
        } else if (body->GetSleeping()) {
            // throw ChException("Cannot use a sleeping body as internal");
        } else {
            n_internal_bodies++;

            body->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
            body->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
            body->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

            body->Setup();  // currently, no-op

            n_internal_coords += body->GetDOF();
            n_internal_coords_w += body->GetDOF_w();
            n_internal_doc_w += body->GetDOC();  // not really needed since ChBody introduces no constraints
        }
    }

    for (auto& link : internal_linklist) {
        if (link->IsActive()) {
            n_internal_links++;

            link->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
            link->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
            link->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

            link->Setup();  // compute DOFs etc. and sets the offsets also in child items, if any

            n_internal_coords += link->GetDOF();
            n_internal_coords_w += link->GetDOF_w();
            n_internal_doc_w += link->GetDOC();
            n_internal_doc_w_C += link->GetDOC_c();
            n_internal_doc_w_D += link->GetDOC_d();
        }
    }

    for (auto& mesh : internal_meshlist) {
        n_internal_meshes++;

        mesh->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
        mesh->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
        mesh->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

        mesh->Setup();  // compute DOFs and iteratively call Setup for child items

        n_internal_coords += mesh->GetDOF();
        n_internal_coords_w += mesh->GetDOF_w();
        n_internal_doc_w += mesh->GetDOC();
        n_internal_doc_w_C += mesh->GetDOC_c();
        n_internal_doc_w_D += mesh->GetDOC_d();
    }

    for (auto& item : internal_otherphysicslist) {
        n_internal_physicsitems++;

        item->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
        item->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
        item->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

        item->Setup();

        n_internal_coords += item->GetDOF();
        n_internal_coords_w += item->GetDOF_w();
        n_internal_doc_w += item->GetDOC();
        n_internal_doc_w_C += item->GetDOC_c();
        n_internal_doc_w_D += item->GetDOC_d();
    }

    n_internal_doc = n_internal_doc_w + n_internal_bodies;  // number of constraints including quaternion constraints.
    n_internal_sysvars =
        n_internal_coords + n_internal_doc;  // total number of variables (coordinates + lagrangian multipliers)
    n_internal_sysvars_w = n_internal_coords_w + n_internal_doc_w;  // total number of variables (with 6 dof per body)
    n_internal_dof = n_internal_coords_w - n_internal_doc_w;

    this->custom_F_full.setZero(this->n_boundary_coords_w + this->n_internal_coords_w);

    // For the modal part:
    //

    // (nothing to count)

    // For the entire assembly:
    //

    if (this->is_modal == false) {
        ncoords = n_boundary_coords + n_internal_coords;
        ncoords_w = n_boundary_coords_w + n_internal_coords_w;
        ndoc = n_boundary_doc + n_internal_doc;
        ndoc_w = n_boundary_doc_w + n_internal_doc_w;
        ndoc_w_C = n_boundary_doc_w_C + n_internal_doc_w_C;
        ndoc_w_D = n_boundary_doc_w_D + n_internal_doc_w_D;
        nsysvars = n_boundary_sysvars + n_internal_sysvars;
        nsysvars_w = n_boundary_sysvars_w + n_internal_sysvars_w;
        ndof = n_boundary_dof + n_internal_dof;
        nbodies += n_internal_bodies;
        nlinks += n_internal_links;
        nmeshes += n_internal_meshes;
        nphysicsitems += n_internal_physicsitems;
    } else {
        ncoords = n_boundary_coords + n_modes_coords_w;  // no need for a n_modes_coords, same as n_modes_coords_w
        ncoords_w = n_boundary_coords_w + n_modes_coords_w;
        ndoc = n_boundary_doc;
        ndoc_w = n_boundary_doc_w;
        ndoc_w_C = n_boundary_doc_w_C;
        ndoc_w_D = n_boundary_doc_w_D;
        nsysvars = n_boundary_sysvars + n_modes_coords_w;  // no need for a n_modes_coords, same as n_modes_coords_w
        nsysvars_w = n_boundary_sysvars_w + n_modes_coords_w;
        ndof = n_boundary_dof + n_modes_coords_w;

        this->custom_F_modal.setZero(this->n_boundary_coords_w + this->n_modes_coords_w);
    }
}

// Update all physical items (bodies, links, meshes, etc), including their auxiliary variables.
// Updates all forces (automatic, as children of bodies)
// Updates all markers (automatic, as children of bodies).
void ChModalAssembly::Update(bool update_assets) {
    ChAssembly::Update(update_assets);  // parent

    if (is_modal == false) {
        //// NOTE: do not switch these to range for loops (may want to use OMP for)
        for (int ip = 0; ip < (int)internal_bodylist.size(); ++ip) {
            internal_bodylist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_otherphysicslist.size(); ++ip) {
            internal_otherphysicslist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_linklist.size(); ++ip) {
            internal_linklist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_meshlist.size(); ++ip) {
            internal_meshlist[ip]->Update(ChTime, update_assets);
        }

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);
    } else {
        // If in modal reduction mode, the internal parts would not be updated (actually, these could even be removed)
        // However one still might want to see the internal nodes "moving" during animations,
        //
        // todo:
        // maybe here we can call the original update to consider the geometrical nonlinearity,
        // for instance, for tower/blade deflections
        if (this->internal_nodes_update)
            this->SetInternalStateWithModes(update_assets);

        if (m_custom_F_modal_callback)
            m_custom_F_modal_callback->evaluate(this->custom_F_modal, *this);

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);

        this->ComputeMassCenter();
        this->UpdateFloatingFrameOfReference();
        this->UpdateTransformationMatrix();
    }
}

void ChModalAssembly::SetNoSpeedNoAcceleration() {
    ChAssembly::SetNoSpeedNoAcceleration();  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->SetNoSpeedNoAcceleration();
        }
        for (auto& link : internal_linklist) {
            link->SetNoSpeedNoAcceleration();
        }
        for (auto& mesh : internal_meshlist) {
            mesh->SetNoSpeedNoAcceleration();
        }
        for (auto& item : internal_otherphysicslist) {
            item->SetNoSpeedNoAcceleration();
        }
    } else {
        this->modal_q_dt.setZero(this->n_modes_coords_w);
        this->modal_q_dtdt.setZero(this->n_modes_coords_w);
    }
}

void ChModalAssembly::GetStateIncrement(ChStateDelta& Dx, ChStateDelta& v) {
    if (is_modal == false) {
        // to do? not useful for the moment.
        return;
    } else {
        Dx.setZero(this->n_boundary_coords_w + this->n_modes_coords_w, nullptr);

        // fetch the state snapshot (modal reduced)
        int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;
        double fooT;
        ChState x_mod;       // =[qB; eta]
        ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        x_mod.setZero(bou_mod_coords, nullptr);
        v_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        // the old state snapshot (modal reduced)
        ChState x0_mod;  // =[qB_old; 0]
        x0_mod.setZero(bou_mod_coords, nullptr);
        x0_mod.segment(0, this->n_boundary_coords) = this->full_assembly_x_old.segment(0, this->n_boundary_coords);

        ChStateDelta assembly_Dx_reduced;  // = [delta_qB; delta_eta]. Note: delta_qB=qB-qB_old, delta_eta=eta-0
        assembly_Dx_reduced.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGetIncrement(0, x_mod, x0_mod, 0, assembly_Dx_reduced);

        Dx = assembly_Dx_reduced;
        v = v_mod;
    }
}

void ChModalAssembly::IntStateGather(const unsigned int off_x,
                                     ChState& x,
                                     const unsigned int off_v,
                                     ChStateDelta& v,
                                     double& T) {
    ChAssembly::IntStateGather(off_x, x, off_v, v, T);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGather(displ_x + body->GetOffset_x(), x, displ_v + body->GetOffset_w(), v, T);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGather(displ_x + link->GetOffset_x(), x, displ_v + link->GetOffset_w(), v, T);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGather(displ_x + mesh->GetOffset_x(), x, displ_v + mesh->GetOffset_w(), v, T);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateGather(displ_x + item->GetOffset_x(), x, displ_v + item->GetOffset_w(), v, T);
        }
    } else {
        x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) = this->modal_q;
        v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_q_dt;

        T = GetChTime();
    }
}

void ChModalAssembly::IntStateScatter(const unsigned int off_x,
                                      const ChState& x,
                                      const unsigned int off_v,
                                      const ChStateDelta& v,
                                      const double T,
                                      bool full_update) {
    ChAssembly::IntStateScatter(off_x, x, off_v, v, T, full_update);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatter(displ_x + body->GetOffset_x(), x, displ_v + body->GetOffset_w(), v, T,
                                      full_update);
            else
                body->Update(T, full_update);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatter(displ_x + mesh->GetOffset_x(), x, displ_v + mesh->GetOffset_w(), v, T, full_update);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatter(displ_x + link->GetOffset_x(), x, displ_v + link->GetOffset_w(), v, T,
                                      full_update);
            else
                link->Update(T, full_update);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateScatter(displ_x + item->GetOffset_x(), x, displ_v + item->GetOffset_w(), v, T, full_update);
        }

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);
    } else {
        this->modal_q = x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w);
        this->modal_q_dt = v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);

        // Update:
        // If in modal reduction mode, the internal parts would not be updated (actually, these could even be removed)
        // However one still might want to see the internal nodes "moving" during animations,
        if (this->internal_nodes_update)
            this->SetInternalStateWithModes(full_update);

        if (m_custom_F_modal_callback)
            m_custom_F_modal_callback->evaluate(this->custom_F_modal, *this);

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);
    }
}

void ChModalAssembly::IntStateGatherAcceleration(const unsigned int off_a, ChStateDelta& a) {
    ChAssembly::IntStateGatherAcceleration(off_a, a);  // parent

    unsigned int displ_a = off_a - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGatherAcceleration(displ_a + body->GetOffset_w(), a);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGatherAcceleration(displ_a + link->GetOffset_w(), a);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGatherAcceleration(displ_a + mesh->GetOffset_w(), a);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateGatherAcceleration(displ_a + item->GetOffset_w(), a);
        }
    } else {
        a.segment(off_a + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_q_dtdt;
    }
}

// From state derivative (acceleration) to system, sometimes might be needed
void ChModalAssembly::IntStateScatterAcceleration(const unsigned int off_a, const ChStateDelta& a) {
    ChAssembly::IntStateScatterAcceleration(off_a, a);  // parent

    unsigned int displ_a = off_a - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatterAcceleration(displ_a + body->GetOffset_w(), a);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatterAcceleration(displ_a + link->GetOffset_w(), a);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatterAcceleration(displ_a + mesh->GetOffset_w(), a);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateScatterAcceleration(displ_a + item->GetOffset_w(), a);
        }
    } else {
        this->modal_q_dtdt = a.segment(off_a + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

// From system to reaction forces (last computed) - some timestepper might need this
void ChModalAssembly::IntStateGatherReactions(const unsigned int off_L, ChVectorDynamic<>& L) {
    ChAssembly::IntStateGatherReactions(off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGatherReactions(displ_L + body->GetOffset_L(), L);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGatherReactions(displ_L + link->GetOffset_L(), L);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGatherReactions(displ_L + mesh->GetOffset_L(), L);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateGatherReactions(displ_L + item->GetOffset_L(), L);
        }
    } else {
        // todo:
        //  there might be reactions in the reduced modal assembly due to the existance of this->modal_Cq
    }
}

// From reaction forces to system, ex. store last computed reactions in ChLinkBase objects for plotting etc.
void ChModalAssembly::IntStateScatterReactions(const unsigned int off_L, const ChVectorDynamic<>& L) {
    ChAssembly::IntStateScatterReactions(off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatterReactions(displ_L + body->GetOffset_L(), L);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatterReactions(displ_L + link->GetOffset_L(), L);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatterReactions(displ_L + mesh->GetOffset_L(), L);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntStateScatterReactions(displ_L + item->GetOffset_L(), L);
        }
    } else {
        // todo:
        //  there might be reactions in the reduced modal assembly due to the existance of this->modal_Cq
    }
}

void ChModalAssembly::IntStateIncrement(const unsigned int off_x,
                                        ChState& x_new,
                                        const ChState& x,
                                        const unsigned int off_v,
                                        const ChStateDelta& Dv) {
    ChAssembly::IntStateIncrement(off_x, x_new, x, off_v, Dv);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateIncrement(displ_x + body->GetOffset_x(), x_new, x, displ_v + body->GetOffset_w(), Dv);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateIncrement(displ_x + link->GetOffset_x(), x_new, x, displ_v + link->GetOffset_w(), Dv);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntStateIncrement(displ_x + mesh->GetOffset_x(), x_new, x, displ_v + mesh->GetOffset_w(), Dv);
        }

        for (auto& item : internal_otherphysicslist) {
            item->IntStateIncrement(displ_x + item->GetOffset_x(), x_new, x, displ_v + item->GetOffset_w(), Dv);
        }
    } else {
        x_new.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) =
            x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) +
            Dv.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntStateGetIncrement(const unsigned int off_x,
                                           const ChState& x_new,
                                           const ChState& x,
                                           const unsigned int off_v,
                                           ChStateDelta& Dv) {
    ChAssembly::IntStateGetIncrement(off_x, x_new, x, off_v, Dv);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGetIncrement(displ_x + body->GetOffset_x(), x_new, x, displ_v + body->GetOffset_w(), Dv);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGetIncrement(displ_x + link->GetOffset_x(), x_new, x, displ_v + link->GetOffset_w(), Dv);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGetIncrement(displ_x + mesh->GetOffset_x(), x_new, x, displ_v + mesh->GetOffset_w(), Dv);
        }

        for (auto& item : internal_otherphysicslist) {
            item->IntStateGetIncrement(displ_x + item->GetOffset_x(), x_new, x, displ_v + item->GetOffset_w(), Dv);
        }
    } else {
        Dv.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) =
            x_new.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) -
            x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntLoadResidual_F(const unsigned int off,  ///< offset in R residual
                                        ChVectorDynamic<>& R,    ///< result: the R residual, R += c*F
                                        const double c)          ///< a scaling factor
{
    ChAssembly::IntLoadResidual_F(off, R, c);  // parent

    unsigned int displ_v = off - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_F(displ_v + body->GetOffset_w(), R, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_F(displ_v + link->GetOffset_w(), R, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_F(displ_v + mesh->GetOffset_w(), R, c);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadResidual_F(displ_v + item->GetOffset_w(), R, c);
        }

        // Add custom forces (applied to the original non reduced system)
        if (!this->custom_F_full.isZero()) {
            R.segment(displ_v, this->n_boundary_coords_w + this->n_internal_coords_w) += c * this->custom_F_full;
        }
    } else {
        // 1-
        // Add elastic forces from current modal deformations
        ChStateDelta Dx_reduced(this->n_boundary_coords_w + this->n_modes_coords_w, nullptr);
        ChStateDelta v_reduced(this->n_boundary_coords_w + this->n_modes_coords_w, nullptr);
        this->GetStateIncrement(Dx_reduced, v_reduced);

        // todo:
        // shall we add the quadratic velocity term here?
        R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) -=
            c * (this->modal_K * Dx_reduced + this->modal_R * v_reduced + g_quad);  //  note -= sign

        // 2-
        // Add custom forces (in modal coordinates)
        if (!this->custom_F_modal.isZero())
            // todo: to check the algorithm of this->custom_F_modal
            R.segment(off + this->n_boundary_coords_w, this->n_modes_coords_w) += c * this->custom_F_modal;

        // 3-
        // Add custom forces (applied to the original non reduced system, and transformed into reduced)
        if (!this->custom_F_full.isZero()) {
            ChVectorDynamic<> F_reduced;
            F_reduced.setZero(this->n_boundary_coords_w + this->n_modes_coords_w);
            F_reduced.head(n_boundary_coords_w) =
                this->custom_F_full.head(this->n_boundary_coords_w) +
                P_B2 * Psi_S.transpose() * P_I2.transpose() * this->custom_F_full.tail(this->n_internal_coords_w);
            F_reduced.tail(n_modes_coords_w) =
                Psi_D.transpose() * P_I2.transpose() * this->custom_F_full.tail(this->n_internal_coords_w);
            R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) += c * F_reduced;
        }
    }
}

void ChModalAssembly::IntLoadResidual_Mv(const unsigned int off,      ///< offset in R residual
                                         ChVectorDynamic<>& R,        ///< result: the R residual, R += c*M*v
                                         const ChVectorDynamic<>& w,  ///< the w vector
                                         const double c               ///< a scaling factor
) {
    unsigned int displ_v = off - this->offset_w;

    if (is_modal == false) {
        ChAssembly::IntLoadResidual_Mv(off, R, w, c);  // parent

        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_Mv(displ_v + body->GetOffset_w(), R, w, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_Mv(displ_v + link->GetOffset_w(), R, w, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_Mv(displ_v + mesh->GetOffset_w(), R, w, c);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadResidual_Mv(displ_v + item->GetOffset_w(), R, w, c);
        }
    } else {
        ChVectorDynamic<> w_modal = w.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w);
        R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) += c * (this->modal_M * w_modal);
    }
}

void ChModalAssembly::IntLoadResidual_CqL(const unsigned int off_L,    ///< offset in L multipliers
                                          ChVectorDynamic<>& R,        ///< result: the R residual, R += c*Cq'*L
                                          const ChVectorDynamic<>& L,  ///< the L vector
                                          const double c               ///< a scaling factor
) {
    ChAssembly::IntLoadResidual_CqL(off_L, R, L, c);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_CqL(displ_L + body->GetOffset_L(), R, L, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_CqL(displ_L + link->GetOffset_L(), R, L, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_CqL(displ_L + mesh->GetOffset_L(), R, L, c);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadResidual_CqL(displ_L + item->GetOffset_L(), R, L, c);
        }
    } else {
        // todo:
        //  there might be residual CqL in the reduced modal assembly
    }
}

void ChModalAssembly::IntLoadConstraint_C(const unsigned int off_L,  ///< offset in Qc residual
                                          ChVectorDynamic<>& Qc,     ///< result: the Qc residual, Qc += c*C
                                          const double c,            ///< a scaling factor
                                          bool do_clamp,             ///< apply clamping to c*C?
                                          double recovery_clamp      ///< value for min/max clamping of c*C
) {
    ChAssembly::IntLoadConstraint_C(off_L, Qc, c, do_clamp, recovery_clamp);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadConstraint_C(displ_L + body->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadConstraint_C(displ_L + link->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadConstraint_C(displ_L + mesh->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadConstraint_C(displ_L + item->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
    } else {
        // todo:
        //  there might be constraint C in the reduced modal assembly
    }
}

void ChModalAssembly::IntLoadConstraint_Ct(const unsigned int off_L,  ///< offset in Qc residual
                                           ChVectorDynamic<>& Qc,     ///< result: the Qc residual, Qc += c*Ct
                                           const double c             ///< a scaling factor
) {
    ChAssembly::IntLoadConstraint_Ct(off_L, Qc, c);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadConstraint_Ct(displ_L + body->GetOffset_L(), Qc, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadConstraint_Ct(displ_L + link->GetOffset_L(), Qc, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadConstraint_Ct(displ_L + mesh->GetOffset_L(), Qc, c);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadConstraint_Ct(displ_L + item->GetOffset_L(), Qc, c);
        }
    } else {
        // todo:
        //  there might be constraint Ct in the reduced modal assembly
    }
}

void ChModalAssembly::IntToDescriptor(const unsigned int off_v,
                                      const ChStateDelta& v,
                                      const ChVectorDynamic<>& R,
                                      const unsigned int off_L,
                                      const ChVectorDynamic<>& L,
                                      const ChVectorDynamic<>& Qc) {
    ChAssembly::IntToDescriptor(off_v, v, R, off_L, L, Qc);  // parent

    unsigned int displ_L = off_L - this->offset_L;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntToDescriptor(displ_v + body->GetOffset_w(), v, R, displ_L + body->GetOffset_L(), L, Qc);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntToDescriptor(displ_v + link->GetOffset_w(), v, R, displ_L + link->GetOffset_L(), L, Qc);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntToDescriptor(displ_v + mesh->GetOffset_w(), v, R, displ_L + mesh->GetOffset_L(), L, Qc);
        }

        for (auto& item : internal_otherphysicslist) {
            item->IntToDescriptor(displ_v + item->GetOffset_w(), v, R, displ_L + item->GetOffset_L(), L, Qc);
        }
    } else {
        this->modal_variables->Get_qb() = v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
        this->modal_variables->Get_fb() = R.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntFromDescriptor(const unsigned int off_v,
                                        ChStateDelta& v,
                                        const unsigned int off_L,
                                        ChVectorDynamic<>& L) {
    ChAssembly::IntFromDescriptor(off_v, v, off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntFromDescriptor(displ_v + body->GetOffset_w(), v, displ_L + body->GetOffset_L(), L);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntFromDescriptor(displ_v + link->GetOffset_w(), v, displ_L + link->GetOffset_L(), L);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntFromDescriptor(displ_v + mesh->GetOffset_w(), v, displ_L + mesh->GetOffset_L(), L);
        }

        for (auto& item : internal_otherphysicslist) {
            item->IntFromDescriptor(displ_v + item->GetOffset_w(), v, displ_L + item->GetOffset_L(), L);
        }
    } else {
        v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_variables->Get_qb();
    }
}

// -----------------------------------------------------------------------------

void ChModalAssembly::InjectVariables(ChSystemDescriptor& mdescriptor) {
    ChAssembly::InjectVariables(mdescriptor);  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->InjectVariables(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectVariables(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectVariables(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectVariables(mdescriptor);
        }
    } else {
        mdescriptor.InsertVariables(this->modal_variables);
    }
}

void ChModalAssembly::InjectConstraints(ChSystemDescriptor& mdescriptor) {
    ChAssembly::InjectConstraints(mdescriptor);  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->InjectConstraints(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectConstraints(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectConstraints(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectConstraints(mdescriptor);
        }
    } else {
        // todo:
        //  there might be constraints for the reduced modal assembly: this->modal_Cq
    }
}

void ChModalAssembly::ConstraintsLoadJacobians() {
    ChAssembly::ConstraintsLoadJacobians();  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->ConstraintsLoadJacobians();
        }
        for (auto& link : internal_linklist) {
            link->ConstraintsLoadJacobians();
        }
        for (auto& mesh : internal_meshlist) {
            mesh->ConstraintsLoadJacobians();
        }
        for (auto& item : internal_otherphysicslist) {
            item->ConstraintsLoadJacobians();
        }
    } else {
        // todo:
        //  there might be constraints for the reduced modal assembly: this->modal_Cq
    }
}

void ChModalAssembly::InjectKRMmatrices(ChSystemDescriptor& mdescriptor) {
    if (is_modal == false) {
        ChAssembly::InjectKRMmatrices(mdescriptor);  // parent

        for (auto& body : internal_bodylist) {
            body->InjectKRMmatrices(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectKRMmatrices(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectKRMmatrices(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectKRMmatrices(mdescriptor);
        }
    } else {
        mdescriptor.InsertKblock(&this->modal_Hblock);
    }
}

void ChModalAssembly::KRMmatricesLoad(double Kfactor, double Rfactor, double Mfactor) {
    if (is_modal == false) {
        ChAssembly::KRMmatricesLoad(Kfactor, Rfactor, Mfactor);  // parent

        for (auto& body : internal_bodylist) {
            body->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& link : internal_linklist) {
            link->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
    } else {
        ComputeInertialKRMmatrix();
        ComputeStiffnessMatrix();
        ComputeDampingMatrix();
        ComputeModalKRMmatrix();

        this->modal_Hblock.Get_K() = this->modal_K * Kfactor + this->modal_R * Rfactor + this->modal_M * Mfactor;
    }
}

// -----------------------------------------------------------------------------
//  STREAMING - FILE HANDLING

void ChModalAssembly::ArchiveOut(ChArchiveOut& marchive) {
    // version number
    marchive.VersionWrite<ChModalAssembly>();

    // serialize parent class
    ChAssembly::ArchiveOut(marchive);

    // serialize all member data:

    marchive << CHNVP(internal_bodylist, "internal_bodies");
    marchive << CHNVP(internal_linklist, "internal_links");
    marchive << CHNVP(internal_meshlist, "internal_meshes");
    marchive << CHNVP(internal_otherphysicslist, "internal_other_physics_items");
    marchive << CHNVP(is_modal, "is_modal");
    marchive << CHNVP(modal_q, "modal_q");
    marchive << CHNVP(modal_q_dt, "modal_q_dt");
    marchive << CHNVP(modal_q_dtdt, "modal_q_dtdt");
    marchive << CHNVP(custom_F_modal, "custom_F_modal");
    marchive << CHNVP(custom_F_full, "custom_F_full");
    marchive << CHNVP(internal_nodes_update, "internal_nodes_update");
}

void ChModalAssembly::ArchiveIn(ChArchiveIn& marchive) {
    // version number
    /*int version =*/marchive.VersionRead<ChModalAssembly>();

    // deserialize parent class
    ChAssembly::ArchiveIn(marchive);

    // stream in all member data:

    // trick needed because the "AddIntenal...()" functions are required
    std::vector<std::shared_ptr<ChBody>> tempbodies;
    marchive >> CHNVP(tempbodies, "internal_bodies");
    RemoveAllBodies();
    for (auto& body : tempbodies)
        AddInternalBody(body);
    std::vector<std::shared_ptr<ChLink>> templinks;
    marchive >> CHNVP(templinks, "internal_links");
    RemoveAllLinks();
    for (auto& link : templinks)
        AddInternalLink(link);
    std::vector<std::shared_ptr<ChMesh>> tempmeshes;
    marchive >> CHNVP(tempmeshes, "internal_mesh");
    RemoveAllMeshes();
    for (auto& mesh : tempmeshes)
        AddInternalMesh(mesh);
    std::vector<std::shared_ptr<ChPhysicsItem>> tempotherphysics;
    marchive >> CHNVP(tempotherphysics, "internal_other_physics_items");
    RemoveAllOtherPhysicsItems();
    for (auto& mphys : tempotherphysics)
        AddInternalOtherPhysicsItem(mphys);

    marchive >> CHNVP(is_modal, "is_modal");
    marchive >> CHNVP(modal_q, "modal_q");
    marchive >> CHNVP(modal_q_dt, "modal_q_dt");
    marchive >> CHNVP(modal_q_dtdt, "modal_q_dtdt");
    marchive >> CHNVP(custom_F_modal, "custom_F_modal");
    marchive >> CHNVP(custom_F_full, "custom_F_full");
    marchive >> CHNVP(internal_nodes_update, "internal_nodes_update");

    // Recompute statistics, offsets, etc.
    Setup();
}

}  // end namespace modal

}  // end namespace chrono
