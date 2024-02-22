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

#ifndef CHFUNCT_SINE_H
#define CHFUNCT_SINE_H

#include "chrono/motion_functions/ChFunctionBase.h"
#include "chrono/utils/ChConstants.h"


namespace chrono {

/// @addtogroup chrono_functions
/// @{

/// Sine function
/// y = A*sin(2*PI*f + phase)`
class ChApi ChFunctionSine : public ChFunction {
  private:
    double m_ampl;
    double m_phase;
    double m_angular_rate;  ///< internal value, w=2*PI*freq

  public:
    ChFunctionSine() : m_ampl(1.0), m_phase(0.0), m_angular_rate(0.0) {}
    ChFunctionSine(double ampl, double freq, double phase = 0)
        : m_ampl(ampl), m_phase(phase), m_angular_rate(2.0 * CH_C_PI * freq) {}
    ChFunctionSine(const ChFunctionSine& other);
    ~ChFunctionSine() {}

    /// "Virtual" copy constructor (covariant return type).
    virtual ChFunctionSine* Clone() const override { return new ChFunctionSine(*this); }

    virtual Type GetType() const override { return ChFunction::Type::SINE; }

    virtual double GetVal(double x) const override;
    virtual double GetDer(double x) const override;
    virtual double GetDer2(double x) const override;

    void SetPhase(double phase) { m_phase = phase; };

    void SetFrequency(double freq) { m_angular_rate = 2.0 * CH_C_PI * freq; }

    void SetAngularRate(double ang_rate) { m_angular_rate = ang_rate; }

    void SetAmplitude(double ampl) { m_ampl = ampl; }

    double GetPhase() const { return m_phase; }

    double GetFrequency() const { return m_angular_rate / 2.0 / CH_C_PI; }

    double GetAngularRate() const { return m_angular_rate; }

    double GetAmplitude() const { return m_ampl; }

    /// Method to allow serialization of transient data to archives.
    virtual void ArchiveOut(ChArchiveOut& marchive) override;

    /// Method to allow de-serialization of transient data from archives.
    virtual void ArchiveIn(ChArchiveIn& marchive) override;
};

/// @} chrono_functions

}  // end namespace chrono

#endif
