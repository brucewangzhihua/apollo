/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/evaluator/vehicle/lane_aggregating_evaluator.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "modules/common/math/vec2d.h"
#include "modules/prediction/common/feature_output.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/prediction/common/prediction_system_gflags.h"
#include "modules/prediction/container/container_manager.h"
#include "modules/prediction/container/obstacles/obstacles_container.h"
#include "modules/prediction/container/pose/pose_container.h"

namespace apollo {
namespace prediction {

using apollo::common::adapter::AdapterConfig;
using apollo::common::math::Vec2d;
using apollo::common::Point3D;
using apollo::common::TrajectoryPoint;
using apollo::perception::PerceptionObstacle;
using apollo::perception::PerceptionObstacles;

LaneAggregatingEvaluator::LaneAggregatingEvaluator()
    : device_(torch::kCPU) {
  // LoadModel();
}

bool LaneAggregatingEvaluator::Evaluate(Obstacle* obstacle_ptr) {
  // Sanity checks.
  CHECK_NOTNULL(obstacle_ptr);
  int id = obstacle_ptr->id();
  if (!obstacle_ptr->latest_feature().IsInitialized()) {
    AERROR << "Obstacle [" << id << "] has no latest feature.";
    return false;
  }
  Feature* latest_feature_ptr = obstacle_ptr->mutable_latest_feature();
  CHECK_NOTNULL(latest_feature_ptr);
  if (!latest_feature_ptr->has_lane() ||
      !latest_feature_ptr->lane().has_lane_graph_ordered()) {
    AERROR << "Obstacle [" << id << "] has no lane graph.";
    return false;
  }
  LaneGraph* lane_graph_ptr =
      latest_feature_ptr->mutable_lane()->mutable_lane_graph_ordered();
  CHECK_NOTNULL(lane_graph_ptr);
  if (lane_graph_ptr->lane_sequence_size() == 0) {
    AERROR << "Obstacle [" << id << "] has no lane sequences.";
    return false;
  }
  ADEBUG << "There are " << lane_graph_ptr->lane_sequence_size()
         << " lane sequences to scan.";

  // Extract features, and do model inferencing.

  return true;
}

bool LaneAggregatingEvaluator::ExtractObstacleFeatures(
    const Obstacle* obstacle_ptr, std::vector<double>* feature_values) {
  // Sanity checks.
  CHECK_NOTNULL(obstacle_ptr);
  feature_values->clear();
  FLAGS_cruise_historical_frame_length = 20;
  std::vector<double> has_history(FLAGS_cruise_historical_frame_length, 1.0);
  std::vector<std::pair<double, double>> pos_history(
      FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<std::pair<double, double>> vel_history(
      FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<std::pair<double, double>> acc_history(
      FLAGS_cruise_historical_frame_length, std::make_pair(0.0, 0.0));
  std::vector<double> vel_heading_history(FLAGS_cruise_historical_frame_length,
                                          0.0);
  std::vector<double> vel_heading_changing_rate_history(
      FLAGS_cruise_historical_frame_length, 0.0);

  // Get obstacle's current position to set up the relative coord. system.
  const Feature& obs_curr_feature = obstacle_ptr->latest_feature();
  double obs_curr_heading = obs_curr_feature.velocity_heading();
  std::pair<double, double> obs_curr_pos = std::make_pair(
      obs_curr_feature.position().x(), obs_curr_feature.position().y());
  double prev_timestamp = obs_curr_feature.timestamp();

  // Starting from the most recent timestamp and going backward.
  ADEBUG << "Obstacle has " << obstacle_ptr->history_size()
         << " history timestamps.";
  for (std::size_t i = 0; i < std::min(obstacle_ptr->history_size(),
                                       FLAGS_cruise_historical_frame_length);
       ++i) {
    const Feature& feature = obstacle_ptr->feature(i);
    if (!feature.IsInitialized()) {
      has_history[i] = 0.0;
      continue;
    }
    if (i != 0 && has_history[i - 1] == 0.0) {
      has_history[i] = 0.0;
      continue;
    }
    // Extract normalized position info.
    if (feature.has_position()) {
      pos_history[i] = WorldCoordToObjCoord(
          std::make_pair(feature.position().x(), feature.position().y()),
          obs_curr_pos, obs_curr_heading);
    } else {
      has_history[i] = 0.0;
    }
    // Extract normalized velocity info.
    if (feature.has_velocity()) {
      auto vel_end = WorldCoordToObjCoord(
          std::make_pair(feature.velocity().x(), feature.velocity().y()),
          obs_curr_pos, obs_curr_heading);
      auto vel_begin = WorldCoordToObjCoord(std::make_pair(0.0, 0.0),
                                            obs_curr_pos, obs_curr_heading);
      vel_history[i] = std::make_pair(vel_end.first - vel_begin.first,
                                      vel_end.second - vel_begin.second);
    } else {
      has_history[i] = 0.0;
    }
    // Extract normalized acceleration info.
    if (feature.has_acceleration()) {
      auto acc_end =
          WorldCoordToObjCoord(std::make_pair(feature.acceleration().x(),
                                              feature.acceleration().y()),
                               obs_curr_pos, obs_curr_heading);
      auto acc_begin = WorldCoordToObjCoord(std::make_pair(0.0, 0.0),
                                            obs_curr_pos, obs_curr_heading);
      acc_history[i] = std::make_pair(acc_end.first - acc_begin.first,
                                      acc_end.second - acc_begin.second);
    } else {
      has_history[i] = 0.0;
    }
    // Extract velocity heading info.
    if (feature.has_velocity_heading()) {
      vel_heading_history[i] =
          WorldAngleToObjAngle(feature.velocity_heading(), obs_curr_heading);
      if (i != 0) {
        vel_heading_changing_rate_history[i] =
            (vel_heading_history[i] - vel_heading_history[i - 1]) /
            (feature.timestamp() - prev_timestamp + FLAGS_double_precision);
        prev_timestamp = feature.timestamp();
      }
    } else {
      has_history[i] = 0.0;
    }
  }

  for (std::size_t i = obstacle_ptr->history_size();
       i < FLAGS_cruise_historical_frame_length; ++i) {
    has_history[i] = 0.0;
  }

  // Update the extracted features into the feature_values vector.
  for (std::size_t i = 0; i < FLAGS_cruise_historical_frame_length; i++) {
    feature_values->push_back(has_history[i]);
    feature_values->push_back(pos_history[i].first);
    feature_values->push_back(pos_history[i].second);
    feature_values->push_back(vel_history[i].first);
    feature_values->push_back(vel_history[i].second);
    feature_values->push_back(acc_history[i].first);
    feature_values->push_back(acc_history[i].second);
    feature_values->push_back(vel_heading_history[i]);
    feature_values->push_back(vel_heading_changing_rate_history[i]);
  }

  return true;
}

bool LaneAggregatingEvaluator::ExtractStaticEnvFeatures(
    const Obstacle* obstacle_ptr, const LaneGraph* lane_graph_ptr,
    std::vector<std::vector<double>>* feature_values,
    std::vector<int>* lane_sequence_idx_to_remove) {
  // Sanity checks.
  CHECK_NOTNULL(lane_graph_ptr);
  feature_values->clear();

  // Get obstacle's current position to set up the relative coord. system.
  const Feature& obs_curr_feature = obstacle_ptr->latest_feature();
  double obs_curr_heading = obs_curr_feature.velocity_heading();
  std::pair<double, double> obs_curr_pos = std::make_pair(
      obs_curr_feature.position().x(), obs_curr_feature.position().y());

  // Go through every lane-sequence (ordered from left to right) and
  // extract needed features.
  for (int i = 0; i < lane_graph_ptr->lane_sequence_size(); ++i) {
    // Get all the properties of the current lane-sequence.
    // Go through all the lane-points to fill up the feature_values.
    const LaneSequence& lane_sequence = lane_graph_ptr->lane_sequence(i);
    std::vector<double> curr_feature_values;

    // Extract features from backward lane-points.
    size_t count = 0;
    std::vector<double> backward_feature_values;
    for (int j = lane_sequence.adc_lane_segment_idx(); j >= 0; --j) {
      if (count >= SINGLE_LANE_FEATURE_SIZE * BACKWARD_LANE_POINTS_SIZE) {
        break;
      }
      const LaneSegment& lane_segment = lane_sequence.lane_segment(j);
      int k_starting_idx = lane_segment.lane_point_size() - 1;
      if (j == lane_sequence.adc_lane_segment_idx()) {
        k_starting_idx = std::min(lane_segment.adc_lane_point_idx(),
                                  lane_segment.lane_point_size() - 1);
      }
      for (int k = k_starting_idx; k >= 0; --k) {
        if (count >= SINGLE_LANE_FEATURE_SIZE * BACKWARD_LANE_POINTS_SIZE) {
          break;
        }
        const LanePoint& lane_point = lane_segment.lane_point(k);
        std::pair<double, double> relative_s_l =
            WorldCoordToObjCoord(std::make_pair(lane_point.position().x(),
                                                lane_point.position().y()),
                                 obs_curr_pos, obs_curr_heading);
        double relative_ang =
            WorldAngleToObjAngle(lane_point.heading(), obs_curr_heading);

        backward_feature_values.push_back(lane_point.kappa());
        backward_feature_values.push_back(relative_ang);
        backward_feature_values.push_back(relative_s_l.first);
        backward_feature_values.push_back(relative_s_l.second);

        count += 4;
      }
    }
    // If lane-points are not enough, then extrapolate linearly.
    while (count >= SINGLE_LANE_FEATURE_SIZE * 2 &&
           count < SINGLE_LANE_FEATURE_SIZE * BACKWARD_LANE_POINTS_SIZE) {
      std::size_t s = backward_feature_values.size();
      double relative_l_new =
          2 * backward_feature_values[s - 1] - backward_feature_values[s - 5];
      double relative_s_new =
          2 * backward_feature_values[s - 2] - backward_feature_values[s - 6];
      double relative_ang_new = backward_feature_values[s - 3];

      backward_feature_values.push_back(0.0);
      backward_feature_values.push_back(relative_ang_new);
      backward_feature_values.push_back(relative_s_new);
      backward_feature_values.push_back(relative_l_new);

      count += 4;
    }

    for (int j = static_cast<int>(backward_feature_values.size()) - 1; j >= 0;
         --j) {
      curr_feature_values.push_back(backward_feature_values[j]);
    }

    // Extract features from forward lane-points.
    count = 0;
    for (int j = lane_sequence.adc_lane_segment_idx();
         j < lane_sequence.lane_segment_size(); ++j) {
      if (count >= SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
        break;
      }
      const LaneSegment& lane_segment = lane_sequence.lane_segment(j);
      int k_starting_idx = 0;
      if (j == lane_sequence.adc_lane_segment_idx()) {
        k_starting_idx = std::min(lane_segment.adc_lane_point_idx(),
                                  lane_segment.lane_point_size() - 1);
      }
      for (int k = k_starting_idx; k < lane_segment.lane_point_size(); ++k) {
        if (count >= SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
          break;
        }
        const LanePoint& lane_point = lane_segment.lane_point(k);
        std::pair<double, double> relative_s_l =
            WorldCoordToObjCoord(std::make_pair(lane_point.position().x(),
                                                lane_point.position().y()),
                                 obs_curr_pos, obs_curr_heading);
        double relative_ang =
            WorldAngleToObjAngle(lane_point.heading(), obs_curr_heading);

        curr_feature_values.push_back(relative_s_l.second);
        curr_feature_values.push_back(relative_s_l.first);
        curr_feature_values.push_back(relative_ang);
        curr_feature_values.push_back(lane_point.kappa());
        count += 4;
      }
    }
    // If lane-points are not enough, then extrapolate linearly.
    while (count >= SINGLE_LANE_FEATURE_SIZE * 2 &&
           count < SINGLE_LANE_FEATURE_SIZE * LANE_POINTS_SIZE) {
      std::size_t s = curr_feature_values.size();
      double relative_l_new = 2 * curr_feature_values[s - 4] -
                              curr_feature_values[s - 8];
      double relative_s_new = 2 * curr_feature_values[s - 3] -
                              curr_feature_values[s - 7];
      double relative_ang_new = curr_feature_values[s - 2];

      curr_feature_values.push_back(relative_l_new);
      curr_feature_values.push_back(relative_s_new);
      curr_feature_values.push_back(relative_ang_new);
      curr_feature_values.push_back(0.0);
      count += 4;
    }

    feature_values->push_back(curr_feature_values);
  }

  return true;
}

}  // namespace prediction
}  // namespace apollo