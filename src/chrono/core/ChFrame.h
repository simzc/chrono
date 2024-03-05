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

#ifndef CH_FRAME_H
#define CH_FRAME_H

#include "chrono/core/ChCoordsys.h"
#include "chrono/core/ChMatrix.h"
#include "chrono/core/ChMatrix33.h"
#include "chrono/core/ChMatrixMBD.h"

namespace chrono {

/// Representation of a 3D transform.
/// A 'frame' coordinate system has a translation and a rotation respect to a 'parent' coordinate system, usually the
/// absolute (world) coordinates. Differently from a simple ChCoordsys object, the ChFrame also stores the 3x3 rotation
/// matrix implements, which permits some optimizations, especially when a large number of vectors must be transformed
/// by the same frame.
///
/// See @ref coordinate_transformations manual page.
template <class Real = double>
class ChFrame {
  public:
    /// Default constructor, or construct from pos and rot (as a quaternion)
    explicit ChFrame(const ChVector3<Real>& v = ChVector3<Real>(0, 0, 0),
                     const ChQuaternion<Real>& q = ChQuaternion<Real>(1, 0, 0, 0))
        : Csys(v, q), Rmat(q) {}

    /// Construct from pos and rotation (as a 3x3 matrix)
    ChFrame(const ChVector3<Real>& v, const ChMatrix33<Real>& R) : Csys(v, R.GetQuaternion()), Rmat(R) {}

    /// Construct from a coordsys
    explicit ChFrame(const ChCoordsys<Real>& C) : Csys(C), Rmat(C.rot) {}

    /// Construct from position mv and rotation of angle alpha around unit vector mu
    ChFrame(const ChVector3<Real>& v, const Real angle, const ChVector3<Real>& u) : Csys(v, angle, u) {
        Rmat.SetFromQuaternion(Csys.rot);
    }

    /// Copy constructor, build from another frame
    ChFrame(const ChFrame<Real>& other) : Csys(other.Csys), Rmat(other.Rmat) {}

    virtual ~ChFrame() {}

    // OPERATORS OVERLOADING

    /// Assignment operator: copy from another frame.
    ChFrame<Real>& operator=(const ChFrame<Real>& other) {
        if (&other == this)
            return *this;
        Csys = other.Csys;
        Rmat = other.Rmat;
        return *this;
    }

    /// Returns true for identical frames.
    bool operator==(const ChFrame<Real>& other) const { return Equals(other); }

    /// Returns true for different frames.
    bool operator!=(const ChFrame<Real>& other) const { return !Equals(other); }

    /// Transform another frame through this frame.
    /// If A is this frame and F another frame expressed in A, then G = A * F is the frame F expresssed in the parent
    /// frame of A. For a sequence of transformations, i.e. a chain of coordinate systems, one can also write:
    ///   G = F_1to0 * F_2to1 * F_3to2 * F;
    /// i.e., just like done with a sequence of Denavitt-Hartemberg matrix multiplications.
    /// This operation is not commutative.
    ChFrame<Real> operator*(const ChFrame<Real>& F) const { return this->TransformLocalToParent(F); }

    /// Transform  another frame through this frame.
    /// If A is this frame and F another frame expressed in A, then G = F >> A is the frame F expresssed in the parent
    /// frame of A. For a sequence of transformations, i.e. a chain of coordinate systems, one can also write:
    ///   G = F >> F_3to2 >> F_2to1 >> F_1to0;
    /// i.e., just like done with a sequence of Denavitt-Hartemberg matrix multiplications (but reverting order).
    /// This operation is not commutative.
    ChFrame<Real> operator>>(const ChFrame<Real>& F) const { return F.TransformLocalToParent(*this); }

    /// Transform a vector through this frame (express in parent frame).
    /// If A is this frame and v a vector expressed in this frame, w = A * v is the vector expressed in the parent
    /// frame of A. For a sequence of transformations, i.e. a chain of coordinate systems, one can also write:
    ///   w = F_1to0 * F_2to1 * F_3to2 * v;
    /// i.e., just like done with a sequence of Denavitt-Hartemberg matrix multiplications.
    /// This operation is not commutative.
    /// NOTE: since c++ operator execution is from left to right, in case of multiple transformations
    ///   w = v >> C >> B >> A
    /// may be faster than
    ///   w = A * B * C * v
    ChVector3<Real> operator*(const ChVector3<Real>& v) const { return TransformPointLocalToParent(v); }

    /// Transform a vector through this frame (express from parent frame).
    /// If A is this frame and v a vector expressed in the parent frame of A, then w = A / v is the vector expressed in
    /// A. In other words, w = A * v  implies v = A/w.
    ChVector3<Real> operator/(const ChVector3<Real>& v) const { return TransformPointParentToLocal(v); }

    /// Transform this frame by pre-multiplication with another frame.
    /// If A is this frame, then A >>= F means A' = F * A or A' = A >> F.
    ChFrame<Real>& operator>>=(const ChFrame<Real>& F) {
        ConcatenatePreTransformation(F);
        return *this;
    }

    /// Transform this frame by post-multiplication with another frame.
    /// If A is this frame, then A *= F means A' = A * F or A' = F >> A.
    ChFrame<Real>& operator*=(const ChFrame<Real>& F) {
        ConcatenatePostTransformation(F);
        return *this;
    }

    // Mixed type operators

    /// Transform this frame by pre-multiplication with a given vector (translate frame).
    ChFrame<Real>& operator>>=(const ChVector3<Real>& v) {
        this->Csys.pos += v;
        return *this;
    }

    /// Transform this frame by pre-multiplication with a given quaternion (rotate frame).
    ChFrame<Real>& operator>>=(const ChQuaternion<Real>& q) {
        this->SetCsys(q.Rotate(this->Csys.pos), this->Csys.rot >> q);
        return *this;
    }

    /// Transform this frame by pre-multiplication with a given coordinate system.
    ChFrame<Real>& operator>>=(const ChCoordsys<Real>& C) {
        this->SetCsys(this->Csys >> C);
        return *this;
    }

    // FUNCTIONS

    /// Return both current rotation and translation as a ChCoordsys object.
    ChCoordsys<Real>& GetCsys() { return Csys; }
    const ChCoordsys<Real>& GetCsys() const { return Csys; }

    /// Return the current translation as a 3d vector.
    ChVector3<Real>& GetPos() { return Csys.pos; }
    const ChVector3<Real>& GetPos() const { return Csys.pos; }

    /// Return the current rotation as a quaternion.
    ChQuaternion<Real>& GetRot() { return Csys.rot; }
    const ChQuaternion<Real>& GetRot() const { return Csys.rot; }

    /// Return the current rotation as a 3x3 matrix.
    ChMatrix33<Real>& GetRotMat() { return Rmat; }
    const ChMatrix33<Real>& GetRotMat() const { return Rmat; }

    /// Get axis of finite rotation, in parent space.
    ChVector3<Real> GetRotAxis() {
        ChVector3<Real> vtmp;
        Real angle;
        Csys.rot.GetAngleAxis(angle, vtmp);
        return vtmp;
    }

    /// Get angle of rotation about axis of finite rotation.
    Real GetRotAngle() {
        ChVector3<Real> vtmp;
        Real angle;
        Csys.rot.GetAngleAxis(angle, vtmp);
        return angle;
    }

    // SET-FUNCTIONS

    /// Impose both translation and rotation as a single ChCoordsys.
    /// Note: the quaternion part must be already normalized.
    void SetCsys(const ChCoordsys<Real>& C) {
        Csys = C;
        Rmat.SetFromQuaternion(C.rot);
    }

    /// Impose both translation and rotation.
    /// Note: the quaternion part must be already normalized.
    void SetCsys(const ChVector3<Real>& v, const ChQuaternion<Real>& q) {
        Csys.pos = v;
        Csys.rot = q;
        Rmat.SetFromQuaternion(q);
    }

    /// Impose the rotation as a quaternion.
    /// Note: the quaternion must be already normalized.
    void SetRot(const ChQuaternion<Real>& q) {
        Csys.rot = q;
        Rmat.SetFromQuaternion(q);
    }

    /// Impose the rotation as a 3x3 matrix.
    /// Note: the rotation matrix must be already orthogonal.
    void SetRot(const ChMatrix33<Real>& R) {
        Csys.rot = R.GetQuaternion();
        Rmat = R;
    }

    /// Impose the translation
    void SetPos(const ChVector3<Real>& mpos) { Csys.pos = mpos; }

    // FUNCTIONS TO TRANSFORM THE FRAME ITSELF

    /// Apply a transformation (rotation and translation) represented by another frame.
    /// This is equivalent to pre-multiply this frame by the other frame F:
    ///     this'= F * this
    ///  or
    ///     this' = this >> F
    void ConcatenatePreTransformation(const ChFrame<Real>& F) {
        this->SetCsys(F.TransformPointLocalToParent(Csys.pos), F.Csys.rot * Csys.rot);
    }

    /// Apply a transformation (rotation and translation) represented by another frame F in local coordinate.
    /// This is equivalent to post-multiply this frame by the other frame F:
    ///    this'= this * F
    ///  or
    ///    this'= F >> this
    void ConcatenatePostTransformation(const ChFrame<Real>& F) {
        this->SetCsys(TransformPointLocalToParent(F.Csys.pos), Csys.rot * F.Csys.rot);
    }

    /// An easy way to move the frame by the amount specified by vector v,
    /// (assuming v expressed in parent coordinates)
    void Move(const ChVector3<Real>& v) { this->Csys.pos += v; }

    /// Apply both translation and rotation, assuming both expressed in parent
    /// coordinates, as a vector for translation and quaternion for rotation,
    void Move(const ChCoordsys<Real>& C) { this->SetCsys(C.TransformPointLocalToParent(Csys.pos), C.rot * Csys.rot); }

    // FUNCTIONS FOR COORDINATE TRANSFORMATIONS

    /// Transform a point from the local frame coordinate system to the parent coordinate system.
    ChVector3<Real> TransformPointLocalToParent(const ChVector3<Real>& v) const { return Csys.pos + Rmat * v; }

    /// Transforms a point from the parent coordinate system to local frame coordinate system.
    ChVector3<Real> TransformPointParentToLocal(const ChVector3<Real>& v) const {
        return Rmat.transpose() * (v - Csys.pos);
    }

    /// Transform a frame from 'this' local coordinate system to parent frame coordinate system.
    ChFrame<Real> TransformLocalToParent(const ChFrame<Real>& F) const {
        return ChFrame<Real>(TransformPointLocalToParent(F.Csys.pos), Csys.rot * F.Csys.rot);
    }

    /// Transform a frame from the parent coordinate system to 'this' local frame coordinate system.
    ChFrame<Real> TransformParentToLocal(const ChFrame<Real>& F) const {
        return ChFrame<>(TransformPointParentToLocal(F.Csys.pos), Csys.rot.GetConjugate() * F.Csys.rot);
    }

    /// Transform a direction from the parent frame coordinate system to 'this' local coordinate system.
    ChVector3<Real> TransformDirectionLocalToParent(const ChVector3<Real>& d) const { return Rmat * d; }

    /// Transforms a direction from 'this' local coordinate system to parent frame coordinate system.
    ChVector3<Real> TransformDirectionParentToLocal(const ChVector3<Real>& d) const { return Rmat.transpose() * d; }

    // OTHER FUNCTIONS

    /// Returns true if this transform is identical to the other transform.
    bool Equals(const ChFrame<Real>& other) const { return Csys.Equals(other.Csys); }

    /// Returns true if this transform is equal to the other transform, within a tolerance 'tol'.
    bool Equals(const ChFrame<Real>& other, Real tol) const { return Csys.Equals(other.Csys, tol); }

    /// Normalize the rotation, so that quaternion has unit length
    void Normalize() {
        Csys.rot.Normalize();
        Rmat.SetFromQuaternion(Csys.rot);
    }

    /// Sets to no translation and no rotation
    virtual void SetIdentity() {
        Csys.SetIdentity();
        Rmat.setIdentity();
    }

    /// Invert in place.
    /// If w = A * v, after A.Invert() we have v = A * w;
    virtual void Invert() {
        Csys.rot.Conjugate();
        Rmat.transposeInPlace();
        Csys.pos = -(Rmat * Csys.pos);
    }

    /// Return the inverse transform.
    ChFrame<Real> GetInverse() const {
        ChFrame<Real> tmp(*this);
        tmp.Invert();
        return tmp;
    }

    /// Method to allow serialization of transient data to archives.
    virtual void ArchiveOut(ChArchiveOut& archive) {
        // suggested: use versioning
        archive.VersionWrite<ChFrame<double>>();
        // stream out all member data
        archive << CHNVP(Csys);
    }

    /// Method to allow de-serialization of transient data from archives.
    virtual void ArchiveIn(ChArchiveIn& archive) {
        // suggested: use versioning
        /*int version =*/archive.VersionRead<ChFrame<double>>();
        // stream in all member data
        if (archive.in(CHNVP(Csys)))
            Rmat.SetFromQuaternion(Csys.rot);
    }

  protected:
    ChCoordsys<Real> Csys;  ///< Rotation and position, as vector+quaternion
    ChMatrix33<Real> Rmat;  ///< 3x3 orthogonal rotation matrix

  public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

CH_CLASS_VERSION(ChFrame<double>, 0)

// -----------------------------------------------------------------------------

/// Alias for double-precision coordinate frames.
/// <pre>
/// Instead of writing
///    ChFrame<double> F;
/// or
///    ChFrame<> F;
/// you can use:
///    ChFramed F;
/// </pre>
typedef ChFrame<double> ChFramed;

/// Alias for double-precision coordinate frames.
/// <pre>
/// Instead of writing
///    ChFrame<float> F;
/// you can use:
///    ChFramef F;
/// </pre>
typedef ChFrame<float> ChFramef;

// -----------------------------------------------------------------------------
// MIXED ARGUMENT OPERATORS

// Mixing with ChCoordsys

/// The '*' operator that transforms a coordinate system of 'mixed' type:
///       frame_C = frame_A * frame_B;
/// where frame_A is  a ChFrame
///       frame_B is  a ChCoordsys
/// Returns a ChCoordsys.
/// The effect is like applying the transformation frame_A to frame_B and get frame_C.
template <class Real>
ChCoordsys<Real> operator*(const ChFrame<Real>& Fa, const ChCoordsys<Real>& Cb) {
    return Fa.GetCsys().TransformLocalToParent(Cb);
}

/// The '*' operator that transforms a coordinate system of 'mixed' type:
///  frame_C = frame_A * frame_B;
/// where frame_A is  a ChCoordsys
///       frame_B is  a ChFrame
/// Returns a ChFrame.
/// The effect is like applying the transformation frame_A to frame_B and get frame_C.
/// Performance warning: this operator promotes frame_A to a temporary ChFrame.
template <class Real>
ChFrame<Real> operator*(const ChCoordsys<Real>& Ca, const ChFrame<Real>& Fb) {
    ChFrame<Real> Fa(Ca);
    return Fa.TransformLocalToParent(Fb);
}

/// The '>>' operator that transforms a coordinate system of 'mixed' type:
///  frame_C = frame_A >> frame_B;
/// where frame_A is  a ChCoordsys
///       frame_B is  a ChFrame
/// Returns a ChCoordsys.
/// The effect is like applying the transformation frame_B to frame_A and get frame_C.
template <class Real>
ChCoordsys<Real> operator>>(const ChCoordsys<Real>& Ca, const ChFrame<Real>& Fb) {
    return Fb.GetCsys().TransformLocalToParent(Ca);
}

/// The '>>' operator that transforms a coordinate system of 'mixed' type:
///  frame_C = frame_A >> frame_B;
/// where frame_A is  a ChFrame
///       frame_B is  a ChCoordsys
/// Returns a ChFrame.
/// The effect is like applying the transformation frame_B to frame_A and get frame_C.
/// Performance warning: this operator promotes frame_B to a temporary ChFrame.
template <class Real>
ChFrame<Real> operator>>(const ChFrame<Real>& Fa, const ChCoordsys<Real>& Cb) {
    ChFrame<Real> Fb(Cb);
    return Fb.TransformLocalToParent(Fa);
}

// Mixing with ChVector

/// The '*' operator that transforms 'mixed' types:
///  vector_C = frame_A * vector_B;
/// where frame_A   is  a ChFrame
///       vector_B is  a ChVector
/// Returns a ChVector.
/// The effect is like applying the transformation frame_A to vector_B and get vector_C.
template <class Real>
ChVector3<Real> operator*(const ChFrame<Real>& Fa, const ChVector3<Real>& vb) {
    return Fa.TransformPointLocalToParent(vb);
}

/// The '*' operator that transforms 'mixed' types:
///  frame_C = vector_A * frame_B;
/// where vector_A is  a ChVector
///       frame_B  is  a ChFrame
/// Returns a ChFrame.
/// The effect is like applying the translation vector_A to frame_B and get frame_C.
template <class Real>
ChFrame<Real> operator*(const ChVector3<Real>& va, const ChFrame<Real>& Fb) {
    ChFrame<Real> res(Fb);
    res.GetPos() += va;
    return res;
}

/// The '>>' operator that transforms 'mixed' types:
///  vector_C = vector_A >> frame_B;
/// where vector_A is  a ChVector
///       frame_B  is  a ChFrame
/// Returns a ChVector.
/// The effect is like applying the transformation frame_B to vector_A and get vector_C.
/// For a sequence of transformations, i.e. a chain of coordinate
/// systems, you can also write this (like you would do with
/// a sequence of Denavitt-Hartemberg matrix multiplications,
/// but in the opposite order...)
///  new_v = old_v >> frame3to2 >> frame2to1 >> frame1to0;
/// This operation is not commutative.
template <class Real>
ChVector3<Real> operator>>(const ChVector3<Real>& va, const ChFrame<Real>& Fb) {
    return Fb.TransformPointLocalToParent(va);
}

/// The '>>' operator that transforms 'mixed' types:
///  frame_C = frame_A >> vector_B;
/// where frame_A is  a ChFrame
///       frame_B is  a ChVector
/// Returns a ChFrame.
/// The effect is like applying the translation vector_B to frame_A and get frame_C.
template <class Real>
ChFrame<Real> operator>>(const ChFrame<Real>& Fa, const ChVector3<Real>& vb) {
    ChFrame<Real> res(Fa);
    res.GetPos() += vb;
    return res;
}

// Mixing with ChQuaternion

/// The '*' operator that transforms 'mixed' types:
///  quat_C = frame_A * quat_B;
/// where frame_A  is  a ChFrame
///       quat_B   is  a ChQuaternion
/// Returns a ChQuaternion.
/// The effect is like applying the transformation frame_A to quat_B and get quat_C.
template <class Real>
ChQuaternion<Real> operator*(const ChFrame<Real>& Fa, const ChQuaternion<Real>& qb) {
    return Fa.GetRot() * qb;
}

/// The '*' operator that transforms 'mixed' types:
///  frame_C = quat_A * frame_B;
/// where quat_A   is  a ChQuaternion
///       frame_B  is  a ChFrame
/// Returns a ChFrame.
/// The effect is like applying the rotation quat_A to frame_B and get frame_C.
template <class Real>
ChFrame<Real> operator*(const ChQuaternion<Real>& qa, const ChFrame<Real>& Fb) {
    ChFrame<Real> res(qa.Rotate(Fb.GetPos()), qa * Fb.GetRot());
    return res;
}

/// The '>>' operator that transforms 'mixed' types:
///  quat_C = quat_A >> frame_B;
/// where quat_A   is  a ChQuaternion
///       frame_B  is  a ChFrame
/// Returns a ChQuaternion.
/// The effect is like applying the transformation frame_B to quat_A and get quat_C.
template <class Real>
ChQuaternion<Real> operator>>(const ChQuaternion<Real>& qa, const ChFrame<Real>& Fb) {
    return qa >> Fb.GetRot();
}

/// The '>>' operator that transforms 'mixed' types:
///  frame_C = frame_A >> quat_B;
/// where frame_A is  a ChFrame
///       frame_B is  a ChQuaternion
/// Returns a ChFrame.
/// The effect is like applying the rotation quat_B to frame_A and get frame_C.
template <class Real>
ChFrame<Real> operator>>(const ChFrame<Real>& Fa, const ChQuaternion<Real>& qb) {
    ChFrame<Real> res(qb.Rotate(Fa.GetPos()), Fa.GetRot() >> qb);
    return res;
}

// -----------------------------------------------------------------------------

// Insertion to output stream
template <typename Real>
inline std::ostream& operator<<(std::ostream& out, const ChFrame<Real>& F) {
    out << F.GetPos().x() << "  " << F.GetPos().y() << "  " << F.GetPos().z() << "\n";
    out << F.GetRot().e0() << "  " << F.GetRot().e1() << "  " << F.GetRot().e2() << "  " << F.GetRot().e3();
    return out;
}

}  // end namespace chrono

#endif
