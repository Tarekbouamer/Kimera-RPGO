/*
Robust solver class
author: Yun Chang, Luca Carlone
*/

#include "kimera_rpgo/RobustSolver.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtsam/nonlinear/DoglegOptimizer.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/slam/dataset.h>

#include "kimera_rpgo/logger.h"
#include "kimera_rpgo/outlier/pcm.h"
#include "kimera_rpgo/utils/type_utils.h"

namespace kimera_rpgo {

typedef std::pair<gtsam::NonlinearFactorGraph, gtsam::Values> GraphAndValues;

RobustSolver::RobustSolver(const RobustSolverParams& params)
    : GenericSolver(params.solver, params.specialSymbols), log_(false) {
  switch (params.outlierRemovalMethod) {
    case OutlierRemovalMethod::NONE: {
      outlier_removal_ =
          nullptr;  // only returns optimize true or optimize false
    } break;
    case OutlierRemovalMethod::PCM2D: {
      outlier_removal_ =
          kimera_rpgo::make_unique<Pcm2D>(params.pcm_odomThreshold,
                                          params.pcm_lcThreshold,
                                          params.specialSymbols);
    } break;
    case OutlierRemovalMethod::PCM3D: {
      outlier_removal_ =
          kimera_rpgo::make_unique<Pcm3D>(params.pcm_odomThreshold,
                                          params.pcm_lcThreshold,
                                          params.specialSymbols);
    } break;
    case OutlierRemovalMethod::PCM_Simple2D: {
      outlier_removal_ =
          kimera_rpgo::make_unique<PcmSimple2D>(params.pcmDist_transThreshold,
                                                params.pcmDist_rotThreshold,
                                                params.specialSymbols);
    } break;
    case OutlierRemovalMethod::PCM_Simple3D: {
      outlier_removal_ =
          kimera_rpgo::make_unique<PcmSimple3D>(params.pcmDist_transThreshold,
                                                params.pcmDist_rotThreshold,
                                                params.specialSymbols);
    } break;
    default: {
      log<WARNING>("Undefined outlier removal method");
      exit(EXIT_FAILURE);
    }
  }

  // toggle verbosity
  switch (params.verbosity) {
    case Verbosity::UPDATE: {
      if (outlier_removal_) outlier_removal_->setQuiet();
    } break;
    case Verbosity::QUIET: {
      if (outlier_removal_)
        outlier_removal_->setQuiet();  // set outlier removal quiet
      setQuiet();                      // set solver quiet
    } break;
    case Verbosity::VERBOSE: {
      log<INFO>("Starting RobustSolver.");
    } break;
    default: {
      log<WARNING>("Unrecognized verbosity. Automatically setting to UPDATE. ");
    }
  }
}

void RobustSolver::optimize() {
  if (solver_type_ == Solver::LM) {
    gtsam::LevenbergMarquardtParams params;
    if (debug_) {
      params.setVerbosityLM("SUMMARY");
      log<INFO>("Running LM");
    }
    params.diagonalDamping = true;
    values_ =
        gtsam::LevenbergMarquardtOptimizer(nfg_, values_, params).optimize();
  } else if (solver_type_ == Solver::GN) {
    gtsam::GaussNewtonParams params;
    if (debug_) {
      params.setVerbosity("ERROR");
      log<INFO>("Running GN");
    }
    values_ = gtsam::GaussNewtonOptimizer(nfg_, values_, params).optimize();
  } else {
    log<WARNING>("Unsupported Solver");
    exit(EXIT_FAILURE);
  }
}

void RobustSolver::forceUpdate(const gtsam::NonlinearFactorGraph& nfg,
                               const gtsam::Values& values) {
  if (outlier_removal_) {
    outlier_removal_->removeOutliers(nfg, values, &nfg_, &values_);
  } else {
    addAndCheckIfOptimize(nfg, values);
  }
  // optimize
  optimize();
}

void RobustSolver::update(const gtsam::NonlinearFactorGraph& factors,
                          const gtsam::Values& values) {
  bool do_optimize;
  if (outlier_removal_) {
    do_optimize =
        outlier_removal_->removeOutliers(factors, values, &nfg_, &values_);
  } else {
    do_optimize = addAndCheckIfOptimize(factors, values);
  }

  if (do_optimize) optimize();  // optimize once after loading
  if (log_) {
    Stats stats = outlier_removal_->getRejectionStats();
    double error = nfg_.error(values_);
    // Write/append to file
    std::string log_file = log_path_ + "/log.txt";
    std::ofstream logfile;
    logfile.open(log_file, std::ios::app);  // append instead of overwrite
    logfile << stats.lc << " " << stats.good_lc << " "
            << stats.odom_consistent_lc << " " << stats.multirobot_lc << " "
            << stats.good_multirobot_lc << " " << stats.landmark_measurements
            << " " << stats.good_landmark_measurements << " " << error
            << std::endl;
    logfile.close();
    std::string error_file = log_path_ + "/error.txt";
    std::ofstream errorfile;
    errorfile.open(error_file, std::ios::app);  // append instead of overwrite
    for (const double e : stats.consistency_error) errorfile << e << " ";
    errorfile << std::endl;
    errorfile.close();
  }
  return;
}

EdgePtr RobustSolver::removeLastLoopClosure(char prefix_1, char prefix_2) {
  ObservationId id(prefix_1, prefix_2);
  EdgePtr removed_edge;
  if (outlier_removal_) {
    // removing loop closure so values should not change
    removed_edge = outlier_removal_->removeLastLoopClosure(id, &nfg_);
  } else {
    removed_edge = removeLastFactor();
  }

  optimize();
  return removed_edge;
}

EdgePtr RobustSolver::removeLastLoopClosure() {
  EdgePtr removed_edge;
  if (outlier_removal_) {
    // removing loop closure so values should not change
    removed_edge = outlier_removal_->removeLastLoopClosure(&nfg_);
  } else {
    removed_edge = removeLastFactor();
  }

  optimize();
  return removed_edge;
}

void RobustSolver::ignorePrefix(char prefix) {
  if (outlier_removal_) {
    outlier_removal_->ignoreLoopClosureWithPrefix(prefix, &nfg_);
  } else {
    log<WARNING>(
        "'ignorePrefix' currently not implemented for no outlier rejection "
        "case");
  }

  optimize();
  return;
}

void RobustSolver::revivePrefix(char prefix) {
  if (outlier_removal_) {
    outlier_removal_->reviveLoopClosureWithPrefix(prefix, &nfg_);
  } else {
    log<WARNING>(
        "'revivePrefix' and 'ignorePrefix' currently not implemented for no "
        "outlier rejection case");
  }

  optimize();
  return;
}

std::vector<char> RobustSolver::getIgnoredPrefixes() {
  if (outlier_removal_) {
    return outlier_removal_->getIgnoredPrefixes();
  } else {
    log<WARNING>(
        "'revivePrefix' and 'ignorePrefix' currently not implemented for no "
        "outlier rejection case");
  }
  std::vector<char> empty;
  return empty;
}

void RobustSolver::saveData(std::string folder_path) const {
  std::string g2o_file_path = folder_path + "/result.g2o";
  gtsam::writeG2o(nfg_, values_, g2o_file_path);
  if (outlier_removal_) {
    outlier_removal_->saveData(folder_path);
  }
}

void RobustSolver::enableLogging(std::string path) {
  log_ = true;
  log_path_ = path;
  // Initialize log file
  std::string log_file = path + "/log.txt";
  std::ofstream logfile;
  logfile.open(log_file);  // append instead of overwrite
  logfile
      << "#lc #good-lc #odom-consistent-lc #multirobot-lc #good-multirobot-lc "
         "#ldmrk-measurements #good-ldmrk-measurements #error"
      << std::endl;
  logfile.close();
  // Initialize data file
  std::string data_file = path + "/error.txt";
  std::ofstream datafile;
  datafile.open(data_file);  // append instead of overwrite
  datafile << "#consistency-error" << std::endl;
  datafile.close();
}
}  // namespace kimera_rpgo
