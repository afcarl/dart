/*
 * Copyright (c) 2011-2017, The DART development contributors
 * All rights reserved.
 *
 * The list of contributors can be found at:
 *   https://github.com/dartsim/dart/blob/master/LICENSE
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#include "dart/constraint/SequentialImpulseConstraintSolver.hpp"

#include <cassert>
#ifndef NDEBUG
#include <iomanip>
#include <iostream>
#endif

#include "dart/external/odelcpsolver/lcp.h"

#include "dart/common/Console.hpp"
#include "dart/constraint/ConstrainedGroup.hpp"
#include "dart/constraint/ConstraintBase.hpp"
#include "dart/constraint/DantzigBoxedLcpSolver.hpp"
#include "dart/lcpsolver/Lemke.hpp"

namespace dart {
namespace constraint {

//==============================================================================
SequentialImpulseConstraintSolver::SequentialImpulseConstraintSolver(
    double timeStep, BoxedLcpSolverPtr boxedLcpSolver)
  : ConstraintSolver(timeStep), mBoxedLcpSolver(std::move(boxedLcpSolver))
{
  if (!mBoxedLcpSolver)
    mBoxedLcpSolver = std::make_shared<DantzigBoxedLcpSolver>();
}

//==============================================================================
void SequentialImpulseConstraintSolver::setBoxedLcpSolver(
    BoxedLcpSolverPtr lcpSolver)
{
  if (!lcpSolver)
  {
    dtwarn << "[SequentialImpulseConstraintSolver::setBoxedLcpSolver] "
           << "nullptr for boxed LCP solver is not allowed.";
    return;
  }

  mBoxedLcpSolver = std::move(lcpSolver);
}

//==============================================================================
ConstBoxedLcpSolverPtr SequentialImpulseConstraintSolver::getBoxedLcpSolver()
    const
{
  return mBoxedLcpSolver;
}

//==============================================================================
void SequentialImpulseConstraintSolver::solveConstrainedGroup(
    ConstrainedGroup& group)
{
  // Build LCP terms by aggregating them from constraints
  std::size_t numConstraints = group.getNumConstraints();
  std::size_t n = group.getTotalDimension();

  // If there is no constraint, then just return.
  if (0u == n)
    return;

  int nSkip = dPAD(n);
  double* A = new double[n * nSkip];
  double* x = new double[n];
  double* b = new double[n];
  double* w = new double[n];
  double* lo = new double[n];
  double* hi = new double[n];
  int* findex = new int[n];

// Set w to 0 and findex to -1
#ifndef NDEBUG
  std::memset(A, 0.0, n * nSkip * sizeof(double));
#endif
  std::memset(w, 0.0, n * sizeof(double));
  std::memset(findex, -1, n * sizeof(int));

  // Compute offset indices
  std::size_t* offset = new std::size_t[n];
  offset[0] = 0;
  //  std::cout << "offset[" << 0 << "]: " << offset[0] << std::endl;
  for (std::size_t i = 1; i < numConstraints; ++i)
  {
    const ConstraintBasePtr& constraint = group.getConstraint(i - 1);
    assert(constraint->getDimension() > 0);
    offset[i] = offset[i - 1] + constraint->getDimension();
    //    std::cout << "offset[" << i << "]: " << offset[i] << std::endl;
  }

  // For each constraint
  ConstraintInfo constInfo;
  constInfo.invTimeStep = 1.0 / mTimeStep;
  for (std::size_t i = 0; i < numConstraints; ++i)
  {
    const ConstraintBasePtr& constraint = group.getConstraint(i);

    constInfo.x = x + offset[i];
    constInfo.lo = lo + offset[i];
    constInfo.hi = hi + offset[i];
    constInfo.b = b + offset[i];
    constInfo.findex = findex + offset[i];
    constInfo.w = w + offset[i];

    // Fill vectors: lo, hi, b, w
    constraint->getInformation(&constInfo);

    // Fill a matrix by impulse tests: A
    constraint->excite();
    for (std::size_t j = 0; j < constraint->getDimension(); ++j)
    {
      // Adjust findex for global index
      if (findex[offset[i] + j] >= 0)
        findex[offset[i] + j] += offset[i];

      // Apply impulse for mipulse test
      constraint->applyUnitImpulse(j);

      // Fill upper triangle blocks of A matrix
      int index = nSkip * (offset[i] + j) + offset[i];
      constraint->getVelocityChange(A + index, true);
      for (std::size_t k = i + 1; k < numConstraints; ++k)
      {
        index = nSkip * (offset[i] + j) + offset[k];
        group.getConstraint(k)->getVelocityChange(A + index, false);
      }

      // Filling symmetric part of A matrix
      for (std::size_t k = 0; k < i; ++k)
      {
        for (std::size_t l = 0; l < group.getConstraint(k)->getDimension(); ++l)
        {
          int index1 = nSkip * (offset[i] + j) + offset[k] + l;
          int index2 = nSkip * (offset[k] + l) + offset[i] + j;

          A[index1] = A[index2];
        }
      }
    }

    assert(
        isSymmetric(
            n, A, offset[i], offset[i] + constraint->getDimension() - 1));

    constraint->unexcite();
  }

  assert(isSymmetric(n, A));

  // Print LCP formulation
  //  dtdbg << "Before solve:" << std::endl;
  //  print(n, A, x, lo, hi, b, w, findex);
  //  std::cout << std::endl;

  // Solve LCP using ODE's Dantzig algorithm
  assert(mBoxedLcpSolver);
  mBoxedLcpSolver->solve(n, A, x, b, 0, lo, hi, findex);

  // Print LCP formulation
  //  dtdbg << "After solve:" << std::endl;
  //  print(n, A, x, lo, hi, b, w, findex);
  //  std::cout << std::endl;

  // Apply constraint impulses
  for (std::size_t i = 0; i < numConstraints; ++i)
  {
    const ConstraintBasePtr& constraint = group.getConstraint(i);
    constraint->applyImpulse(x + offset[i]);
    constraint->excite();
  }

  delete[] offset;

  delete[] A;
  delete[] x;
  delete[] b;
  delete[] w;
  delete[] lo;
  delete[] hi;
  delete[] findex;
}

//==============================================================================
#ifndef NDEBUG
bool SequentialImpulseConstraintSolver::isSymmetric(std::size_t n, double* A)
{
  std::size_t nSkip = dPAD(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    for (std::size_t j = 0; j < n; ++j)
    {
      if (std::abs(A[nSkip * i + j] - A[nSkip * j + i]) > 1e-6)
      {
        std::cout << "A: " << std::endl;
        for (std::size_t k = 0; k < n; ++k)
        {
          for (std::size_t l = 0; l < nSkip; ++l)
          {
            std::cout << std::setprecision(4) << A[k * nSkip + l] << " ";
          }
          std::cout << std::endl;
        }

        std::cout << "A(" << i << ", " << j << "): " << A[nSkip * i + j]
                  << std::endl;
        std::cout << "A(" << j << ", " << i << "): " << A[nSkip * j + i]
                  << std::endl;
        return false;
      }
    }
  }

  return true;
}

//==============================================================================
bool SequentialImpulseConstraintSolver::isSymmetric(
    std::size_t n, double* A, std::size_t begin, std::size_t end)
{
  std::size_t nSkip = dPAD(n);
  for (std::size_t i = begin; i <= end; ++i)
  {
    for (std::size_t j = begin; j <= end; ++j)
    {
      if (std::abs(A[nSkip * i + j] - A[nSkip * j + i]) > 1e-6)
      {
        std::cout << "A: " << std::endl;
        for (std::size_t k = 0; k < n; ++k)
        {
          for (std::size_t l = 0; l < nSkip; ++l)
          {
            std::cout << std::setprecision(4) << A[k * nSkip + l] << " ";
          }
          std::cout << std::endl;
        }

        std::cout << "A(" << i << ", " << j << "): " << A[nSkip * i + j]
                  << std::endl;
        std::cout << "A(" << j << ", " << i << "): " << A[nSkip * j + i]
                  << std::endl;
        return false;
      }
    }
  }

  return true;
}

//==============================================================================
void SequentialImpulseConstraintSolver::print(
    std::size_t n,
    double* A,
    double* x,
    double* /*lo*/,
    double* /*hi*/,
    double* b,
    double* w,
    int* findex)
{
  std::size_t nSkip = dPAD(n);
  std::cout << "A: " << std::endl;
  for (std::size_t i = 0; i < n; ++i)
  {
    for (std::size_t j = 0; j < nSkip; ++j)
    {
      std::cout << std::setprecision(4) << A[i * nSkip + j] << " ";
    }
    std::cout << std::endl;
  }

  std::cout << "b: ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << std::setprecision(4) << b[i] << " ";
  }
  std::cout << std::endl;

  std::cout << "w: ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << w[i] << " ";
  }
  std::cout << std::endl;

  std::cout << "x: ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << x[i] << " ";
  }
  std::cout << std::endl;

  //  std::cout << "lb: ";
  //  for (int i = 0; i < dim; ++i)
  //  {
  //    std::cout << lb[i] << " ";
  //  }
  //  std::cout << std::endl;

  //  std::cout << "ub: ";
  //  for (int i = 0; i < dim; ++i)
  //  {
  //    std::cout << ub[i] << " ";
  //  }
  //  std::cout << std::endl;

  std::cout << "frictionIndex: ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << findex[i] << " ";
  }
  std::cout << std::endl;

  double* Ax = new double[n];

  for (std::size_t i = 0; i < n; ++i)
  {
    Ax[i] = 0.0;
  }

  for (std::size_t i = 0; i < n; ++i)
  {
    for (std::size_t j = 0; j < n; ++j)
    {
      Ax[i] += A[i * nSkip + j] * x[j];
    }
  }

  std::cout << "Ax   : ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << Ax[i] << " ";
  }
  std::cout << std::endl;

  std::cout << "b + w: ";
  for (std::size_t i = 0; i < n; ++i)
  {
    std::cout << b[i] + w[i] << " ";
  }
  std::cout << std::endl;

  delete[] Ax;
}
#endif

} // namespace constraint
} // namespace dart
