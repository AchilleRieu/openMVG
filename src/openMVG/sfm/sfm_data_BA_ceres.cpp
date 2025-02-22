// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.

// Copyright (c) 2015 Pierre Moulon.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/sfm/sfm_data_BA_ceres.hpp"

#ifdef OPENMVG_USE_OPENMP
#include <omp.h>
#endif

#include "ceres/problem.h"
#include "ceres/solver.h"
#include "openMVG/cameras/Camera_Common.hpp"
#include "openMVG/cameras/Camera_Intrinsics.hpp"
#include "openMVG/geometry/Similarity3.hpp"
#include "openMVG/geometry/Similarity3_Kernel.hpp"
//- Robust estimation - LMeds (since no threshold can be defined)
#include "openMVG/robust_estimation/robust_estimator_LMeds.hpp"
#include "openMVG/sfm/sfm_data_BA_ceres_camera_functor.hpp"
#include "openMVG/sfm/sfm_data_transform.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/system/logger.hpp"
#include "openMVG/types.hpp"

#include <ceres/rotation.h>
#include <ceres/types.h>

#include <iostream>
#include <limits>

namespace openMVG {
namespace sfm {

#define OPENMVG_CERES_HAS_MANIFOLD ((CERES_VERSION_MAJOR * 100 + CERES_VERSION_MINOR) >= 201)

using namespace openMVG::cameras;
using namespace openMVG::geometry;

const double PI = 4.0 * atan( 1.0 ), RADTODEG = 180.0 / PI;

template <typename T>
void getAngles(const T* R, T* euler) {
  getAngles(ceres::ColumnMajorAdapter3x3(R), euler);
}

template <typename T, int row_stride, int col_stride>
void getAngles(const ceres::MatrixAdapter<const T, row_stride, col_stride>& R, T* euler)
{
   const double EPS = 1.0e-6;
   T X{}, Y{}, Z{};
                                           //    Matrix would be Rx.Ry.Rz
    Y = ceres::asin( R(0,2) );                       // Unique angle in [-pi/2,pi/2]

    if ( abs( abs( R(0,2) ) - 1.0 ) < EPS )   // Yuk! Gimbal lock. Infinite choice of X and Z
    {
        X = ceres::atan2( R(2,1), R(1,1) );          // One choice amongst many
        Z = T(0.0);
    }
    else                                       // Unique solutions in (-pi,pi]
    {
        X = ceres::atan2( -R(1,2), R(2,2) );         // atan2 gives correct quadrant and unique solutions
        Z = ceres::atan2( -R(0,1), R(0,0) );
    }
   
   //X *= RADTODEG;   Y *= RADTODEG;   Z *= RADTODEG;
   
   euler[0] = X;
   euler[1] = Y;
   euler[2] = Z;

}

// Ceres CostFunctor used for SfM pose center to GPS pose center minimization
struct PoseCenterConstraintCostFunction
{
  Vec3 weight_;
  Vec3 pose_center_constraint_;

  PoseCenterConstraintCostFunction
  (
    const Vec3 & center,
    const Vec3 & weight
  ): weight_(weight), pose_center_constraint_(center)
  {
  }

  template <typename T> bool
  operator()
  (
    const T* const cam_extrinsics, // R_t
    T* residuals
  )
  const
  {
    using Vec3T = Eigen::Matrix<T,3,1>;
    Eigen::Map<const Vec3T> cam_R(&cam_extrinsics[0]);
    Eigen::Map<const Vec3T> cam_t(&cam_extrinsics[3]);
    const Vec3T cam_R_transpose(-cam_R);

    Vec3T pose_center;
    // Rotate the point according the camera rotation
    ceres::AngleAxisRotatePoint(cam_R_transpose.data(), cam_t.data(), pose_center.data());
    pose_center = pose_center * T(-1);

    Eigen::Map<Vec3T> residuals_eigen(residuals);
    residuals_eigen = weight_.cast<T>().cwiseProduct(pose_center - pose_center_constraint_.cast<T>());

    // OPENMVG_LOG_INFO << 
    // "\nPosition value : x = " << pose_center.data()[0] << ", y = "<< pose_center.data()[1]<< ", z = "<< pose_center.data()[2]<< "\n" <<
    // "Position prior : x = " << pose_center_constraint_(0) << ", y = "<< pose_center_constraint_(1)<< ", z = "<< pose_center_constraint_(2) << "\n" <<
    // "Position error : " << residuals_eigen.data()[0] << ", y = "<< residuals_eigen.data()[1]<< ", z = "<< residuals_eigen.data()[2] << "\n";

    return true;
  }
};

struct PoseRotationConstraintCostFunction
{
  double weight_;
  Mat3 pose_rotation_constraint_; //voir type

  PoseRotationConstraintCostFunction
  (
    const Mat3 & rotation,
    const double & weight
  ): weight_(weight), pose_rotation_constraint_(rotation)
  {
  }

  template <typename T> bool
  operator()
  (
    const T* const cam_extrinsics, // R_t
    T* residuals
  )
  const
  {
    using Vec3T = Eigen::Matrix<T,3,1>;
    using Mat3T = Eigen::Matrix<T,3,3>;
    Eigen::Map<const Vec3T> cam_R(&cam_extrinsics[0]);

    Mat3T R_mat;
    Vec3T R_euler;
    ceres::AngleAxisToRotationMatrix(cam_R.data(), R_mat.data());
    getAngles(R_mat.data(), R_euler.data());
 
    // if(R_euler(2) - pose_rotation_constraint_(0,0) > 180.0){
    //   residuals[0] = T(weight_)*T(R_euler(2) - pose_rotation_constraint_(0,0)-static_cast<T>(360));
    // }
    // else if (R_euler(2) - pose_rotation_constraint_(0,0) <-180.0){
    //   residuals[0] = T(weight_)*T(R_euler(2) - pose_rotation_constraint_(0,0)+static_cast<T>(360));
    // }
    // else{
    //   residuals[0] = T(weight_)*T(R_euler(2) - pose_rotation_constraint_(0,0));
    // }

    residuals[0] = T(weight_)*T(pow(cos(R_euler(2)) - cos(pose_rotation_constraint_(0,0)), 2) + pow(sin(R_euler(2)) - sin(pose_rotation_constraint_(0,0)),2));
    // OPENMVG_LOG_INFO << 
    // "\nRotation values : Yaw = " << R_euler(2) << ", Pitch =" << R_euler(0) << ",  Roll =" << R_euler(1) << "\n" <<
    // "Rotation prior : Yaw = " << pose_rotation_constraint_(0,0) << ", Pitch = " << pose_rotation_constraint_(0,1) << ", Roll = " << pose_rotation_constraint_(0,2) << "\n" <<
    // "Rotation error : " << residuals[0] << "\n";

    return true;
  }
};

/// Create the appropriate cost functor according the provided input camera intrinsic model.
/// The residual can be weighetd if desired (default 0.0 means no weight).
ceres::CostFunction * IntrinsicsToCostFunction
(
  IntrinsicBase * intrinsic,
  const Vec2 & observation,
  const double weight
)
{
  switch (intrinsic->getType())
  {
    case PINHOLE_CAMERA:
      return ResidualErrorFunctor_Pinhole_Intrinsic::Create(observation, weight);
    case PINHOLE_CAMERA_RADIAL1:
      return ResidualErrorFunctor_Pinhole_Intrinsic_Radial_K1::Create(observation, weight);
    case PINHOLE_CAMERA_RADIAL3:
      return ResidualErrorFunctor_Pinhole_Intrinsic_Radial_K3::Create(observation, weight);
    case PINHOLE_CAMERA_BROWN:
      return ResidualErrorFunctor_Pinhole_Intrinsic_Brown_T2::Create(observation, weight);
    case PINHOLE_CAMERA_FISHEYE:
      return ResidualErrorFunctor_Pinhole_Intrinsic_Fisheye::Create(observation, weight);
    case CAMERA_SPHERICAL:
      return ResidualErrorFunctor_Intrinsic_Spherical::Create(intrinsic, observation, weight);
    default:
      return {};
  }
}

Bundle_Adjustment_Ceres::BA_Ceres_options::BA_Ceres_options
(
  const bool bVerbose,
  bool bmultithreaded
)
: bVerbose_(bVerbose),
  nb_threads_(1),
  parameter_tolerance_(1e-8),
  gradient_tolerance_(1e-10),
  bUse_loss_function_(true),
  max_num_iterations_(50),
  max_linear_solver_iterations_(500)
{
  #ifdef OPENMVG_USE_OPENMP
    nb_threads_ = omp_get_max_threads();
  #endif // OPENMVG_USE_OPENMP
  if (!bmultithreaded)
    nb_threads_ = 1;

  bCeres_summary_ = false;

  // Default configuration use a DENSE representation
  linear_solver_type_ = ceres::DENSE_SCHUR;
  preconditioner_type_ = ceres::JACOBI;
  // If Sparse linear solver are available
  // Descending priority order by efficiency (SUITE_SPARSE > CX_SPARSE > EIGEN_SPARSE)
  if (ceres::IsSparseLinearAlgebraLibraryTypeAvailable(ceres::SUITE_SPARSE))
  {
    sparse_linear_algebra_library_type_ = ceres::SUITE_SPARSE;
    linear_solver_type_ = ceres::SPARSE_SCHUR;
  }
  else
  {
    if (ceres::IsSparseLinearAlgebraLibraryTypeAvailable(ceres::EIGEN_SPARSE))
    {
      sparse_linear_algebra_library_type_ = ceres::EIGEN_SPARSE;
      linear_solver_type_ = ceres::SPARSE_SCHUR;
    }
  }
}


Bundle_Adjustment_Ceres::Bundle_Adjustment_Ceres
(
  const Bundle_Adjustment_Ceres::BA_Ceres_options & options
)
: ceres_options_(options)
{}

Bundle_Adjustment_Ceres::BA_Ceres_options &
Bundle_Adjustment_Ceres::ceres_options()
{
  return ceres_options_;
}

bool Bundle_Adjustment_Ceres::Adjust
(
  SfM_Data & sfm_data,     // the SfM scene to refine
  const Optimize_Options & options
)
{
  //----------
  // Add camera parameters
  // - intrinsics
  // - poses [R|t]

  // Create residuals for each observation in the bundle adjustment problem. The
  // parameters for cameras and points are added automatically.
  //----------


  double pose_center_robust_fitting_error = 0.0;
  double pose_rotation_robust_fitting_error = 0.0;
  openMVG::geometry::Similarity3 sim_to_center;
  bool b_usable_prior = false;
  if (options.use_motion_priors_opt && sfm_data.GetViews().size() > 3)
  {
    // - Compute a robust X-Y affine transformation & apply it
    // - This early transformation enhance the conditionning (solution closer to the Prior coordinate system)
    {
      // Collect corresponding camera centers
      std::vector<Vec3> X_SfM, X_GPS;
      std::vector<Vec2> R_SfM, R_GPS;
      for (const auto & view_it : sfm_data.GetViews())
      {
        const sfm::ViewPriors * prior = dynamic_cast<sfm::ViewPriors*>(view_it.second.get());
        if (prior != nullptr && sfm_data.IsPoseAndIntrinsicDefined(prior))
        {
          if(prior->b_use_pose_center_)
          {
            X_SfM.push_back( sfm_data.GetPoses().at(prior->id_pose).center() );
            X_GPS.push_back( prior->pose_center_ );
          }
          if(prior->b_use_pose_rotation_)
          {
            double R_SfM_euler[3];
            double R_GPS_euler[3];
            getAngles((const double*)sfm_data.GetPoses().at(prior->id_pose).rotation().data(), R_SfM_euler);
            getAngles((const double*)prior->pose_rotation_.data(), R_GPS_euler);
            R_SfM.push_back(Vec2(cos(R_SfM_euler[2]), sin(R_SfM_euler[2])));
            R_GPS.push_back(Vec2(cos(R_GPS_euler[2]), sin(R_GPS_euler[2])));
          }
        }
      }
      openMVG::geometry::Similarity3 sim;

      // Compute the registration:
      if (X_GPS.size() > 3)
      {
        const Mat X_SfM_Mat = Eigen::Map<Mat>(X_SfM[0].data(),3, X_SfM.size());
        const Mat X_GPS_Mat = Eigen::Map<Mat>(X_GPS[0].data(),3, X_GPS.size());
        geometry::kernel::Similarity3_Kernel kernel(X_SfM_Mat, X_GPS_Mat);
        const double lmeds_median = openMVG::robust::LeastMedianOfSquares(kernel, &sim);
        if (lmeds_median != std::numeric_limits<double>::max())
        {
          b_usable_prior = true; // PRIOR can be used safely

          // Compute the median residual error once the registration is applied
          for (Vec3 & pos : X_SfM) // Transform SfM poses for residual computation
          {
            pos = sim(pos);
          }
          Vec residual = (Eigen::Map<Mat3X>(X_SfM[0].data(), 3, X_SfM.size()) - Eigen::Map<Mat3X>(X_GPS[0].data(), 3, X_GPS.size())).colwise().norm();
          std::sort(residual.data(), residual.data() + residual.size());
          pose_center_robust_fitting_error = residual(residual.size()/2);

          Vec residual_R = (Eigen::Map<Mat2X>(R_SfM[0].data(), 2, R_SfM.size())- Eigen::Map<Mat2X>(R_GPS[0].data(), 2, R_GPS.size())).colwise().squaredNorm();  
          std::sort(residual_R.data(), residual_R.data() + residual_R.size());
          pose_rotation_robust_fitting_error = residual_R(residual_R.size()/2);

          // Apply the found transformation to the SfM Data Scene
          openMVG::sfm::ApplySimilarity(sim, sfm_data);
          
          // Move entire scene to center for better numerical stability
          Vec3 pose_centroid = Vec3::Zero();
          for (const auto & pose_it : sfm_data.poses)
          {
            pose_centroid += (pose_it.second.center() / (double)sfm_data.poses.size());
          }
          sim_to_center = openMVG::geometry::Similarity3(openMVG::sfm::Pose3(Mat3::Identity(), pose_centroid), 1.0);
          openMVG::sfm::ApplySimilarity(sim_to_center, sfm_data, true);
        }
      }
      else
      {
        OPENMVG_LOG_WARNING << "Cannot used the motion prior, insufficient number of motion priors/poses";
      }
    }
  }

  ceres::Problem::Options problem_options;

  // Set a LossFunction to be less penalized by false measurements
  //  - set it to nullptr if you don't want use a lossFunction.
  std::unique_ptr<ceres::LossFunction> p_LossFunction;
  if (ceres_options_.bUse_loss_function_)
  {
    p_LossFunction.reset(new ceres::HuberLoss(Square(4.0)));
    problem_options.loss_function_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
  }

  ceres::Problem problem(problem_options);

  // Data wrapper for refinement:
  Hash_Map<IndexT, std::vector<double>> map_intrinsics;
  Hash_Map<IndexT, std::vector<double>> map_poses;

  // Setup Poses data & subparametrization
  for (const auto & pose_it : sfm_data.poses)
  {
    const IndexT indexPose = pose_it.first;

    const Pose3 & pose = pose_it.second;
    const Mat3 R = pose.rotation();
    const Vec3 t = pose.translation();

    double angleAxis[3];
    ceres::RotationMatrixToAngleAxis((const double*)R.data(), angleAxis);
    // angleAxis + translation
    map_poses[indexPose] = {angleAxis[0], angleAxis[1], angleAxis[2], t(0), t(1), t(2)};

    double * parameter_block = &map_poses.at(indexPose)[0];
    problem.AddParameterBlock(parameter_block, 6);
    if (options.extrinsics_opt == Extrinsic_Parameter_Type::NONE)
    {
      // set the whole parameter block as constant for best performance
      problem.SetParameterBlockConstant(parameter_block);
    }
    else  // Subset parametrization
    {
      std::vector<int> vec_constant_extrinsic;
      // If we adjust only the translation, we must set ROTATION as constant
      if (options.extrinsics_opt == Extrinsic_Parameter_Type::ADJUST_TRANSLATION)
      {
        // Subset rotation parametrization
        vec_constant_extrinsic.insert(vec_constant_extrinsic.end(), {0,1,2});
      }
      // If we adjust only the rotation, we must set TRANSLATION as constant
      if (options.extrinsics_opt == Extrinsic_Parameter_Type::ADJUST_ROTATION)
      {
        // Subset translation parametrization
        vec_constant_extrinsic.insert(vec_constant_extrinsic.end(), {3,4,5});
      }
      if (!vec_constant_extrinsic.empty())
      {
#if OPENMVG_CERES_HAS_MANIFOLD
        auto* subset_manifold = new ceres::SubsetManifold(6, vec_constant_extrinsic);
        problem.SetManifold(parameter_block, subset_manifold);
#else
        auto *subset_parameterization =
          new ceres::SubsetParameterization(6, vec_constant_extrinsic);
        problem.SetParameterization(parameter_block, subset_parameterization);
#endif
      }
    }
  }

  // Setup Intrinsics data & subparametrization
  for (const auto & intrinsic_it : sfm_data.intrinsics)
  {
    const IndexT indexCam = intrinsic_it.first;

    if (isValid(intrinsic_it.second->getType()))
    {
      map_intrinsics[indexCam] = intrinsic_it.second->getParams();
      if (!map_intrinsics.at(indexCam).empty())
      {
        double * parameter_block = &map_intrinsics.at(indexCam)[0];
        problem.AddParameterBlock(parameter_block, map_intrinsics.at(indexCam).size());
        if (options.intrinsics_opt == Intrinsic_Parameter_Type::NONE)
        {
          // set the whole parameter block as constant for best performance
          problem.SetParameterBlockConstant(parameter_block);
        }
        else
        {
          const std::vector<int> vec_constant_intrinsic =
            intrinsic_it.second->subsetParameterization(options.intrinsics_opt);
          if (!vec_constant_intrinsic.empty())
          {
#if OPENMVG_CERES_HAS_MANIFOLD
            auto* subset_manifold =
              new ceres::SubsetManifold(
                map_intrinsics.at(indexCam).size(), vec_constant_intrinsic);
            problem.SetManifold(parameter_block, subset_manifold);
#else
            auto *subset_parameterization =
              new ceres::SubsetParameterization(
                map_intrinsics.at(indexCam).size(), vec_constant_intrinsic);
            problem.SetParameterization(parameter_block, subset_parameterization);
#endif
          }
        }
      }
    }
    else
    {
      OPENMVG_LOG_ERROR << "Unsupported camera type.";
    }
  }

  // For all visibility add reprojections errors:
  for (auto & structure_landmark_it : sfm_data.structure)
  {
    const Observations & obs = structure_landmark_it.second.obs;

    for (const auto & obs_it : obs)
    {
      // Build the residual block corresponding to the track observation:
      const View * view = sfm_data.views.at(obs_it.first).get();

      // Each Residual block takes a point and a camera as input and outputs a 2
      // dimensional residual. Internally, the cost function stores the observed
      // image location and compares the reprojection against the observation.
      ceres::CostFunction* cost_function =
        IntrinsicsToCostFunction(sfm_data.intrinsics.at(view->id_intrinsic).get(),
                                 obs_it.second.x);

      if (cost_function)
      {
        if (!map_intrinsics.at(view->id_intrinsic).empty())
        {
          problem.AddResidualBlock(cost_function,
            p_LossFunction.get(),
            &map_intrinsics.at(view->id_intrinsic)[0],
            &map_poses.at(view->id_pose)[0],
            structure_landmark_it.second.X.data());
        }
        else
        {
          problem.AddResidualBlock(cost_function,
            p_LossFunction.get(),
            &map_poses.at(view->id_pose)[0],
            structure_landmark_it.second.X.data());
        }
      }
      else
      {
        OPENMVG_LOG_ERROR << "Cannot create a CostFunction for this camera model.";
        return false;
      }
    }
    if (options.structure_opt == Structure_Parameter_Type::NONE)
      problem.SetParameterBlockConstant(structure_landmark_it.second.X.data());
  }

  if (options.control_point_opt.bUse_control_points)
  {
    // Use Ground Control Point:
    // - fixed 3D points with weighted observations
    for (auto & gcp_landmark_it : sfm_data.control_points)
    {
      const Observations & obs = gcp_landmark_it.second.obs;

      for (const auto & obs_it : obs)
      {
        // Build the residual block corresponding to the track observation:
        const View * view = sfm_data.views.at(obs_it.first).get();

        // Each Residual block takes a point and a camera as input and outputs a 2
        // dimensional residual. Internally, the cost function stores the observed
        // image location and compares the reprojection against the observation.
        ceres::CostFunction* cost_function =
          IntrinsicsToCostFunction(
            sfm_data.intrinsics.at(view->id_intrinsic).get(),
            obs_it.second.x,
            options.control_point_opt.weight);

        if (cost_function)
        {
          if (!map_intrinsics.at(view->id_intrinsic).empty())
          {
            problem.AddResidualBlock(cost_function,
                                     nullptr,
                                     &map_intrinsics.at(view->id_intrinsic)[0],
                                     &map_poses.at(view->id_pose)[0],
                                     gcp_landmark_it.second.X.data());
          }
          else
          {
            problem.AddResidualBlock(cost_function,
                                     nullptr,
                                     &map_poses.at(view->id_pose)[0],
                                     gcp_landmark_it.second.X.data());
          }
        }
      }
      if (obs.empty())
      {
        OPENMVG_LOG_ERROR
          << "Cannot use this GCP id: " << gcp_landmark_it.first
          << ". There is not linked image observation.";
      }
      else
      {
        // Set the 3D point as FIXED (it's a valid GCP)
        problem.SetParameterBlockConstant(gcp_landmark_it.second.X.data());
      }
    }
  }

  // Add Pose prior constraints if any
  if (b_usable_prior)
  {
    for (const auto & view_it : sfm_data.GetViews())
    {
      const sfm::ViewPriors * prior = dynamic_cast<sfm::ViewPriors*>(view_it.second.get());
      if (prior != nullptr && sfm_data.IsPoseAndIntrinsicDefined(prior))
      {
        if(prior->b_use_pose_center_)
        {
          // Add the cost functor (distance from Pose prior to the SfM_Data Pose center)
          ceres::CostFunction * cost_function =
            new ceres::AutoDiffCostFunction<PoseCenterConstraintCostFunction, 3, 6>(
              new PoseCenterConstraintCostFunction(prior->pose_center_, prior->center_weight_));

          problem.AddResidualBlock(
            cost_function,
            new ceres::HuberLoss(Square(pose_center_robust_fitting_error)),
            &map_poses.at(prior->id_view)[0]);
        }
        if(prior->b_use_pose_rotation_)
        {
          // Add the cost functor (distance from rotation prior to the SfM_Data rotation)
          ceres::CostFunction * cost_function =
            new ceres::AutoDiffCostFunction<PoseRotationConstraintCostFunction, 1, 6>(
              new PoseRotationConstraintCostFunction(prior->pose_rotation_, prior->rotation_weight_));

            problem.AddResidualBlock(
              cost_function,
              new ceres::HuberLoss(
              Square(pose_rotation_robust_fitting_error)),
              &map_poses.at(prior->id_view)[0]); 
        }
      }
    }
  }

  // Configure a BA engine and run it
  //  Make Ceres automatically detect the bundle structure.
  ceres::Solver::Options ceres_config_options;
  ceres_config_options.max_num_iterations = ceres_options_.max_num_iterations_;
  ceres_config_options.max_linear_solver_iterations = ceres_options_.max_linear_solver_iterations_;
  ceres_config_options.preconditioner_type =
    static_cast<ceres::PreconditionerType>(ceres_options_.preconditioner_type_);
  ceres_config_options.linear_solver_type =
    static_cast<ceres::LinearSolverType>(ceres_options_.linear_solver_type_);
  ceres_config_options.sparse_linear_algebra_library_type =
    static_cast<ceres::SparseLinearAlgebraLibraryType>(ceres_options_.sparse_linear_algebra_library_type_);
  ceres_config_options.minimizer_progress_to_stdout = ceres_options_.bVerbose_;
  ceres_config_options.logging_type = ceres::SILENT;
  ceres_config_options.num_threads = ceres_options_.nb_threads_;
#if CERES_VERSION_MAJOR < 2
  ceres_config_options.num_linear_solver_threads = ceres_options_.nb_threads_;
#endif
  ceres_config_options.parameter_tolerance = ceres_options_.parameter_tolerance_;
  ceres_config_options.gradient_tolerance = ceres_options_.gradient_tolerance_;


  // Solve BA
  ceres::Solver::Summary summary;
  ceres::Solve(ceres_config_options, &problem, &summary);
  if (ceres_options_.bCeres_summary_)
    OPENMVG_LOG_INFO << summary.FullReport();

  // If no error, get back refined parameters
  if (!summary.IsSolutionUsable())
  {
    OPENMVG_LOG_ERROR << "IsSolutionUsable is false. Bundle Adjustment failed.";
    return false;
  }
  else // Solution is usable
  {
    if (ceres_options_.bVerbose_)
    {
      // Display statistics about the minimization
      OPENMVG_LOG_INFO
        << "\nBundle Adjustment statistics (approximated RMSE):\n"
        << " #views: " << sfm_data.views.size() << "\n"
        << " #poses: " << sfm_data.poses.size() << "\n"
        << " #intrinsics: " << sfm_data.intrinsics.size() << "\n"
        << " #tracks: " << sfm_data.structure.size() << "\n"
        << " #residuals: " << summary.num_residuals << "\n"
        << " Initial RMSE: " << std::sqrt( summary.initial_cost / summary.num_residuals) << "\n"
        << " Final RMSE: " << std::sqrt( summary.final_cost / summary.num_residuals) << "\n"
        << " Time (s): " << summary.total_time_in_seconds
        << " \n--\n"
        << " Used motion prior: " << static_cast<int>(b_usable_prior);
    }

    // Update camera poses with refined data
    if (options.extrinsics_opt != Extrinsic_Parameter_Type::NONE)
    {
      for (auto & pose_it : sfm_data.poses)
      {
        const IndexT indexPose = pose_it.first;

        Mat3 R_refined;
        ceres::AngleAxisToRotationMatrix(&map_poses.at(indexPose)[0], R_refined.data());
        Vec3 t_refined(map_poses.at(indexPose)[3], map_poses.at(indexPose)[4], map_poses.at(indexPose)[5]);
        // Update the pose
        Pose3 & pose = pose_it.second;
        if (options.extrinsics_opt == Extrinsic_Parameter_Type::ADJUST_ROTATION)
        {
            // Update only rotation
            pose.rotation() = R_refined;
        }
        else if (options.extrinsics_opt == Extrinsic_Parameter_Type::ADJUST_TRANSLATION)
        {
            // Update only translation
            Vec3 C_refined = -R_refined.transpose() * t_refined;
            pose.center() = C_refined;
        }
        else
        {
            // Update rotation + translation
            pose = Pose3(R_refined, -R_refined.transpose() * t_refined);
        }
      }
    }

    // Update camera intrinsics with refined data
    if (options.intrinsics_opt != Intrinsic_Parameter_Type::NONE)
    {
      for (auto & intrinsic_it : sfm_data.intrinsics)
      {
        const IndexT indexCam = intrinsic_it.first;

        const std::vector<double> & vec_params = map_intrinsics.at(indexCam);
        intrinsic_it.second->updateFromParams(vec_params);
      }
    }

    // Structure is already updated directly if needed (no data wrapping)

    if (b_usable_prior)
    {
      // set back to the original scene centroid
      openMVG::sfm::ApplySimilarity(sim_to_center.inverse(), sfm_data, true);

      //--
      // - Compute some fitting statistics
      //--

      // Collect corresponding camera centers
      std::vector<Vec3> X_SfM, X_GPS;
      std::vector<Vec2> R_SfM, R_GPS;
      for (const auto & view_it : sfm_data.GetViews())
      {
        const sfm::ViewPriors * prior = dynamic_cast<sfm::ViewPriors*>(view_it.second.get());
        if (prior != nullptr && sfm_data.IsPoseAndIntrinsicDefined(prior))
        {
          if(prior->b_use_pose_center_)
          {
            X_SfM.push_back( sfm_data.GetPoses().at(prior->id_pose).center() );
            X_GPS.push_back( prior->pose_center_ );
          }
          if(prior->b_use_pose_rotation_)
          {
            double R_SfM_euler[3];
            double R_GPS_euler[3];
            getAngles((const double*)sfm_data.GetPoses().at(prior->id_pose).rotation().data(), R_SfM_euler);
            getAngles((const double*)prior->pose_rotation_.data(), R_GPS_euler);
            R_SfM.push_back(Vec2(cos(R_SfM_euler[2]), sin(R_SfM_euler[2])));
            R_GPS.push_back(Vec2(cos(R_GPS_euler[2]), sin(R_GPS_euler[2])));
          }
        }
      }

      // Compute the registration fitting error (once BA with Prior have been used):
      if (X_GPS.size() > 3)
      {
        // Compute the median residual error
        const Vec residual = (Eigen::Map<Mat3X>(X_SfM[0].data(), 3, X_SfM.size()) - Eigen::Map<Mat3X>(X_GPS[0].data(), 3, X_SfM.size())).colwise().norm();  
        std::ostringstream os;
        os
          << "Pose prior statistics (user units):\n"
          << " - Starting median fitting error: " << pose_center_robust_fitting_error << "\n"
          << " - Final fitting error:\n";
        minMaxMeanMedian<Vec::Scalar>(residual.data(), residual.data() + residual.size(), os);
        OPENMVG_LOG_INFO << os.str();
      }

      // Compute the registration fitting error (once BA with Prior have been used):
      if (R_GPS.size() > 3){
        std::ostringstream os;
        const Vec residual_R = (Eigen::Map<Mat2X>(R_SfM[0].data(), 2, R_SfM.size())- Eigen::Map<Mat2X>(R_GPS[0].data(), 2, R_GPS.size())).colwise().squaredNorm();
        os
          << "Rotation prior statistics (user units):\n"
          << " - Starting median fitting error: " << pose_rotation_robust_fitting_error << "\n"
          << " - Final fitting error:\n";
        minMaxMeanMedian<Vec::Scalar>(residual_R.data(), residual_R.data() + residual_R.size(), os);
        OPENMVG_LOG_INFO << os.str();
      }

    }
    return true;
  }
}

} // namespace sfm pose_rotation_robust_fitting_error
} // namespace openMVG
